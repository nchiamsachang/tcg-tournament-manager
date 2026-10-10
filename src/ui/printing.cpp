#include "printing.h"

#include "dialogs.h"
#include "store.h"
#include "theme.h"

#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QPainter>
#include <QPdfWriter>
#include <QPrintDialog>
#include <QRadioButton>
#include <QFontMetricsF>
#include <QVBoxLayout>
#include <algorithm>

namespace printing {

static const int MARGIN_MM = 13;
static const char *FACE = "Segoe UI";
static const int WRAP = Qt::TextWordWrap | Qt::TextWrapAnywhere;

// `text` made to wrap well in `width` with WORDS: lines break between words, and after a
// hyphen.  Only a word (or hyphenated part) too long for a line of its own is given break
// points inside it, so one very long name does not cause other words to be cut in two.
// Measuring and drawing a text must both use this, with the same width.
static const int WORDS = Qt::TextWordWrap;

static QString fitted(const QPainter &p, qreal width, const QString &text)
{
    const QFontMetricsF fm(p.font(), p.device());
    const QChar breakPoint(0x200B);     // zero-width space
    QStringList words = text.split(QChar(' '));
    for (QString &word : words) {
        if (fm.horizontalAdvance(word) <= width)
            continue;
        QStringList parts;
        int from = 0;
        for (int i = 0; i < word.size(); ++i) {
            if (word[i] == QChar('-') || i == word.size() - 1) {
                parts << word.mid(from, i - from + 1);
                from = i + 1;
            }
        }
        for (QString &part : parts) {
            if (fm.horizontalAdvance(part) <= width)
                continue;
            QString spaced;
            for (int i = 0; i < part.size(); ++i) {
                // not inside a surrogate pair
                if (i > 0 && !part[i].isLowSurrogate())
                    spaced += breakPoint;
                spaced += part[i];
            }
            part = spaced;
        }
        word = parts.join(breakPoint);
    }
    return words.join(QChar(' '));
}

RoundData modernRoundData(const db::Row &tournament, int roundNumber, const db::Rows &pairings)
{
    RoundData d;
    for (const db::Row &p : pairings) {
        if (p["player2_name"].isNull()) {
            d.byes << p["player1_name"].toString();
        } else {
            Group g;
            g.number = p["table_number"].toInt();
            g.players = {{p["player1_name"].toString(), 0}, {p["player2_name"].toString(), 0}};
            d.groups << g;
        }
    }
    const QString game = tournament["game"].toString();
    d.event = tournament["name"].toString();
    d.detail = (T::gameShort(game) + " " + tournament["format"].toString()).trimmed();
    d.round = QStringLiteral("Round %1").arg(roundNumber);
    d.unit = "Table";
    d.seated = false;
    return d;
}

RoundData commanderRoundData(const QString &eventName, const QString &roundLabel, const QVariantMap &round)
{
    RoundData d;
    for (const QVariant &pv : round["pods"].toList()) {
        const QVariantMap pod = pv.toMap();
        Group g;
        g.number = pod["pod_number"].toInt();
        for (const QVariant &sv : pod["seats"].toList())
            g.players << Player{sv.toMap()["display_name"].toString(), sv.toMap()["seat"].toInt()};
        d.groups << g;
    }
    for (const QVariant &b : round["byes"].toList())
        d.byes << b.toMap()["display_name"].toString();
    d.event = eventName;
    d.detail = "MTG Commander";
    d.round = roundLabel;
    d.unit = "Pod";
    d.seated = true;
    return d;
}

Doc pairingsDoc(const RoundData &data, const QString &sort)
{
    Doc doc;
    const QString unit = data.unit;
    QString what;
    if (sort == "name") {
        for (const Group &g : data.groups) {
            for (int i = 0; i < g.players.size(); ++i) {
                QStringList others;
                for (int j = 0; j < g.players.size(); ++j)
                    if (j != i)
                        others << g.players[j].name;
                QStringList row{g.players[i].name, QStringLiteral("%1 %2").arg(unit).arg(g.number)};
                if (data.seated)
                    row << QString::number(g.players[i].seat);
                row << others.join(", ");
                doc.rows << row;
            }
        }
        for (const QString &b : data.byes) {
            QStringList row{b, "Bye"};
            if (data.seated)
                row << "";
            row << "";
            doc.rows << row;
        }
        std::stable_sort(doc.rows.begin(), doc.rows.end(), [](const QStringList &a, const QStringList &b) {
            return a[0].toCaseFolded() < b[0].toCaseFolded();
        });
        doc.columns = {{"Player", 0.34}, {unit, 0.14}};
        if (data.seated)
            doc.columns << qMakePair(QString("Seat"), 0.09);
        doc.columns << qMakePair(QString("Playing against"), data.seated ? 0.43 : 0.52);
        doc.bold = {0, 1};
        what = "Pairings by player name";
    } else {
        int width = 2;
        if (!data.groups.isEmpty()) {
            width = 0;
            for (const Group &g : data.groups)
                width = qMax(width, int(g.players.size()));
        }
        const double first = 0.13;
        doc.columns << qMakePair(unit, first);
        for (int i = 0; i < width; ++i) {
            const QString label = data.seated
                ? QStringLiteral("Seat %1").arg(i + 1) + (i == 0 ? QStringLiteral(" · first turn") : QString())
                : QStringLiteral("Player %1").arg(i + 1);
            doc.columns << qMakePair(label, (1 - first) / width);
        }
        QList<Group> groups = data.groups;
        std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
            return std::make_pair(a.number == 0, a.number) < std::make_pair(b.number == 0, b.number);
        });
        for (const Group &g : groups) {
            QStringList row{QStringLiteral("%1 %2").arg(unit).arg(g.number)};
            for (const Player &p : g.players)
                row << p.name;
            while (row.size() < width + 1)
                row << "";
            doc.rows << row;
        }
        for (int i = 0; i <= width; ++i)
            doc.bold << i;
        what = "Pairings by " + unit.toLower();
        if (!data.byes.isEmpty())
            doc.note = "Bye: " + data.byes.join(", ");
    }
    doc.title = data.event;
    doc.sub = QStringLiteral("%1  ·  %2  ·  %3").arg(data.round, data.detail, what);
    doc.empty = "No pairings in this round.";
    return doc;
}

