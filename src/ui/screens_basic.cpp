// Screen routing, the home page, the game hub and tournament setup.
#include "commander_db.h"
#include "screens.h"

#include <QApplication>
#include <QBitmap>
#include <QDateTime>
#include <QDesktopServices>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QUrl>
#include <stdexcept>

QWidget *makeScreen(const QString &requested, MainWindow *mw, QVariantMap args)
{
    QString name = requested;
    static const QHash<QString, QString> hubs{{"op_hub", "ONEPIECE"}, {"poke_hub", "POKEMON"}, {"mtg_hub", "MTG"}};
    if (hubs.contains(name)) {
        if (!args.contains("game"))
            args["game"] = hubs.value(name);
        name = "hub";
    }
    const qint64 tid = args.value("tournament_id").toLongLong();
    const int roundNumber = args.value("round_number").toInt();
    // A removed player has no profile, however it is asked for (a remembered page, Back):
    // the directory is shown instead.  Their name stays, as plain text, in tournament history.
    if (name == "player_profile") {
        const db::Row player = pdb::playerById(args.value("player_id").toLongLong());
        if (player.isEmpty() || !player["deleted_at"].isNull())
            name = "players";
    }

    // Commander events have their own multiplayer screens
    if (tid && (name == "registration" || name == "round" || name == "standings" || name == "round_select")
        && cdb::isCommander(tid)) {
        if (name == "registration") {
            auto *s = new CommanderRegistrationScreen(mw, tid);
            s->setup();
            return s;
        }
        if (name == "standings")
            return new CommanderStandingsScreen(mw, tid);
        return new CommanderEventScreen(mw, tid, roundNumber);
    }
    if (name == "home")
        return new HomeScreen(mw);
    if (name == "hub")
        return new HubScreen(mw, args.value("game", "ONEPIECE").toString());
    if (name == "tournament_setup")
        return new TournamentSetupScreen(mw, args.value("game").toString());
    if (name == "registration") {
        auto *s = new RegistrationScreen(mw, tid);
        s->setup();
        return s;
    }
    if (name == "round")
        return new RoundScreen(mw, tid, roundNumber);
    if (name == "standings")
        return new StandingsScreen(mw, tid);
    if (name == "players")
        return new PlayersScreen(mw);
    if (name == "player_profile")
        return new PlayerProfileScreen(mw, args.value("player_id").toLongLong());
    if (name == "round_select")
        return new RoundSelectScreen(mw, tid);
    throw std::invalid_argument(("Unknown screen: " + requested).toStdString());
}

const QString HomeScreen::DESCRIPTION =
    "Organize trading card tournaments, manage players, and keep every round running smoothly. "
    "Track pairings, standings, and results for formats including MTG Modern and Commander.";

static const int CARD_W = 340, BANNER_H = 170;
static const int CLOCK_REREAD_TICKS = 5;     // hub countdowns re-read their saved clock every five seconds

// The logos are dark-on-white; turn the dark parts into `color` on transparent.
static QPixmap tintedLogo(const QString &path, const QString &color, int maxW, int maxH)
{
    QImage src(path);
    if (src.isNull())
        return {};
    if (src.width() > 800)
        src = src.scaledToWidth(800, Qt::SmoothTransformation);
    QImage flat(src.size(), QImage::Format_RGB32);
    flat.fill(QColor("#ffffff"));
    {
        QPainter p(&flat);
        p.drawImage(0, 0, src);
    }
    QImage mask = flat.convertToFormat(QImage::Format_Grayscale8);
    mask.invertPixels();
    QImage out(src.size(), QImage::Format_ARGB32);
    out.fill(QColor(color));
    out.setAlphaChannel(mask);
    QPixmap px = QPixmap::fromImage(out);
    const QRect box = QRegion(px.mask()).boundingRect();
    if (!box.isEmpty())
        px = px.copy(box);
    return px.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

static QPixmap bannerPixmap(const QString &image, const QString &color)
{
    const int w = CARD_W - 6, h = BANNER_H;
    const QScreen *screen = QApplication::primaryScreen();
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    QPixmap px(int(w * dpr), int(h * dpr));
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, w, h + T::RADIUS), T::RADIUS - 1, T::RADIUS - 1);     // round the top corners only
    p.setClipPath(clip);
    // a deep wash of the game's colour over the surface
    QColor tint(color);
    tint.setAlpha(T::MODE == "dark" ? 30 : 40);
    p.fillRect(QRectF(0, 0, w, h), QColor(T::MODE == "dark" ? T::BG : T::SURFACE3));
    p.fillRect(QRectF(0, 0, w, h), tint);
    const QPixmap logo = tintedLogo(":/assets/" + image, color, int(230 * dpr), int(100 * dpr));
    if (!logo.isNull()) {
        const qreal lw = logo.width() / dpr, lh = logo.height() / dpr;
        p.drawPixmap(QRectF((w - lw) / 2, (h - lh) / 2, lw, lh), logo, QRectF(logo.rect()));
    }
    return px;
}

