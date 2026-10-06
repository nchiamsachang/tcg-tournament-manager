// Registration, one-on-one rounds and standings, round review, and the player screens.
#include "commander_db.h"
#include "printing.h"
#include "screens.h"

#include <QFile>
#include <QFileDialog>
#include <QTextStream>

RegistrationScreen::RegistrationScreen(MainWindow *mw, qint64 tid) : tournamentId(tid), mw_(mw)
{
    const db::Row t = tdb::tournamentById(tid);
    game = t.isEmpty() ? QString("ONEPIECE") : t["game"].toString();
    tName = t["name"].toString();
}

void RegistrationScreen::setup()
{
    buildUi();
    refreshEnrolled();
}

void RegistrationScreen::buildUi()
{
    // Everything except the action bar scrolls, so Start stays reachable in a short window.
    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body), 1);

    QBoxLayout *hdr = flip(T::row(12));
    hdr->addWidget(T::lbl(QStringLiteral("Player Registration — ") + tName, T::TEXT, 24, 700, true), 1);
    enrolledCountLabel = T::lbl("0 players enrolled", T::MUTED, 14);
    hdr->addWidget(enrolledCountLabel);
    body->addLayout(hdr);
    body->addSpacing(16);

    // search / add: side by side when there is room, stacked when there is not
    QBoxLayout *searchRow = flip(T::row(10));
    searchBox = new QLineEdit;
    searchBox->setPlaceholderText("Search existing players...");
    searchBox->setAccessibleName("Search existing players");
    searchRow->addWidget(searchBox, 3);
    newNameBox = new QLineEdit;
    newNameBox->setPlaceholderText("New player name...");
    newNameBox->setAccessibleName("New player name");
    connect(newNameBox, &QLineEdit::returnPressed, this, [this] { addAndEnroll(); });
    searchRow->addWidget(newNameBox, 2);
    addBtn = T::button("+ Add && enroll", "primary");
    connect(addBtn, &QPushButton::clicked, this, [this] { addAndEnroll(); });
    searchRow->addWidget(addBtn);
    body->addLayout(searchRow);
    body->addSpacing(10);

    searchResultsFrame_ = T::panel("srf", {}, 1, 16, T::SURFACE);
    searchResultsLayout_ = new QVBoxLayout(searchResultsFrame_);
    searchResultsLayout_->setContentsMargins(10, 10, 10, 10);
    searchResultsLayout_->setSpacing(6);
    searchResultsFrame_->hide();
    body->addWidget(searchResultsFrame_);
    body->addSpacing(10);

    body->addWidget(T::caps("Enrolled players"));
    body->addSpacing(8);
    enrolledLayout = new QVBoxLayout;
    enrolledLayout->setSpacing(8);
    body->addLayout(enrolledLayout);
    body->addSpacing(14);
    extraSlot = new QVBoxLayout;            // format-specific setup (Commander) goes here
    body->addLayout(extraSlot);
    body->addStretch();

    root->addSpacing(12);
    auto *bottom = new QWidget;
    auto *bl = new T::FlowLayout(bottom, 10);
    QPushButton *back = T::button(QStringLiteral("← Back"), "ghost");
    connect(back, &QPushButton::clicked, this, [this] { mw_->navigateTo("hub", {{"game", game}}); });
    bl->addWidget(back);
    startBtn = T::button(QStringLiteral("Start tournament  →"), "primary");
    connect(startBtn, &QPushButton::clicked, this, [this] { confirmStart(); });
    bl->addWidget(startBtn);
    root->addWidget(bottom);

    searchTimer_.setSingleShot(true);
    connect(&searchTimer_, &QTimer::timeout, this, [this] { runSearch(); });
    connect(searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (!text.trimmed().isEmpty()) {
            searchTimer_.start(250);
        } else {
            searchResultsFrame_->hide();
            T::clear(searchResultsLayout_);
        }
    });
}

void RegistrationScreen::runSearch()
{
    const QString text = searchBox->text().trimmed();
    if (text.isEmpty())
        return;
    T::clear(searchResultsLayout_);
    const db::Rows results = pdb::searchPlayers(text);
    QSet<qint64> enrolled;
    for (const db::Row &p : tdb::enrolledPlayers(tournamentId))
        enrolled.insert(p["player_id"].toLongLong());
    if (results.isEmpty())
        searchResultsLayout_->addWidget(T::lbl(QStringLiteral("No players found — use Add & enroll to create one."),
                                               T::MUTED, 13, 400, true));
    for (int i = 0; i < results.size() && i < 10; ++i) {
        const qint64 pid = results[i]["player_id"].toLongLong();
        auto *row = new QFrame;
        row->setObjectName("sr");
        row->setStyleSheet("#sr{background:" + T::SURFACE2 + ";border:2px solid " + T::BORDER + ";border-radius:14px;}");
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(14, 6, 10, 6);
        hl->addWidget(T::lbl(results[i]["display_name"].toString(), T::TEXT, 14, 600, true), 1);
        if (enrolled.contains(pid)) {
            hl->addWidget(T::pill("Enrolled", T::GREEN, false));
        } else {
            QPushButton *btn = T::button("Enroll");
            btn->setAccessibleName("Enroll " + results[i]["display_name"].toString());
            connect(btn, &QPushButton::clicked, this, [this, pid] { enrollExisting(pid); });
            hl->addWidget(btn);
        }
        searchResultsLayout_->addWidget(row);
    }
    searchResultsFrame_->show();
}

void RegistrationScreen::enrollExisting(qint64 playerId)
{
    tdb::enrollPlayer(tournamentId, playerId);
    refreshEnrolled();
    runSearch();
}

void RegistrationScreen::addAndEnroll()
{
    const QString name = newNameBox->text().trimmed();
    if (name.isEmpty()) {
        warn(this, "Missing Name", "Enter a player name first.");
        return;
    }
    for (const db::Row &p : pdb::searchPlayers(name)) {
        if (p["display_name"].toString().compare(name, Qt::CaseInsensitive) == 0) {
            if (confirm(this, "Player Exists", p["display_name"].toString() + " already exists. Enroll them?"))
                enrollExisting(p["player_id"].toLongLong());
            return;
        }
    }
    try {
        tdb::enrollPlayer(tournamentId, pdb::addPlayer(name));
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        return;
    }
    newNameBox->clear();
    refreshEnrolled();
}

void RegistrationScreen::unenroll(qint64 playerId, const QString &name)
{
    if (confirm(this, "Remove Player", "Remove " + name + " from this tournament?")) {
        tdb::unenrollPlayer(tournamentId, playerId);
        refreshEnrolled();
    }
}

