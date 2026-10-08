#include "theme.h"

#include "prefs.h"

#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QColor>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QCoreApplication>
#include <QFontDatabase>
#include <QHash>
#include <QSvgRenderer>
#include <QWheelEvent>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>

namespace T {

const QString UCA_PURPLE = "#582C83";
const QString UCA_GRAY = "#7C878E";

QString BG, SURFACE, SURFACE2, SURFACE3, SELECTED, BORDER, BORDER2, TEXT, MUTED, DIM;
QString PURPLE, PURPLE_HOVER, PURPLE_LT, PURPLE_DIM, PURPLE_BDR;
QString GREY, GREY_LT;
QString GREEN, RED, ORANGE, AMBER, LIVE, FOCUS;
QString OP, POKE, MTG, GOLD, SILVER, BRONZE;

QString MODE = "dark";
double SCALE = 1.0;

const QString FONT = "Segoe UI";
QString HEAD = "Segoe UI";              // replaced by chooseDisplayFont() once the application exists
const QString MONO = "Consolas";

QString alpha(const QString &hex, int a)
{
    // Qt reads '#RRGGBBAA' as '#AARRGGBB', so use rgba()
    const QColor c(hex);
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
}

void apply(const QString &mode, const QString &textSize)
{
    MODE = mode == "light" ? "light" : "dark";
    // Screens are written against a 14px body size.  Standard shows that at 16px (controls 16,
    // secondary text 14); Large shows it at 20px (controls 18-20, secondary 16-17).
    SCALE = textSize == "large" ? 1.4 : 1.15;
    if (QCoreApplication::instance())
        HEAD = chooseDisplayFont();
    if (MODE == "dark") {           // calm charcoal with a purple cast
        BG = "#17161b"; SURFACE = "#1d1c22"; SURFACE2 = "#24222a"; SURFACE3 = "#2f2c38";
        SELECTED = "#3a2d4a";
        BORDER = "#3a3644"; BORDER2 = "#5b556e"; TEXT = "#f3f1f6"; MUTED = "#bdb7c9"; DIM = "#7c768a";
        PURPLE = UCA_PURPLE; PURPLE_HOVER = "#6b3994"; PURPLE_LT = "#c3a2e6";       // lighter purple outlines read on dark
        GREY = UCA_GRAY; GREY_LT = "#b9c0c6";
        GREEN = "#6fd394"; RED = "#f0605d"; ORANGE = "#f08a3c"; AMBER = "#f2b033";
        LIVE = "#6fd394"; FOCUS = "#e2d0f7";
        OP = "#f0525e"; POKE = "#f4c542"; MTG = "#38c8e0";
        GOLD = "#f4c542"; SILVER = "#aab1ba"; BRONZE = "#c98845";
    } else {                        // soft off-white and gray, never pure white
        BG = "#e6e5eb"; SURFACE = "#eae7ef"; SURFACE2 = "#f0eef4"; SURFACE3 = "#dfdbe7";
        SELECTED = "#dcd1e6";
        BORDER = "#c9c5d2"; BORDER2 = "#a39cb3"; TEXT = "#272331"; MUTED = "#615b70"; DIM = "#8f899c";
        PURPLE = UCA_PURPLE; PURPLE_HOVER = "#6b3994"; PURPLE_LT = UCA_PURPLE;
        GREY = "#5f696f"; GREY_LT = "#3f474d";
        GREEN = "#23834b"; RED = "#c62828"; ORANGE = "#c2570c"; AMBER = "#9a6700";
        LIVE = "#23834b"; FOCUS = "#8a5bb8";
        OP = "#c81e2c"; POKE = "#8a6a00"; MTG = "#0e7490";
        GOLD = "#9a7400"; SILVER = "#5f6870"; BRONZE = "#8f5a24";
    }
    PURPLE_DIM = alpha(PURPLE_LT, MODE == "dark" ? 34 : 26);
    PURPLE_BDR = alpha(PURPLE_LT, 110);
}

QString chooseDisplayFont()
{
    // clean, slightly squared faces that ship with Windows; digits and names stay easy to tell apart
    for (const QString &family : {QString("Bahnschrift"), QString("Segoe UI Variable Display"), QString("Segoe UI Semibold"),
                                  QString("Trebuchet MS"), QString("Arial")})
        if (QFontDatabase::hasFamily(family))
            return family;
    return FONT;
}

namespace {
struct Init {
    Init() { apply(); }
} init;
}

QString gameColor(const QString &game)
{
    return game == "ONEPIECE" ? OP : game == "POKEMON" ? POKE : game == "MTG" ? MTG : PURPLE_LT;
}

QString placeColor(int place)
{
    switch (place) {
    case 1: return GOLD;
    case 2: return SILVER;
    case 3: return BRONZE;
    case 4: return PURPLE_LT;
    default: return {};
    }
}

QString gameLabel(const QString &game)
{
    return game == "ONEPIECE" ? "One Piece TCG" : game == "POKEMON" ? "Pokémon TCG"
         : game == "MTG" ? "Magic: The Gathering" : game;
}

QString gameShort(const QString &game)
{
    return game == "ONEPIECE" ? "One Piece" : game == "POKEMON" ? "Pokémon" : game == "MTG" ? "MTG" : game;
}

int px(int n)
{
    return qRound(n * SCALE);
}

QString P(int n)
{
    return QString::number(px(n));
}

QString breakable(const QString &text, int run)
{
    QStringList out;
    for (QString word : text.split(' ')) {
        if (word.size() > run) {
            QStringList parts;
            for (int i = 0; i < word.size(); i += run)
                parts << word.mid(i, run);
            word = parts.join(QChar(0x200B));       // zero-width break points
        }
        out << word;
    }
    return out.join(' ');
}

QFont font(int size, int weight, bool mono, double spacing, int head)
{
    const bool useHead = head < 0 ? size >= 18 : head != 0;
    QFont f(mono ? MONO : (useHead ? HEAD : FONT));
    f.setPixelSize(px(size));
    f.setWeight(QFont::Weight(weight));
    if (spacing != 0.0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    return f;
}

QLabel *lbl(const QString &text, const QString &color, int size, int weight, bool wrap, bool mono, double spacing)
{
    auto *l = new QLabel(wrap ? breakable(text) : text);
    l->setTextFormat(Qt::PlainText);
    l->setFont(font(size, weight, mono, spacing));
    l->setStyleSheet("color:" + (color.isEmpty() ? TEXT : color) + ";background:transparent;border:none;");
    if (wrap) {
        l->setWordWrap(true);
        l->setMinimumWidth(40);
        l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    return l;
}

void hugText(QLabel *label)
{
    const QString text = label->text().remove(QChar(0x200B));
    label->setMaximumWidth(QFontMetrics(label->font()).horizontalAdvance(text) + 6);
}

QLabel *caps(const QString &text, const QString &color, int size)
{
    return lbl(text.toUpper(), color.isEmpty() ? MUTED : color, size, 600, false, false, 0.8);
}

QLabel *pill(const QString &text, const QString &color, bool dot, const QString &bg)
{
    auto *l = new QLabel((dot ? QStringLiteral("●  ") : QString()) + text);
    l->setFont(font(11, 700));
    l->setStyleSheet("color:" + color + ";background:" + (bg.isEmpty() ? alpha(color, 28) : bg) + ";border:1px solid "
                     + alpha(color, 110) + ";border-radius:" + P(10) + "px;padding:" + P(3) + "px " + P(10) + "px;");
    l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return l;
}

QString gameName(const QString &game)
{
    return game == "ONEPIECE" ? "One Piece" : game == "POKEMON" ? "Pokémon"
         : game == "MTG" ? "Magic: The Gathering" : game;
}

QString gameTagName(const QString &game)
{
    return game == "MTG" ? QString("Magic") : gameName(game);
}

QLabel *gameTag(const QString &game)
{
    const QString color = gameColor(game);
    auto *l = new QLabel(gameTagName(game));
    l->setToolTip(gameName(game));
    l->setFont(font(12, 600));
    l->setStyleSheet("color:" + color + ";background:" + alpha(color, MODE == "dark" ? 34 : 26) + ";border:1px solid "
                     + alpha(color, 150) + ";border-radius:" + P(9) + "px;padding:" + P(1) + "px " + P(8) + "px;");
    l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    l->setAccessibleName("Played " + gameName(game));
    return l;
}

QFrame *hline(const QString &color)
{
    auto *f = new QFrame;
    f->setFixedHeight(1);
    f->setStyleSheet("background:" + (color.isEmpty() ? BORDER : color) + ";border:none;");
    return f;
}

// Pill-shaped, with a 2px outline all the way round.  Hover, pressed, focus and disabled
// each change the outline or fill, so state never depends on colour alone.
QString buttonQss(const QString &kind, int height)
{
    const QString base = "border-radius:" + QString::number(height / 2) + "px;font-family:'" + HEAD + "';font-size:" + P(14)
                         + "px;font-weight:600;padding:0 " + P(18) + "px;min-height:" + QString::number(height - 2 * LINE) + "px;";
    const QString line = QString::number(LINE) + "px solid ";
    const QString strong = QString::number(LINE_STRONG) + "px solid ";
    const QString inset = "padding:0 " + QString::number(px(18) - 1) + "px;min-height:" + QString::number(height - 2 * LINE_STRONG) + "px;";
    if (kind == "primary")
        return "QPushButton{background:" + PURPLE + ";color:#ffffff;border:" + line + PURPLE + ";" + base + "}"
               "QPushButton:hover{background:" + PURPLE_HOVER + ";border-color:" + PURPLE_HOVER + ";}"
               "QPushButton:pressed{background:#47226b;border-color:#47226b;}"
               "QPushButton:focus{border:" + strong + FOCUS + ";" + inset + "}"
               "QPushButton:disabled{background:" + SURFACE3 + ";color:" + DIM + ";border:" + line + BORDER + ";}";
    if (kind == "danger")
        return "QPushButton{background:transparent;color:" + RED + ";border:" + line + alpha(RED, 150) + ";" + base + "}"
               "QPushButton:hover{background:" + alpha(RED, 30) + ";border-color:" + RED + ";}"
               "QPushButton:pressed{background:" + alpha(RED, 60) + ";}"
               "QPushButton:focus{border:" + strong + RED + ";" + inset + "}"
               "QPushButton:disabled{color:" + DIM + ";border:" + line + BORDER + ";}";
    if (kind == "ghost")
        return "QPushButton{background:transparent;color:" + TEXT + ";border:" + line + BORDER2 + ";" + base + "}"
               "QPushButton:hover{border-color:" + PURPLE_LT + ";background:" + PURPLE_DIM + ";}"
               "QPushButton:pressed{background:" + SELECTED + ";}"
               "QPushButton:focus{border:" + strong + FOCUS + ";" + inset + "}"
               "QPushButton:disabled{color:" + DIM + ";border:" + line + BORDER + ";}";
    return "QPushButton{background:" + SURFACE3 + ";color:" + TEXT + ";border:" + line + BORDER2 + ";" + base + "}"
           "QPushButton:hover{border-color:" + PURPLE_LT + ";background:" + SELECTED + ";}"
           "QPushButton:pressed{background:" + PURPLE_DIM + ";}"
           "QPushButton:focus{border:" + strong + FOCUS + ";" + inset + "}"
           "QPushButton:disabled{background:transparent;color:" + DIM + ";border:" + line + BORDER + ";}";
}

QPixmap iconPixmap(const QString &name, const QString &color, int size)
{
    static QHash<QString, QPixmap> cache;
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int side = px(size);
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name, color).arg(side).arg(dpr);
    const auto found = cache.constFind(key);
    if (found != cache.constEnd())
        return *found;
    QPixmap pm;
    QFile file(QStringLiteral(":/assets/icons/%1.svg").arg(name));
    if (file.open(QIODevice::ReadOnly)) {
        // the icons are drawn in "currentColor"; here that is the colour asked for
        QByteArray svg = file.readAll();
        svg.replace("currentColor", color.toLatin1());
        QSvgRenderer renderer(svg);
        pm = QPixmap(QSize(side, side) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        renderer.render(&p, QRectF(0, 0, side, side));
    }
    cache.insert(key, pm);
    return pm;
}

QLabel *iconLabel(const QString &name, const QString &color, int size)
{
    auto *l = new QLabel;
    l->setPixmap(iconPixmap(name, color, size));
    l->setFixedSize(px(size), px(size));
    l->setStyleSheet("background:transparent;border:none;");
    return l;
}

void setButtonIcon(QAbstractButton *b, const QString &name, const QString &color, bool after, int size)
{
    // The style leaves 4px between an icon and a label; 4 more are added here, on the label's side.
    const int side = px(size), gap = b->text().isEmpty() ? 0 : 4;
    const auto padded = [&](const QString &c) {
        const QPixmap src = iconPixmap(name, c, size);
        QPixmap pm(QSize(side + gap, side) * src.devicePixelRatio());
        pm.setDevicePixelRatio(src.devicePixelRatio());
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.drawPixmap(after ? gap : 0, 0, src);
        return pm;
    };
    QIcon icon;
    icon.addPixmap(padded(color), QIcon::Normal);
    icon.addPixmap(padded(DIM), QIcon::Disabled);
    b->setIcon(icon);
    b->setIconSize(QSize(side + gap, side));
    b->setLayoutDirection(after ? Qt::RightToLeft : Qt::LeftToRight);      // a button draws its icon first
}

void setFieldIcon(QLineEdit *field, const QString &name)
{
    // A picture laid over the field's left edge, with the text moved clear of it.  It is not a
    // button: clicks pass through to the field, and it adds nothing for the keyboard or a
    // screen reader.
    QLabel *icon = iconLabel(name, MUTED, 18);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *lay = new QHBoxLayout(field);
    lay->setContentsMargins(px(12), 0, 0, 0);
    lay->addWidget(icon);
    lay->addStretch();
    field->setTextMargins(px(18) + 8, 0, 0, 0);
}

QString labelColor(const QString &kind)
{
    return kind == "primary" ? QStringLiteral("#ffffff") : kind == "danger" ? RED : TEXT;
}

QPushButton *button(const QString &text, const QString &kind, const QString &icon, bool iconAfter)
{
    auto *b = new QPushButton(text);
    const int h = qMax(px(35), BUTTON_H);
    b->setMinimumHeight(h);
    b->setCursor(QCursor(Qt::PointingHandCursor));
    b->setStyleSheet(buttonQss(kind, h));
    b->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (!icon.isEmpty())
        setButtonIcon(b, icon, labelColor(kind), iconAfter);
    return b;
}

QPushButton *roundButton(const QString &icon, const QString &tooltip)
{
    auto *b = new QPushButton;
    const int size = 44;
    b->setFixedSize(size, size);
    setButtonIcon(b, icon, TEXT, false, 22);
    b->setCursor(QCursor(Qt::PointingHandCursor));
    b->setToolTip(tooltip);
    b->setAccessibleName(tooltip);
    b->setStyleSheet("QPushButton{background:" + SURFACE2 + ";color:" + TEXT + ";border:" + QString::number(LINE) + "px solid " + BORDER2
                     + ";border-radius:" + QString::number(size / 2) + "px;padding:0;}"
                     "QPushButton:hover{border-color:" + PURPLE_LT + ";background:" + SELECTED + ";}"
                     "QPushButton:pressed{background:" + PURPLE_DIM + ";}"
                     "QPushButton:focus{border:" + QString::number(LINE_STRONG) + "px solid " + FOCUS + ";}");
    return b;
}

QPushButton *iconButton(const QString &icon, const QString &tooltip)
{
    auto *b = new QPushButton;
    b->setFixedSize(44, 44);
    setButtonIcon(b, icon, MUTED, false, 18);
    b->setCursor(QCursor(Qt::PointingHandCursor));
    b->setToolTip(tooltip);
    b->setAccessibleName(tooltip);
    b->setStyleSheet("QPushButton{background:transparent;border:2px solid transparent;border-radius:22px;padding:0;"
                     "min-width:40px;min-height:40px;}"
                     "QPushButton:hover{background:" + SURFACE3 + ";border-color:" + BORDER2 + ";}"
                     "QPushButton:pressed{background:" + SELECTED + ";}"
                     "QPushButton:focus{border-color:" + FOCUS + ";}");
    return b;
}

// A tab or choice: 2px quiet outline normally; when selected, a 3px purple outline around
// the whole pill, a tinted fill and bolder text.
QString pillQss(bool selected, int height)
{
    const int line = selected ? LINE_STRONG : LINE;
    return "QPushButton{background:" + (selected ? SELECTED : QString("transparent")) + ";color:" + (selected ? TEXT : MUTED)
           + ";border:" + QString::number(line) + "px solid " + (selected ? PURPLE_LT : BORDER2) + ";border-radius:"
           + QString::number(height / 2) + "px;font-family:'" + HEAD + "';font-size:" + P(14) + "px;font-weight:"
           + (selected ? "700" : "500") + ";padding:0 " + QString::number(px(16) - (line - LINE)) + "px;min-height:"
           + QString::number(height - 2 * line) + "px;}"
           "QPushButton:hover{color:" + TEXT + ";border-color:" + PURPLE_LT + ";background:" + (selected ? SELECTED : PURPLE_DIM) + ";}"
           "QPushButton:pressed{background:" + SELECTED + ";}"
           "QPushButton:focus{border:" + QString::number(LINE_STRONG) + "px solid " + FOCUS + ";padding:0 "
           + QString::number(px(16) - (LINE_STRONG - LINE)) + "px;min-height:" + QString::number(height - 2 * LINE_STRONG) + "px;}"
           "QPushButton:disabled{color:" + DIM + ";border-color:" + BORDER + ";}";
}

QPushButton *choicePill(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setCheckable(true);
    b->setProperty("label", text);
    const int h = qMax(px(35), BUTTON_H);
    b->setMinimumHeight(h);
    b->setCursor(QCursor(Qt::PointingHandCursor));
    auto restyle = [b, h, text](bool on) {
        b->setText(on ? QStringLiteral("✓  ") + text : text);       // a tick as well as the outline
        b->setStyleSheet(pillQss(on, h));
    };
    QObject::connect(b, &QPushButton::toggled, b, restyle);
    restyle(false);
    return b;
}

QComboBox *combo()
{
    auto *c = new QComboBox;
    // a plain list view (not the platform popup) so the rows take the stylesheet, and an
    // opaque popup window so nothing behind the open list shows through
    auto *view = new QListView(c);
    view->setAutoFillBackground(true);
    c->setView(view);
    c->setStyleSheet(comboStyle());
    c->setMaxVisibleItems(10);
    if (QWidget *popup = view->window()) {
        popup->setAttribute(Qt::WA_TranslucentBackground, false);
        popup->setAutoFillBackground(true);
    }
    return c;
}

QFrame *panel(const QString &name, const QString &outline, int width, int radius, const QString &bg)
{
    auto *f = new QFrame;
    f->setObjectName(name);
    // the outline always runs round the whole panel; a status colour gets the stronger 3px line
    const int line = (!outline.isEmpty() && width >= 2) ? LINE_STRONG : LINE;
    f->setStyleSheet("#" + name + "{background:" + (bg.isEmpty() ? SURFACE2 : bg) + ";border:" + QString::number(line)
                     + "px solid " + (outline.isEmpty() ? BORDER : outline) + ";border-radius:" + QString::number(qMin(radius, RADIUS)) + "px;}");
    return f;
}

QBoxLayout *row(int spacing)
{
    auto *r = new QBoxLayout(QBoxLayout::LeftToRight);
    r->setSpacing(spacing);
    return r;
}

void clear(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
            delete item;
        } else if (QLayout *sub = item->layout()) {
            clear(sub);
            delete sub;
        } else {
            delete item;
        }
    }
}

// A small chevron drawn in the given colour and saved where the stylesheet can load it
// (stylesheets can only show arrows from image files).
static QString chevron(const QString &color, bool up)
{
    const QString file = QDir::temp().filePath(QStringLiteral("tcg_chevron_%1_%2.png").arg(QString(color).remove('#'), up ? "up" : "down"));
    if (!QFile::exists(file)) {
        QImage img(24, 24, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(color), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (up)
            p.drawPolyline(QPolygonF({QPointF(5, 15), QPointF(12, 8), QPointF(19, 15)}));
        else
            p.drawPolyline(QPolygonF({QPointF(5, 9), QPointF(12, 16), QPointF(19, 9)}));
        p.end();
        img.save(file);
    }
    return QString(file).replace('\\', '/');
}

QString inputStyle()
{
    const QString h = QString::number(qMax(px(35), MIN_HIT) - 2 * LINE);
    return "QLineEdit,QSpinBox,QComboBox{background:" + SURFACE + ";color:" + TEXT + ";border:" + QString::number(LINE) + "px solid " + BORDER2
           + ";border-radius:" + QString::number(RADIUS_SM) + "px;padding:0 " + P(12) + "px;font-size:" + P(14)
           + "px;min-height:" + h + "px;selection-background-color:" + PURPLE + ";selection-color:#ffffff;}"
           "QLineEdit:hover,QSpinBox:hover,QComboBox:hover{border-color:" + PURPLE_LT + ";}"
           "QLineEdit:focus,QSpinBox:focus,QComboBox:focus{border-color:" + FOCUS + ";}"
           "QLineEdit:disabled,QSpinBox:disabled,QComboBox:disabled{color:" + DIM + ";border-color:" + BORDER + ";}"
           "QSpinBox::up-button,QSpinBox::down-button{width:" + P(26) + "px;border:none;background:transparent;}"
           "QSpinBox::up-arrow{image:url(" + chevron(MUTED, true) + ");width:" + P(11) + "px;height:" + P(11) + "px;}"
           "QSpinBox::down-arrow{image:url(" + chevron(MUTED, false) + ");width:" + P(11) + "px;height:" + P(11) + "px;}"
           "QSpinBox::up-arrow:hover{image:url(" + chevron(TEXT, true) + ");}"
           "QSpinBox::down-arrow:hover{image:url(" + chevron(TEXT, false) + ");}"
           + comboStyle();
}

QString comboStyle()
{
    const QString rowH = QString::number(qMax(px(32), 36));
    return "QComboBox::drop-down{border:none;width:" + P(30) + "px;}"
           // a small chevron so the control reads as a dropdown
           "QComboBox::down-arrow{image:url(" + chevron(MUTED, false) + ");width:" + P(13) + "px;height:" + P(13)
           + "px;margin-right:" + P(10) + "px;}"
           "QComboBox::down-arrow:hover,QComboBox::down-arrow:on{image:url(" + chevron(TEXT, false) + ");}"
           // the popup window that holds the list: opaque, with its own outline
           "QComboBoxPrivateContainer{background:" + SURFACE2 + ";border:1px solid " + BORDER2 + ";border-radius:0;padding:0;margin:0;}"
           "QComboBox QFrame{background:" + SURFACE2 + ";}"
           "QComboBox QAbstractItemView{background:" + SURFACE2 + ";color:" + TEXT + ";border:1px solid " + BORDER2
           + ";outline:0;padding:" + P(4) + "px;font-size:" + P(14) + "px;selection-background-color:" + PURPLE
           + ";selection-color:#ffffff;}"
           // each option row: normal, hovered, keyboard-focused / chosen
           "QComboBox QAbstractItemView::item{background:" + SURFACE2 + ";color:" + TEXT + ";min-height:" + rowH
           + "px;padding:0 " + P(10) + "px;border:none;border-radius:" + QString::number(RADIUS_SM - 3) + "px;}"
           "QComboBox QAbstractItemView::item:hover{background:" + SELECTED + ";color:" + TEXT + ";}"
           "QComboBox QAbstractItemView::item:selected{background:" + PURPLE + ";color:#ffffff;}"
           "QComboBox QAbstractItemView::item:selected:hover{background:" + PURPLE_HOVER + ";color:#ffffff;}";
}

QString scrollStyle()
{
    return "QScrollArea{border:none;background:transparent;}"
           "QScrollBar:vertical{background:transparent;width:10px;margin:0;}"
           "QScrollBar::handle:vertical{background:" + BORDER2 + ";border-radius:4px;min-height:28px;margin:0 2px;}"
           "QScrollBar:horizontal{background:transparent;height:10px;margin:0;}"
           "QScrollBar::handle:horizontal{background:" + BORDER2 + ";border-radius:4px;min-width:28px;margin:2px 0;}"
           "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
           "QScrollBar::add-page,QScrollBar::sub-page{background:none;}";
}

QString appQss()
{
    return "QMainWindow,QDialog,QMessageBox{background:" + BG + ";}"
           "QWidget{color:" + TEXT + ";}"
           "QToolTip{background:" + SURFACE2 + ";color:" + TEXT + ";border:1px solid " + BORDER2 + ";padding:4px 8px;}"
           "QDialog QLabel,QMessageBox QLabel{color:" + TEXT + ";}"
           "QMessageBox QLabel{font-size:" + P(13) + "px;}"
           + buttonQss("secondary", BUTTON_H) + inputStyle() + scrollStyle()
           + "QCheckBox,QRadioButton{color:" + TEXT + ";font-size:" + P(14) + "px;spacing:" + P(10) + "px;min-height:"
           + QString::number(MIN_HIT) + "px;}"
           "QCheckBox::indicator,QRadioButton::indicator{width:" + P(20) + "px;height:" + P(20) + "px;border:2px solid "
           + BORDER2 + ";background:" + SURFACE + ";}"
           "QCheckBox::indicator{border-radius:5px;}QRadioButton::indicator{border-radius:" + P(12) + "px;}"
           "QCheckBox::indicator:checked,QRadioButton::indicator:checked{background:" + PURPLE + ";border-color:" + PURPLE_LT + ";}"
           "QCheckBox:focus,QRadioButton:focus{color:" + PURPLE_LT + ";}"
           "QCheckBox::indicator:focus,QRadioButton::indicator:focus{border-color:" + FOCUS + ";}"
           "QPlainTextEdit{background:" + SURFACE + ";color:" + TEXT + ";border:1px solid " + BORDER2
           + ";border-radius:10px;font-family:" + MONO + ";font-size:" + P(12) + "px;}";
}

WrapButton::WrapButton(const QString &text, const QString &kind)
{
    setAccessibleName(text);
    label_ = new QLabel(breakable(text), this);
    label_->setTextFormat(Qt::PlainText);
    label_->setWordWrap(true);
    label_->setAlignment(Qt::AlignCenter);
    label_->setFont(T::font(14, 600));
    label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(px(16), px(8), px(16), px(8));
    lay->addWidget(label_);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setStyleSheet(buttonQss(kind, MIN_HIT).replace("min-height", "min-width:60px;min-height"));
    setCursor(QCursor(Qt::PointingHandCursor));
    paintLabel();
}

void WrapButton::paintLabel()
{
    label_->setStyleSheet("color:" + (isEnabled() ? TEXT : MUTED) + ";background:transparent;border:none;");
}

void WrapButton::changeEvent(QEvent *e)
{
    QPushButton::changeEvent(e);
    if (label_ && e->type() == QEvent::EnabledChange) {
        paintLabel();
        if (!isEnabled())
            unsetCursor();
    }
}

int WrapButton::heightForWidth(int w) const
{
    return qMax(MIN_HIT, layout()->totalHeightForWidth(w));
}

QSize WrapButton::sizeHint() const
{
    const QSize s = layout()->totalSizeHint();
    return QSize(qMax(s.width(), 96), qMax(s.height(), MIN_HIT));
}

ClickFrame::ClickFrame(const QString &name)
{
    setObjectName(name);
    setCursor(QCursor(Qt::PointingHandCursor));
    setFocusPolicy(Qt::StrongFocus);
}

void ClickFrame::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && rect().contains(e->pos()))
        emit clicked();
    QFrame::mouseReleaseEvent(e);
}