HomeScreen::HomeScreen(MainWindow *mw) : mw_(mw)
{
    QVBoxLayout *body = nullptr;
    root->addWidget(page(&body));

    body->addSpacing(34);
    auto *badge = new QLabel(QStringLiteral("v%1").arg(prefs::VERSION));
    badge->setFont(T::font(12, 500));
    badge->setStyleSheet("color:" + T::PURPLE_LT + ";background:" + T::PURPLE_DIM + ";border:1px solid " + T::PURPLE_BDR
                         + ";border-radius:" + T::P(11) + "px;padding:" + T::P(4) + "px " + T::P(14) + "px;");
    body->addWidget(badge, 0, Qt::AlignHCenter);
    body->addSpacing(22);

    QLabel *title = T::lbl("TCG Tournament Manager", T::TEXT, 34, 700, true);
    title->setAlignment(Qt::AlignHCenter);
    body->addWidget(title);
    body->addSpacing(10);
    QLabel *sub = T::lbl(QStringLiteral("Swiss-format tournament management for One Piece, Pokémon, and MTG"),
                         T::MUTED, 15, 400, true);
    sub->setAlignment(Qt::AlignHCenter);
    body->addWidget(sub);
    body->addSpacing(40);

    // game cards wrap onto more rows as the window narrows
    auto *cards = new QWidget;
    auto *flow = new T::FlowLayout(cards, 24, true);
    flow->addWidget(card("ONEPIECE", "optcg_logo.png", "One Piece TCG", "One Piece", "Standard Format", "op_hub"));
    flow->addWidget(card("POKEMON", "pokemon.png", QStringLiteral("Pokémon TCG"), QStringLiteral("Pokémon"), "Standard Format", "poke_hub"));
    flow->addWidget(card("MTG", "mtg.png", "Magic: The Gathering", "Magic: The Gathering",
                         QStringLiteral("Modern · Commander"), "mtg_hub"));
    body->addWidget(cards);
    body->addStretch();
    body->addSpacing(40);

    body->addWidget(T::hline());
    body->addSpacing(16);
    QLabel *about = T::lbl(DESCRIPTION, T::MUTED, 13, 400, true);
    about->setAlignment(Qt::AlignHCenter);
    body->addWidget(about);
    // the bug-report link and the credit are in the footer every page shares (T::pageFooter)
}

QWidget *HomeScreen::card(const QString &game, const QString &image, const QString &tag, const QString &name,
                          const QString &subtitle, const QString &screen)
{
    const QString color = T::gameColor(game);
    auto *c = new T::ClickFrame("gc");
    c->setMaximumWidth(CARD_W);         // shrinks a little in a very narrow window instead of being cut off
    c->setMinimumWidth(240);
    c->setAccessibleName(name + " hub");
    c->setStyleSheet("#gc{background:" + T::SURFACE2 + ";border:" + QString::number(T::LINE) + "px solid " + T::BORDER2
                     + ";border-radius:" + QString::number(T::RADIUS) + "px;}"
                     "#gc:hover{border-color:" + color + ";}"
                     "#gc:focus{border:" + QString::number(T::LINE_STRONG) + "px solid " + T::FOCUS + ";}");
    connect(c, &T::ClickFrame::clicked, this, [this, screen] { mw_->navigateTo(screen); });

    auto *vl = new QVBoxLayout(c);
    vl->setContentsMargins(3, 3, 3, 0);
    vl->setSpacing(0);
    auto *banner = new QLabel;
    banner->setFixedHeight(BANNER_H);
    banner->setMinimumWidth(1);
    banner->setScaledContents(true);
    banner->setStyleSheet("background:transparent;border:none;");
    banner->setPixmap(bannerPixmap(image, color));
    vl->addWidget(banner);

    auto *bl = new QVBoxLayout;
    bl->setContentsMargins(20, 18, 20, 20);
    bl->setSpacing(0);
    auto *gtag = new QLabel(tag.toUpper());
    gtag->setFont(T::font(10, 700, false, 0.8));
    gtag->setStyleSheet("color:" + color + ";background:" + T::alpha(color, 26) + ";border:1px solid " + T::alpha(color, 150)
                        + ";border-radius:" + QString::number((T::px(10) + 10) / 2) + "px;padding:2px 10px;");
    bl->addWidget(gtag, 0, Qt::AlignLeft);
    bl->addSpacing(12);
    bl->addWidget(T::lbl(name, T::TEXT, 16, 700, true));
    bl->addSpacing(4);
    bl->addWidget(T::lbl(subtitle, T::MUTED, 12, 400, true));
    bl->addSpacing(18);
    QPushButton *cta = T::button("Open Hub", "ghost", "arrow-right", true);
    connect(cta, &QPushButton::clicked, this, [this, screen] { mw_->navigateTo(screen); });
    bl->addWidget(cta);
    vl->addLayout(bl);
    return c;
}