void RegistrationScreen::refreshEnrolled()
{
    T::ScrollKeeper keep(this);         // the page stays where it is while the list is rebuilt
    T::clear(enrolledLayout);
    const db::Rows players = tdb::enrolledPlayers(tournamentId);
    const int count = int(players.size());
    enrolledCountLabel->setText(QStringLiteral("%1 player%2 enrolled").arg(count).arg(count == 1 ? "" : "s"));
    for (const db::Row &p : players) {
        const qint64 pid = p["player_id"].toLongLong();
        const QString name = p["display_name"].toString();
        QPushButton *rm = T::button("Remove", "danger");
        rm->setAccessibleName("Remove " + name);
        connect(rm, &QPushButton::clicked, this, [this, pid, name] { unenroll(pid, name); });
        enrolledLayout->addWidget(playerRow(name, {rm}));
    }
    if (players.isEmpty())
        enrolledLayout->addWidget(T::lbl("Nobody enrolled yet.", T::MUTED, 13));
    startBtn->setEnabled(count >= 2);
    startBtn->setToolTip(count >= 2 ? "" : "Need at least 2 players");
}

// A rounded row: wrapping name, then its buttons (stacked under the name when very narrow).
QWidget *RegistrationScreen::playerRow(const QString &name, const QList<QPushButton *> &buttons, bool dim)
{
    auto *row = new QFrame;
    row->setObjectName("er");
    row->setStyleSheet("#er{background:" + T::SURFACE2 + ";border:2px solid " + T::BORDER2 + ";border-radius:16px;}");
    QBoxLayout *hl = flip(T::row(8), true);
    hl->setContentsMargins(16, 8, 10, 8);
    row->setLayout(hl);
    hl->addWidget(T::lbl(name, dim ? T::MUTED : T::TEXT, 14, 600, true), 1);
    auto *acts = new QWidget;
    auto *al = new T::FlowLayout(acts, 8);
    for (QPushButton *b : buttons)
        al->addWidget(b);
    hl->addWidget(acts);
    return row;
}

void RegistrationScreen::confirmStart()
{
    const int count = int(tdb::enrolledPlayers(tournamentId).size());
    if (!confirm(this, "Start Tournament",
                 QStringLiteral("Start %1 with %2 players?\n\nThis will generate Round 1 pairings.").arg(tName).arg(count)))
        return;
    try {
        swiss::startTournament(tournamentId);
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        return;
    }
    mw_->navigateTo("round", {{"tournament_id", tournamentId}, {"tournament_name", tName}});
}

void CommanderRegistrationScreen::buildUi()
{
    RegistrationScreen::buildUi();
    QFrame *panel = T::panel("cfg");
    auto *pv = new QVBoxLayout(panel);
    pv->setContentsMargins(18, 14, 18, 14);
    pv->setSpacing(8);
    pv->addWidget(T::caps("Commander setup"));
    recLabel = T::lbl("", T::TEXT, 13, 400, true);
    pv->addWidget(recLabel);

    roundsSpin = new QSpinBox;
    roundsSpin->setRange(1, 15);
    roundsSpin->setAccessibleName("Number of rounds");
    roundsSpin->setToolTip("The total number of rounds. The tournament ends after the last one.");
    drawCombo = T::combo();
    drawCombo->addItem("Eliminated players lose", "ELIMINATED_LOSE");
    drawCombo->addItem("Whole pod draws", "ALL_DRAW");
    drawCombo->setAccessibleName("Draws");
    shortCombo = T::combo();
    shortCombo->addItem("Lowest standings", "LOW");
    shortCombo->addItem("Highest standings", "HIGH");
    shortCombo->addItem("Best points match", "BEST_MATCH");
    shortCombo->setAccessibleName("Three-player pods");
    shortCombo->setToolTip("Which players sit in three-player pods after round one.\n"
                           "Lowest standings keeps the leaders in four-player pods.");

    // One line per setting: the control with its label, and a short explanation beside it
    // (directly below it when the window is narrow).
    auto addSetting = [&](const QString &label, QWidget *control, QLabel **hint) {
        QBoxLayout *line = flip(T::row(16));
        auto *col = new QVBoxLayout;
        col->setSpacing(4);
        col->addWidget(T::caps(label, {}, 10));
        control->setMinimumWidth(T::px(150));
        control->setMaximumWidth(T::px(240));
        col->addWidget(control);
        line->addLayout(col);
        *hint = T::lbl("", T::MUTED, 12, 400, true);
        (*hint)->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
        (*hint)->setContentsMargins(0, 0, 0, 8);
        line->addWidget(*hint, 1);
        pv->addLayout(line);
    };
    addSetting("Rounds", roundsSpin, &roundsHint);
    addSetting("Draws", drawCombo, &drawHint);
    addSetting("3-player pods", shortCombo, &shortHint);
    warnLabel = T::lbl("", T::AMBER, 12, 400, true);
    pv->addWidget(warnLabel);

    auto *actions = new QWidget;
    auto *al = new T::FlowLayout(actions, 8);
    resetBtn = T::button("Use recommended", "ghost");
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        const int n = cdb::getEvent(tournamentId)["checked_in_count"].toInt();
        cdb::updateEventConfig(tournamentId, cdb::recommendation(n)["rounds"].toInt());
        updateCounts();
    });
    al->addWidget(resetBtn);
    QPushButton *rules = T::button("Rules", "ghost");
    connect(rules, &QPushButton::clicked, this, [this] {
        showText(this, "Event rules", cdb::rulesText(cdb::getEvent(tournamentId)));
    });
    al->addWidget(rules);
    pv->addWidget(actions);
    extraSlot->addWidget(panel);

    connect(roundsSpin, &QSpinBox::valueChanged, this, [this] { configChanged(); });
    connect(drawCombo, &QComboBox::currentIndexChanged, this, [this] { configChanged(); });
    connect(shortCombo, &QComboBox::currentIndexChanged, this, [this] { configChanged(); });
}

// Shows one row's check-in state on its own button and name; nothing else is touched.
static void showCheckedIn(QPushButton *toggle, QLabel *nameLabel, const QString &name, bool in)
{
    toggle->setProperty("checkedIn", in);
    toggle->setText(in ? QStringLiteral("✓  Checked in") : QStringLiteral("Check in"));
    toggle->setStyleSheet(T::buttonQss(in ? "secondary" : "ghost", toggle->minimumHeight()));
    toggle->setAccessibleName((in ? "Undo check-in for " : "Check in ") + name);
    if (nameLabel)
        nameLabel->setStyleSheet("color:" + (in ? T::TEXT : T::MUTED) + ";background:transparent;border:none;");
}

void CommanderRegistrationScreen::refreshEnrolled()
{
    T::ScrollKeeper keep(this);         // the page stays where it is while the list is rebuilt
    T::clear(enrolledLayout);
    const db::Rows players = cdb::getRegistrations(tournamentId);
    for (const db::Row &p : players) {
        const qint64 pid = p["player_id"].toLongLong();
        const QString name = p["display_name"].toString();
        const bool in = p["checked_in"].toInt() != 0;
        // as wide as its longer label, so the button does not shift sideways when it changes
        QPushButton *toggle = T::button(QStringLiteral("✓  Checked in"), "secondary");
        toggle->setMinimumWidth(toggle->sizeHint().width());
        QPushButton *rm = T::button("Remove", "danger");
        rm->setAccessibleName("Remove " + name);
        connect(rm, &QPushButton::clicked, this, [this, pid, name] { unenroll(pid, name); });
        QWidget *row = playerRow(name, {toggle, rm}, !in);
        QLabel *nameLabel = row->findChild<QLabel *>();
        showCheckedIn(toggle, nameLabel, name, in);
        // Checking in changes this one row in place.  The list is not rebuilt, so the page does
        // not move and the button that was pressed keeps the keyboard focus.
        connect(toggle, &QPushButton::clicked, this, [this, pid, name, toggle, nameLabel] {
            const bool in = !toggle->property("checkedIn").toBool();
            try {
                cdb::setCheckedIn(tournamentId, pid, in);
            } catch (const cdb::CommanderError &e) {
                warn(this, "Check-in", e.message);
                return;
            }
            showCheckedIn(toggle, nameLabel, name, in);
            updateCounts();
        });
        enrolledLayout->addWidget(row);
    }
    if (players.isEmpty())
        enrolledLayout->addWidget(T::lbl("Nobody registered yet.", T::MUTED, 13));
    updateCounts();
}

