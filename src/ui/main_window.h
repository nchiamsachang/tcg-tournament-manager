#pragma once

#include "round_status.h"
#include "store.h"

#include <QDialog>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVariantMap>

class QHBoxLayout;
class QVBoxLayout;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

    // Screens: home, hub (game), op_hub, poke_hub, mtg_hub, players, player_profile (player_id),
    // tournament_setup (game), registration / round / standings / round_select (tournament_id), settings.
    void navigateTo(const QString &screen, const QVariantMap &args = {});
    void goBack();
    QWidget *currentScreen() const { return stack ? stack->currentWidget() : nullptr; }
    QList<QPair<QString, QVariantMap>> history;

    static QString systemMode();
    QString effectiveMode() const;
    void restyle();                         // re-read preferences and rebuild in the new look, keeping the screen
    void setTheme(const QString &mode);     // explicit Light/Dark choice (also used by the toggle)
    void toggleTheme();
    void openSettings();
    void reopenSettingsAfterRestyle();

    void notify(const QString &message, int seconds = 15);
    void roundAlert(const QString &message);        // a round clock reached zero while its screen was open

    // The pills under the top bar, one per running tournament.  A pill shows the round and
    // clock saved in the database, so callers only say which tournament to show or re-read.
    void addTournamentTab(qint64 tournamentId, const QString &game, const QString &name);
    void updateTournamentTab(qint64 tournamentId);
    void removeTournamentTab(qint64 tournamentId);
    // A tournament was renamed: its pill, and the pages remembered for Back, take the new name.
    void renameTournamentTab(qint64 tournamentId, const QString &name);
    int liveTabCount() const { return int(tabOrder_.size()); }
    QString tabText(qint64 tournamentId) const;     // what the tournament's pill in the top area says
    QPushButton *tabButton(qint64 tournamentId) const { return tabButtons_.value(tournamentId); }
    // The colour of the clock part of that pill: green while it runs, red once expired, and
    // empty (the pill's own text colour) while it is paused, not started or absent.
    QString tabClockColor(qint64 tournamentId) const { return tabLabel(tournamentId).tailColor; }

    QStackedWidget *stack = nullptr;
    QPushButton *themeBtn = nullptr, *gearBtn = nullptr, *backBtn = nullptr;
    QLabel *logo = nullptr, *appLabel = nullptr, *livePill = nullptr, *toastLabel = nullptr;
    QFrame *toast = nullptr;
    QHash<QString, QPushButton *> navButtons;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    struct TabMeta {
        QString game, name;
        live::RoundStatus round;        // as last read from the database
        timerdb::Reading clock;         // the saved clock of the round being played
    };
    struct TabLabel {                   // a pill's wording: name and round, then the clock or status
        QString head, sep, tail;
        QString tailColor;              // empty: the pill's own text colour
        bool clock = false;             // the tail is a round clock (drawn with the timer icon)
    };
    TabLabel tabLabel(qint64 tournamentId) const;
    void loadTabClock(qint64 tournamentId);
    void showTab(qint64 tournamentId);
    void refreshTabs(bool reload);
    void styleNav(QPushButton *button, bool active);
    QTimer tabTimer_;
    int tabTicks_ = 0;
    void applyTheme();
    void buildChrome();
    void fitChrome();
    void addTabButton(qint64 tournamentId);
    void refreshLivePill();
    void restoreActiveTabs();

    QHBoxLayout *navBar_ = nullptr;
    bool compactNav_ = false, navStyled_ = false;

    QHash<qint64, TabMeta> tabMeta_;
    QList<qint64> tabOrder_;
    QHash<qint64, QPushButton *> tabButtons_;
    QLayout *tabArea_ = nullptr;
    qint64 currentTid_ = 0;
    QPointer<QDialog> settingsDialog_;
    QTimer toastTimer_;
};

// Builds the screen for a name; throws std::invalid_argument for an unknown one.
QWidget *makeScreen(const QString &name, MainWindow *mw, QVariantMap args);

void playAlertSound();