HubScreen::HubScreen(MainWindow *mw, const QString &game) : mw_(mw), game_(game)
{
    build();
    load();
    // countdowns are recomputed from the saved timestamps; the saved state is re-read every few seconds
    clockTimer_.setInterval(1000);
    connect(&clockTimer_, &QTimer::timeout, this, [this] {
        ++ticks_;
        for (Clock &c : clocks_) {
            if (ticks_ % CLOCK_REREAD_TICKS == 0)
                c.reading = timerdb::read(c.kind, c.roundId);
            showClock(c);
        }
    });
    clockTimer_.start();
}

// The row clocks only tick while the hub is the page being shown.
void HubScreen::hideEvent(QHideEvent *e)
{
    T::Screen::hideEvent(e);
    clockTimer_.stop();
}

void HubScreen::showEvent(QShowEvent *e)
{
    T::Screen::showEvent(e);
    clockTimer_.start();
}

void HubScreen::build()
{
    QVBoxLayout *body = nullptr;
    root->addWidget(page(&body));      // the whole hub scrolls, so a short window never hides anything

    QBoxLayout *cols = flip(T::row(28));
    auto *left = new QVBoxLayout;
    left->setSpacing(0);

    QBoxLayout *top = flip(T::row(14), true);
    auto *tv = new QVBoxLayout;
    tv->setSpacing(6);
    tv->addWidget(T::lbl("Tournaments", T::TEXT, 24, 700, true));
    tv->addWidget(T::lbl(T::gameLabel(game_) + QStringLiteral("  ·  Most recent first"), T::MUTED, 13, 400, true));
    top->addLayout(tv, 1);
    QPushButton *create = T::button("New Tournament", "primary", "plus");
    connect(create, &QPushButton::clicked, this, [this] { mw_->navigateTo("tournament_setup", {{"game", game_}}); });
    top->addWidget(create, 0, Qt::AlignTop);
    left->addLayout(top);
    left->addSpacing(28);

    left->addWidget(T::caps("Active"));
    left->addSpacing(12);
    activeBox_ = new QVBoxLayout;
    activeBox_->setSpacing(10);
    left->addLayout(activeBox_);
    left->addSpacing(26);
    left->addWidget(T::caps("History"));
    left->addSpacing(12);
    doneBox_ = new QVBoxLayout;
    doneBox_->setSpacing(10);
    left->addLayout(doneBox_);
    left->addStretch();
    cols->addLayout(left, 3);

    auto *side = new QVBoxLayout;
    side->setSpacing(16);
    side->setContentsMargins(0, 0, 0, 0);
    statsBox_ = statCard(side, "Club stats");
    topBox_ = statCard(side, "Top players");
    side->addStretch();
    side_ = new QWidget;
    side_->setLayout(side);
    cols->addWidget(side_, 1);
    body->addLayout(cols);
    body->addStretch();
    reflow();
}

void HubScreen::reflow()
{
    if (!side_)
        return;
    side_->setMaximumWidth(narrow ? QWIDGETSIZE_MAX : 340);
    side_->setMinimumWidth(narrow ? 0 : 260);
}

