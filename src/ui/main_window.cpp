#include "main_window.h"

#include "applog.h"
#include "commander_db.h"
#include "dialogs.h"
#include "screens.h"
#include "store.h"
#include "theme.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QPalette>
#include <QStyleHints>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QVBoxLayout>
#include <QtDebug>

// A tournament's pill in the bar under the navigation.  It is an ordinary button (its text is
// the whole label), drawn in two parts so the clock can have its own colour and a timer icon.
class TabPill : public QPushButton {
public:
    void setParts(const QString &head, const QString &tail, const QString &tailColor, bool clock, bool selected)
    {
        if (head == head_ && tail == tail_ && tailColor == tailColor_ && clock == clock_ && selected == selected_)
            return;
        const bool resized = clock != clock_;
        head_ = head;
        tail_ = tail;
        tailColor_ = tailColor;
        clock_ = clock;
        selected_ = selected;
        if (resized)
            updateGeometry();
        update();
    }
    QSize sizeHint() const override
    {
        QSize s = QPushButton::sizeHint();
        if (clock_)
            s.rwidth() += iconRoom();
        return s;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
    QString tailColor() const { return tailColor_; }
    bool showsClock() const { return clock_; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        QStyleOptionButton o;
        initStyleOption(&o);
        o.text.clear();                 // the style draws the pill; the label is drawn below
        p.drawControl(QStyle::CE_PushButton, o);
        const QFontMetrics fm(font());
        const int headW = fm.horizontalAdvance(head_), tailW = fm.horizontalAdvance(tail_);
        const int room = clock_ ? iconRoom() : 0;
        int x = (width() - headW - room - tailW) / 2;
        const QColor base(!isEnabled() ? T::DIM : (selected_ || underMouse() || hasFocus()) ? T::TEXT : T::MUTED);
        const QColor accent = tailColor_.isEmpty() ? base : QColor(tailColor_);
        p.setFont(font());
        p.setPen(base);
        p.drawText(QRect(x, 0, headW + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, head_);
        x += headW;
        if (clock_) {
            const int side = T::px(ICON);
            p.drawPixmap(x, (height() - side) / 2, T::iconPixmap("timer", accent.name(), ICON));
            x += room;
        }
        p.setPen(accent);
        p.drawText(QRect(x, 0, tailW + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, tail_);
    }

private:
    static constexpr int ICON = 15;
    static int iconRoom() { return T::px(ICON) + 6; }
    QString head_, tail_, tailColor_;
    bool clock_ = false, selected_ = false;
};

MainWindow::MainWindow()
{
    setWindowTitle("TCG Tournament Manager");
    setMinimumSize(360, 480);     // the smallest window in which every screen is verified to fit, at both text sizes
    resize(1180, 780);
    // the clocks in the tab bar are recomputed from saved timestamps every second, and the saved
    // state is re-read every few seconds (so another window's Start/Pause shows up here too)
    tabTimer_.setInterval(1000);
    connect(&tabTimer_, &QTimer::timeout, this, [this] { refreshTabs(++tabTicks_ % 5 == 0); });
    tabTimer_.start();
    toastTimer_.setSingleShot(true);
    connect(&toastTimer_, &QTimer::timeout, this, [this] { if (toast) toast->hide(); });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (prefs::load()["theme"].toString() == "system")
            restyle();
    });

    db::initialize();
    applyTheme();
    buildChrome();
    restoreActiveTabs();
    navigateTo("home");
}

QString MainWindow::systemMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light ? "light" : "dark";
}

QString MainWindow::effectiveMode() const
{
    const QString choice = prefs::load()["theme"].toString();
    return choice == "system" ? systemMode() : choice;
}

void MainWindow::applyTheme()
{
    T::apply(effectiveMode(), prefs::load()["text_size"].toString());
    qApp->setFont(T::font(14));
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(T::BG));
    pal.setColor(QPalette::WindowText, QColor(T::TEXT));
    pal.setColor(QPalette::Base, QColor(T::SURFACE));
    pal.setColor(QPalette::AlternateBase, QColor(T::SURFACE2));
    pal.setColor(QPalette::Text, QColor(T::TEXT));
    pal.setColor(QPalette::PlaceholderText, QColor(T::DIM));
    pal.setColor(QPalette::Button, QColor(T::SURFACE3));
    pal.setColor(QPalette::ButtonText, QColor(T::TEXT));
    pal.setColor(QPalette::ToolTipBase, QColor(T::SURFACE2));
    pal.setColor(QPalette::ToolTipText, QColor(T::TEXT));
    pal.setColor(QPalette::Highlight, QColor(T::PURPLE));
    pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(T::DIM));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(T::DIM));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(T::DIM));
    qApp->setPalette(pal);
    setStyleSheet(T::appQss());
}

