#include "dialogs.h"

#include "applog.h"
#include "main_window.h"
#include "theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QSaveFile>
#include <QUrl>

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QTimeZone>
#include <QVBoxLayout>

TimerWidget::TimerWidget(timerdb::Kind kind, qint64 roundId, std::function<void()> onExpire)
    : kind_(kind), roundId_(roundId), onExpire_(std::move(onExpire))
{
    setObjectName("tw");
    setStyleSheet("#tw{background:" + T::SURFACE2 + ";border:1px solid " + T::BORDER + ";border-radius:16px;}");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    lay_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    lay_->setContentsMargins(T::px(18), T::px(10), T::px(14), T::px(10));
    lay_->setSpacing(T::px(12));
    auto *text = new QVBoxLayout;
    text->setSpacing(0);
    caption = T::lbl("ROUND CLOCK", T::MUTED, 10, 700, false, false, 1.0);
    display = T::lbl("--:--", T::GREY_LT, 28, 600, false, true, 1.5);
    display->setMinimumWidth(T::px(118));
    clockIcon = T::iconLabel("timer", T::GREY_LT, 22);
    auto *time = new QHBoxLayout;
    time->setSpacing(T::px(8));
    time->addWidget(clockIcon);
    time->addWidget(display, 1);
    text->addWidget(caption);
    text->addLayout(time);
    lay_->addLayout(text, 1);
    toggleBtn = T::button("Start", "primary");
    resetBtn = T::button("Reset", "ghost");
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(T::px(8));
    buttons->addWidget(toggleBtn);
    buttons->addWidget(resetBtn);
    lay_->addLayout(buttons);

    connect(toggleBtn, &QPushButton::clicked, this, [this] {
        const bool running = clock_.valid() && clock_.state() == timerdb::State::Running;
        show(running ? timerdb::pause(kind_, roundId_) : timerdb::start(kind_, roundId_));
    });
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        lastRemaining_ = -1.0;
        show(timerdb::reset(kind_, roundId_));
    });

    tick_.setInterval(250);
    connect(&tick_, &QTimer::timeout, this, [this] { render(); });
    resync_.setInterval(5000);
    connect(&resync_, &QTimer::timeout, this, [this] { sync(); });
    sync();
    tick_.start();
    resync_.start();
}