void ClickFrame::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space)
        emit clicked();
    else
        QFrame::keyPressEvent(e);
}

FlowLayout::FlowLayout(QWidget *parent, int spacing, bool center) : QLayout(parent), gap_(spacing), center_(center)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, true);
}

QSize FlowLayout::sizeHint() const
{
    int width = 0, height = 0;
    for (QLayoutItem *item : items_) {
        const QSize s = item->sizeHint();
        width += s.width() + (width ? gap_ : 0);
        height = qMax(height, s.height());
    }
    const QMargins m = contentsMargins();
    return QSize(width + m.left() + m.right(), height + m.top() + m.bottom());
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (QLayoutItem *item : items_)
        size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect &rect, bool apply) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    struct Line {
        QList<QPair<QLayoutItem *, int>> items;
        int width = 0;
    };
    QList<Line> lines;
    Line line;
    int used = 0;
    for (QLayoutItem *item : items_) {
        if (QWidget *w = item->widget(); w && apply && !w->isVisibleTo(w->parentWidget()))
            continue;
        const int w = qMin(item->sizeHint().width(), qMax(area.width(), item->minimumSize().width()));
        if (!line.items.isEmpty() && used + w > area.width()) {
            line.width = used - gap_;
            lines.append(line);
            line = Line();
            used = 0;
        }
        line.items.append({item, w});
        used += w + gap_;
    }
    if (!line.items.isEmpty()) {
        line.width = used - gap_;
        lines.append(line);
    }
    if (lines.isEmpty())
        return m.top() + m.bottom();
    int y = area.y();
    for (const Line &ln : lines) {
        int x = area.x() + (center_ ? qMax(0, (area.width() - ln.width) / 2) : 0);
        QList<int> heights;
        int h = 0;
        for (const auto &it : ln.items) {
            const int ih = it.first->hasHeightForWidth() ? it.first->heightForWidth(it.second) : it.first->sizeHint().height();
            heights << ih;
            h = qMax(h, ih);
        }
        for (int i = 0; i < ln.items.size(); ++i) {
            if (apply)
                ln.items[i].first->setGeometry(QRect(QPoint(x, y + (h - heights[i]) / 2), QSize(ln.items[i].second, heights[i])));
            x += ln.items[i].second + gap_;
        }
        y += h + gap_;
    }
    return y - gap_ - rect.y() + m.bottom();
}