void MainWindow::restyle()
{
    applyTheme();
    const QPair<QString, QVariantMap> current = history.isEmpty() ? qMakePair(QString("home"), QVariantMap()) : history.last();
    if (!history.isEmpty())
        history.removeLast();
    buildChrome();
    for (qint64 tid : tabOrder_)
        addTabButton(tid);
    navigateTo(current.first, current.second);
}

void MainWindow::setTheme(const QString &mode)
{
    prefs::setPref("theme", mode.toLower());
    restyle();
}

void MainWindow::toggleTheme()
{
    setTheme(effectiveMode() == "dark" ? "light" : "dark");
    themeBtn->setFocus();
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(this);
    settingsDialog_ = &dialog;
    dialog.exec();
    settingsDialog_ = nullptr;
    gearBtn->setFocus();            // return focus to where the panel was opened from
}

void MainWindow::reopenSettingsAfterRestyle()
{
    const QPointer<QDialog> dialog = settingsDialog_;
    restyle();
    if (dialog) {
        dialog->done(0);
        QTimer::singleShot(0, this, [this] { openSettings(); });
    }
}

void MainWindow::buildChrome()
{
    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // top bar: logo, name, navigation; live indicator and theme switch on the right
    auto *nav = new QFrame;
    nav->setObjectName("nav");
    nav->setFixedHeight(64);
    nav->setStyleSheet("#nav{background:" + T::SURFACE + ";border-bottom:1px solid " + T::BORDER + ";}");
    auto *nh = navBar_ = new QHBoxLayout(nav);
    nh->setContentsMargins(20, 0, 16, 0);
    nh->setSpacing(6);
    navStyled_ = false;

    logo = new QLabel;
    logo->setPixmap(T::iconPixmap("swords", "#ffffff", 19));
    logo->setFixedSize(36, 36);
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet("background:" + T::PURPLE + ";border-radius:18px;");
    nh->addWidget(logo);
    appLabel = T::lbl("TCG Manager", T::TEXT, 18, 700);
    appLabel->setFont(T::font(18, 700, false, 0.0, 1));
    appLabel->setContentsMargins(7, 0, 14, 0);
    nh->addWidget(appLabel);

    navButtons.clear();
    for (const auto &item : {qMakePair(QString("Home"), QString("home")), qMakePair(QString("Players"), QString("players"))}) {
        auto *b = new QPushButton(item.first);
        b->setMinimumHeight(T::MIN_HIT);
        b->setCursor(QCursor(Qt::PointingHandCursor));
        b->setProperty("label", item.first);
        b->setProperty("iconName", item.second == "home" ? "house" : "users");
        styleNav(b, false);
        const QString screen = item.second;
        connect(b, &QPushButton::clicked, this, [this, screen] { navigateTo(screen); });
        nh->addWidget(b);
        navButtons.insert(screen, b);
    }
    nh->addStretch();

    livePill = new QLabel;
    livePill->setFont(T::font(12, 600));
    livePill->setStyleSheet("color:" + T::PURPLE_LT + ";background:" + T::PURPLE_DIM + ";border:1px solid " + T::PURPLE_BDR
                            + ";border-radius:" + T::P(11) + "px;padding:" + T::P(4) + "px " + T::P(14) + "px;");
    livePill->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    livePill->hide();
    nh->addWidget(livePill);

    const bool dark = effectiveMode() == "dark";
    // the icon shows what pressing it does: a sun switches to light, a moon to dark
    themeBtn = T::roundButton(dark ? "sun" : "moon", dark ? "Switch to light mode" : "Switch to dark mode");
    connect(themeBtn, &QPushButton::clicked, this, [this] { toggleTheme(); });
    nh->addWidget(themeBtn);
    gearBtn = T::roundButton("settings", "Settings");
    gearBtn->setAccessibleDescription("Appearance, text size and round-end alerts");
    connect(gearBtn, &QPushButton::clicked, this, [this] { openSettings(); });
    nh->addWidget(gearBtn);
    root->addWidget(nav);

    // second bar: back, then one pill per tournament in progress with its round and clock.
    // The pills wrap onto more lines when the window is narrow, so the bar grows instead of clipping.
    auto *tbar = new QFrame;
    tbar->setObjectName("tbar");
    tbar->setStyleSheet("#tbar{background:" + T::SURFACE + ";border-bottom:1px solid " + T::BORDER + ";}");
    auto *th = new QHBoxLayout(tbar);
    th->setContentsMargins(12, 6, 16, 6);       // 44px Back button in the room the 40px one had
    th->setSpacing(6);
    backBtn = new QPushButton;
    backBtn->setFixedSize(44, 44);
    T::setButtonIcon(backBtn, "arrow-left", T::MUTED, false, 20);
    backBtn->setToolTip("Back");
    backBtn->setAccessibleName("Back");
    backBtn->setCursor(QCursor(Qt::PointingHandCursor));
    backBtn->setStyleSheet("QPushButton{background:transparent;color:" + T::MUTED + ";border:2px solid transparent;border-radius:"
                           "22px;padding:0;min-width:40px;min-height:40px;}"      // 44 with the 2px border
                           "QPushButton:hover{background:" + T::SURFACE3 + ";color:" + T::TEXT + ";border-color:" + T::BORDER2 + ";}"
                           "QPushButton:focus{border-color:" + T::FOCUS + ";}");
    connect(backBtn, &QPushButton::clicked, this, [this] { goBack(); });
    th->addWidget(backBtn, 0, Qt::AlignTop);
    auto *holder = new QWidget;
    tabArea_ = new T::FlowLayout(holder, 8);
    th->addWidget(holder, 1);
    root->addWidget(tbar);

    // in-app notice (round-end alerts); sits in the layout so it never covers content
    toast = new QFrame;
    toast->setObjectName("toast");
    toast->setStyleSheet("#toast{background:" + T::alpha(T::AMBER, 40) + ";border-bottom:2px solid " + T::AMBER + ";}");
    auto *tl = new QHBoxLayout(toast);
    tl->setContentsMargins(20, 8, 12, 8);
    toastLabel = T::lbl("", T::TEXT, 13, 600, true);
    tl->addWidget(toastLabel, 1);
    QPushButton *dismiss = T::button("Dismiss", "ghost");
    connect(dismiss, &QPushButton::clicked, toast, &QWidget::hide);
    tl->addWidget(dismiss);
    toast->hide();
    root->addWidget(toast);

    stack = new QStackedWidget;
    stack->setObjectName("stack");
    stack->setStyleSheet("#stack{background:" + T::BG + ";}");
    root->addWidget(stack, 1);

    tabButtons_.clear();
    setCentralWidget(central);
    refreshLivePill();
}