void TimerWidget::resizeEvent(QResizeEvent *e)
{
    QFrame::resizeEvent(e);
    // in a very narrow window the buttons drop below the clock instead of being squeezed
    const bool stacked = window()->width() < T::px(400);
    lay_->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

// A clock on a page that is not being shown does not tick: it is re-read when it is shown again.
void TimerWidget::hideEvent(QHideEvent *e)
{
    QFrame::hideEvent(e);
    tick_.stop();
    resync_.stop();
}

void TimerWidget::showEvent(QShowEvent *e)
{
    QFrame::showEvent(e);
    if (!tick_.isActive()) {
        sync();
        tick_.start();
        resync_.start();
    }
}

void TimerWidget::sync()
{
    show(timerdb::get(kind_, roundId_));
}

// Takes over a clock just read from (or written to) the database.  A round that no longer
// exists reads as invalid; the last good reading is kept on screen in that case.
void TimerWidget::show(const timerdb::Timer &saved)
{
    if (saved.valid)
        clock_ = timerdb::Reading(saved);
    render();
}

double TimerWidget::remaining() const
{
    return clock_.remaining();
}

void TimerWidget::render()
{
    if (!clock_.valid())
        return;
    using timerdb::State;
    const State state = clock_.state();
    const double left = remaining();
    // green while it counts down, red once the time is up, neutral while paused or not started;
    // the caption above says the same in words
    const bool expired = left <= 0;
    const bool running = state == State::Running && !expired;
    const bool closed = clock_.timer().closed;      // the tournament finished or was ended early
    const QString color = expired ? T::RED : (running ? T::GREEN : T::GREY_LT);
    display->setText(expired ? QString("Time expired") : timerdb::clockText(clock_.secondsLeft()));
    // The digits change every second; the look only changes with the clock's state, so the
    // fonts, colours, caption and buttons are redone then and not on every tick.
    const QString look = QStringLiteral("%1|%2|%3|%4").arg(color).arg(int(state)).arg(expired).arg(closed);
    if (look != look_) {
        look_ = look;
        display->setFont(expired ? T::font(17, 700, false, 0.0, 0) : T::font(28, 600, true, 1.5));
        display->setStyleSheet("color:" + color + ";background:transparent;border:none;");
        clockIcon->setPixmap(T::iconPixmap("timer", color, 22));
        caption->setText(running ? QStringLiteral("ROUND CLOCK · RUNNING")
                         : !expired && state == State::Paused ? QStringLiteral("ROUND CLOCK · PAUSED")
                         : QStringLiteral("ROUND CLOCK"));
        toggleBtn->setText(running ? "Pause" : (state == State::Paused ? "Resume" : "Start"));
        toggleBtn->setEnabled(!expired && !closed);
        resetBtn->setEnabled(!closed);
        toggleBtn->setStyleSheet(T::buttonQss(running ? "secondary" : "primary", T::MIN_HIT));
        T::setButtonIcon(toggleBtn, running ? "pause" : "play", T::labelColor(running ? "secondary" : "primary"));
    }
    // alert once, at the moment the clock runs out while this screen is open
    if (lastRemaining_ > 0 && left <= 0 && state == State::Running && onExpire_)
        onExpire_();
    lastRemaining_ = left;
}

SettingsDialog::SettingsDialog(MainWindow *mw) : QDialog(mw), mw_(mw)
{
    setWindowTitle("Settings");
    setModal(true);
    resize(qMin(T::px(440), mw->width()), qMin(T::px(600), mw->height()));
    setMinimumSize(280, 260);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(10);
    outer->addWidget(T::lbl("Settings", T::TEXT, 22, 700));

    QVBoxLayout *body = nullptr;
    outer->addWidget(T::scrollArea(&body), 1);
    body->setSpacing(2);

    const QJsonObject data = prefs::load();
    // "Follow system" is still honoured when it was chosen before (or after Restore defaults);
    // the pill that is lit is the look actually in use
    theme = radios(body, "Appearance", "theme", mw->effectiveMode(), {{"Light", "light"}, {"Dark", "dark"}});
    text = radios(body, "Text size", "text_size", data["text_size"].toString(),
                  {{"Standard", "standard"}, {"Large", "large"}});

    body->addSpacing(10);
    body->addWidget(T::caps("Round-end alerts"));
    body->addWidget(T::lbl("When a round clock reaches zero. Results are never changed automatically.", T::MUTED, 12, 400, true));
    sound = new QCheckBox("Play a sound");
    sound->setChecked(data["alert_sound"].toBool());
    connect(sound, &QCheckBox::toggled, this, [this](bool on) { set("alert_sound", on); });
    auto *soundRow = new QHBoxLayout;
    soundRow->addWidget(sound, 1);
    QPushButton *test = T::button("Test sound", "ghost");
    connect(test, &QPushButton::clicked, this, [] { playAlertSound(); });
    soundRow->addWidget(test);
    body->addLayout(soundRow);
    notifyBox = new QCheckBox("Show a notice in the app");
    notifyBox->setChecked(data["alert_notify"].toBool());
    connect(notifyBox, &QCheckBox::toggled, this, [this](bool on) { set("alert_notify", on); });
    body->addWidget(notifyBox);

    body->addSpacing(10);
    body->addWidget(T::caps("Data"));
    body->addWidget(T::lbl(db::path(), T::MUTED, 11, 400, true));
    QPushButton *backup = T::button(QStringLiteral("Export database backup…"), "ghost");
    connect(backup, &QPushButton::clicked, this, [this] { exportBackup(); });
    body->addWidget(backup, 0, Qt::AlignLeft);

    body->addSpacing(10);
    body->addWidget(T::caps("Diagnostics"));
    body->addWidget(T::lbl("The app keeps a log of what it did and of any errors, on this computer only. Nothing is sent anywhere.",
                           T::MUTED, 12, 400, true));
    // shown only while the log cannot be written; the app itself is not affected
    const applog::Status logging = applog::status();
    if (!logging.available) {
        QLabel *status = T::lbl(QStringLiteral("Logging unavailable: %1. The app works normally, but what it does is not "
                                               "being recorded (%2 entries so far). It tries again by itself.")
                                    .arg(applog::redact(logging.reason)).arg(logging.lost), T::AMBER, 12, 600, true);
        status->setObjectName("logStatus");
        status->setAccessibleName("Logging unavailable");
        body->addWidget(status);
    }
    auto *logButtons = new QWidget;
    auto *lr = new T::FlowLayout(logButtons, 8);
    QPushButton *openLogs = T::button("Open log folder", "ghost");
    connect(openLogs, &QPushButton::clicked, this, [this] { openLogFolder(); });
    QPushButton *report = T::button(QStringLiteral("Export diagnostic report…"), "ghost");
    connect(report, &QPushButton::clicked, this, [this] { exportReport(); });
    lr->addWidget(openLogs);
    lr->addWidget(report);
    body->addWidget(logButtons);
    body->addSpacing(8);
    QLabel *about = T::lbl(QStringLiteral("TCG Tournament Manager  v%1  ·  build %2").arg(prefs::VERSION, prefs::BUILD), T::MUTED, 11, 400, true);
    about->setObjectName("buildLabel");
    about->setTextInteractionFlags(Qt::TextSelectableByMouse);      // so it can be copied into a bug report
    body->addWidget(about);
    body->addStretch();

    // actions stay outside the scrolling area so they are always reachable
    auto *buttons = new QHBoxLayout;
    QPushButton *restore = T::button("Restore defaults", "ghost");
    connect(restore, &QPushButton::clicked, this, [this] {
        if (confirm(this, "Restore defaults",
                    "Reset appearance, text size and round-end alerts to their defaults?\n\n"
                    "Tournaments, players and results are not affected."))
            restoreDefaults();
    });
    QPushButton *done = T::button("Done", "primary");
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    done->setDefault(true);
    buttons->addWidget(restore);
    buttons->addStretch();
    buttons->addWidget(done);
    outer->addLayout(buttons);

    if (theme->checkedButton())
        theme->checkedButton()->setFocus();
}

QButtonGroup *SettingsDialog::radios(QVBoxLayout *layout, const QString &title, const QString &key,
                                     const QString &current, const QList<QPair<QString, QString>> &options)
{
    layout->addSpacing(12);
    layout->addWidget(T::caps(title));
    layout->addSpacing(6);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    auto *holder = new QWidget;
    holder->setAccessibleName(title);
    auto *flow = new T::FlowLayout(holder, 8);          // the pills wrap in a narrow dialog
    for (const auto &option : options) {
        QPushButton *pill = T::choicePill(option.first);
        pill->setProperty("value", option.second);
        pill->setAccessibleName(title + ": " + option.first);
        pill->setChecked(option.second == current);
        group->addButton(pill);
        flow->addWidget(pill);
    }
    layout->addWidget(holder);
    connect(group, &QButtonGroup::buttonClicked, this, [this, key](QAbstractButton *b) {
        set(key, b->property("value").toString());
    });
    return group;
}

void SettingsDialog::set(const QString &key, const QJsonValue &value)
{
    if (prefs::load().value(key) == value)
        return;
    prefs::setPref(key, value);
    if (key == "theme" || key == "text_size")
        mw_->reopenSettingsAfterRestyle();
}

void SettingsDialog::restoreDefaults()
{
    prefs::restoreDefaults();
    mw_->reopenSettingsAfterRestyle();
}

void SettingsDialog::exportBackup()
{
    const QString dest = QFileDialog::getSaveFileName(this, "Save database backup", "tcg_backup.db", "SQLite Database (*.db)");
    if (dest.isEmpty())
        return;
    QFile::remove(dest);
    if (QFile::copy(db::path(), dest))
        inform(this, "Backup saved", "Database backed up to:\n" + dest);
    else
        warn(this, "Backup failed", "The database could not be copied to:\n" + dest);
}

QString logFolder()
{
    const QString dir = applog::directory().isEmpty() ? db::dataDir() + "/logs" : applog::directory();
    QDir().mkpath(dir);
    return dir;
}

void SettingsDialog::openLogFolder()
{
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(logFolder())))
        warn(this, "Log folder", "The log folder could not be opened:\n" + QDir::toNativeSeparators(logFolder()));
}