// The header count and the setup advice, from the saved registrations.
void CommanderRegistrationScreen::updateCounts()
{
    const db::Rows players = cdb::getRegistrations(tournamentId);
    int checked = 0;
    for (const db::Row &p : players)
        checked += p["checked_in"].toInt() ? 1 : 0;
    enrolledCountLabel->setText(QStringLiteral("%1 checked in  ·  %2 registered").arg(checked).arg(players.size()));
    syncConfig(checked);
}

// A few words on what the chosen option does, taken from how pairing and scoring really work.
void CommanderRegistrationScreen::updateHints()
{
    const int n = roundsSpin->value();
    roundsHint->setText(QStringLiteral("Total rounds. The event ends after Round %1.").arg(n));
    drawHint->setText(drawCombo->currentData().toString() == "ELIMINATED_LOSE"
                          ? "Knocked-out players take a loss; survivors share the draw."
                          : "Everyone seated in the pod gets the draw point.");
    const QString placement = shortCombo->currentData().toString();
    shortHint->setText(placement == "LOW" ? "Lowest-ranked players fill the three-player pods."
                       : placement == "HIGH" ? "Highest-ranked players fill the three-player pods."
                       : "Three-player pods go wherever point totals match best.");
}

void CommanderRegistrationScreen::syncConfig(int checked)
{
    const QVariantMap ev = cdb::syncRecommendation(tournamentId);
    const QVariantMap rec = cdb::recommendation(checked);
    const QVariantMap settings = ev["settings"].toMap();
    const int n = ev["swiss_rounds"].toInt();
    loading_ = true;
    roundsSpin->setValue(n);
    drawCombo->setCurrentIndex(settings["draw_policy"].toString() == "ELIMINATED_LOSE" ? 0 : 1);
    shortCombo->setCurrentIndex(qMax(0, shortCombo->findData(settings.value("three_pod_placement", "BEST_MATCH"))));
    loading_ = false;
    updateHints();

    const QString error = rec["error"].toString();
    if (!error.isEmpty()) {
        recLabel->setText(error);
        recLabel->setStyleSheet("color:" + T::RED + ";background:transparent;border:none;");
    } else {
        QStringList pods;
        for (const QVariant &p : rec["pods"].toList())
            pods << p.toString();
        recLabel->setText(
            QStringLiteral("%1 checked in  →  pods of %2%3.   Recommended: %4 rounds.   This tournament will be %5 "
                           "round%6 in total and ends after Round %5.")
                .arg(checked).arg(pods.join(" + "), rec["byes"].toInt() ? QStringLiteral(" and %1 bye").arg(rec["byes"].toInt()) : QString())
                .arg(rec["rounds"].toInt()).arg(n).arg(n == 1 ? "" : "s"));
        recLabel->setStyleSheet("color:" + T::TEXT + ";background:transparent;border:none;");
    }
    const bool overridden = n != rec["rounds"].toInt();
    warnLabel->setText(overridden ? QStringLiteral("Organizer override: %1 rounds (recommended %2).").arg(n).arg(rec["rounds"].toInt())
                                  : QString());
    warnLabel->setVisible(overridden);
    resetBtn->setEnabled(overridden);
    startBtn->setEnabled(error.isEmpty());
    startBtn->setToolTip(error);
}

void CommanderRegistrationScreen::configChanged()
{
    if (loading_)
        return;
    try {
        cdb::updateEventConfig(tournamentId, roundsSpin->value(), drawCombo->currentData().toString(),
                               shortCombo->currentData().toString());
    } catch (const cdb::CommanderError &e) {
        warn(this, "Setup", e.message);
    }
    updateCounts();         // changing a setting does not rebuild the player list
}

void CommanderRegistrationScreen::confirmStart()
{
    const QVariantMap ev = cdb::getEvent(tournamentId);
    const QVariantMap rec = cdb::recommendation(ev["checked_in_count"].toInt());
    QStringList pods;
    for (const QVariant &p : rec["pods"].toList())
        pods << p.toString();
    const int n = ev["swiss_rounds"].toInt();
    if (!confirm(this, "Start Commander event",
                 QStringLiteral("Start %1 with %2 checked-in players?\n\nRound 1 pods: %3%4\nRounds: %5 in total — the "
                                "tournament ends after Round %5\n\nThe number of rounds, scoring and tiebreakers are "
                                "frozen once the event starts.")
                     .arg(tName).arg(ev["checked_in_count"].toInt())
                     .arg(pods.join(" + "), rec["byes"].toInt() ? QStringLiteral(" and %1 bye").arg(rec["byes"].toInt()) : QString())
                     .arg(n)))
        return;
    try {
        cdb::startEvent(tournamentId);
    } catch (const cdb::CommanderError &e) {
        warn(this, "Cannot start", e.message);
        refreshEnrolled();
        return;
    }
    mw_->navigateTo("round", {{"tournament_id", tournamentId}, {"tournament_name", tName}});
}

