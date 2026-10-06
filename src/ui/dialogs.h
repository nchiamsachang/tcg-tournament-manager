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
    QLabel *caption = nullptr, *display = nullptr;
    QPushButton *toggleBtn = nullptr, *resetBtn = nullptr;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void render();
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