Screen::Screen()
{
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addStretch();
    auto *column = new QWidget;
    column->setMaximumWidth(CONTENT_MAX_W);
    outer->addWidget(column, 100);
    outer->addStretch();
    root = new QVBoxLayout(column);
    root->setSpacing(0);
    setMargins();
}

void Screen::setMargins()
{
    const int side = tiny ? 14 : (narrow ? 20 : 36);
    root->setContentsMargins(side, narrow ? 18 : 30, side, narrow ? 14 : 22);
}

QBoxLayout *Screen::flip(QBoxLayout *layout, bool atTiny)
{
    flips_.append({QPointer<QBoxLayout>(layout), atTiny});
    layout->setDirection((atTiny ? tiny : narrow) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    return layout;
}

void Screen::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const bool n = width() < 760, t = width() < 500;
    if (n == narrow && t == tiny)
        return;
    narrow = n;
    tiny = t;
    setMargins();
    for (int i = flips_.size() - 1; i >= 0; --i)
        if (!flips_[i].first)
            flips_.removeAt(i);
    for (const auto &f : flips_)
        f.first->setDirection((f.second ? tiny : narrow) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    reflow();
}

QWidget *pageFooter()
{
    auto *footer = new QWidget;
    footer->setObjectName("pageFooter");
    auto *row = new FlowLayout(footer, px(14), true);
    row->setContentsMargins(0, 4, 0, 0);
    const int h = MIN_HIT;          // small text, but the link is as easy to hit as any other control

    auto *bug = new QPushButton("Report a bug");
    setButtonIcon(bug, "bug", MUTED, false, 13);
    bug->setCursor(QCursor(Qt::PointingHandCursor));
    bug->setToolTip(prefs::issuesUrl());
    bug->setAccessibleDescription("Opens the project's GitHub issues page in your browser");
    bug->setStyleSheet("QPushButton{background:transparent;color:" + MUTED + ";border:2px solid transparent;border-radius:8px;"
                       "font-family:'" + FONT + "';font-size:" + P(11) + "px;font-weight:400;text-decoration:underline;"
                       "padding:0 6px;min-height:" + QString::number(h - 4)
                       + "px;}"
                       "QPushButton:hover{color:" + PURPLE_LT + ";}"
                       "QPushButton:focus{border-color:" + FOCUS + ";color:" + PURPLE_LT + ";}");
    QObject::connect(bug, &QPushButton::clicked, bug, [] { QDesktopServices::openUrl(QUrl(prefs::issuesUrl())); });
    row->addWidget(bug);

    QLabel *credit = lbl(QStringLiteral("Produced by %1").arg(prefs::PRODUCER), MUTED, 11);
    credit->setMinimumHeight(h);
    row->addWidget(credit);
    return footer;
}

QScrollArea *Screen::page(QVBoxLayout **body)
{
    return scrollArea(body, true);
}

QScrollArea *scrollArea(QVBoxLayout **box, bool footer)
{
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setStyleSheet(scrollStyle());
    // only these two widgets are transparent: an unqualified "background:transparent" would be
    // inherited by everything inside, including the list of an opened dropdown
    area->viewport()->setObjectName("scrollViewport");
    area->viewport()->setStyleSheet("#scrollViewport{background:transparent;}");
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *holder = new QWidget;
    holder->setObjectName("scrollHolder");
    holder->setStyleSheet("#scrollHolder{background:transparent;}");
    auto *layout = new QVBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    area->setWidget(holder);
    // the scrollbar takes 10px on the right while it is shown: give the left the same, so content stays centred
    QObject::connect(area->verticalScrollBar(), &QScrollBar::rangeChanged, layout, [layout](int, int max) {
        const int inset = max > 0 ? 10 : 0;
        if (layout->contentsMargins().left() != inset)
            layout->setContentsMargins(inset, 0, 0, 0);
    });
    if (footer) {
        // the page's own content, then the footer: at the bottom of a short page, after the
        // content of a long one, and never on top of anything
        auto *content = new QVBoxLayout;
        content->setSpacing(0);
        layout->addLayout(content, 1);
        layout->addWidget(pageFooter());
        *box = content;
    } else {
        *box = layout;
    }
    return area;
}

NameScroll::NameScroll(const QString &text, int size, int weight)
{
    label = lbl(text, TEXT, size, weight);
    label->setContentsMargins(0, 0, 2, 0);
    setWidget(label);
    setWidgetResizable(false);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);            // Tab to it, then the arrow keys scroll
    setToolTip(text);
    setAccessibleName(text);
    setAccessibleDescription(text + ". Scroll sideways to read the whole name.");
    setObjectName("nameScroll");
    viewport()->setObjectName("nameViewport");
    viewport()->setStyleSheet("#nameViewport{background:transparent;}");
    // a thin bar that is visible whenever there is more to read
    setStyleSheet("#nameScroll{background:transparent;border:2px solid transparent;border-radius:6px;}"
                  "#nameScroll:focus{border-color:" + FOCUS + ";}"
                  "QScrollBar:horizontal{background:" + BORDER + ";height:6px;margin:0;border-radius:3px;}"
                  "QScrollBar::handle:horizontal{background:" + PURPLE_LT + ";border-radius:3px;min-width:24px;}"
                  "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
                  "QScrollBar::add-page,QScrollBar::sub-page{background:none;}");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(label->sizeHint().height() + 6 + 6);     // text, bar and focus outline
    horizontalScrollBar()->setSingleStep(24);
}