RoundScreen::RoundScreen(MainWindow *mw, qint64 tid, int viewRound) : tournamentId(tid), mw_(mw), viewRound_(viewRound)
{
    tournament_ = tdb::tournamentById(tid);
    game_ = tournament_.isEmpty() ? QString("ONEPIECE") : tournament_["game"].toString();
    tName_ = tournament_["name"].toString();
    totalRounds = tournament_.isEmpty() ? 3 : tournament_["total_rounds"].toInt();
    format_ = tournament_["format"].toString();
    status_ = tournament_.isEmpty() ? QString("IN_PROGRESS") : tournament_["status"].toString();

    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body), 1);

    QBoxLayout *top = flip(T::row(16));
    auto *info = new QVBoxLayout;
    info->setSpacing(3);
    roundLabel = T::lbl(QStringLiteral("Round —"), T::TEXT, 24, 700, true);
    info->addWidget(roundLabel);
    subLabel = T::lbl("", T::MUTED, 13, 400, true);
    info->addWidget(subLabel);
    top->addLayout(info, 1);
    timerSlot_ = new QVBoxLayout;
    top->addLayout(timerSlot_);
    body->addLayout(top);
    body->addSpacing(16);

    progressRow_ = new QHBoxLayout;
    progressRow_->setSpacing(6);
    body->addLayout(progressRow_);
    body->addSpacing(14);

    pairingsLayout_ = new QVBoxLayout;
    pairingsLayout_->setSpacing(10);
    body->addLayout(pairingsLayout_);
    body->addStretch();
    root->addSpacing(12);

    // bottom action bar (wraps when the window is narrow)
    auto *actions = new QWidget;
    auto *bottom = new T::FlowLayout(actions, 10);
    printBtn = T::button(QStringLiteral("🖨  Print"), "ghost");
    printBtn->setToolTip(QStringLiteral("Print this round’s pairings or table signs"));
    connect(printBtn, &QPushButton::clicked, this, [this] {
        if (!currentRoundId)
            return;
        printing::PrintDialog(this, printing::modernRoundData(tournament_, currentRoundNum, tdb::roundPairings(currentRoundId))).exec();
    });
    bottom->addWidget(printBtn);
    standingsBtn = T::button("View standings");
    connect(standingsBtn, &QPushButton::clicked, this, [this] {
        mw_->navigateTo("standings", {{"tournament_id", tournamentId}, {"tournament_name", tName_}});
    });
    bottom->addWidget(standingsBtn);
    nextRoundBtn = T::button(QStringLiteral("End round  →"), "primary");
    nextRoundBtn->setEnabled(false);
    connect(nextRoundBtn, &QPushButton::clicked, this, [this] { endRound(); });
    bottom->addWidget(nextRoundBtn);
    finalizeBtn = T::button(QStringLiteral("Finalize tournament  →"), "primary");
    finalizeBtn->hide();
    connect(finalizeBtn, &QPushButton::clicked, this, [this] { finalizeTournament(); });
    bottom->addWidget(finalizeBtn);
    root->addWidget(actions);

    loadRound();
}

void RoundScreen::loadRound()
{
    const db::Row latest = tdb::currentRound(tournamentId);
    db::Row info = latest;
    if (viewRound_ && !latest.isEmpty() && viewRound_ != latest["round_number"].toInt()) {
        const db::Row chosen = tdb::roundByNumber(tournamentId, viewRound_);
        if (!chosen.isEmpty())
            info = chosen;
    }
    if (info.isEmpty()) {
        roundLabel->setText(T::breakable(tName_ + QStringLiteral(" — No active round")));
        printBtn->setEnabled(false);
        nextRoundBtn->hide();
        return;
    }
    currentRoundId = info["round_id"].toLongLong();
    currentRoundNum = info["round_number"].toInt();
    roundLabel->setText(T::breakable(QStringLiteral("Round %1 — %2").arg(currentRoundNum).arg(tName_)));

    // results can only be entered for the current round of a running tournament
    const bool live = status_ == "IN_PROGRESS" && currentRoundId == latest["round_id"].toLongLong();
    if (live && timerRound_ != currentRoundId) {            // a clock per round, saved with the round
        T::clear(timerSlot_);
        MainWindow *mw = mw_;
        const QString message = QStringLiteral("Time expired — %1, round %2. Results are not finalized automatically.")
                                    .arg(tName_).arg(currentRoundNum);
        timerSlot_->addWidget(new TimerWidget(timerdb::Kind::OneOnOne, currentRoundId, [mw, message] { mw->roundAlert(message); }));
        timerRound_ = currentRoundId;
    }

    const db::Rows pairings = tdb::roundPairings(currentRoundId);
    T::ScrollKeeper keep(this);         // recording a result must not move the page
    T::clear(pairingsLayout_);
    for (const db::Row &p : pairings)
        pairingsLayout_->addWidget(pairingRow(p, live));
    updatePendingCount(pairings, live);
    if (live) {
        mw_->addTournamentTab(tournamentId, game_, tName_);
        mw_->updateTournamentTab(tournamentId);
    }
}

QWidget *RoundScreen::pairingRow(const db::Row &pairing, bool live)
{
    const bool isBye = pairing["player2_name"].isNull();
    const QString result = pairing["result"].toString();
    const qint64 matchId = pairing["match_id"].toLongLong();
    const QString p1 = pairing["player1_name"].toString();
    const QString p2 = isBye ? QString("BYE") : pairing["player2_name"].toString();
    const bool resolved = !result.isEmpty() || isBye;

    auto *row = new QFrame;
    row->setObjectName("mc");
    row->setStyleSheet("#mc{background:" + T::SURFACE2 + ";border:2px solid " + (resolved ? T::GREEN : T::BORDER)
                       + ";border-radius:16px;}");
    auto *outer = new QVBoxLayout(row);
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(8);

    outer->addWidget(T::lbl(isBye ? QString("BYE") : "TABLE " + pairing["table_number"].toString(), T::MUTED, 11, 700, false, true));
    // a text status as well as the outline colour; it wraps, so a long winner name is never cut off
    QLabel *status;
    if (isBye)
        status = T::lbl(QStringLiteral("✓  Bye"), T::GREEN, 12, 700, true);
    else if (result == "DRAW")
        status = T::lbl("=  Draw", T::AMBER, 12, 700, true);
    else if (!result.isEmpty())
        status = T::lbl(QStringLiteral("✓  ") + (result == "PLAYER1" ? p1 : p2) + " wins", T::GREEN, 12, 700, true);
    else
        status = T::lbl(QStringLiteral("●  Awaiting result"), T::PURPLE_LT, 12, 700, true);
    outer->addWidget(status);

    QBoxLayout *players = flip(T::row(10), true);
    const bool canReport = live && !resolved;
    auto *b1 = new T::WrapButton(p1);
    auto *b2 = new T::WrapButton(p2);
    b1->setEnabled(canReport);
    b2->setEnabled(canReport);
    if (canReport) {
        b1->setToolTip(p1 + " wins");
        b2->setToolTip(p2 + " wins");
        connect(b1, &QPushButton::clicked, this, [this, matchId] { recordResult(matchId, "PLAYER1"); });
        connect(b2, &QPushButton::clicked, this, [this, matchId] { recordResult(matchId, "PLAYER2"); });
    }
    players->addWidget(b1, 1);
    players->addWidget(T::lbl("VS", T::DIM, 11, 700, false, false, 1.0));
    players->addWidget(b2, 1);
    if (live && !result.isEmpty() && !isBye) {
        QPushButton *undo = T::button("Undo", "danger");
        connect(undo, &QPushButton::clicked, this, [this, matchId] {
            if (confirm(this, "Undo Result", "Clear this match result?"))
                recordResult(matchId, {});
        });
        players->addWidget(undo);
    }
    outer->addLayout(players);
    return row;
}