Doc signsDoc(const RoundData &data)
{
    Doc doc;
    doc.signs = true;
    doc.unit = data.unit.toUpper();
    for (const Group &g : data.groups)
        if (g.number)
            doc.numbers << g.number;
    std::sort(doc.numbers.begin(), doc.numbers.end());
    doc.event = data.event;
    doc.round = data.round;
    doc.title = data.event;
    return doc;
}

Doc buildDoc(const RoundData &data, const QString &what, const QString &sort)
{
    return what == "signs" ? signsDoc(data) : pairingsDoc(data, sort);
}

static QFont face(double points, bool bold = false)
{
    QFont f(FACE);
    f.setPointSizeF(points);
    f.setBold(bold);
    return f;
}

static void renderTable(QPainter &p, QPagedPaintDevice *device, const Doc &doc, double w, double h, double pt, int &pages)
{
    const QColor black("#000000");
    const double padX = 5 * pt, padY = 4.5 * pt;
    const double footH = 16 * pt;
    const double bottom = h - footH;
    QList<double> widths;
    for (const auto &c : doc.columns)
        widths << c.second * w;
    const QFont headFont = face(9, true), cellFont = face(11.5), boldFont = face(11.5, true);

    auto textH = [&](const QFont &font, const QString &text, double width) {
        p.setFont(font);
        const qreal inner = qMax(10.0, width - 2 * padX);
        return p.boundingRect(QRectF(0, 0, inner, 1e6), WORDS, fitted(p, inner, text)).height();
    };
    auto footer = [&] {
        p.setFont(face(8));
        p.drawText(QRectF(0, h - footH + 4 * pt, w, footH), Qt::AlignLeft, doc.title);
        p.drawText(QRectF(0, h - footH + 4 * pt, w, footH), Qt::AlignRight, QStringLiteral("Page %1").arg(pages));
    };
    auto header = [&](double y) {
        double hh = 0;
        for (int i = 0; i < doc.columns.size(); ++i)
            hh = qMax(hh, textH(headFont, doc.columns[i].first.toUpper(), widths[i]));
        hh += 2 * padY;
        double x = 0;
        p.setFont(headFont);
        for (int i = 0; i < doc.columns.size(); ++i) {
            const QString heading = doc.columns[i].first.toUpper();
            p.drawText(QRectF(x + padX, y + padY, widths[i] - 2 * padX, hh), WORDS, fitted(p, qMax(10.0, widths[i] - 2 * padX), heading));
            x += widths[i];
        }
        p.setPen(QPen(black, 1.6 * pt));
        p.drawLine(QPointF(0, y + hh), QPointF(w, y + hh));
        p.setPen(black);
        return y + hh;
    };

    double y = 0;
    p.setFont(face(20, true));
    QRectF r = p.boundingRect(QRectF(0, 0, w, 1e6), WORDS, fitted(p, w, doc.title));
    p.drawText(QRectF(0, y, w, r.height()), WORDS, fitted(p, w, doc.title));
    y += r.height() + 2 * pt;
    p.setFont(face(10.5));
    r = p.boundingRect(QRectF(0, 0, w, 1e6), WORDS, fitted(p, w, doc.sub));
    p.drawText(QRectF(0, y, w, r.height()), WORDS, fitted(p, w, doc.sub));
    y += r.height() + 8 * pt;
    y = header(y);

    if (doc.rows.isEmpty()) {
        p.setFont(cellFont);
        p.drawText(QRectF(padX, y + padY, w, 30 * pt), WRAP, doc.empty);
        y += 30 * pt;
    }
    for (const QStringList &row : doc.rows) {
        double rh = 0;
        for (int i = 0; i < row.size() && i < widths.size(); ++i)
            rh = qMax(rh, textH(doc.bold.contains(i) ? boldFont : cellFont, row[i], widths[i]));
        rh += 2 * padY;
        if (y + rh > bottom && y > 0) {         // never split a row: move it whole to the next page
            footer();
            device->newPage();
            ++pages;
            y = header(0.0);
        }
        double x = 0;
        for (int i = 0; i < row.size() && i < widths.size(); ++i) {
            p.setFont(doc.bold.contains(i) ? boldFont : cellFont);
            p.drawText(QRectF(x + padX, y + padY, widths[i] - 2 * padX, rh), WORDS, fitted(p, qMax(10.0, widths[i] - 2 * padX), row[i]));
            x += widths[i];
        }
        p.setPen(QPen(black, 0.6 * pt));
        p.drawLine(QPointF(0, y + rh), QPointF(w, y + rh));
        p.setPen(black);
        y += rh;
    }
    if (!doc.note.isEmpty()) {
        const double nh = textH(boldFont, doc.note, w) + 10 * pt;
        if (y + nh > bottom) {
            footer();
            device->newPage();
            ++pages;
            y = 0;
        }
        p.setFont(boldFont);
        p.drawText(QRectF(0, y + 8 * pt, w, nh), WRAP, doc.note);
    }
    footer();
}