void MainWindow::fitChrome()
{
    // drop the optional parts of the top bar as the window narrows, never the controls
    if (!appLabel)
        return;
    const int w = width();
    const int roomy = T::px(365);                   // 420 at the standard text size, more at Large
    logo->setVisible(w >= roomy);
    appLabel->setVisible(w >= T::px(520) + 2);      // 600 at the standard text size; it needs more room at Large
    livePill->setVisible(w >= 980 && !tabOrder_.isEmpty());
    // in a very narrow window the navigation keeps every label by giving up padding, not text
    const bool compact = w < roomy;
    if (compact != compactNav_ || !navStyled_) {
        compactNav_ = compact;
        navStyled_ = true;
        navBar_->setContentsMargins(compact ? 6 : 20, 0, compact ? 6 : 16, 0);
        navBar_->setSpacing(compact ? 0 : 6);
        const QString current = history.isEmpty() ? QString("home") : history.last().first;
        for (auto it = navButtons.begin(); it != navButtons.end(); ++it)
            styleNav(it.value(), it.key() == current);
    }
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    fitChrome();
    refreshTabs(false);         // the pills' wording follows the room available
}

void MainWindow::notify(const QString &message, int seconds)
{
    toastLabel->setText(T::breakable(message));
    toast->show();
    toastTimer_.start(seconds * 1000);
}

void MainWindow::roundAlert(const QString &message)
{
    const QJsonObject data = prefs::load();
    if (data["alert_sound"].toBool())
        playAlertSound();
    if (data["alert_notify"].toBool())
        notify(message);
}

void MainWindow::navigateTo(const QString &screen, const QVariantMap &args)
{
    if (screen == "settings") {
        openSettings();
        return;
    }
    QWidget *widget = nullptr;
    try {
        widget = makeScreen(screen, this, args);
    } catch (const std::exception &e) {
        applog::error("page.open", {{"status", "failed"}, {"page", screen}, {"tournament", args.value("tournament_id").toLongLong()},
                                    {"player", args.value("player_id").toLongLong()}}, QString::fromUtf8(e.what()));
        return;
    }
    history.append({screen, args});
    QList<QWidget *> old;
    for (int i = 0; i < stack->count(); ++i)
        old << stack->widget(i);
    stack->setCurrentIndex(stack->addWidget(widget));
    for (QWidget *w : old) {            // one live screen at a time; old ones stop their timers
        stack->removeWidget(w);
        w->hide();
        w->deleteLater();
    }
    for (auto it = navButtons.begin(); it != navButtons.end(); ++it)
        styleNav(it.value(), it.key() == screen);
    currentTid_ = (screen == "round" || screen == "standings") ? args.value("tournament_id").toLongLong() : 0;
    refreshTabs(true);
}