QVBoxLayout *HubScreen::statCard(QVBoxLayout *parent, const QString &title)
{
    QFrame *card = T::panel("sw");
    auto *vb = new QVBoxLayout(card);
    vb->setContentsMargins(22, 20, 22, 12);
    vb->setSpacing(0);
    vb->addWidget(T::caps(title));
    vb->addSpacing(8);
    auto *rows = new QVBoxLayout;
    rows->setSpacing(0);
    vb->addLayout(rows);
    parent->addWidget(card);
    return rows;
}

void HubScreen::statRows(QVBoxLayout *box, const QList<std::tuple<QString, QString, QString>> &items)
{
    for (int i = 0; i < items.size(); ++i) {
        if (i)
            box->addWidget(T::hline());
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 11, 0, 11);
        rl->addWidget(T::lbl(std::get<0>(items[i]), T::MUTED, 13, 400, true), 1);
        rl->addWidget(T::lbl(std::get<1>(items[i]), std::get<2>(items[i]), 14, 700));
        box->addWidget(row);
    }
}

void HubScreen::load()
{
    const db::Rows all = tdb::tournamentsByGame(game_);
    db::Rows active, pending, done;
    for (const db::Row &t : all) {
        const QString status = t["status"].toString();
        if (status == "IN_PROGRESS")
            active << t;
        else if (status == "PENDING")
            pending << t;
        else if (status == "COMPLETED" || status == tdb::TERMINATED)
            done << t;          // history: finished, or ended early and marked "Terminated"
    }

    clocks_.clear();
    T::clear(activeBox_);
    for (const db::Row &t : active + pending)
        activeBox_->addWidget(tournamentRow(t));
    if (active.isEmpty() && pending.isEmpty())
        activeBox_->addWidget(T::lbl("No active tournaments", T::MUTED, 13));

    T::clear(doneBox_);
    for (const db::Row &t : done)
        doneBox_->addWidget(tournamentRow(t));
    if (done.isEmpty())
        doneBox_->addWidget(T::lbl("No finished tournaments yet", T::MUTED, 13));

    const tdb::GameStats stats = tdb::gameStats(game_);
    const db::Rows &top = stats.topPlayers;

    T::clear(statsBox_);
    statRows(statsBox_, {
        {"Total Tournaments", QString::number(all.size()), T::TEXT},
        {"Total Players", QString::number(stats.players), T::TEXT},
        {"Active Now", QString::number(active.size()), active.isEmpty() ? T::TEXT : T::LIVE},
        {"Matches Played", QString::number(stats.gamesPlayed), T::TEXT},
    });
    T::clear(topBox_);
    QList<std::tuple<QString, QString, QString>> leaders;
    for (int i = 0; i < top.size(); ++i)
        leaders.append({top[i]["display_name"].toString(), top[i]["wins"].toString() + "W", i == 0 ? T::GOLD : T::TEXT});
    if (leaders.isEmpty())
        leaders.append({"No results yet", "", T::TEXT});
    statRows(topBox_, leaders);
}