void RoundScreen::updatePendingCount(const db::Rows &pairings, bool live)
{
    int total = 0, pending = 0;
    for (const db::Row &p : pairings) {
        if (p["player2_name"].isNull())
            continue;
        ++total;
        if (p["result"].isNull())
            ++pending;
    }
    const int done = total - pending;
    QStringList parts{T::gameShort(game_)};
    if (!format_.isEmpty())
        parts << format_;
    parts << QStringLiteral("Round %1 of %2").arg(currentRoundNum).arg(totalRounds)
          << QStringLiteral("%1 %2").arg(total).arg(total == 1 ? "match" : "matches")
          << (live ? QStringLiteral("%1 pending").arg(pending) : QString("Tournament complete"));
    subLabel->setText(parts.join(QStringLiteral("  ·  ")));

    T::clear(progressRow_);
    for (int i = 0; i < qMin(total, 60); ++i) {
        auto *bar = new QFrame;
        bar->setFixedHeight(5);
        bar->setStyleSheet("background:" + (i < done ? T::GREEN : T::BORDER2) + ";border:none;border-radius:2px;");
        progressRow_->addWidget(bar, 1);
    }
    progressRow_->addSpacing(6);
    progressRow_->addWidget(T::lbl(QStringLiteral("%1 / %2 reported").arg(done).arg(total), T::MUTED, 12));

    const bool isLast = currentRoundNum >= totalRounds;
    nextRoundBtn->setVisible(live && !isLast);
    nextRoundBtn->setEnabled(pending == 0 && total > 0);
    finalizeBtn->setVisible(live && isLast);
    finalizeBtn->setEnabled(pending == 0 && total > 0);
}

void RoundScreen::recordResult(qint64 matchId, const QString &result)
{
    try {
        tdb::reportMatchResult(matchId, result);
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        return;
    }
    loadRound();
}

void RoundScreen::endRound()
{
    const int next = currentRoundNum + 1;
    if (next > totalRounds)
        return;                         // the configured number of rounds is the whole tournament
    if (!confirm(this, QStringLiteral("Advance to Round %1").arg(next),
                 QStringLiteral("End Round %1 and generate Round %2 pairings?").arg(currentRoundNum).arg(next)))
        return;
    try {
        swiss::advanceToNextRound(tournamentId, currentRoundNum);
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        return;
    }
    loadRound();
}

void RoundScreen::finalizeTournament()
{
    if (!confirm(this, "Finalize Tournament", "Lock final standings and record all placements?\nThis cannot be undone."))
        return;
    try {
        swiss::finalizeTournament(tournamentId, game_);
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        return;
    }
    mw_->removeTournamentTab(tournamentId);
    mw_->navigateTo("standings", {{"tournament_id", tournamentId}, {"tournament_name", tName_}});
}

// One-on-one standings.

static const int COL_PLACE = 56, COL_PTS = 60, COL_W = 46, COL_L = 46, COL_TB = 84;

static QString ordinal(int n)
{
    if (n % 100 >= 10 && n % 100 <= 20)
        return QString::number(n) + "th";
    const int last = n % 10;
    return QString::number(n) + (last == 1 ? "st" : last == 2 ? "nd" : last == 3 ? "rd" : "th");
}

StandingsScreen::StandingsScreen(MainWindow *mw, qint64 tid) : mw_(mw), tournamentId_(tid)
{
    const db::Row t = tdb::tournamentById(tid);
    game_ = t.isEmpty() ? QString("ONEPIECE") : t["game"].toString();
    tName_ = t["name"].toString();
    status_ = t.isEmpty() ? QString("IN_PROGRESS") : t["status"].toString();
    format_ = t["format"].toString();
    const int totalRounds = t["total_rounds"].toInt();
    tiebreaks_ = swiss::tiebreakColumns(game_);

    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body));      // the whole page scrolls; the table also scrolls sideways
    QBoxLayout *top = flip(T::row(12));
    auto *tv = new QVBoxLayout;
    tv->setSpacing(3);
    const bool final = status_ == "COMPLETED";
    tv->addWidget(T::lbl(final ? "Final Standings" : "Standings", T::TEXT, 26, 700, true));
    QStringList sub{tName_, (T::gameShort(game_) + " " + format_).trimmed(),
                    QStringLiteral("%1 round%2").arg(totalRounds).arg(totalRounds == 1 ? "" : "s")};
    if (!final)
        sub << "In progress";
    tv->addWidget(T::lbl(sub.join(QStringLiteral("  ·  ")), T::MUTED, 13, 400, true));
    top->addLayout(tv, 1);
    auto *actions = new QWidget;
    auto *exp = new T::FlowLayout(actions, 8);
    QPushButton *pdf = T::button("Export PDF");
    connect(pdf, &QPushButton::clicked, this, [this] { exportPdf(); });
    QPushButton *csv = T::button("Export CSV");
    connect(csv, &QPushButton::clicked, this, [this] { exportCsv(); });
    exp->addWidget(pdf);
    exp->addWidget(csv);
    top->addWidget(actions);
    body->addLayout(top);
    body->addSpacing(18);

    QFrame *table = T::panel("st");
    const int fixed = COL_PLACE + COL_PTS + COL_W + COL_L + COL_TB * int(tiebreaks_.size());
    table->setMinimumWidth(T::px(fixed) + 190);
    auto *tl = new QVBoxLayout(table);
    tl->setContentsMargins(1, 1, 1, 1);
    tl->setSpacing(0);

    auto *head = new QWidget;
    auto *hh = new QHBoxLayout(head);
    hh->setContentsMargins(20, 11, 20, 11);
    hh->setSpacing(0);
    QList<QPair<QString, int>> cols{{"#", COL_PLACE}, {"Player", 0}, {"Pts", COL_PTS}, {"W", COL_W}, {"L", COL_L}};
    for (const auto &c : tiebreaks_)
        cols.append({c.second, COL_TB});
    for (int i = 0; i < cols.size(); ++i) {
        QLabel *lb = T::lbl(cols[i].first.toUpper(), T::MUTED, 10, 700, false, false, 1.0);
        if (cols[i].second) {
            lb->setFixedWidth(T::px(cols[i].second));
            lb->setAlignment((i == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);   // figures line up on the right
            hh->addWidget(lb);
        } else {
            hh->addWidget(lb, 1);                   // player names stay left-aligned
        }
    }
    tl->addWidget(head);
    tl->addWidget(T::hline());
    rowsBox_ = new QVBoxLayout;
    rowsBox_->setContentsMargins(0, 0, 0, 0);
    rowsBox_->setSpacing(0);
    tl->addLayout(rowsBox_);
    tableScroll_ = new T::HScroll(table);       // sideways when the window is narrower than the columns
    body->addWidget(tableScroll_);
    body->addStretch();
    load();
}

void StandingsScreen::load()
{
    try {
        standings = swiss::currentStandings(tournamentId_, game_);
    } catch (const std::exception &e) {
        warn(this, "Error", QString::fromUtf8(e.what()));
        standings.clear();
    }
    T::clear(rowsBox_);
    for (int i = 0; i < standings.size(); ++i)
        rowsBox_->addWidget(standingRow(i + 1, standings[i], i == standings.size() - 1));
    if (standings.isEmpty()) {
        QLabel *empty = T::lbl("No standings yet", T::MUTED, 13);
        empty->setContentsMargins(20, 16, 20, 16);
        rowsBox_->addWidget(empty);
    }
    tableScroll_->fit();
}