bool saveDiagnosticReport(const QString &file, const QString &text)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(text.toUtf8());
    return f.commit();
}

// The report is shown first, exactly as it will be saved; nothing leaves the computer
// unless the organizer saves the file and sends it themselves.
void SettingsDialog::exportReport()
{
    const QString text = applog::diagnosticReport();
    QDialog dlg(this);
    dlg.setObjectName("reportDialog");
    dlg.setWindowTitle("Diagnostic report");
    fitDialog(&dlg, this, 760, 560);
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(T::lbl("Diagnostic report", T::TEXT, 20, 700, true));
    QLabel *notice = T::lbl("This is exactly what will be saved: version details and recent log entries, never the database, and "
                            "nothing is sent anywhere. Actions are recorded by id, but error messages can contain personal "
                            "information. Names the app recognises were replaced by ids on a best-effort basis; short or "
                            "unrecognised names and other typed-in text may remain. Please read it through before sharing it.",
                            T::MUTED, 12, 400, true);
    notice->setObjectName("reportNotice");
    v->addWidget(notice);
    if (!applog::status().available) {
        QLabel *missing = T::lbl("Logging is unavailable at the moment, so recent entries may be missing from this report.",
                                 T::AMBER, 12, 600, true);
        missing->setObjectName("reportLogStatus");
        v->addWidget(missing);
    }
    auto *preview = new QPlainTextEdit(text);
    preview->setObjectName("reportPreview");
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(T::font(11, 400, true));
    v->addWidget(preview, 1);
    auto *buttons = new QWidget;
    auto *row = new T::FlowLayout(buttons, 10);
    QPushButton *save = T::button(QStringLiteral("Save report…"), "primary");
    QPushButton *close = T::button("Close", "ghost");
    connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dlg, [&] {
        const QString name = QStringLiteral("tcg-diagnostic-report-%1.txt").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmm"));
        const QString dest = QFileDialog::getSaveFileName(&dlg, "Save diagnostic report", name, "Text files (*.txt)");
        if (dest.isEmpty())
            return;
        if (saveDiagnosticReport(dest, text)) {
            applog::info("report.export", {{"status", "ok"}});
            inform(&dlg, "Report saved", "The report was saved to:\n" + dest);
            dlg.accept();
        } else {
            warn(&dlg, "Report not saved", "The report could not be saved to:\n" + dest);
        }
    });
    row->addWidget(save);
    row->addWidget(close);
    v->addWidget(buttons);
    close->setFocus();
    dlg.exec();
}