// A tournament: a card with a coloured left edge, its name and details, and its status on the right.
QWidget *HubScreen::tournamentRow(const db::Row &t)
{
    const QString status = t["status"].toString();
    const qint64 tid = t["tournament_id"].toLongLong();
    const QString name = t["name"].toString();
    const bool commander = t["format"].toString() == cdb::FORMAT;
    const bool inProgress = status == "IN_PROGRESS";
    const bool terminated = status == tdb::TERMINATED;
    const live::RoundStatus round = inProgress || terminated ? live::roundStatus(tid) : live::RoundStatus();
    const QString accent = round.playing ? T::LIVE : T::BORDER2;

    auto *row = new T::ClickFrame("tr");
    row->setAccessibleName(name + ", " + (inProgress ? QString("in progress") : status.toLower()));
    row->setStyleSheet("#tr{background:" + T::SURFACE2 + ";border:" + QString::number(T::LINE_STRONG) + "px solid " + accent
                       + ";border-radius:" + QString::number(T::RADIUS) + "px;}"
                       "#tr:hover{background:" + T::SURFACE + ";}"
                       "#tr:focus{border-color:" + T::FOCUS + ";}");
    QBoxLayout *hl = flip(T::row(14), true);
    hl->setContentsMargins(22, 16, 20, 16);
    row->setLayout(hl);

    auto *info = new QVBoxLayout;
    info->setSpacing(5);
    info->addWidget(T::lbl(name, T::TEXT, 15, 700, true));
    const int total = t["total_rounds"].toInt();
    QString prog;
    if (inProgress && commander) {
        prog = cdb::progress(tid)["long"].toString();
    } else if (inProgress) {
        prog = round.playing ? QStringLiteral("Round %1 of %2").arg(round.roundNumber).arg(total) : QString("Not started");
    } else if (status == "PENDING") {
        prog = "Not started";
    } else if (terminated) {
        prog = round.roundId ? QStringLiteral("Ended early in round %1 of %2").arg(round.roundNumber).arg(total)
                             : QString("Ended early before round 1");
    } else {
        prog = QStringLiteral("%1 round%2").arg(total).arg(total == 1 ? "" : "s");
    }
    const int n = t["player_count"].toInt();
    QStringList parts{T::gameShort(game_)};
    if (!t["format"].toString().isEmpty())
        parts << t["format"].toString();
    parts << QStringLiteral("%1 player%2").arg(n).arg(n == 1 ? "" : "s") << prog;
    info->addWidget(T::lbl(parts.join(QStringLiteral("  ·  ")), T::MUTED, 12, 400, true));
    hl->addLayout(info, 1);

    // status on the right: the pill, with the round clock above it while a round is being played
    auto *state = new QWidget;
    auto *sl = new QVBoxLayout(state);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(6);
    if (round.playing) {
        // read from the timestamps saved with the round
        Clock c;
        c.value = T::lbl("--:--", T::TEXT, 14, 600, false, true);
        c.caption = T::lbl("", T::MUTED, 12);
        c.icon = T::iconLabel("timer", T::MUTED, 15);
        c.kind = round.kind;
        c.roundId = round.roundId;
        c.reading = timerdb::read(c.kind, c.roundId);
        showClock(c);
        clocks_.append(c);
        auto *clock = new QHBoxLayout;
        clock->setSpacing(8);
        clock->addWidget(c.icon);
        clock->addWidget(c.value);
        clock->addWidget(c.caption);
        clock->addStretch();
        sl->addLayout(clock);
        sl->addWidget(T::pill("Round in progress", T::LIVE), 0, Qt::AlignLeft);
    } else if (inProgress) {
        sl->addWidget(T::pill(round.needsFinish ? "Needs finishing" : "Between rounds", T::PURPLE_LT),
                      0, Qt::AlignLeft);
    } else if (status == "PENDING") {
        sl->addWidget(T::pill("Not started", T::PURPLE_LT), 0, Qt::AlignLeft);
    } else if (terminated) {
        sl->addWidget(T::pill("Terminated", T::RED), 0, Qt::AlignLeft);
    } else {
        sl->addWidget(T::pill("Completed", T::GREY), 0, Qt::AlignLeft);
    }
    hl->addWidget(state, 0, Qt::AlignVCenter);

    const QString target = status == "PENDING" ? "registration"
                           : status == "COMPLETED" || terminated ? "round_select" : "round";
    const QString game = game_;
    MainWindow *mw = mw_;
    connect(row, &T::ClickFrame::clicked, this, [mw, target, tid, name, game] {
        mw->navigateTo(target, {{"tournament_id", tid}, {"tournament_name", name}, {"game", game}});
    });
    return row;
}

void HubScreen::showClock(const Clock &c)
{
    if (!c.reading.valid() || !c.value || !c.caption)
        return;
    const int left = c.reading.secondsLeft();
    const bool running = c.reading.state() == timerdb::State::Running;
    c.value->setText(timerdb::clockText(left));       // 00:00 once the time is up
    c.caption->setText(left <= 0 ? "Time expired" : running ? "remaining"
                       : c.reading.state() == timerdb::State::Paused ? "clock paused" : "clock not started");
    // green while counting down, red at zero, neutral otherwise; the caption says it in words too
    const QString color = left <= 0 ? T::RED : running ? T::GREEN : T::MUTED;
    if (c.value->property("clockColor").toString() == color)
        return;                 // the colours change with the clock's state, not on every tick
    c.value->setProperty("clockColor", color);
    c.value->setStyleSheet("color:" + (left > 0 && !running ? T::TEXT : color) + ";background:transparent;border:none;");
    c.caption->setStyleSheet("color:" + color + ";background:transparent;border:none;");
    if (c.icon)
        c.icon->setPixmap(T::iconPixmap("timer", color, 15));
}