QWidget *StandingsScreen::standingRow(int place, const db::Row &player, bool last)
{
    const QString pc = T::placeColor(place);
    auto *row = new QFrame;
    row->setObjectName("sr");
    row->setStyleSheet("#sr{background:transparent;border:none;border-bottom:" + (last ? QString("none") : "1px solid " + T::BORDER)
                       + ";border-left:4px solid " + (pc.isEmpty() ? QString("transparent") : pc) + ";}");
    auto *hl = new QHBoxLayout(row);
    hl->setContentsMargins(16, 12, 20, 12);
    hl->setSpacing(0);
    auto fixed = [&](QLabel *l, int width, bool number = true) {
        l->setFixedWidth(T::px(width));
        l->setAlignment((number ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        hl->addWidget(l);
    };
    fixed(T::lbl(ordinal(place), pc.isEmpty() ? T::MUTED : pc, 13, 700, false, true), COL_PLACE, false);
    hl->addWidget(T::lbl(player["display_name"].toString(), T::TEXT, 14, 600, true), 1);
    fixed(T::lbl(player["match_points"].toString(), T::TEXT, 15, 700), COL_PTS);
    fixed(T::lbl(player["match_wins"].toString(), T::GREEN, 13, 600), COL_W);
    fixed(T::lbl(player["match_losses"].toString(), T::RED, 13), COL_L);
    for (const auto &c : tiebreaks_)
        fixed(T::lbl(QString::number(player[c.first].toDouble(), 'f', 3), T::MUTED, 12, 400, false, true), COL_TB);
    return row;
}

void StandingsScreen::exportPdf()
{
    const QString path = QFileDialog::getSaveFileName(this, "Export PDF", QString(tName_).replace(" ", "_") + "_tournament.pdf",
                                                      "PDF Files (*.pdf)");
    if (path.isEmpty())
        return;
    try {
        if (!printing::savePdf(printing::tournamentReport(tournamentId_), path))
            throw std::runtime_error("The file could not be written.");
        inform(this, "Exported", "PDF saved to:\n" + path);
    } catch (const std::exception &e) {
        warn(this, "Failed", QString::fromUtf8(e.what()));
    }
}

void StandingsScreen::exportCsv()
{
    if (standings.isEmpty()) {
        inform(this, "No Data", "Nothing to export.");
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, "Export CSV", QString(tName_).replace(" ", "_") + "_standings.csv",
                                                      "CSV Files (*.csv)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(this, "Failed", f.errorString());
        return;
    }
    QTextStream out(&f);
    QStringList header{"Place", "Player", "Points", "Wins", "Losses"};
    for (const auto &c : tiebreaks_)
        header << c.second;
    out << header.join(",") << "\n";
    for (int i = 0; i < standings.size(); ++i) {
        const db::Row &p = standings[i];
        QStringList row{QString::number(i + 1), printing::csvCell(p["display_name"].toString()), p["match_points"].toString(),
                        p["match_wins"].toString(), p["match_losses"].toString()};
        for (const auto &c : tiebreaks_)
            row << QString::number(p[c.first].toDouble(), 'f', 3);
        out << row.join(",") << "\n";
    }
    f.close();
    inform(this, "Exported", "Saved to:\n" + path);
}

RoundSelectScreen::RoundSelectScreen(MainWindow *mw, qint64 tid)
{
    const db::Row t = tdb::tournamentById(tid);
    const QString name = t["name"].toString(), game = t["game"].toString();
    const int players = t["player_count"].toInt();
    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body));

    // One results panel, centred in the content area by the layout: equal stretch on both
    // sides, and a width cap so it does not sprawl in a wide window.
    panel = T::panel("rs", {}, 1, 23);
    panel->setMaximumWidth(760);
    auto *centre = new QHBoxLayout;
    centre->setContentsMargins(0, 0, 0, 0);
    centre->addStretch(1);
    centre->addWidget(panel, 100);
    centre->addStretch(1);
    body->addSpacing(8);
    body->addLayout(centre);
    body->addStretch();

    auto *v = new QVBoxLayout(panel);
    const int pad = mw->width() < 520 ? 14 : T::px(28);        // less padding when there is little room
    v->setContentsMargins(pad, T::px(22), pad, T::px(24));
    v->setSpacing(0);
    auto centred = [v](QLabel *l) {
        l->setAlignment(Qt::AlignHCenter);
        v->addWidget(l);
    };
    centred(T::caps(QStringLiteral("Results  ·  ") + (T::gameShort(game) + " " + t["format"].toString()).trimmed(), T::PURPLE_LT, 12));
    v->addSpacing(6);
    centred(T::lbl(name, T::TEXT, 28, 700, true));
    v->addSpacing(6);
    centred(T::lbl(QStringLiteral("Tournament complete  ·  %1 player%2").arg(players).arg(players == 1 ? "" : "s"),
                   T::MUTED, 13, 400, true));
    v->addSpacing(20);

    auto *actions = new QWidget;
    auto *al = new T::FlowLayout(actions, 10, true);
    QPushButton *standings = T::button(QStringLiteral("View final standings  →"), "primary");
    connect(standings, &QPushButton::clicked, this, [mw, tid, name, game] {
        mw->navigateTo("standings", {{"tournament_id", tid}, {"tournament_name", name}, {"game", game}});
    });
    al->addWidget(standings);
    v->addWidget(actions);
    v->addSpacing(22);
    v->addWidget(T::hline());
    v->addSpacing(18);

    centred(T::caps("Review a round"));
    v->addSpacing(10);
    auto *grid = new QWidget;
    auto *flow = new T::FlowLayout(grid, 10, true);
    const db::Rows rounds = tdb::rounds(tid);
    for (const db::Row &r : rounds) {
        const int number = r["round_number"].toInt();
        QPushButton *btn = T::button(QStringLiteral("Round %1").arg(number));
        btn->setMinimumWidth(T::px(120));
        connect(btn, &QPushButton::clicked, this, [mw, tid, name, game, number] {
            mw->navigateTo("round", {{"tournament_id", tid}, {"tournament_name", name}, {"game", game}, {"round_number", number}});
        });
        flow->addWidget(btn);
    }
    if (rounds.isEmpty())
        flow->addWidget(T::lbl("No rounds were played.", T::MUTED, 13));
    v->addWidget(grid);
}

PlayersScreen::PlayersScreen(MainWindow *mw) : mw_(mw)
{
    QBoxLayout *hdr = flip(T::row(12), true);
    hdr->addWidget(T::lbl("Player Management", T::TEXT, 26, 700, true), 1);
    countLabel = T::lbl("", T::MUTED, 13);
    hdr->addWidget(countLabel);
    root->addLayout(hdr);
    root->addSpacing(14);

    QBoxLayout *bar = flip(T::row(10), true);
    searchBox = new QLineEdit;
    searchBox->setPlaceholderText("Search players...");
    searchBox->setAccessibleName("Search players");
    bar->addWidget(searchBox, 1);
    QPushButton *add = T::button("+ New player", "primary");
    connect(add, &QPushButton::clicked, this, [this] {
        QString name;
        if (!askText(this, "Add new player", "Display name", "Add player", &name))
            return;
        if (name.isEmpty()) {
            warn(this, "Missing Name", "Enter a name.");
            return;
        }
        pdb::addPlayer(name);
        populate(pdb::allPlayers());
    });
    bar->addWidget(add);
    root->addLayout(bar);
    root->addSpacing(14);

    root->addWidget(T::scrollArea(&list_), 1);
    list_->setSpacing(6);

    searchTimer_.setSingleShot(true);
    connect(&searchTimer_, &QTimer::timeout, this, [this] { runSearch(); });
    connect(searchBox, &QLineEdit::textChanged, this, [this] { searchTimer_.start(250); });
    populate(pdb::allPlayers());
}