void fitDialog(QDialog *dialog, QWidget *parent, int width, int height)
{
    const QSize avail = parent->window()->size();
    dialog->resize(qMin(width, avail.width()), qMin(height, avail.height()));
    dialog->setMinimumSize(qMin(280, avail.width()), 220);
}

void showText(QWidget *parent, const QString &title, const QStringList &lines)
{
    QDialog d(parent);
    d.setWindowTitle(title);
    fitDialog(&d, parent, 720, 480);
    auto *v = new QVBoxLayout(&d);
    v->addWidget(T::lbl(title, T::TEXT, 20, 700, true));
    auto *box = new QPlainTextEdit(lines.isEmpty() ? QString("Nothing recorded yet.") : lines.join("\n\n"));
    box->setReadOnly(true);
    v->addWidget(box, 1);
    QPushButton *close = T::button("Close", "primary");
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::accept);
    v->addWidget(close, 0, Qt::AlignRight);
    close->setFocus();
    d.exec();
}

static bool g_autoConfirm = false;

void setAutoConfirm(bool on)
{
    g_autoConfirm = on;
}

bool confirm(QWidget *parent, const QString &title, const QString &text)
{
    if (g_autoConfirm)
        return true;
    return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

void inform(QWidget *parent, const QString &title, const QString &text)
{
    if (!g_autoConfirm)
        QMessageBox::information(parent, title, text);
}

void warn(QWidget *parent, const QString &title, const QString &text)
{
    // Every error the organizer is shown is in the log, under the reference shown with it.
    // When an action has just failed, this is that action's reference and operation id, so
    // the attempt, the failure and this message can be matched.  Names the app recognises
    // are replaced by ids before the text is written.
    QString op;
    const QString ref = applog::errorReference(&op);
    applog::Fields fields{{"ref", ref}, {"title", title}};
    if (!op.isEmpty())
        fields.append({"op", op});
    applog::warning("dialog.error", fields, applog::scrub(text));
    if (!g_autoConfirm)
        QMessageBox::warning(parent, title, text + "\n\nReference: " + ref);
}

bool askText(QWidget *parent, const QString &title, const QString &label, const QString &okText, QString *value)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setMinimumWidth(qMin(360, parent->window()->width() - 24));
    auto *layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(T::lbl(title, T::TEXT, 20, 700));
    layout->addWidget(T::caps(label));
    auto *edit = new QLineEdit(*value);
    edit->setPlaceholderText(label);
    edit->setAccessibleName(label);
    layout->addWidget(edit);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    QPushButton *cancel = T::button("Cancel", "ghost");
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QPushButton *ok = T::button(okText, "primary");
    ok->setDefault(true);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    layout->addLayout(buttons);
    edit->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *value = edit->text().trimmed();
    return true;
}