TournamentSetupScreen::TournamentSetupScreen(MainWindow *mw, const QString &initialGame) : mw_(mw)
{
    QVBoxLayout *body = nullptr;
    root->addWidget(page(&body), 1);

    body->addWidget(T::lbl(initialGame.isEmpty() ? QString("New Tournament")
                                                 : QStringLiteral("New %1 Tournament").arg(T::gameLabel(initialGame)),
                           T::TEXT, 24, 700, true));
    body->addSpacing(6);
    body->addWidget(T::lbl("Fill in the details below to set up your tournament.", T::MUTED, 13, 400, true));
    body->addSpacing(22);

    QFrame *card = T::panel("form");
    auto *form = new QVBoxLayout(card);
    form->setContentsMargins(24, 22, 24, 22);
    form->setSpacing(7);

    auto field = [](const QString &label, QWidget *widget) {
        auto *col = new QVBoxLayout;
        col->setSpacing(7);                 // a label sits close to its own field, and clear of the one above
        col->addWidget(T::caps(label));
        col->addWidget(widget);
        return col;
    };

    form->addWidget(T::caps("Tournament name"));
    nameInput = new QLineEdit;
    nameInput->setPlaceholderText("e.g. Friday Night Commander");
    nameInput->setAccessibleName("Tournament name");
    nameInput->setMaxLength(tdb::MAX_NAME_LENGTH);
    form->addWidget(nameInput);
    form->addSpacing(12);

    // paired fields sit side by side when there is room and stack when there is not
    QBoxLayout *r0 = flip(T::row(14));
    gameCombo = T::combo();
    gameCombo->addItem("Magic: The Gathering", "MTG");
    gameCombo->addItem(QStringLiteral("Pokémon"), "POKEMON");
    gameCombo->addItem("One Piece", "ONEPIECE");
    gameCombo->setCurrentIndex(qMax(0, gameCombo->findData(initialGame)));
    gameCombo->setAccessibleName("Game");
    if (initialGame.isEmpty())
        r0->addLayout(field("Game", gameCombo), 1);
    else
        gameCombo->setParent(this), gameCombo->hide();      // chosen by the hub this screen was opened from
    formatCombo = T::combo();
    formatCombo->setAccessibleName("Format");
    r0->addLayout(field("Format", formatCombo), 1);
    form->addLayout(r0);
    form->addSpacing(12);

    QBoxLayout *r1 = flip(T::row(14));
    playerCount = new QSpinBox;
    playerCount->setRange(2, 256);
    playerCount->setValue(8);
    playerCount->setAccessibleName("Expected number of players");
    r1->addLayout(field("Expected players", playerCount), 1);
    roundsSpin = new QSpinBox;
    roundsSpin->setRange(1, 15);
    roundsSpin->setValue(3);
    roundsSpin->setAccessibleName("Number of rounds");
    r1->addLayout(field("Rounds", roundsSpin), 1);
    // Round length: only the number goes in the field; the unit sits just outside it.  Anything
    // can be typed while editing (including nothing); it is checked when the tournament is created.
    minutesEdit = new QLineEdit;
    minutesEdit->setAccessibleName("Round length");
    minutesEdit->setAccessibleDescription(QStringLiteral("Minutes per round, a whole number from %1 to %2")
                                              .arg(tdb::MIN_ROUND_MINUTES).arg(tdb::MAX_ROUND_MINUTES));
    minutesEdit->setToolTip(QStringLiteral("Minutes per round (%1 to %2)").arg(tdb::MIN_ROUND_MINUTES).arg(tdb::MAX_ROUND_MINUTES));
    minutesEdit->setMaxLength(4);
    minutesEdit->setMinimumWidth(T::px(64));
    auto *lengthCol = new QVBoxLayout;
    lengthCol->setSpacing(7);
    lengthCol->addWidget(T::caps("Round length"));
    auto *lengthRow = new QHBoxLayout;
    lengthRow->setSpacing(8);
    lengthRow->addWidget(minutesEdit, 1);
    minutesUnit = T::lbl("min", T::MUTED, 14);
    lengthRow->addWidget(minutesUnit);
    lengthCol->addLayout(lengthRow);
    r1->addLayout(lengthCol, 1);
    form->addLayout(r1);
    form->addSpacing(8);

    suggestionLabel = T::lbl("", T::PURPLE_LT, 12, 400, true);
    form->addWidget(suggestionLabel);
    // Commander only: what the pods will look like
    commanderNote = T::lbl("", T::MUTED, 12, 400, true);
    form->addWidget(commanderNote);
    body->addWidget(card);
    body->addStretch();

    root->addSpacing(12);
    auto *bottom = new QWidget;
    auto *bl = new T::FlowLayout(bottom, 10);
    QPushButton *back = T::button("Back", "ghost", "arrow-left");
    connect(back, &QPushButton::clicked, this, [this] { mw_->navigateTo("hub", {{"game", game()}}); });
    bl->addWidget(back);
    QPushButton *create = T::button("Create && register players", "primary", "arrow-right", true);
    connect(create, &QPushButton::clicked, this, [this] { createTournament(); });
    bl->addWidget(create);
    root->addWidget(bottom);

    auto formatChanged = [this] {
        const bool commander = isCommander();
        commanderNote->setVisible(commander);
        playerCount->setRange(commander ? 3 : 2, commander ? 2000 : 256);
        updateSuggestedRounds(playerCount->value());
    };
    // the formats on offer, and the suggested round length, follow the game
    auto gameChanged = [this] {
        const QSignalBlocker block(formatCombo);
        formatCombo->clear();
        formatCombo->addItems(game() == "MTG" ? QStringList{"Modern", "Commander"} : QStringList{"Standard"});
        minutesEdit->setText(QString::number(prefs::defaultRoundMinutes(game())));
    };
    connect(playerCount, &QSpinBox::valueChanged, this, [this](int n) { updateSuggestedRounds(n); });
    connect(formatCombo, &QComboBox::currentTextChanged, this, formatChanged);
    connect(gameCombo, &QComboBox::currentIndexChanged, this, [gameChanged, formatChanged] {
        gameChanged();
        formatChanged();
    });
    gameChanged();
    formatChanged();
}

