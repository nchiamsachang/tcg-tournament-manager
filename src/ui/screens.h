// Every screen of the app.  Screens re-read the database when they are built
// or refreshed; none of them keeps tournament state of its own.
#pragma once

#include "dialogs.h"
#include "main_window.h"
#include "store.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QLineEdit>
#include <QSet>
#include <QSpinBox>

// Home: pick a game.
class HomeScreen : public T::Screen {
    Q_OBJECT
public:
    explicit HomeScreen(MainWindow *mw);
    static const QString DESCRIPTION;

private:
    QWidget *card(const QString &game, const QString &image, const QString &tag, const QString &name,
                  const QString &subtitle, const QString &screen);
    MainWindow *mw_;
};

// Hub: one game's tournaments.
class HubScreen : public T::Screen {
    Q_OBJECT
public:
    HubScreen(MainWindow *mw, const QString &game);

protected:
    void reflow() override;
    void hideEvent(QHideEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    struct Clock {              // the countdown shown on one tournament row
        QPointer<QLabel> value, caption, icon;
        timerdb::Kind kind = timerdb::Kind::OneOnOne;
        qint64 roundId = 0;
        timerdb::Reading reading;
    };
    void build();
    void load();
    QVBoxLayout *statCard(QVBoxLayout *parent, const QString &title);
    void statRows(QVBoxLayout *box, const QList<std::tuple<QString, QString, QString>> &items);
    QWidget *tournamentRow(const db::Row &t);
    void showClock(const Clock &c);

    MainWindow *mw_;
    QString game_;
    QVBoxLayout *activeBox_ = nullptr, *doneBox_ = nullptr, *statsBox_ = nullptr, *topBox_ = nullptr;
    QWidget *side_ = nullptr;
    QList<Clock> clocks_;
    int ticks_ = 0;
    QTimer clockTimer_;
};

class TournamentSetupScreen : public T::Screen {
    Q_OBJECT
public:
    TournamentSetupScreen(MainWindow *mw, const QString &game = {});
    void createTournament();
    QLineEdit *nameInput = nullptr;
    QComboBox *gameCombo = nullptr, *formatCombo = nullptr;
    QSpinBox *playerCount = nullptr, *roundsSpin = nullptr;
    QLineEdit *minutesEdit = nullptr;       // digits only; the "min" unit is a label beside it
    QLabel *minutesUnit = nullptr;
    QString lastError;                      // why the last Create was refused (empty when it was not)
    QLabel *suggestionLabel = nullptr, *commanderNote = nullptr;

private:
    bool isCommander() const;
    QString game() const;
    void updateSuggestedRounds(int players);
    MainWindow *mw_;
};

class RegistrationScreen : public T::Screen {
    Q_OBJECT
public:
    RegistrationScreen(MainWindow *mw, qint64 tournamentId);
    void setup();                           // builds the screen (after construction, so subclasses can extend it)
    void addAndEnroll();
    virtual void confirmStart();
    qint64 tournamentId;
    QString game, tName;
    QLabel *enrolledCountLabel = nullptr, *titleLabel = nullptr;
    QLineEdit *searchBox = nullptr, *newNameBox = nullptr;
    QPushButton *startBtn = nullptr, *addBtn = nullptr, *renameBtn = nullptr;
    void rename();                          // the pencil beside the title

protected:
    virtual void buildUi();
    virtual void refreshEnrolled();
    QWidget *playerRow(const QString &name, const QList<QPushButton *> &buttons, bool dim = false);
    void unenroll(qint64 playerId, const QString &name);
    MainWindow *mw_;
    QVBoxLayout *enrolledLayout = nullptr, *extraSlot = nullptr;

private:
    void runSearch();
    void enrollExisting(qint64 playerId);
    QFrame *searchResultsFrame_ = nullptr;
    QVBoxLayout *searchResultsLayout_ = nullptr;
    QTimer searchTimer_;
};

// The normal registration screen plus check-in and the Commander setup recommendations.
class CommanderRegistrationScreen : public RegistrationScreen {
    Q_OBJECT
public:
    using RegistrationScreen::RegistrationScreen;
    void confirmStart() override;
    QSpinBox *roundsSpin = nullptr;
    QComboBox *drawCombo = nullptr, *shortCombo = nullptr;
    QLabel *recLabel = nullptr, *warnLabel = nullptr;
    QLabel *roundsHint = nullptr, *drawHint = nullptr, *shortHint = nullptr;    // a few words on each setting
    QPushButton *resetBtn = nullptr;

protected:
    void buildUi() override;
    void refreshEnrolled() override;

private:
    void syncConfig(int checkedIn);
    void configChanged();
    void updateCounts();
    void updateHints();
    bool loading_ = false;
};

// A one-on-one round.
class RoundScreen : public T::Screen {
    Q_OBJECT
public:
    RoundScreen(MainWindow *mw, qint64 tournamentId, int viewRound = 0);
    void recordResult(qint64 matchId, const QString &result);
    void endRound();
    void finalizeTournament();
    void loadRound();
    qint64 tournamentId, currentRoundId = 0;
    int currentRoundNum = 0, totalRounds = 3;
    QPushButton *printBtn = nullptr, *standingsBtn = nullptr, *nextRoundBtn = nullptr, *finalizeBtn = nullptr;
    QPushButton *endEarlyBtn = nullptr;     // only while the tournament is running
    QPushButton *renameBtn = nullptr;       // the pencil beside the title, in every state
    void rename();
    QLabel *roundLabel = nullptr, *subLabel = nullptr;

private:
    QWidget *pairingRow(const db::Row &pairing, bool live);
    void updatePendingCount(const db::Rows &pairings, bool live);
    MainWindow *mw_;
    db::Row tournament_;
    QString game_, tName_, format_, status_;
    int viewRound_;
    qint64 timerRound_ = 0;
    QVBoxLayout *timerSlot_ = nullptr, *pairingsLayout_ = nullptr;
    QHBoxLayout *progressRow_ = nullptr;
};

class StandingsScreen : public T::Screen {
    Q_OBJECT
public:
    StandingsScreen(MainWindow *mw, qint64 tournamentId);
    db::Rows standings;

private:
    void load();
    QWidget *standingRow(int place, const db::Row &player, bool last);
    void exportPdf();
    void exportCsv();
    MainWindow *mw_;
    qint64 tournamentId_;
    QString game_, tName_, status_, format_;
    QList<QPair<QString, QString>> tiebreaks_;
    QVBoxLayout *rowsBox_ = nullptr;
    T::HScroll *tableScroll_ = nullptr;
};

// A completed tournament: pick a round to review, or open the final standings.
class RoundSelectScreen : public T::Screen {
    Q_OBJECT
public:
    RoundSelectScreen(MainWindow *mw, qint64 tournamentId);
    QFrame *panel = nullptr;            // the centred results panel
    QLabel *nameLabel = nullptr;
    QPushButton *renameBtn = nullptr;   // the pencil beside the name
};

class PlayersScreen : public T::Screen {
    Q_OBJECT
public:
    explicit PlayersScreen(MainWindow *mw);
    QLineEdit *searchBox = nullptr;
    QLabel *countLabel = nullptr;
    QPushButton *filterBtn = nullptr;
    // The game filter: the ids of the ticked games; empty means "All games" (everybody,
    // including players who have not played yet).  A player is shown when they have played
    // any ticked game.  Passing an empty id is the "All games" box, which clears the others;
    // unticking the last game goes back to it.
    QSet<QString> gameFilter() const { return gameFilter_; }
    void setGameChecked(const QString &game, bool on);
    void openFilterMenu();
    QFrame *filterMenu() const { return filterMenu_; }       // the open panel of checkboxes, if any

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    int tagAreaWidth() const;
    int tagArea_ = 0;
    void populate(const db::Rows &players, int outOf);
    void runSearch();
    void showFilterState();
    MainWindow *mw_;
    QVBoxLayout *list_ = nullptr;
    QTimer searchTimer_;
    QSet<QString> gameFilter_;
    QPointer<QFrame> filterMenu_;
};

class PlayerProfileScreen : public T::Screen {
    Q_OBJECT
public:
    PlayerProfileScreen(MainWindow *mw, qint64 playerId);

private:
    void loadStats();
    void loadFeed();
    void styleFilters();
    QWidget *historyCard(const db::Row &entry);
    QWidget *detailPanel(qint64 tournamentId);
    void editName();
    void deletePlayer();
    MainWindow *mw_;
    qint64 playerId_;
    QString playerName_, filter_ = "ALL";
    QSet<qint64> expanded_;
    QLabel *nameLabel_ = nullptr, *idLabel_ = nullptr;
    T::FlowLayout *statsLayout_ = nullptr;
    QVBoxLayout *feed_ = nullptr;
    QHash<QString, QPushButton *> filterButtons_;
};

// Pick one winner or a draw (ticking anyone already eliminated).  Used for draws and corrections.
class ResultDialog : public QDialog {
    Q_OBJECT
public:
    ResultDialog(QWidget *parent, const QVariantMap &pod, const QString &title, const QString &drawPolicy,
                 bool startAsDraw = false, bool askReason = false);
    QString outcome() const;
    qint64 winner() const;
    QList<qint64> eliminated() const;
    QString reason() const;
    QComboBox *outcomeCombo = nullptr;

private:
    QWidget *elimBox_ = nullptr;
    QHash<qint64, QCheckBox *> checks_;
    QLineEdit *reason_ = nullptr;
};

// Rounds, pods and results for one Commander event.
class CommanderEventScreen : public T::Screen {
    Q_OBJECT
public:
    CommanderEventScreen(MainWindow *mw, qint64 tournamentId, int viewRound = 0);
    void refresh();
    void report(const QVariantMap &pod, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminated);
    void finalizeRound(const QVariantMap &round);
    void finish(const QVariantMap &round);
    void publish(int expectedRound);
    QVariantMap state;
    QLabel *title = nullptr, *sub = nullptr;
    QPushButton *printBtn = nullptr;
    QPointer<QPushButton> endEarlyBtn;      // only while the event is running; rebuilt with the page
    QPushButton *renameBtn = nullptr;       // the pencil beside the title, in every state
    void rename();

protected:
    void reflow() override;

private:
    QVariantMap currentRound() const;       // empty when no round exists
    void buildStrip();
    QPushButton *chipButton(const QString &text, bool selected, const QString &color, bool enabled);
    QWidget *banner(const QString &text, const QString &color, const QString &icon = {});
    void buildLegacyNotice();
    void buildRound(const QVariantMap &round);
    QWidget *podCard(const QVariantMap &pod, const QVariantMap &round, const QHash<qint64, int> &points);
    void fillGrid(QGridLayout *grid, const QList<QWidget *> &cards);
    template <typename F> void guard(F fn, const QString &title);
    template <typename F> void completeWith(F fn);
    void dialogReport(const QVariantMap &pod, bool startAsDraw);
    void correct(const QVariantMap &pod);
    void openDrops();
    void openHistory();

    MainWindow *mw_;
    qint64 tournamentId_;
    int view_;                              // a round number, or 0 for the latest
    qint64 timerRound_ = 0;
    QVBoxLayout *timerSlot_ = nullptr, *body_ = nullptr;
    T::FlowLayout *strip_ = nullptr, *footer_ = nullptr;
    QWidget *footerHolder_ = nullptr;
    QList<QPair<QPointer<QGridLayout>, QList<QWidget *>>> grids_;
};

class CommanderStandingsScreen : public T::Screen {
    Q_OBJECT
public:
    CommanderStandingsScreen(MainWindow *mw, qint64 tournamentId);
    QVariantMap state;
    QVariantList rows;                      // each row gains "place"

private:
    QWidget *standingRow(const QVariantMap &r, const QHash<qint64, int> &seeds, bool last);
    void exportCsv();
    MainWindow *mw_;
    qint64 tournamentId_;
};