bool confirmEndEarly(QWidget *parent, const QString &tournamentName)
{
    QDialog dlg(parent);
    dlg.setObjectName("endEarlyDialog");
    dlg.setWindowTitle("End this tournament early?");
    dlg.setModal(true);
    const int room = parent->window()->width() - 24;
    dlg.setMinimumWidth(qMin(T::px(300), room));
    dlg.resize(qMin(T::px(460), room), 10);
    auto *layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 18, 20, 18);

    auto *head = new QHBoxLayout;
    head->setSpacing(10);
    head->addWidget(T::iconLabel("triangle-alert", T::RED, 22), 0, Qt::AlignTop);
    head->addWidget(T::lbl("End this tournament early?", T::TEXT, 20, 700, true), 1);
    layout->addLayout(head);
    layout->addWidget(T::lbl(QStringLiteral("This will end %1 and remove it from active tournaments. Its saved rounds and "
                                            "results will remain in History, marked \u2018Terminated.\u2019 Unreported "
                                            "matches will remain unfinished.").arg(tournamentName),
                             T::TEXT, 14, 400, true));

    auto *buttons = new QWidget;
    auto *row = new T::FlowLayout(buttons, 10);
    QPushButton *keep = T::button("Keep tournament", "primary");
    QPushButton *end = T::button("End tournament", "danger", "octagon-x");
    // Enter and Space press the button with the focus, which is the safe one; a repeated click
    // or keystroke from opening the dialog cannot end the tournament
    keep->setDefault(true);
    end->setAutoDefault(false);
    QObject::connect(keep, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(end, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addWidget(keep);
    row->addWidget(end);
    layout->addWidget(buttons);
    keep->setFocus();
    return dlg.exec() == QDialog::Accepted;     // Escape and the close button reject
}

bool endTournamentEarly(MainWindow *mw, QWidget *parent, qint64 tournamentId)
{
    const db::Row t = tdb::tournamentById(tournamentId);
    if (t.isEmpty())
        return false;
    const QString name = t["name"].toString(), game = t["game"].toString();
    if (!confirmEndEarly(parent, name))
        return false;
    try {
        tdb::terminateTournament(tournamentId);     // false when it was already ended: the result is the same
    } catch (const std::exception &e) {
        warn(parent, "Tournament not ended",
             name + " could not be ended early, and nothing was changed.\n\n" + QString::fromUtf8(e.what()));
        return false;
    }
    mw->removeTournamentTab(tournamentId);
    mw->notify(name + QStringLiteral(" was ended early. Its rounds and results are in History, marked Terminated."), 10);
    mw->navigateTo("hub", {{"game", game}});
    return true;
}

QPushButton *endEarlyButton()
{
    QPushButton *b = T::button("End tournament early", "danger", "octagon-x");
    b->setToolTip("Stops this tournament now and keeps its rounds and results as a record marked Terminated");
    return b;
}

QWidget *terminatedNotice(const QString &terminatedAt)
{
    QDateTime when = QDateTime::fromString(terminatedAt, "yyyy-MM-dd HH:mm:ss");
    when.setTimeZone(QTimeZone::utc());
    const QString date = when.isValid() ? QStringLiteral(" on ") + QLocale().toString(when.toLocalTime(), "d MMM yyyy, HH:mm")
                                        : QString();
    QFrame *f = T::panel("tn", T::RED, 2);
    auto *h = new QHBoxLayout(f);
    h->setContentsMargins(18, 12, 18, 12);
    h->setSpacing(10);
    h->addWidget(T::iconLabel("octagon-x", T::RED, 20), 0, Qt::AlignTop);
    h->addWidget(T::lbl(QStringLiteral("Terminated. This tournament was ended early%1. Its rounds and results are a read-only "
                                       "record; matches that were not reported stay unfinished.").arg(date),
                        T::TEXT, 13, 600, true), 1);
    return f;
}

QString editTournamentName(MainWindow *mw, QWidget *parent, qint64 tournamentId)
{
    const QString current = tdb::tournamentById(tournamentId)["name"].toString();
    QDialog dlg(parent);
    dlg.setObjectName("renameDialog");
    dlg.setWindowTitle("Edit tournament name");
    dlg.setModal(true);
    const int room = parent->window()->width() - 24;
    dlg.setMinimumWidth(qMin(T::px(280), room));
    dlg.resize(qMin(T::px(440), room), 10);
    auto *layout = new QVBoxLayout(&dlg);
    layout->setSpacing(10);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(T::lbl("Edit tournament name", T::TEXT, 20, 700, true));
    layout->addWidget(T::caps("Tournament name"));
    auto *edit = new QLineEdit(current);
    edit->setAccessibleName("Tournament name");
    edit->setMaxLength(tdb::MAX_NAME_LENGTH);
    layout->addWidget(edit);
    QLabel *error = T::lbl("", T::RED, 12, 600, true);
    error->setObjectName("renameError");
    error->hide();
    layout->addWidget(error);

    auto *buttons = new QWidget;
    auto *row = new T::FlowLayout(buttons, 10);
    QPushButton *save = T::button("Save", "primary");
    QPushButton *cancel = T::button("Cancel", "ghost");
    save->setDefault(true);             // Enter saves
    cancel->setAutoDefault(false);
    row->addWidget(save);
    row->addWidget(cancel);
    layout->addWidget(buttons);

    QString saved;
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, &dlg, [&] {
        const QString name = edit->text().trimmed();
        QString problem;
        if (name.isEmpty()) {
            problem = "Enter a name for the tournament.";
        } else {
            try {
                if (tdb::renameTournament(tournamentId, name))
                    saved = name;
                dlg.accept();
                return;
            } catch (const std::exception &e) {
                problem = QStringLiteral("The name was not changed. ") + QString::fromUtf8(e.what());
            }
        }
        // the dialog stays open with what was typed, and the tournament keeps its name
        {
            QString op;
            const QString ref = applog::errorReference(&op);
            applog::Fields fields{{"ref", ref}, {"title", "Edit tournament name"}, {"tournament", tournamentId}};
            if (!op.isEmpty())
                fields.append({"op", op});
            applog::warning("dialog.error", fields, applog::scrub(problem));
            problem += QStringLiteral(" (Reference: %1)").arg(ref);
        }
        error->setText(T::breakable(problem));
        error->show();
        edit->setFocus();
        edit->selectAll();
    });
    edit->setFocus();
    edit->selectAll();
    dlg.exec();                         // Escape and the close button cancel
    if (!saved.isEmpty())
        mw->renameTournamentTab(tournamentId, saved);
    return saved;
}