QString TournamentSetupScreen::game() const
{
    return gameCombo->currentData().toString();
}

bool TournamentSetupScreen::isCommander() const
{
    return formatCombo->currentText() == cdb::FORMAT;
}

void TournamentSetupScreen::updateSuggestedRounds(int players)
{
    if (!isCommander()) {
        roundsSpin->setValue(swiss::suggestedRounds(players));
        suggestionLabel->setText(QStringLiteral("%1 rounds suggested for %2 players.").arg(roundsSpin->value()).arg(players));
        return;
    }
    const QVariantMap rec = cdb::recommendation(players);
    roundsSpin->setValue(rec["rounds"].toInt());
    suggestionLabel->setText(QStringLiteral("%1 rounds recommended for %2 players.").arg(rec["rounds"].toInt()).arg(players));
    QStringList pods;
    for (const QVariant &p : rec["pods"].toList())
        pods << p.toString();
    commanderNote->setText(
        QStringLiteral("Pods for %1 players: %2%3. The number of rounds is the whole tournament: it ends after the last "
                       "round and the standings are final. Rounds are re-recommended from the checked-in count at "
                       "registration and freeze when the event starts.")
            .arg(players).arg(pods.join(" + "), rec["byes"].toInt() ? " and 1 bye" : ""));
}

void TournamentSetupScreen::createTournament()
{
    const QString name = nameInput->text().trimmed();
    if (name.isEmpty()) {
        warn(this, "Missing Name", "Please enter a tournament name.");
        nameInput->setFocus();
        return;
    }
    lastError.clear();
    bool isNumber = false;
    const int minutes = minutesEdit->text().trimmed().toInt(&isNumber);
    if (!isNumber || !tdb::isValidRoundMinutes(minutes)) {
        lastError = QStringLiteral("Round length must be a whole number of minutes from %1 to %2.")
                        .arg(tdb::MIN_ROUND_MINUTES).arg(tdb::MAX_ROUND_MINUTES);
        warn(this, "Round length", lastError);
        minutesEdit->setFocus();
        minutesEdit->selectAll();
        return;
    }
    const int rounds = roundsSpin->value();
    qint64 tid = 0;
    try {
        if (isCommander()) {
            // only store an override when the organizer changed the suggested values
            const int n = playerCount->value();
            const int recommended = cdb::recommendation(n)["rounds"].toInt();
            tid = cdb::createCommanderTournament(name, n, rounds == recommended ? 0 : rounds, {}, -1, {}, minutes);
        } else {
            tid = tdb::createTournament(name, game(), rounds, formatCombo->currentText(), {}, minutes);
        }
    } catch (const std::exception &e) {
        warn(this, "Error Creating Tournament", QString::fromUtf8(e.what()));
        return;
    }
    mw_->navigateTo("registration", {{"tournament_id", tid}, {"tournament_name", name}});
}
