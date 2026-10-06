#include "dialogs.h"

#include "main_window.h"
#include "theme.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QVBoxLayout>

// the countdown turns orange in the last ten minutes and red in the last five
static const int WARNING_SECS = 600, URGENT_SECS = 300;

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
    text->addWidget(caption);
    text->addWidget(display);
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
    QString color;
    if (left <= 0) {
        display->setText("Time expired");
        display->setFont(T::font(17, 700, false, 0.0, 0));
        color = T::RED;
    } else {
        display->setText(timerdb::clockText(clock_.secondsLeft()));
        display->setFont(T::font(28, 600, true, 1.5));
        color = left < URGENT_SECS ? T::RED : (left < WARNING_SECS ? T::ORANGE : T::GREY_LT);
    }
    display->setStyleSheet("color:" + color + ";background:transparent;border:none;");
    caption->setText(left > 0 && state == State::Running ? QStringLiteral("ROUND CLOCK · RUNNING")
                     : left > 0 && state == State::Paused ? QStringLiteral("ROUND CLOCK · PAUSED")
                     : QStringLiteral("ROUND CLOCK"));
    const bool running = state == State::Running && left > 0;
    toggleBtn->setText(running ? "Pause" : (state == State::Paused ? "Resume" : "Start"));
    toggleBtn->setEnabled(left > 0);
    toggleBtn->setStyleSheet(T::buttonQss(running ? "secondary" : "primary", T::MIN_HIT));
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
    body->addSpacing(8);
    body->addWidget(T::lbl(QStringLiteral("TCG Tournament Manager  v%1").arg(prefs::VERSION), T::MUTED, 11));
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
    if (!g_autoConfirm)
        QMessageBox::warning(parent, title, text);
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