QSize NameScroll::sizeHint() const
{
    return QSize(label->sizeHint().width() + 8, height());
}

QSize NameScroll::minimumSizeHint() const
{
    return QSize(90, height());
}

// An ordinary (vertical) wheel turn always belongs to the page, so a strip under the pointer
// never stops the page from scrolling.  Only a sideways wheel, or Shift+wheel, moves the strip.
static void sidewaysWheel(QScrollBar *bar, QWheelEvent *e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const int delta = e->angleDelta().x() ? e->angleDelta().x() : (shift ? e->angleDelta().y() : 0);
    if (!delta || bar->maximum() <= 0) {
        e->ignore();
        return;
    }
    bar->setValue(bar->value() - delta / 2);
    e->accept();
}

void NameScroll::wheelEvent(QWheelEvent *e)
{
    sidewaysWheel(horizontalScrollBar(), e);
}

TagStrip::TagStrip(const QStringList &games)
{
    auto *holder = new QWidget;
    holder->setObjectName("tagHolder");
    holder->setStyleSheet("#tagHolder{background:transparent;}");
    auto *row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    for (const QString &game : games) {
        QLabel *tag = gameTag(game);
        tags << tag;
        row->addWidget(tag);
    }
    if (games.isEmpty())
        row->addWidget(lbl("No games yet", DIM, 12));
    holder->adjustSize();
    setWidget(holder);
    setWidgetResizable(false);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);     // a bar only when the tags really overflow
    setObjectName("tagStrip");
    QStringList names;
    for (const QString &game : games)
        names << gameName(game);
    setAccessibleName(games.isEmpty() ? QString("No games played yet") : "Games played: " + names.join(", "));
    viewport()->setObjectName("tagViewport");
    viewport()->setStyleSheet("#tagViewport{background:transparent;}");
    setStyleSheet("#tagStrip{background:transparent;border:2px solid transparent;border-radius:6px;}"
                  "#tagStrip:focus{border-color:" + FOCUS + ";}"
                  "QScrollBar:horizontal{background:" + BORDER + ";height:6px;margin:0;border-radius:3px;}"
                  "QScrollBar::handle:horizontal{background:" + PURPLE_LT + ";border-radius:3px;min-width:24px;}"
                  "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;}"
                  "QScrollBar::add-page,QScrollBar::sub-page{background:none;}");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(holder->height() + 6 + 6);               // tags, bar and focus outline
    horizontalScrollBar()->setSingleStep(24);
    setFocusPolicy(Qt::NoFocus);
}