static void renderSigns(QPainter &p, QPagedPaintDevice *device, const Doc &doc, double w, double h, int &pages)
{
    if (doc.numbers.isEmpty()) {
        p.setFont(face(14));
        p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, "No tables in this round.");
        return;
    }
    const int center = Qt::AlignHCenter | Qt::AlignVCenter;
    for (int i = 0; i < doc.numbers.size(); ++i) {
        if (i) {
            device->newPage();
            ++pages;
        }
        const QString number = QString::number(doc.numbers[i]);
        p.setFont(face(44, true));
        p.drawText(QRectF(0, h * 0.06, w, h * 0.12), center, doc.unit);
        double size = 330.0;                        // shrink until the number fits its box
        const QRectF box(0, h * 0.18, w, h * 0.56);
        while (size > 40) {
            p.setFont(face(size, true));
            const QRectF r = p.boundingRect(box, center, number);
            if (r.width() <= w * 0.92 && r.height() <= box.height())
                break;
            size -= 10;
        }
        p.drawText(box, center, number);
        p.setFont(face(20, true));
        p.drawText(QRectF(0, h * 0.78, w, h * 0.08), center | WRAP, doc.event);
        p.setFont(face(14));
        p.drawText(QRectF(0, h * 0.86, w, h * 0.06), center, doc.round);
    }
}

int render(QPagedPaintDevice *device, const QList<Doc> &docs)
{
    QPainter p(device);
    const double pt = device->logicalDpiY() / 72.0;
    const double w = device->width(), h = device->height();
    p.setPen(QColor("#000000"));
    int pages = 1;
    for (int i = 0; i < docs.size(); ++i) {
        if (i) {
            device->newPage();
            ++pages;
        }
        if (docs[i].signs)
            renderSigns(p, device, docs[i], w, h, pages);
        else
            renderTable(p, device, docs[i], w, h, pt, pages);
    }
    p.end();
    return pages;
}

QPageLayout pageLayout(const QString &paper)
{
    return QPageLayout(QPageSize(paper == "A4" ? QPageSize::A4 : QPageSize::Letter), QPageLayout::Portrait,
                       QMarginsF(MARGIN_MM, MARGIN_MM, MARGIN_MM, MARGIN_MM), QPageLayout::Millimeter);
}