void PlayersScreen::runSearch()
{
    const QString text = searchBox->text().trimmed();
    populate(text.isEmpty() ? pdb::allPlayers() : pdb::searchPlayers(text));
}

void PlayersScreen::populate(const db::Rows &players)
{
    T::ScrollKeeper keep(this);
    T::clear(list_);
    countLabel->setText(QStringLiteral("%1 player%2").arg(players.size()).arg(players.size() == 1 ? "" : "s"));
    if (players.isEmpty())
        list_->addWidget(T::lbl("No players found.", T::MUTED, 13));
    // one query for everybody's game history, not one per row
    const QHash<qint64, QStringList> played = pdb::gamesPlayed();

    const int tagArea = tagArea_ = tagAreaWidth();
    const int gap = 20;                                  // between the tag area and the name

    for (const db::Row &p : players) {
        const qint64 pid = p["player_id"].toLongLong();
        const QString name = p["display_name"].toString();
        auto *row = new QFrame;
        row->setObjectName("pr");
        row->setStyleSheet("#pr{background:" + T::SURFACE2 + ";border:" + QString::number(T::LINE) + "px solid " + T::BORDER2
                           + ";border-radius:16px;}");
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);       // as tall as its content, never stretched
        QBoxLayout *outer = flip(T::row(10), true);     // the action drops to a second line in a narrow window
        outer->setContentsMargins(14, 4, 10, 4);
        row->setLayout(outer);

        auto *lead = new QHBoxLayout;                   // tags left, name right — at every window size
        lead->setSpacing(gap);
        auto *tags = new T::TagStrip(played.value(pid));
        tags->setFixedWidth(tagArea);
        lead->addWidget(tags, 0, Qt::AlignVCenter);
        lead->addWidget(new T::NameScroll(name), 1, Qt::AlignVCenter);
        outer->addLayout(lead, 1);

        QPushButton *view = T::button("View profile");
        view->setAccessibleName("View profile of " + name);
        connect(view, &QPushButton::clicked, this, [this, pid, name] {
            mw_->navigateTo("player_profile", {{"player_id", pid}, {"player_name", name}});
        });
        outer->addWidget(view, 0, Qt::AlignVCenter);
        list_->addWidget(row);
    }
    list_->addStretch();
}

// The tag area is the same width in every row, so every name starts at the same place.
// It is wide enough for every game side by side when there is room; when there is not, it
// takes a share of the row and scrolls sideways instead of wrapping or pushing the name.
int PlayersScreen::tagAreaWidth() const
{
    T::TagStrip probe({"POKEMON", "ONEPIECE", "MTG"});
    const int allTags = probe.contentWidth();
    const int side = tiny ? 14 : (narrow ? 20 : 36);
    const int rowRoom = width() - 2 * side - 28 - 10 - (tiny ? 0 : T::px(150));
    return qMin(allTags, qMax(T::px(96), rowRoom * 45 / 100));
}

void PlayersScreen::resizeEvent(QResizeEvent *e)
{
    T::Screen::resizeEvent(e);
    if (list_ && tagAreaWidth() != tagArea_)
        runSearch();        // the tag area follows the room available
}

static QString profileGameLabel(const QString &game)
{
    return game == "ALL" ? QString("All games") : T::gameShort(game);
}

PlayerProfileScreen::PlayerProfileScreen(MainWindow *mw, qint64 playerId) : mw_(mw), playerId_(playerId)
{
    playerName_ = pdb::playerById(playerId)["display_name"].toString();

    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body));

    QBoxLayout *hdr = flip(T::row(12));
    nameLabel_ = T::lbl(playerName_, T::TEXT, 26, 700, true);
    hdr->addWidget(nameLabel_, 1);
    auto *actions = new QWidget;
    auto *al = new T::FlowLayout(actions, 8);
    QPushButton *edit = T::button("Edit name");
    connect(edit, &QPushButton::clicked, this, [this] { editName(); });
    QPushButton *del = T::button("Delete player", "danger");
    connect(del, &QPushButton::clicked, this, [this] { deletePlayer(); });
    QPushButton *back = T::button("Back", "ghost");
    connect(back, &QPushButton::clicked, this, [this] { mw_->goBack(); });
    for (QPushButton *b : {edit, del, back})
        al->addWidget(b);
    hdr->addWidget(actions);
    body->addLayout(hdr);
    body->addSpacing(14);

    QFrame *stats = T::panel("stats");
    statsLayout_ = new T::FlowLayout(stats, 26);
    statsLayout_->setContentsMargins(20, 14, 20, 14);
    body->addWidget(stats);
    body->addSpacing(14);

    auto *filters = new QWidget;
    auto *fr = new T::FlowLayout(filters, 8);
    for (const QString &game : {QString("ALL"), QString("ONEPIECE"), QString("POKEMON"), QString("MTG")}) {
        QPushButton *btn = T::button(profileGameLabel(game), "ghost");
        btn->setCheckable(true);
        connect(btn, &QPushButton::clicked, this, [this, game] {
            filter_ = game;
            styleFilters();
            expanded_.clear();
            loadFeed();
        });
        fr->addWidget(btn);
        filterButtons_.insert(game, btn);
    }
    body->addWidget(filters);
    body->addSpacing(12);

    body->addWidget(T::caps("Tournament history"));
    body->addSpacing(8);
    feed_ = new QVBoxLayout;
    feed_->setSpacing(10);
    body->addLayout(feed_);
    body->addStretch();
    styleFilters();
    loadStats();
    loadFeed();
}

void PlayerProfileScreen::styleFilters()
{
    for (auto it = filterButtons_.begin(); it != filterButtons_.end(); ++it) {
        it.value()->setChecked(it.key() == filter_);
        it.value()->setStyleSheet(T::buttonQss(it.key() == filter_ ? "primary" : "ghost", T::MIN_HIT));
    }
}

void PlayerProfileScreen::loadStats()
{
    T::clear(statsLayout_);
    auto stat = [this](const QString &label, const QString &value, const QString &color = {}) {
        auto *box = new QWidget;
        auto *col = new QVBoxLayout(box);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);
        col->addWidget(T::lbl(label, T::MUTED, 11));
        col->addWidget(T::lbl(value, color.isEmpty() ? T::TEXT : color, 15, 700));
        statsLayout_->addWidget(box);
    };
    const db::Rows history = pdb::tournamentHistory(playerId_);
    stat("Tournaments", QString::number(history.size()));
    for (const QString &game : {QString("ONEPIECE"), QString("POKEMON"), QString("MTG")}) {
        int w = 0, l = 0, entries = 0, best = 0;
        for (const db::Row &h : history) {
            if (h["game"].toString() != game)
                continue;
            ++entries;
            w += h["match_wins"].toInt();
            l += h["match_losses"].toInt();
            const int placed = h["final_placement"].toInt();
            if (placed && (!best || placed < best))
                best = placed;
        }
        if (!entries)
            continue;
        const QString pct = (w + l) > 0 ? QStringLiteral("%1%").arg(w * 100 / (w + l)) : QString("--");
        const QString color = T::gameColor(game);
        stat(profileGameLabel(game), QStringLiteral("%1W / %2L  (%3)").arg(w).arg(l).arg(pct), color);
        if (best)
            stat(profileGameLabel(game) + " best finish", QStringLiteral("#%1").arg(best), color);
    }
}