int TagStrip::contentWidth() const
{
    return widget() ? widget()->width() + 4 : 0;
}

void TagStrip::resizeEvent(QResizeEvent *e)
{
    QScrollArea::resizeEvent(e);
    // it only joins the Tab order when there is something to scroll
    setFocusPolicy(widget() && widget()->width() > viewport()->width() ? Qt::StrongFocus : Qt::NoFocus);
}

void TagStrip::wheelEvent(QWheelEvent *e)
{
    sidewaysWheel(horizontalScrollBar(), e);
}

ScrollKeeper::ScrollKeeper(QWidget *scope) : scope_(scope)
{
    if (!scope)
        return;
    QList<QScrollArea *> areas = scope->findChildren<QScrollArea *>();
    if (auto *self = qobject_cast<QScrollArea *>(scope))
        areas << self;
    for (QScrollArea *area : areas) {
        bars_.append({QPointer<QScrollBar>(area->verticalScrollBar()), area->verticalScrollBar()->value()});
        bars_.append({QPointer<QScrollBar>(area->horizontalScrollBar()), area->horizontalScrollBar()->value()});
    }
    if (QWidget *f = QApplication::focusWidget(); f && (f == scope || scope->isAncestorOf(f))) {
        focusName_ = f->accessibleName();
        focusClass_ = f->metaObject()->className();
    }
}