void MainWindow::goBack()
{
    if (history.size() > 1) {
        history.removeLast();
        const auto prev = history.takeLast();
        navigateTo(prev.first, prev.second);
    }
}

void MainWindow::addTournamentTab(qint64 tournamentId, const QString &game, const QString &name)
{
    if (tabMeta_.contains(tournamentId))
        return;
    TabMeta meta;
    meta.game = game;
    meta.name = name;
    tabMeta_.insert(tournamentId, meta);
    tabOrder_.append(tournamentId);
    addTabButton(tournamentId);
}

void MainWindow::addTabButton(qint64 tournamentId)
{
    const TabMeta meta = tabMeta_.value(tournamentId);
    auto *btn = new TabPill;
    btn->setCursor(QCursor(Qt::PointingHandCursor));
    connect(btn, &QPushButton::clicked, this, [this, tournamentId, meta] {
        navigateTo("round", {{"tournament_id", tournamentId}, {"game", meta.game}, {"tournament_name", meta.name}});
    });
    tabArea_->addWidget(btn);
    tabButtons_.insert(tournamentId, btn);
    loadTabClock(tournamentId);
    showTab(tournamentId);
    refreshLivePill();
}

void MainWindow::updateTournamentTab(qint64 tournamentId)
{
    if (!tabButtons_.contains(tournamentId))
        return;
    loadTabClock(tournamentId);
    showTab(tournamentId);
}

void MainWindow::renameTournamentTab(qint64 tournamentId, const QString &name)
{
    for (auto &page : history)
        if (page.second.value("tournament_id").toLongLong() == tournamentId && page.second.contains("tournament_name"))
            page.second["tournament_name"] = name;
    if (!tabMeta_.contains(tournamentId))
        return;
    tabMeta_[tournamentId].name = name;
    if (QPushButton *btn = tabButtons_.value(tournamentId))
        btn->setText(QString());        // so the tooltip and accessible name are rewritten even if the visible text is the same
    showTab(tournamentId);
    if (tabArea_)
        tabArea_->invalidate();
}

void MainWindow::removeTournamentTab(qint64 tournamentId)
{
    tabMeta_.remove(tournamentId);
    tabOrder_.removeAll(tournamentId);
    if (QPushButton *btn = tabButtons_.take(tournamentId)) {
        tabArea_->removeWidget(btn);
        btn->hide();
        btn->deleteLater();
    }
    refreshLivePill();
}

// Reads this tournament's current round and the clock saved with it.
void MainWindow::loadTabClock(qint64 tournamentId)
{
    if (!tabMeta_.contains(tournamentId))
        return;
    TabMeta &m = tabMeta_[tournamentId];
    try {
        m.round = live::roundStatus(tournamentId);
        m.clock = m.round.playing ? timerdb::read(m.round.kind, m.round.roundId) : timerdb::Reading();
    } catch (const std::exception &) {
        // the database could not be read just now: show no clock and try again on the next refresh
        m.round.playing = false;
        m.clock = timerdb::Reading();
    }
}

// "Commander Night · Round 2 of 3 · 24:18 remaining", from the saved clock.  The name and
// round are one part and the clock the other, so only the clock takes a colour: green while
// it counts down, red once the time is up, and the pill's own colour while it is paused or
// has not been started.
MainWindow::TabLabel MainWindow::tabLabel(qint64 tournamentId) const
{
    const TabMeta m = tabMeta_.value(tournamentId);
    // the pill never has to be wider than the window: in a narrow window the name is cut
    // shorter (the full name is the tooltip) and the wording is tightened
    const int room = width() - 100;
    const int perChar = qMax(6, T::px(8));
    const bool tight = room < 46 * perChar;
    const int nameMax = qBound(6, room / perChar - (tight ? 22 : 34), 22);
    TabLabel label;
    label.sep = tight ? QStringLiteral(" · ") : QStringLiteral("  ·  ");
    label.head = m.name.size() > nameMax ? m.name.left(nameMax).trimmed() + QStringLiteral("…") : m.name;
    const int number = m.round.roundNumber, total = m.round.totalRounds;
    if (tight)
        label.head += label.sep + (total ? QStringLiteral("R%1/%2").arg(number).arg(total) : QStringLiteral("R%1").arg(number));
    else
        label.head += label.sep + (total ? QStringLiteral("Round %1 of %2").arg(number).arg(total)
                                         : QStringLiteral("Round %1").arg(number));
    if (m.round.playing && m.clock.valid()) {
        const int left = m.clock.secondsLeft();
        const QString clock = timerdb::clockText(left);
        label.clock = !tight;           // a narrow window keeps the wording and the colour, and gives up the icon
        if (left <= 0) {
            label.tail = "Time expired";
            label.tailColor = T::RED;
        } else if (m.clock.state() == timerdb::State::Running) {
            label.tail = clock + (tight ? " left" : " remaining");
            label.tailColor = T::GREEN;
        } else if (m.clock.state() == timerdb::State::Paused) {
            label.tail = (tight ? "Paused " : "Paused at ") + clock;
        } else {
            label.tail = "Not started";
        }
    } else if (m.round.needsFinish) {
        label.tail = "Needs finishing";
    } else if (m.round.kind == timerdb::Kind::Commander && !m.round.playing) {
        label.tail = "Between rounds";
    }
    return label;
}

