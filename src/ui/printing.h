// Printing for one tournament round: pairing sheets and table/pod signs.
//
// Everything here is read-only.  Sheets are built from the saved round (names,
// table or pod numbers, Commander seats) and painted in black on white through
// Qt's print system: live preview, a physical printer, or Save as PDF, on
// Letter or A4.  Pages are laid out here rather than by a text engine so that
// table headers repeat on every page, a row is never split across pages, and
// long names wrap instead of being clipped.
#pragma once

#include "db.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QDialog>
#include <QLabel>
#include <QPagedPaintDevice>
#include <QPageLayout>
#include <QPrintPreviewWidget>
#include <QPrinter>
#include <QPushButton>

namespace printing {

struct Player {
    QString name;
    int seat = 0;               // 0: no seat order (one-on-one tables)
};
struct Group {
    int number = 0;             // table or pod number; 0 when there is none
    QList<Player> players;
};
struct RoundData {
    QString event, detail, round, unit;     // unit: "Table" | "Pod"
    bool seated = false;
    QList<Group> groups;
    QStringList byes;
};

// pairings: rows from tdb::roundPairings()
RoundData modernRoundData(const db::Row &tournament, int roundNumber, const db::Rows &pairings);
// round: one round from cdb::getState()["rounds"]
RoundData commanderRoundData(const QString &eventName, const QString &roundLabel, const QVariantMap &round);

struct Doc {
    bool signs = false;
    // table
    QString title, sub, note, empty;
    QList<QPair<QString, double>> columns;      // label, fraction of the page width
    QList<QStringList> rows;
    QList<int> bold;                            // column indexes printed bold
    // signs
    QString unit, event, round;
    QList<int> numbers;
};

// sort "table": each table/pod in number order; "name": every player alphabetically.
Doc pairingsDoc(const RoundData &data, const QString &sort = "table");
Doc signsDoc(const RoundData &data);            // one sign per page: a large number with the event name
Doc buildDoc(const RoundData &data, const QString &what, const QString &sort = "table");

QPageLayout pageLayout(const QString &paper);   // "Letter" | "A4"
// Paints the documents onto a paged device, each starting on a new page.  Returns the number of pages.
int render(QPagedPaintDevice *device, const QList<Doc> &docs);
QByteArray pdfBytes(const QList<Doc> &docs, const QString &paper = "Letter", int *pages = nullptr);
int pageCount(const Doc &doc, const QString &paper = "Letter");
bool savePdf(const QList<Doc> &docs, const QString &path, const QString &paper = "Letter");

// Every round's pairings and results, then the standings, for a one-on-one tournament.
QList<Doc> tournamentReport(qint64 tournamentId);

// One value of a CSV export, quoted when it contains a comma, a quote or a line break.
QString csvCell(QString text);

// Print menu for one round: choose what to print, see the preview, then print or save a PDF.
class PrintDialog : public QDialog {
    Q_OBJECT
public:
    PrintDialog(QWidget *parent, const RoundData &data);
    Doc doc() const;
    QString what() const, sort() const, paper() const;
    void refresh();
    QButtonGroup *whatGroup = nullptr, *paperGroup = nullptr;
    QLabel *pagesLabel = nullptr;
    QPushButton *printBtn = nullptr, *pdfBtn = nullptr, *closeBtn = nullptr;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    QButtonGroup *group(QVBoxLayout *layout, const QString &title, const QList<QPair<QString, QString>> &options);
    void print();
    void savePdfAs();
    RoundData data_;
    QPrinter printer_;
    QBoxLayout *outer_ = nullptr;
    QWidget *left_ = nullptr;
    QPrintPreviewWidget *preview_ = nullptr;
};

} // namespace printing