QPushButton *editNameButton()
{
    return T::iconButton("pencil", "Edit tournament name");
}

qint64 chooseSameName(QWidget *parent, const QString &name, const db::Rows &matches, const QString &verb)
{
    QDialog dlg(parent);
    dlg.setObjectName("sameNameDialog");
    dlg.setWindowTitle("A player with this name already exists");
    dlg.setModal(true);
    fitDialog(&dlg, parent, T::px(480), T::px(170) + int(qMin(matches.size(), 5)) * (T::BUTTON_H + 10));
    auto *layout = new QVBoxLayout(&dlg);
    layout->setSpacing(10);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(T::lbl("A player with this name already exists", T::TEXT, 20, 700, true));
    layout->addWidget(T::lbl(matches.size() == 1
                                 ? QStringLiteral("“%1” is already in the player directory. Is this the same person?").arg(name.simplified())
                                 : QStringLiteral("%1 players named “%2” are already in the player directory. Is this one of them?")
                                       .arg(matches.size()).arg(name.simplified()),
                             T::TEXT, 14, 400, true));
    QVBoxLayout *list = nullptr;
    layout->addWidget(T::scrollArea(&list), 1);       // many namesakes scroll; the choices below stay in view
    list->setSpacing(8);
    qint64 choice = 0;
    for (const db::Row &m : matches) {
        const qint64 id = m["player_id"].toLongLong();
        // the id is always shown here: it is what tells the records apart
        auto *pick = new T::WrapButton(QStringLiteral("%1 %2 · %3").arg(verb, m["display_name"].toString(), pdb::formatId(id)), "secondary");
        pick->setProperty("playerId", id);
        QObject::connect(pick, &QPushButton::clicked, &dlg, [&dlg, &choice, id] {
            choice = id;
            dlg.accept();
        });
        list->addWidget(pick);
    }
    list->addStretch();
    auto *buttons = new QWidget;
    auto *row = new T::FlowLayout(buttons, 10);
    QPushButton *different = T::button("Create a different player with this name", "ghost", "user-plus");
    QPushButton *cancel = T::button("Cancel", "ghost");
    for (QPushButton *b : {different, cancel})
        b->setAutoDefault(false);       // Enter picks nothing by itself: the organizer chooses
    QObject::connect(different, &QPushButton::clicked, &dlg, [&dlg, &choice] {
        choice = -1;
        dlg.accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    row->addWidget(different);
    row->addWidget(cancel);
    layout->addWidget(buttons);
    cancel->setFocus();
    return dlg.exec() == QDialog::Accepted ? choice : 0;
}

bool confirmRemovePlayer(QWidget *parent, const QString &shownName)
{
    QDialog dlg(parent);
    dlg.setObjectName("removePlayerDialog");
    dlg.setWindowTitle("Remove player");
    dlg.setModal(true);
    const int room = parent->window()->width() - 24;
    dlg.setMinimumWidth(qMin(T::px(300), room));
    dlg.resize(qMin(T::px(480), room), 10);
    auto *layout = new QVBoxLayout(&dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->addWidget(T::lbl(QStringLiteral("Remove %1 from the player directory?").arg(shownName), T::TEXT, 20, 700, true));
    layout->addWidget(T::lbl("They will no longer appear in player searches or be available for new enrollment. Their name and "
                             "existing tournament results will remain in history, but their profile will no longer be accessible.",
                             T::TEXT, 14, 400, true));
    auto *buttons = new QWidget;
    auto *row = new T::FlowLayout(buttons, 10);
    QPushButton *cancel = T::button("Cancel", "primary");
    QPushButton *remove = T::button("Remove player", "danger", "trash-2");
    cancel->setDefault(true);
    remove->setAutoDefault(false);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(remove, &QPushButton::clicked, &dlg, &QDialog::accept);
    row->addWidget(cancel);
    row->addWidget(remove);
    layout->addWidget(buttons);
    cancel->setFocus();
    return dlg.exec() == QDialog::Accepted;
}