QString MainWindow::tabText(qint64 tournamentId) const
{
    const TabLabel label = tabLabel(tournamentId);
    return label.tail.isEmpty() ? label.head : label.head + label.sep + label.tail;
}

void MainWindow::showTab(qint64 tournamentId)
{
    auto *btn = static_cast<TabPill *>(tabButtons_.value(tournamentId));
    if (!btn)
        return;
    const TabLabel label = tabLabel(tournamentId);
    const QString text = label.tail.isEmpty() ? label.head : label.head + label.sep + label.tail;
    const bool selected = tournamentId == currentTid_;
    const int h = qMax(T::px(33), T::MIN_HIT);
    if (btn->text() != text) {
        btn->setText(text);
        btn->setAccessibleName(tabMeta_.value(tournamentId).name + ", " + text.section(QStringLiteral(" · "), 1).trimmed()
                               + (selected ? ", open now" : ""));
        btn->setToolTip(tabMeta_.value(tournamentId).name + QStringLiteral(" — open this tournament"));
    }
    btn->setParts(label.head + (label.tail.isEmpty() ? QString() : label.sep), label.tail, label.tailColor, label.clock, selected);
    const QString style = T::pillQss(selected, h);
    if (btn->property("pill").toString() != style) {
        btn->setProperty("pill", style);
        btn->setMinimumHeight(h);
        btn->setStyleSheet(style);
    }
}

void MainWindow::refreshTabs(bool reload)
{
    QList<qint64> ended;
    for (qint64 tid : tabOrder_) {
        if (reload)
            loadTabClock(tid);
        if (tabMeta_.value(tid).round.terminated)
            ended << tid;       // ended early (here or in another window): no longer an active tournament
        else
            showTab(tid);
    }
    for (qint64 tid : ended)
        removeTournamentTab(tid);
}

void MainWindow::refreshLivePill()
{
    if (tabOrder_.size() == 1)
        livePill->setText(QStringLiteral("●  1 tournament — Live"));
    else if (!tabOrder_.isEmpty())
        livePill->setText(QStringLiteral("●  %1 tournaments — Live").arg(tabOrder_.size()));
    fitChrome();
}

void MainWindow::restoreActiveTabs()
{
    const db::Rows active = tdb::activeTournaments();
    for (int i = 0; i < active.size() && i < 8; ++i) {
        const db::Row &t = active[i];
        addTournamentTab(t["tournament_id"].toLongLong(), t["game"].toString(), t["name"].toString());
    }
}

// Navigation pills: the open page has a 3px purple outline all the way round, a tint and bolder text.
void MainWindow::styleNav(QPushButton *button, bool active)
{
    const int h = qMax(T::px(33), T::MIN_HIT);
    button->setMinimumHeight(h);
    QString style = T::pillQss(active, h);
    if (!active)
        style.replace("border:" + QString::number(T::LINE) + "px solid " + T::BORDER2, "border:" + QString::number(T::LINE) + "px solid transparent");
    if (compactNav_)
        style.replace("padding:0 " + QString::number(T::px(16) - (active ? T::LINE_STRONG - T::LINE : 0)) + "px", "padding:0 6px");
    button->setStyleSheet(style);
    if (compactNav_)
        button->setIcon(QIcon());       // a very narrow window keeps the labels and gives up the icons
    else
        T::setButtonIcon(button, button->property("iconName").toString(), active ? T::TEXT : T::MUTED);
    button->setAccessibleName(button->property("label").toString() + (active ? ", current page" : ""));
}

void playAlertSound()
{
    QApplication::beep();
}