ScrollKeeper::~ScrollKeeper()
{
    const auto bars = bars_;
    const QPointer<QWidget> scope = scope_;
    const QString name = focusName_;
    const QByteArray cls = focusClass_;
    auto restore = [bars, scope, name, cls] {
        if (!scope)
            return;
        if (!name.isEmpty()) {
            QWidget *f = QApplication::focusWidget();
            if (!f || f->accessibleName() != name || !f->isVisible()) {
                for (QWidget *w : scope->findChildren<QWidget *>()) {
                    if (w->isVisible() && w->isEnabled() && w->accessibleName() == name && cls == w->metaObject()->className()) {
                        w->setFocus(Qt::OtherFocusReason);      // setFocus itself does not scroll
                        break;
                    }
                }
            }
        }
        for (const auto &b : bars)
            if (b.first && b.first->value() != b.second)
                b.first->setValue(qMin(b.second, b.first->maximum()));
    };
    restore();
    // and again once the rebuilt rows have been laid out and shown
    if (scope)
        QTimer::singleShot(0, scope, restore);
}

HScroll::HScroll(QWidget *widget)
{
    setWidgetResizable(true);
    setStyleSheet(scrollStyle());
    viewport()->setObjectName("scrollViewport");
    viewport()->setStyleSheet("#scrollViewport{background:transparent;}");
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setWidget(widget);
}

void HScroll::fit()
{
    QWidget *w = widget();
    if (!w)
        return;
    const int width = qMax(viewport()->width(), w->minimumWidth());
    int h = w->hasHeightForWidth() ? w->heightForWidth(width) : -1;
    if (h < 0)
        h = w->sizeHint().height();
    setFixedHeight(h + 14);         // room for the horizontal scrollbar
}

void HScroll::resizeEvent(QResizeEvent *e)
{
    QScrollArea::resizeEvent(e);
    fit();
}

void HScroll::showEvent(QShowEvent *e)
{
    QScrollArea::showEvent(e);
    fit();
}

} // namespace T
