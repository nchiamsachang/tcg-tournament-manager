// The round clock widget, the Settings panel and small shared dialogs.
#pragma once

#include "store.h"

#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <functional>

class MainWindow;

// Round countdown.  The display is recomputed from the saved start time on
// every tick, and the saved state is re-read every few seconds, so it stays
// right across restarts, minimising and other windows on the same event.
// Reaching zero only shows "Time expired" — it never finalizes anything.
class TimerWidget : public QFrame {
    Q_OBJECT
public:
    TimerWidget(timerdb::Kind kind, qint64 roundId, std::function<void()> onExpire = {});
    void sync();                    // re-read the saved clock
    double remaining() const;
    QLabel *caption = nullptr, *display = nullptr, *clockIcon = nullptr;
    QPushButton *toggleBtn = nullptr, *resetBtn = nullptr;

protected:
    void resizeEvent(QResizeEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void render();
    QString look_;                  // the state the colours, caption and buttons were last drawn for
    void show(const timerdb::Timer &saved);
    timerdb::Kind kind_;
    qint64 roundId_;
    std::function<void()> onExpire_;
    timerdb::Reading clock_;        // the saved clock, as last read
    double lastRemaining_ = -1.0;   // negative: not rendered yet
    QBoxLayout *lay_ = nullptr;
    QTimer tick_, resync_;
};

// Small, global interface preferences.  Every change applies and saves immediately.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(MainWindow *mw);
    QButtonGroup *theme = nullptr, *text = nullptr;
    QCheckBox *sound = nullptr, *notifyBox = nullptr;
    void set(const QString &key, const QJsonValue &value);
    void restoreDefaults();

private:
    QButtonGroup *radios(QVBoxLayout *layout, const QString &title, const QString &key, const QString &current,
                         const QList<QPair<QString, QString>> &options);
    void exportBackup();
    MainWindow *mw_;
};

// Size a dialog to the window it belongs to, so it is never larger than the area available.
void fitDialog(QDialog *dialog, QWidget *parent, int width, int height);
// A read-only text panel (rules, history).
void showText(QWidget *parent, const QString &title, const QStringList &lines);
// Yes/No question.  Tests can make every question answer Yes with setAutoConfirm(true).
bool confirm(QWidget *parent, const QString &title, const QString &text);
void setAutoConfirm(bool on);
// Message boxes that tests can silence the same way.
void inform(QWidget *parent, const QString &title, const QString &text);
void warn(QWidget *parent, const QString &title, const QString &text);
// One-line text prompt with Cancel / OK; returns false when cancelled.
bool askText(QWidget *parent, const QString &title, const QString &label, const QString &okText, QString *value);

// Ending a tournament early (any format).
//
// The question names the tournament.  "Keep tournament" is the default button, and Escape or
// closing the window also keeps it; only pressing "End tournament" returns true.  It is never
// answered automatically, not even by setAutoConfirm.
bool confirmEndEarly(QWidget *parent, const QString &tournamentName);
// Asks, then saves the tournament as Terminated.  Only once that has been saved does the
// window change: the tournament's pill is removed and its game's hub is shown, where it is
// listed under History.  If it cannot be saved, the reason is shown and nothing changes.
// Returns true when the tournament was ended.
bool endTournamentEarly(MainWindow *mw, QWidget *parent, qint64 tournamentId);
// The red "End tournament early" button a round screen places apart from its routine actions.
QPushButton *endEarlyButton();
// A notice for the screens of a tournament that was ended early; `terminatedAt` is the saved
// UTC time.
QWidget *terminatedNotice(const QString &terminatedAt);

// Renaming a tournament (in any state).
//
// A compact dialog with the current name filled in and selected.  Save (or Enter) trims the
// name and saves it; a blank name, or a save that fails, is explained in the dialog and
// nothing changes.  Cancel or Escape changes nothing.  After a save the tournament's pill in
// the top bar shows the new name; the caller updates its own heading.  Returns the saved
// name, or an empty string when the name did not change.
QString editTournamentName(MainWindow *mw, QWidget *parent, qint64 tournamentId);
// The small pencil button placed beside a tournament's title.
QPushButton *editNameButton();

// Players with the same name.
//
// Shown when a name being added is already the name of one or more players in the
// directory (`matches`, each listed with its id).  The organizer picks one of them, or says
// this is a different person.  Nothing is merged or assumed.  Returns the chosen player's
// id, -1 for "Create a different player with this name", or 0 when cancelled.  `verb` words
// the pick: "Enroll" on a registration page, "Open" in the directory.
qint64 chooseSameName(QWidget *parent, const QString &name, const db::Rows &matches, const QString &verb);
// The question before a player is removed from the directory; `shownName` carries the id
// when another player has the same name.  Cancel is the default; Escape or closing cancels.
bool confirmRemovePlayer(QWidget *parent, const QString &shownName);