QByteArray pdfBytes(const QList<Doc> &docs, const QString &paper, int *pages)
{
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    int n = 0;
    {
        QPdfWriter writer(&buf);
        writer.setResolution(300);
        writer.setPageLayout(pageLayout(paper));
        n = render(&writer, docs);
    }
    buf.close();
    if (pages)
        *pages = n;
    return buf.data();
}

int pageCount(const Doc &doc, const QString &paper)
{
    int pages = 0;
    pdfBytes({doc}, paper, &pages);
    return pages;
}

bool savePdf(const QList<Doc> &docs, const QString &path, const QString &paper)
{
    const QByteArray data = pdfBytes(docs, paper);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(data) == data.size();
}

QList<Doc> tournamentReport(qint64 tournamentId)
{
    const db::Row t = tdb::tournamentById(tournamentId);
    if (t.isEmpty())
        throw std::runtime_error("Tournament not found");
    const QString game = t["game"].toString();
    const QString name = t["name"].toString();
    const QString detail = (T::gameShort(game) + " " + t["format"].toString()).trimmed()
                           + QStringLiteral("  ·  ") + t["tournament_date"].toString().left(10);
    QList<Doc> docs;
    for (const db::Row &r : tdb::rounds(tournamentId)) {
        Doc d;
        d.title = name;
        d.sub = QStringLiteral("Round %1  ·  %2  ·  Pairings and results").arg(r["round_number"].toInt()).arg(detail);
        d.columns = {{"Table", 0.11}, {"Player 1", 0.32}, {"Player 2", 0.32}, {"Result", 0.25}};
        d.bold = {0};
        d.empty = "No pairings in this round.";
        for (const db::Row &m : tdb::roundPairings(r["round_id"].toLongLong())) {
            const QString w = m["winner"].toString();
            const QString p1 = m["player1_name"].toString(), p2 = m["player2_name"].toString();
            const QString result = w == "BYE" ? "Bye" : w == "DRAW" ? "Draw" : w == "PLAYER1" ? p1 + " wins"
                                 : w == "PLAYER2" ? p2 + " wins" : "Not reported";
            d.rows << QStringList{m["table_number"].isNull() ? QStringLiteral("—") : m["table_number"].toString(), p1,
                                  p2.isEmpty() ? QStringLiteral("—") : p2, result};
        }
        docs << d;
    }
    Doc s;
    s.title = name;
    s.sub = (t["status"].toString() == "COMPLETED" ? QStringLiteral("Final standings") : QStringLiteral("Standings"))
            + QStringLiteral("  ·  ") + detail;
    const QList<QPair<QString, QString>> tiebreaks = swiss::tiebreakColumns(game);
    // a tiebreaker column, wider for a long heading ("Opp Opp Win%")
    const auto tb = [](const QString &heading) { return heading.size() > 8 ? 0.17 : 0.12; };
    double tiebreakShare = 0;
    for (const auto &c : tiebreaks)
        tiebreakShare += tb(c.second);
    s.columns = {{"#", 0.08}, {"Player", 0.92 - 0.30 - tiebreakShare}, {"Pts", 0.10}, {"W", 0.10}, {"L", 0.10}};
    for (const auto &c : tiebreaks)
        s.columns << qMakePair(c.second, tb(c.second));
    s.bold = {0, 1, 2};
    s.empty = "No standings yet.";
    for (const db::Row &p : swiss::viewStandings(tournamentId, game)) {
        QStringList row{p["standing"].toString(), p["display_name"].toString(), p["match_points"].toString(),
                        p["match_wins"].toString(), p["match_losses"].toString()};
        for (const auto &c : tiebreaks)
            row << swiss::tiebreakText(game, c.first, p[c.first].toDouble());
        s.rows << row;
    }
    docs << s;
    return docs;
}

QString csvCell(QString text)
{
    if (text.contains(',') || text.contains('"') || text.contains('\n'))
        text = "\"" + text.replace("\"", "\"\"") + "\"";
    return text;
}