void PlayerProfileScreen::loadFeed()
{
    T::ScrollKeeper keep(this);
    T::clear(feed_);
    bool any = false;
    for (const db::Row &h : pdb::tournamentHistory(playerId_)) {
        if (filter_ != "ALL" && h["game"].toString() != filter_)
            continue;
        feed_->addWidget(historyCard(h));
        any = true;
    }
    if (!any)
        feed_->addWidget(T::lbl("No tournaments found.", T::MUTED, 13));
}

QWidget *PlayerProfileScreen::historyCard(const db::Row &entry)
{
    const qint64 tid = entry["tournament_id"].toLongLong();
    const QString game = entry["game"].toString();
    const QString color = T::gameColor(game);
    const bool open = expanded_.contains(tid);

    QFrame *outer = T::panel("hc");
    auto *vbox = new QVBoxLayout(outer);
    vbox->setContentsMargins(1, 1, 1, 1);
    vbox->setSpacing(0);

    auto *summary = new T::ClickFrame("hs");
    summary->setAccessibleName(entry["tournament_name"].toString() + ": " + (open ? "hide" : "show") + " matches");
    summary->setStyleSheet("#hs{background:transparent;border:2px solid transparent;border-radius:14px;}"
                           "#hs:focus{border-color:" + T::FOCUS + ";}");
    QBoxLayout *hl = flip(T::row(10), true);
    hl->setContentsMargins(14, 10, 14, 10);
    summary->setLayout(hl);
    auto *title = new QVBoxLayout;
    title->setSpacing(2);
    title->addWidget(T::lbl(entry["tournament_name"].toString(), T::TEXT, 14, 700, true));
    QStringList fmt{profileGameLabel(game)};
    if (!entry["format"].toString().isEmpty())
        fmt << entry["format"].toString();
    if (!entry["tournament_date"].toString().isEmpty())
        fmt << entry["tournament_date"].toString().left(10);
    title->addWidget(T::lbl(fmt.join(QStringLiteral("  ·  ")), color, 12, 400, true));
    hl->addLayout(title, 1);
    hl->addWidget(T::lbl(QStringLiteral("%1W / %2L").arg(entry["match_wins"].toInt()).arg(entry["match_losses"].toInt()), T::MUTED, 13));
    if (entry["final_placement"].toInt())
        hl->addWidget(T::lbl(QStringLiteral("#%1").arg(entry["final_placement"].toInt()), color, 13, 700));
    hl->addWidget(T::lbl(open ? QStringLiteral("▾") : QStringLiteral("▸"), T::MUTED, 13));
    vbox->addWidget(summary);
    if (open) {
        vbox->addWidget(T::hline());
        vbox->addWidget(detailPanel(tid));
    }
    connect(summary, &T::ClickFrame::clicked, this, [this, tid] {
        if (!expanded_.remove(tid))
            expanded_.insert(tid);
        loadFeed();
    });       // the card that was clicked is rebuilt, so finish the click first
    return outer;
}

QWidget *PlayerProfileScreen::detailPanel(qint64 tournamentId)
{
    auto *panel = new QWidget;
    auto *vbox = new QVBoxLayout(panel);
    vbox->setContentsMargins(16, 10, 16, 12);
    vbox->setSpacing(6);

    struct Line {
        QString when, where, result, color;
    };
    QList<Line> lines;
    for (const db::Row &m : pdb::matchHistory(playerId_, tournamentId)) {
        const QString outcome = m["result"].toString();
        Line line;
        if (outcome == "BYE") { line.result = "Bye"; line.color = T::MUTED; }
        else if (outcome == "PENDING") { line.result = "Pending"; line.color = T::MUTED; }
        else if (outcome == "DRAW") { line.result = "Draw"; line.color = T::AMBER; }
        else if (outcome == "WIN") { line.result = "Win"; line.color = T::GREEN; }
        else { line.result = "Loss"; line.color = T::RED; }
        const QString opp = m["opponent_name"].toString();
        line.when = QStringLiteral("Round %1").arg(m["round_number"].toInt());
        line.where = QStringLiteral("Table %1  ·  vs %2")
                         .arg(m["table_number"].isNull() ? QStringLiteral("—") : m["table_number"].toString(),
                              opp.isEmpty() ? QStringLiteral("—") : opp);
        lines << line;
    }
    for (const db::Row &p : pdb::podHistory(playerId_, tournamentId)) {
        const QString stage = p["stage"].toString(), res = p["result"].toString();
        Line line;
        line.when = stage == "SWISS" ? QStringLiteral("Round %1").arg(p["stage_round"].toInt())
                  : stage == "SEMIFINAL" ? QString("Semifinal (earlier version)") : QString("Final pod (earlier version)");
        line.where = QStringLiteral("Pod %1  ·  seat %2").arg(p["pod_number"].toInt()).arg(p["seat_number"].toInt());
        line.result = res == "WIN" ? "Win" : res == "LOSS" ? "Loss" : res == "DRAW" ? "Draw" : "Pending";
        line.color = res == "WIN" ? T::GREEN : res == "LOSS" ? T::RED : res == "DRAW" ? T::AMBER : T::MUTED;
        lines << line;
    }
    if (lines.isEmpty()) {
        vbox->addWidget(T::lbl("No match details available.", T::MUTED, 13));
        return panel;
    }
    for (const Line &line : lines) {
        auto *row = new QHBoxLayout;
        QLabel *when = T::lbl(line.when, T::MUTED, 12, 600);
        when->setMinimumWidth(T::px(78));
        row->addWidget(when);
        row->addWidget(T::lbl(line.where, T::TEXT, 13, 400, true), 1);
        row->addWidget(T::lbl(line.result, line.color, 13, 700));
        vbox->addLayout(row);
    }
    return panel;
}

void PlayerProfileScreen::editName()
{
    QString name = playerName_;
    if (!askText(this, "Edit player name", "Player name", "Save", &name))
        return;
    if (name.isEmpty()) {
        warn(this, "Invalid Name", "Name cannot be empty.");
        return;
    }
    pdb::renamePlayer(playerId_, name);
    playerName_ = name;
    nameLabel_->setText(T::breakable(name));
    inform(this, "Saved", "Name updated to: " + name);
}

void PlayerProfileScreen::deletePlayer()
{
    if (!confirm(this, "Delete Player",
                 "Permanently delete " + playerName_ + "?\n\nTheir enrollment and match records will be removed.\n"
                 "Completed tournament results will be preserved."))
        return;
    try {
        pdb::deletePlayer(playerId_);
    } catch (const db::Error &e) {
        warn(this, "Could not delete", playerName_ + " could not be deleted.\n\n" + e.message);
        return;
    }
    inform(this, "Deleted", playerName_ + " has been deleted.");
    mw_->navigateTo("players");
}