PrintDialog::PrintDialog(QWidget *parent, const RoundData &data)
    : QDialog(parent), data_(data), printer_(QPrinter::HighResolution)
{
    setWindowTitle(QStringLiteral("Print — ") + data.round);
    const QSize avail = parent ? parent->window()->size() : QSize(980, 680);
    resize(qMin(980, avail.width()), qMin(680, avail.height()));
    setMinimumSize(300, 320);

    outer_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    outer_->setContentsMargins(16, 16, 16, 16);
    outer_->setSpacing(14);

    QVBoxLayout *side = nullptr;
    QScrollArea *sideScroll = T::scrollArea(&side);
    sideScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    side->setSpacing(4);
    side->addWidget(T::lbl("Print", T::TEXT, 20, 700));
    side->addWidget(T::lbl(QStringLiteral("%1 · %2. Printing is optional and never changes results.").arg(data.event, data.round),
                           T::MUTED, 12, 400, true));
    side->addSpacing(8);
    whatGroup = group(side, "What to print", {
        {QStringLiteral("Pairings, by %1").arg(data.unit.toLower()), "pairings/table"},
        {"Pairings, by player name", "pairings/name"},
        {QStringLiteral("%1-number signs").arg(data.unit), "signs/table"},
    });
    paperGroup = group(side, "Paper", {{"Letter", "Letter"}, {"A4", "A4"}});
    pagesLabel = T::lbl("", T::MUTED, 12);
    side->addSpacing(6);
    side->addWidget(pagesLabel);
    side->addStretch();

    auto *left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(8);
    left->addWidget(sideScroll, 1);
    printBtn = T::button(QStringLiteral("Print…"), "primary");
    connect(printBtn, &QPushButton::clicked, this, [this] { print(); });
    pdfBtn = T::button(QStringLiteral("Save as PDF…"));
    connect(pdfBtn, &QPushButton::clicked, this, [this] { savePdfAs(); });
    closeBtn = T::button("Close", "ghost");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    for (QPushButton *b : {printBtn, pdfBtn, closeBtn})      // always reachable: outside the scrolling part
        left->addWidget(b);
    left_ = new QWidget;
    left_->setLayout(left);
    left_->setMaximumWidth(300);
    outer_->addWidget(left_);

    preview_ = new QPrintPreviewWidget(&printer_);
    preview_->setMinimumSize(200, 140);
    connect(preview_, &QPrintPreviewWidget::paintRequested, this, [this](QPrinter *printer) { printing::render(printer, {doc()}); });
    preview_->setZoomMode(QPrintPreviewWidget::FitToWidth);
    outer_->addWidget(preview_, 1);
    refresh();
}

QButtonGroup *PrintDialog::group(QVBoxLayout *layout, const QString &title, const QList<QPair<QString, QString>> &options)
{
    layout->addSpacing(6);
    layout->addWidget(T::caps(title));
    auto *g = new QButtonGroup(this);
    for (int i = 0; i < options.size(); ++i) {
        auto *rb = new QRadioButton(options[i].first);
        rb->setProperty("value", options[i].second);
        rb->setChecked(i == 0);
        g->addButton(rb);
        layout->addWidget(rb);
    }
    connect(g, &QButtonGroup::buttonClicked, this, [this] { refresh(); });
    return g;
}

void PrintDialog::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    const bool stacked = width() < 640;
    outer_->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    left_->setMaximumWidth(stacked ? QWIDGETSIZE_MAX : 300);
}

QString PrintDialog::what() const { return whatGroup->checkedButton()->property("value").toString().section('/', 0, 0); }
QString PrintDialog::sort() const { return whatGroup->checkedButton()->property("value").toString().section('/', 1, 1); }
QString PrintDialog::paper() const { return paperGroup->checkedButton()->property("value").toString(); }

Doc PrintDialog::doc() const
{
    return buildDoc(data_, what(), sort());
}

void PrintDialog::refresh()
{
    printer_.setPageLayout(pageLayout(paper()));
    const int n = pageCount(doc(), paper());
    pagesLabel->setText(QStringLiteral("%1 page%2 on %3").arg(n).arg(n == 1 ? "" : "s").arg(paper()));
    preview_->updatePreview();
}

void PrintDialog::print()
{
    QPrintDialog dlg(&printer_, this);
    if (dlg.exec() == QDialog::Accepted)
        printing::render(&printer_, {doc()});
}

void PrintDialog::savePdfAs()
{
    QString name = QStringLiteral("%1 %2 %3.pdf").arg(data_.event, data_.round, what() == "signs" ? "signs" : "pairings");
    for (QChar &c : name)
        if (!c.isLetterOrNumber() && !QStringLiteral("._-").contains(c))
            c = '_';
    const QString path = QFileDialog::getSaveFileName(this, "Save as PDF", name, "PDF Files (*.pdf)");
    if (path.isEmpty())
        return;
    if (savePdf({doc()}, path, paper()))
        inform(this, "Saved", "PDF saved to:\n" + path);
    else
        warn(this, "Could not save", "The file could not be written:\n" + path);
}

} // namespace printing
