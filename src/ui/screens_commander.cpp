// Commander (multiplayer) screens: rounds of pods and results, and standings.
#include "applog.h"
#include "commander.h"
#include "commander_db.h"
#include "printing.h"
#include "screens.h"

#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTextStream>

static QPushButton *smallButton(const QString &text, bool danger = false, const QString &icon = {})
{
    return T::button(text, danger ? "danger" : "ghost", icon);
}

ResultDialog::ResultDialog(QWidget *parent, const QVariantMap &pod, const QString &title, const QString &drawPolicy,
                           bool startAsDraw, bool askReason, bool scoring, bool decide)
    : QDialog(parent)
{
    setWindowTitle(title);
    fitDialog(this, parent, 460, 460);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(10);
    outer->addWidget(T::lbl(title, T::TEXT, 20, 700, true));

    QVBoxLayout *v = nullptr;
    outer->addWidget(T::scrollArea(&v), 1);     // the form scrolls; Cancel and Save stay in view
    v->setSpacing(8);
    v->addWidget(T::caps("Result"));
    outcomeCombo = T::combo();
    outcomeCombo->setAccessibleName("Result");
    const QVariantList seats = pod["seats"].toList();
    qint64 savedWinner = 0;
    for (const QVariant &sv : seats) {
        const QVariantMap s = sv.toMap();
        outcomeCombo->addItem(s["display_name"].toString() + " wins", s["player_id"].toLongLong());
        if (s["result"].toString() == "WIN")
            savedWinner = s["player_id"].toLongLong();
    }
    outcomeCombo->addItem(QStringLiteral("Draw, no winner"), qint64(0));
    v->addWidget(outcomeCombo);

    // points in events that score it, recognition only in earlier ones; never a tiebreaker
    secondBox_ = new QWidget;
    auto *sb = new QVBoxLayout(secondBox_);
    sb->setContentsMargins(0, 4, 0, 0);
    sb->setSpacing(4);
    sb->addWidget(T::caps("Second place"));
    secondCombo = T::combo();
    secondCombo->setAccessibleName("Second place");
    sb->addWidget(secondCombo);
    sb->addWidget(T::lbl(scoring ? QStringLiteral("A clear second place earns %1 points and still counts as a loss. Choose "
                                                  "No clear second when the last players were eliminated at the same moment.")
                                       .arg(cmdr::pointsText(cmdr::defaultSettings()["second_place_points"].toDouble()))
                                 : QStringLiteral("Optional recognition; no extra points."),
                         T::MUTED, 12, 400, true));
    v->addWidget(secondBox_);

    elimBox_ = new QWidget;
    auto *ev = new QVBoxLayout(elimBox_);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(0);
    ev->addWidget(T::lbl(drawPolicy == "ELIMINATED_LOSE"
                             ? "Tick anyone already eliminated when the game was drawn. They take a loss."
                             : "Tick anyone already eliminated when the game was drawn (everyone still receives the draw).",
                         T::MUTED, 12, 400, true));
    const bool wasDraw = pod["outcome"].toString() == "DRAW";
    for (const QVariant &sv : seats) {
        const QVariantMap s = sv.toMap();
        auto *cb = new QCheckBox(T::breakable(QStringLiteral("Seat %1  ·  %2").arg(s["seat"].toInt()).arg(s["display_name"].toString())));
        cb->setChecked(wasDraw && s["eliminated"].toInt());
        checks_.insert(s["player_id"].toLongLong(), cb);
        ev->addWidget(cb);
    }
    v->addWidget(elimBox_);

    v->addStretch();

    error = T::lbl("", T::RED, 12, 700, true);
    error->hide();
    outer->addWidget(error);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    if (askReason) {
        reason_ = new QLineEdit;
        reason_->setPlaceholderText("Reason for the correction (required)");
        reason_->setAccessibleName("Reason for the correction");
        buttons->addWidget(reason_, 1);
    } else {
        buttons->addStretch();
    }
    QPushButton *cancel = T::button("Cancel", "ghost");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    saveBtn = T::button(askReason ? "Save correction" : "Save result", "primary");
    saveBtn->setDefault(true);
    connect(saveBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(saveBtn);
    outer->addLayout(buttons);
    if (reason_) {
        saveBtn->setEnabled(false);
        connect(reason_, &QLineEdit::textChanged, this, [this] { saveBtn->setEnabled(!reason().isEmpty()); });
    }

    if (startAsDraw || wasDraw)
        outcomeCombo->setCurrentIndex(outcomeCombo->count() - 1);
    else if (savedWinner)
        outcomeCombo->setCurrentIndex(outcomeCombo->findData(savedWinner));
    // the winner is never offered as second, so a new winner drops a choice that named them
    auto fillSecond = [this, seats, scoring, decide](qint64 keep) {
        const QSignalBlocker block(secondCombo);
        secondCombo->clear();
        if (!scoring) {
            secondCombo->addItem("Not recorded", NOT_RECORDED);
        } else {
            if (!decide)
                secondCombo->addItem("Not recorded yet", NOT_RECORDED);
            secondCombo->addItem("No clear second (eliminated at the same moment)", NO_CLEAR_SECOND);
        }
        for (const QVariant &sv : seats) {
            const QVariantMap s = sv.toMap();
            if (s["player_id"].toLongLong() != winner())
                secondCombo->addItem(s["display_name"].toString(), s["player_id"].toLongLong());
        }
        secondCombo->setCurrentIndex(std::max(0, secondCombo->findData(keep)));
    };
    fillSecond(pod["second_place_status"].toString() == "NONE" ? NO_CLEAR_SECOND : pod["second_place_player_id"].toLongLong());
    auto sync = [this, fillSecond] {
        fillSecond(secondCombo->currentData().toLongLong());
        elimBox_->setVisible(winner() == 0);
        secondBox_->setVisible(winner() != 0);
    };
    connect(outcomeCombo, &QComboBox::currentIndexChanged, this, sync);
    sync();
    outcomeCombo->setFocus();
}

QString ResultDialog::outcome() const { return winner() ? "WIN" : "DRAW"; }
qint64 ResultDialog::winner() const { return outcomeCombo->currentData().toLongLong(); }
QString ResultDialog::reason() const { return reason_ ? reason_->text().trimmed() : QString(); }

void ResultDialog::showError(const QString &text)
{
    error->setText(T::breakable("Not saved: " + text));
    error->setVisible(!text.isEmpty());
}
cdb::SecondPlace ResultDialog::secondPlace() const
{
    if (!winner())
        return {};
    const qint64 chosen = secondCombo->currentData().toLongLong();
    return chosen == NO_CLEAR_SECOND ? cdb::SecondPlace::noClearSecond() : cdb::SecondPlace(chosen);
}

static QString placeText(int place)
{
    return place == 1 ? "1st" : place == 2 ? "2nd" : place == 3 ? "3rd" : QStringLiteral("%1th").arg(place);
}

QList<qint64> ResultDialog::eliminated() const
{
    QList<qint64> out;
    if (winner())
        return out;
    for (auto it = checks_.begin(); it != checks_.end(); ++it)
        if (it.value()->isChecked())
            out << it.key();
    return out;
}

CommanderEventScreen::CommanderEventScreen(MainWindow *mw, qint64 tournamentId, int viewRound)
    : mw_(mw), tournamentId_(tournamentId), view_(viewRound)
{
    QVBoxLayout *body = nullptr;
    root->addWidget(page(&body), 1);   // header and pods scroll together; actions stay put
    QBoxLayout *top = flip(T::row(12));
    auto *info = new QVBoxLayout;
    info->setSpacing(3);
    title = T::lbl("", T::TEXT, 24, 700, true);
    sub = T::lbl("", T::MUTED, 13, 400, true);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(6);
    titleRow->addWidget(title, 100);
    renameBtn = editNameButton();
    connect(renameBtn, &QPushButton::clicked, this, [this] { rename(); });
    titleRow->addWidget(renameBtn, 0, Qt::AlignTop);
    titleRow->addStretch(1);
    info->addLayout(titleRow);
    info->addWidget(sub);
    top->addLayout(info, 1);
    timerSlot_ = new QVBoxLayout;
    top->addLayout(timerSlot_);
    body->addLayout(top);
    body->addSpacing(10);

    auto *tools = new QWidget;                   // wraps onto more lines instead of running off the edge
    auto *tl = new T::FlowLayout(tools, 8);
    printBtn = smallButton("Print", false, "printer");
    printBtn->setToolTip(QStringLiteral("Print this round’s pairings or pod signs"));
    connect(printBtn, &QPushButton::clicked, this, [this] {
        const QVariantMap rnd = currentRound();
        if (rnd.isEmpty())
            return;
        if (showingFinal()) {
            QVariantList seats;
            for (const QVariant &p : state["final"].toMap()["players"].toList())
                seats << QVariantMap{{"seat", p.toMap()["seat"]}, {"display_name", p.toMap()["display_name"]}};
            std::sort(seats.begin(), seats.end(), [](const QVariant &a, const QVariant &b) {
                return a.toMap()["seat"].toInt() < b.toMap()["seat"].toInt();
            });
            const QVariantMap pod{{"pod_number", 1}, {"seats", seats}};
            printing::PrintDialog(this, printing::commanderRoundData(state["event"].toMap()["name"].toString(), "Top-four final",
                                                                     {{"pods", QVariantList{pod}}})).exec();
            return;
        }
        printing::PrintDialog(this, printing::commanderRoundData(state["event"].toMap()["name"].toString(),
                                                                 cdb::stageLabel(rnd), rnd)).exec();
    });
    tl->addWidget(printBtn);
    QPushButton *standings = smallButton("Standings", false, "trophy");
    connect(standings, &QPushButton::clicked, this, [this] { mw_->navigateTo("standings", {{"tournament_id", tournamentId_}}); });
    QPushButton *drops = smallButton("Players && drops");
    connect(drops, &QPushButton::clicked, this, [this] { openDrops(); });
    QPushButton *rules = smallButton("Rules", false, "circle-help");
    connect(rules, &QPushButton::clicked, this, [this] { showText(this, "Event rules", cdb::rulesText(state["event"].toMap())); });
    QPushButton *history = smallButton("History", false, "history");
    connect(history, &QPushButton::clicked, this, [this] { openHistory(); });
    for (QPushButton *b : {standings, drops, rules, history})
        tl->addWidget(b);
    body->addWidget(tools);
    body->addSpacing(10);

    auto *stripHolder = new QWidget;
    strip_ = new T::FlowLayout(stripHolder, 6);
    body->addWidget(stripHolder);
    body->addSpacing(12);

    body_ = new QVBoxLayout;
    body_->setSpacing(14);
    body->addLayout(body_);
    body->addStretch();

    root->addSpacing(12);
    footerHolder_ = new QWidget;
    footer_ = new T::FlowLayout(footerHolder_, 10);
    root->addWidget(footerHolder_);

    refresh();
}

// refresh() re-reads the event, so the heading, the clock's notice and the next printout take
// the new name; it keeps the scroll position, and nothing about the event itself changes.
void CommanderEventScreen::rename()
{
    const QString name = editTournamentName(mw_, this, tournamentId_);
    renameBtn->setFocus();
    if (name.isEmpty())
        return;
    timerRound_ = -1;                   // the clock widget is rebuilt so its notice names the event correctly
    refresh();
}

void CommanderEventScreen::fillGrid(QGridLayout *grid, const QList<QWidget *> &cards)
{
    while (grid->count())
        delete grid->takeAt(0);
    const int cols = narrow ? 1 : 2;
    for (int i = 0; i < cards.size(); ++i) {
        // an odd last pod spans the row, so the grid stays balanced instead of leaving a gap on the right
        const bool alone = cols == 2 && i == cards.size() - 1 && i % 2 == 0;
        grid->addWidget(cards[i], i / cols, i % cols, 1, alone ? 2 : 1);
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, cols == 2 ? 1 : 0);
}

void CommanderEventScreen::reflow()
{
    if (!podCards_.isEmpty() && cardsTiny_ != tiny) {
        refresh();          // the pod cards place their actions for the width they were built at
        return;
    }
    for (const auto &g : grids_)
        if (g.first)
            fillGrid(g.first, g.second);
}

void CommanderEventScreen::refresh()
{
    try {
        state = cdb::getState(tournamentId_);
    } catch (const cdb::CommanderError &e) {
        title->setText(e.message);
        T::hugText(title);
        return;
    }
    const QVariantMap ev = state["event"].toMap();
    const QVariantList rounds = state["rounds"].toList();
    const QVariantMap current = currentRound();
    const QString stage = ev["stage"].toString();
    const QString name = ev["name"].toString();
    int done = 0;
    for (const QVariant &r : rounds)
        if (!r.toMap()["legacy"].toBool() && r.toMap()["status"].toString() == "FINALIZED")
            ++done;
    const int total = state["total_rounds"].toInt();
    const bool terminated = state["terminated"].toBool();
    sub->setText(QStringLiteral("Commander  ·  %1 active players  ·  %2 of %3 round%4 complete")
                     .arg(state["active_count"].toInt()).arg(done).arg(total).arg(total == 1 ? "" : "s")
                 + (terminated ? QStringLiteral("  ·  Terminated") : QString()));
    const QVariantMap fin = state["final"].toMap();
    const bool finalShown = showingFinal();
    title->setText(T::breakable(finalShown ? QStringLiteral("Top-four final — ") + name
                                : current.isEmpty() ? name : cdb::stageLabel(current) + QStringLiteral(" — ") + name));
    T::hugText(title);
    printBtn->setEnabled(!current.isEmpty() || finalShown);

    // the clock belongs to the round or final in play and is saved with it; a final's is
    // remembered as a negative number so it is never mistaken for a round's
    const bool active = !finalShown && !current.isEmpty() && current["status"].toString() == "ACTIVE"
                        && !current["legacy"].toBool() && stage == "SWISS" && !terminated;
    const bool finalLive = finalShown && fin["status"].toString() == "PLAYING" && stage == "SWISS" && !terminated;
    const qint64 wanted = finalLive ? -(fin["final_id"].toLongLong() + 1) : active ? current["round_id"].toLongLong() : 0;
    if (wanted != timerRound_) {
        T::clear(timerSlot_);
        MainWindow *mw = mw_;
        if (finalLive) {
            const QString message = QStringLiteral("Time expired — %1, top-four final. Nothing is decided automatically: "
                                                   "record how the final ended.").arg(name);
            timerSlot_->addWidget(new TimerWidget(timerdb::Kind::CommanderFinal, fin["final_id"].toLongLong(),
                                                  [mw, message] { mw->roundAlert(message); }));
        } else if (active) {
            const QString message = QStringLiteral("Time expired — %1, %2. Results are not finalized automatically.")
                                        .arg(name, cdb::stageLabel(current));
            timerSlot_->addWidget(new TimerWidget(timerdb::Kind::Commander, wanted, [mw, message] { mw->roundAlert(message); }));
        }
        timerRound_ = wanted;
    }

    T::ScrollKeeper keep(this);         // reporting a result must not move the page
    grids_.clear();
    podCards_.clear();
    podErrors_.clear();
    buildStrip();
    T::clear(body_);
    T::clear(footer_);
    if (stage == "COMPLETE") {
        const QString first = ev["champion_name"].toString();
        QString how;
        if (fin["status"].toString() == "CONFIRMED")
            how = fin["outcome"].toString() == "WIN" ? QStringLiteral(" (won the final)")
                                                    : QStringLiteral(" (no winner in the final: placed by qualifying seed)");
        body_->addWidget(banner("Tournament complete." + (first.isEmpty() ? QString() : "  1st place: " + first + how),
                                T::GOLD, "trophy"));
    }
    if (terminated)
        body_->addWidget(terminatedNotice(tdb::tournamentById(tournamentId_)["terminated_at"].toString()));
    else if (state["legacy"].toMap()["flagged"].toBool())
        buildLegacyNotice();
    if (finalShown)
        buildFinal();
    else if (!current.isEmpty())
        buildRound(current);
    if (!terminated && stage != "COMPLETE" && stage != "REGISTRATION") {
        // apart from the round's routine actions in the bar below, so it is not pressed by accident
        auto *apart = new QWidget;
        auto *av = new QVBoxLayout(apart);
        av->setContentsMargins(0, 16, 0, 0);
        av->setSpacing(12);
        av->addWidget(T::hline());
        endEarlyBtn = endEarlyButton();
        connect(endEarlyBtn, &QPushButton::clicked, this, [this] { endTournamentEarly(mw_, this, tournamentId_); });
        av->addWidget(endEarlyBtn, 0, Qt::AlignLeft);
        body_->addWidget(apart);
    }
    if (stage == "COMPLETE") {
        QPushButton *results = T::button("View final standings", "primary", "trophy");
        connect(results, &QPushButton::clicked, this, [this] { mw_->navigateTo("standings", {{"tournament_id", tournamentId_}}); });
        footer_->addWidget(results);
    }
    footerHolder_->setVisible(footer_->count() > 0);

    if (terminated || stage == "COMPLETE") {
        mw_->removeTournamentTab(tournamentId_);
    } else if (!rounds.isEmpty()) {
        mw_->addTournamentTab(tournamentId_, "MTG", name);
        mw_->updateTournamentTab(tournamentId_);
    }
}

bool CommanderEventScreen::showingFinal() const
{
    return !state["final"].toMap().isEmpty() && (view_ == FINAL_VIEW || view_ == 0);
}

QVariantMap CommanderEventScreen::currentRound() const
{
    const QVariantList rounds = state["rounds"].toList();
    if (rounds.isEmpty())
        return {};
    if (view_) {
        for (const QVariant &r : rounds)
            if (r.toMap()["round_number"].toInt() == view_)
                return r.toMap();
    }
    // the latest scheduled round; playoff rounds kept from an earlier version are only shown on request
    for (int i = rounds.size() - 1; i >= 0; --i)
        if (!rounds[i].toMap()["legacy"].toBool())
            return rounds[i].toMap();
    return rounds.last().toMap();
}

// One chip per configured round — the whole event.  Nothing follows the last one.
void CommanderEventScreen::buildStrip()
{
    T::clear(strip_);
    const int shown = showingFinal() ? 0 : currentRound()["round_number"].toInt();
    QLabel *label = T::caps("Rounds");
    label->setMinimumHeight(T::MIN_HIT);
    strip_->addWidget(label);
    QHash<int, QVariantMap> byNumber;
    for (const QVariant &r : state["rounds"].toList())
        if (!r.toMap()["legacy"].toBool())
            byNumber.insert(r.toMap()["round_number"].toInt(), r.toMap());
    for (int n = 1; n <= state["total_rounds"].toInt(); ++n) {
        const QString name = QStringLiteral("Round %1").arg(n);
        if (!byNumber.contains(n)) {
            QPushButton *b = chipButton(QString::number(n), false, T::MUTED, false);
            b->setAccessibleName(name + ", not started yet");
            strip_->addWidget(b);
            continue;
        }
        const bool final = byNumber[n]["status"].toString() == "FINALIZED";
        QPushButton *b = chipButton((final ? QStringLiteral("✓ ") : QStringLiteral("● ")) + QString::number(n), shown == n,
                                    final ? T::GREEN : T::PURPLE_LT, true);
        b->setAccessibleName(name + (final ? ", final" : ", in progress"));
        connect(b, &QPushButton::clicked, this, [this, n] { view_ = n; refresh(); });
        strip_->addWidget(b);
    }
    if (state["final_enabled"].toBool()) {
        const QVariantMap fin = state["final"].toMap();
        if (fin.isEmpty()) {
            QPushButton *b = chipButton("Final", false, T::MUTED, false);
            b->setAccessibleName("Top-four final, not created yet");
            strip_->addWidget(b);
        } else {
            const bool done = fin["status"].toString() == "CONFIRMED";
            QPushButton *b = chipButton((done ? QStringLiteral("✓ ") : QStringLiteral("● ")) + "Final", showingFinal(),
                                        done ? T::GREEN : T::PURPLE_LT, true);
            b->setAccessibleName(done ? "Top-four final, confirmed" : "Top-four final, in play");
            connect(b, &QPushButton::clicked, this, [this] { view_ = FINAL_VIEW; refresh(); });
            strip_->addWidget(b);
        }
    }
    for (const QVariant &rv : state["rounds"].toList()) {
        const QVariantMap r = rv.toMap();
        if (!r["legacy"].toBool())
            continue;
        const int n = r["round_number"].toInt();
        const QString name = r["stage"].toString() == "SEMIFINAL" ? "Semifinals" : "Final pod";
        QPushButton *b = chipButton(name + QStringLiteral(" · earlier version"), shown == n, T::GREY, true);
        b->setAccessibleName(name + " from an earlier version, read-only record");
        connect(b, &QPushButton::clicked, this, [this, n] { view_ = n; refresh(); });
        strip_->addWidget(b);
    }
}

QPushButton *CommanderEventScreen::chipButton(const QString &text, bool selected, const QString &color, bool enabled)
{
    auto *b = new QPushButton(text);
    b->setMinimumSize(T::MIN_HIT, T::MIN_HIT);
    b->setEnabled(enabled);
    if (enabled)
        b->setCursor(QCursor(Qt::PointingHandCursor));
    b->setStyleSheet("QPushButton{background:" + T::alpha(color, 34) + ";color:" + color + ";border:2px solid "
                     + (selected ? T::TEXT : T::alpha(color, 120)) + ";border-radius:" + QString::number(T::MIN_HIT / 2)
                     + "px;font-size:" + T::P(12) + "px;font-weight:700;padding:0 " + T::P(12) + "px;min-height:"
                     + QString::number(T::MIN_HIT - 4) + "px;}"
                     "QPushButton:hover{border-color:" + T::TEXT + ";}"
                     "QPushButton:focus{border-color:" + T::FOCUS + ";}"
                     "QPushButton:disabled{background:transparent;color:" + T::DIM + ";border:2px dashed " + T::BORDER2 + ";}");
    return b;
}

QWidget *CommanderEventScreen::banner(const QString &text, const QString &color, const QString &icon)
{
    QFrame *f = T::panel("bn", color, 2);
    auto *h = new QHBoxLayout(f);
    h->setContentsMargins(18, 12, 18, 12);
    h->setSpacing(8);
    if (!icon.isEmpty())
        h->addWidget(T::iconLabel(icon, color, 20));
    h->addWidget(T::lbl(text, color, 15, 700, true), 1);
    return f;
}

// Events that reached a playoff before playoffs were removed: say so, and offer the way to close them.
void CommanderEventScreen::buildLegacyNotice()
{
    const QVariantMap legacy = state["legacy"].toMap();
    const int total = state["total_rounds"].toInt();
    const int kept = int(legacy["playoff_rounds"].toList().size());
    const QString record = kept ? QStringLiteral("The playoff round%1 already created %2 kept as a read-only record. ")
                                      .arg(kept != 1 ? "s" : "", kept != 1 ? "are" : "is")
                                : QString();
    const bool needsFinish = legacy["needs_finish"].toBool();
    const QString text = needsFinish
        ? QStringLiteral("This tournament was set up with a Top %1 playoff in an earlier version. Tournaments now end "
                         "after their last round, so no playoff will be run. %2Finish the tournament to save the final "
                         "standings from Round%3.")
              .arg(legacy["cut"].toInt()).arg(record, total > 1 ? QStringLiteral("s 1–%1").arg(total) : QStringLiteral(" 1"))
        : QStringLiteral("This tournament was finished in an earlier version with a Top %1 playoff. %2Its saved "
                         "placings are unchanged.").arg(legacy["cut"].toInt()).arg(record);
    QFrame *f = T::panel("lg", T::AMBER, 2);
    auto *v = new QVBoxLayout(f);
    v->setContentsMargins(18, 12, 18, 12);
    v->setSpacing(4);
    v->addWidget(T::lbl("Earlier-version playoff", T::AMBER, 12, 700, false, false, 1.0));
    v->addWidget(T::lbl(text, T::TEXT, 13, 400, true));
    body_->addWidget(f);
    if (needsFinish) {
        QPushButton *fin = T::button("Finish tournament", "primary");
        connect(fin, &QPushButton::clicked, this, [this] {
            if (confirm(this, "Finish tournament",
                        QStringLiteral("Finish %1 on the standings after Round %2?\n\nNo playoff will be run. Playoff "
                                       "rounds already created stay on record but do not count.")
                            .arg(state["event"].toMap()["name"].toString()).arg(state["total_rounds"].toInt())))
                completeWith([this] { cdb::finishLegacyPlayoff(tournamentId_); });
        });
        footer_->addWidget(fin);
    }
}

void CommanderEventScreen::buildRound(const QVariantMap &rnd)
{
    const QVariantMap ev = state["event"].toMap();
    const bool legacy = rnd["legacy"].toBool();
    int latestNumber = 0;
    for (const QVariant &r : state["rounds"].toList())
        if (!r.toMap()["legacy"].toBool())
            latestNumber = r.toMap()["round_number"].toInt();
    const int n = rnd["round_number"].toInt();
    const bool latest = !legacy && n == latestNumber;
    const bool active = rnd["status"].toString() == "ACTIVE" && !legacy && ev["stage"].toString() == "SWISS"
                        && !state["terminated"].toBool();

    roundBar_ = new QWidget;
    auto *bar = new QHBoxLayout(roundBar_);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(6);
    body_->addWidget(roundBar_);

    const QHash<qint64, double> pts = points();
    QList<QWidget *> cards;
    podCards_.clear();
    cardsTiny_ = tiny;
    for (const QVariant &pv : rnd["pods"].toList()) {
        QWidget *card = podCard(pv.toMap(), rnd, pts);
        podCards_.insert(pv.toMap()["pod_id"].toLongLong(), card);
        cards << card;
    }
    auto *grid = new QGridLayout;
    grid->setSpacing(12);
    grids_.append({QPointer<QGridLayout>(grid), cards});
    fillGrid(grid, cards);
    body_->addLayout(grid);

    for (const QVariant &bv : rnd["byes"].toList()) {
        QFrame *f = T::panel("by", T::GREEN, 2);
        auto *h = new QHBoxLayout(f);
        h->setContentsMargins(18, 12, 18, 12);
        h->addWidget(T::lbl("BYE", T::MUTED, 11, 700, false, true));
        h->addWidget(T::lbl(bv.toMap()["display_name"].toString(), T::TEXT, 14, 600, true), 1);
        h->addWidget(T::pill(QStringLiteral("Bye  +%1").arg(ev["settings"].toMap()["bye_points"].toInt()), T::GREEN, false));
        body_->addWidget(f);
    }

    roundNotes_ = new QWidget;
    auto *notes = new QVBoxLayout(roundNotes_);
    notes->setContentsMargins(0, 0, 0, 0);
    notes->setSpacing(6);
    body_->addWidget(roundNotes_);
    fillRoundStatus(rnd);
    if (active)
        return;
    if (latest && state["next"].toString() == "FINAL") {
        buildFinalCreate();
    } else if (latest && state["next"].toString() == "SWISS") {
        if (state["can_pair"].toBool()) {
            body_->addWidget(T::lbl(QStringLiteral("Round %1 is final. Drops made now apply to Round %2.").arg(n).arg(n + 1),
                                    T::MUTED, 12, 400, true));
            QPushButton *next = T::button(QStringLiteral("Start Round %1").arg(n + 1), "primary", "play");
            next->setToolTip(QStringLiteral("Generates and publishes the Round %1 pods").arg(n + 1));
            connect(next, &QPushButton::clicked, this, [this, n] { publish(n + 1); });
            footer_->addWidget(next);
        } else {
            body_->addWidget(banner("Fewer than three active players remain, so another pod cannot be formed.", T::RED));
            QPushButton *end = T::button("Finish tournament on current standings");
            connect(end, &QPushButton::clicked, this, [this] {
                if (confirm(this, "Finish tournament", "Too few active players remain to form a pod. "
                                                       "Finish the tournament on the current standings?"))
                    completeWith([this] { cdb::endSwissEarly(tournamentId_); });
            });
            footer_->addWidget(end);
        }
    }
    footerHolder_->setVisible(footer_->count() > 0);
}

// redrawn on its own after each pod save, so the cards above it keep their place
void CommanderEventScreen::fillRoundStatus(const QVariantMap &rnd)
{
    const QVariantMap ev = state["event"].toMap();
    const bool legacy = rnd["legacy"].toBool();
    const bool terminated = state["terminated"].toBool();
    const bool active = rnd["status"].toString() == "ACTIVE" && !legacy && ev["stage"].toString() == "SWISS" && !terminated;
    const int n = rnd["round_number"].toInt();
    const int total = int(rnd["pods"].toList().size());
    const int pending = rnd["pending"].toInt();
    const int done = total - pending;
    QStringList secondPods;
    for (const QVariant &p : state["second_pending"].toList())
        if (p.toMap()["round_number"].toInt() == n)
            secondPods << p.toMap()["pod_number"].toString();
    const QString secondsMissing = QStringLiteral("Second place is not recorded yet in pod%1 %2.")
                                       .arg(secondPods.size() == 1 ? "" : "s", secondPods.join(", "));

    if (roundBar_) {
        QLayout *bar = roundBar_->layout();
        T::clear(bar);
        for (int i = 0; i < qMin(total, 60); ++i) {
            auto *seg = new QFrame;
            seg->setFixedHeight(5);
            seg->setStyleSheet("background:" + (i < done ? T::GREEN : T::BORDER2) + ";border:none;border-radius:2px;");
            bar->addWidget(seg);
            static_cast<QHBoxLayout *>(bar)->setStretchFactor(seg, 1);
        }
        const QString text = legacy ? QStringLiteral("Read-only record · not counted")
                     : active ? QStringLiteral("%1 / %2 pods reported").arg(done).arg(total)
                     : terminated && pending ? QStringLiteral("%1 / %2 pods reported · round not finished").arg(done).arg(total)
                                             : QStringLiteral("Results final");
        bar->addWidget(T::lbl(text, legacy ? T::GREY : active || (terminated && pending) ? T::MUTED : T::GREEN, 12));
    }

    for (const QPointer<QWidget> &w : std::as_const(roundFooter_))
        if (w) {
            footer_->removeWidget(w);
            w->hide();
            w->deleteLater();
        }
    roundFooter_.clear();
    if (roundNotes_) {
        auto *notes = static_cast<QVBoxLayout *>(roundNotes_->layout());
        T::clear(notes);
        if (active) {
            const int roundsTotal = state["total_rounds"].toInt();
            const bool withFinal = state["final_enabled"].toBool();
            const bool ready = pending == 0 && secondPods.isEmpty();
            if (!secondPods.isEmpty())
                notes->addWidget(T::lbl(QStringLiteral("Points are provisional. ") + secondsMissing
                                            + " Choose the second-place player, or No clear second, before the round is "
                                              "finalized.",
                                        T::AMBER, 12, 400, true));
            QPushButton *fin;
            if (n >= roundsTotal && withFinal) {
                notes->addWidget(T::lbl(QStringLiteral("Round %1 is the last qualifying round. The top-four final follows.").arg(n),
                                        T::MUTED, 12, 400, true));
                fin = T::button(QStringLiteral("Finalize Round %1").arg(n), "primary", "arrow-right", true);
                connect(fin, &QPushButton::clicked, this, [this, rnd] { finalizeRound(rnd); });
            } else if (n >= roundsTotal) {
                // the last configured round: finalizing it finishes the tournament, nothing follows
                notes->addWidget(T::lbl(QStringLiteral("Round %1 is the last round. Finishing saves the final standings.").arg(n),
                                        T::MUTED, 12, 400, true));
                fin = T::button("Finish tournament", "primary");
                connect(fin, &QPushButton::clicked, this, [this, rnd] { finish(rnd); });
            } else {
                notes->addWidget(T::lbl("Results are provisional until the round is finalized.", T::MUTED, 12, 400, true));
                fin = T::button(QStringLiteral("Finalize Round %1").arg(n), "primary", "arrow-right", true);
                connect(fin, &QPushButton::clicked, this, [this, rnd] { finalizeRound(rnd); });
            }
            fin->setEnabled(ready);
            fin->setToolTip(ready ? "" : pending ? QStringLiteral("Every pod needs a result first.") : secondsMissing);
            footer_->addWidget(fin);
            roundFooter_ << fin;
        }
        roundNotes_->setVisible(notes->count() > 0);
    }
    footerHolder_->setVisible(footer_->count() > 0);
}

QHash<qint64, double> CommanderEventScreen::points() const
{
    QHash<qint64, double> out;
    for (const QVariant &r : state["standings"].toList())
        out.insert(r.toMap()["player_id"].toLongLong(), r.toMap()["points"].toDouble());
    return out;
}

// a name inside a check box or radio button, where an ampersand would otherwise become a shortcut
static QString buttonText(const QString &text)
{
    return QString(text).replace('&', "&&");
}

QWidget *CommanderEventScreen::podCard(const QVariantMap &pod, const QVariantMap &rnd, const QHash<qint64, double> &points)
{
    const bool reported = pod["reported"].toBool();
    const QString outcome = pod["outcome"].toString();
    const bool legacy = rnd["legacy"].toBool();
    const QVariantMap ev = state["event"].toMap();
    const QVariantMap settings = ev["settings"].toMap();
    const bool terminated = state["terminated"].toBool();
    const bool editable = rnd["status"].toString() == "ACTIVE" && !legacy && ev["stage"].toString() == "SWISS" && !terminated;
    const QVariantList seats = pod["seats"].toList();
    const qint64 podId = pod["pod_id"].toLongLong();
    const int podNumber = pod["pod_number"].toInt();
    const int version = pod["result_version"].toInt();
    const bool scoring = state["scores_second"].toBool();
    const QString secondState = pod["second_place_status"].toString();
    const bool secondPending = scoring && reported && outcome == "WIN" && secondState == "PENDING";
    const QString winPoints = cmdr::pointsText(settings["win_points"].toDouble());
    const QString secondPoints = cmdr::pointsText(settings["second_place_points"].toDouble());
    qint64 winnerId = 0;
    QString winnerName;
    for (const QVariant &s : seats)
        if (s.toMap()["result"].toString() == "WIN") {
            winnerId = s.toMap()["player_id"].toLongLong();
            winnerName = s.toMap()["display_name"].toString();
        }

    QFrame *card = T::panel("pc", !reported ? T::BORDER : secondPending || outcome != "WIN" ? T::AMBER : T::GREEN, 2);
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(16, 13, 16, 13);
    v->setSpacing(8);

    auto *head = new QHBoxLayout;
    head->addWidget(T::lbl(QStringLiteral("POD %1").arg(podNumber), T::TEXT, 13, 700, false, false, 1.0));
    head->addWidget(T::lbl(QStringLiteral("%1 players").arg(seats.size()), T::MUTED, 11));
    head->addStretch();
    v->addLayout(head);
    // the state in words on its own line, so a long winner name wraps and colour is never the only cue
    QString statusText, statusColor;
    if (!reported) {
        statusText = editable ? QStringLiteral("●  No result yet") : QStringLiteral("No result recorded");
        statusColor = editable ? T::PURPLE_LT : T::GREY;
    } else {
        statusText = outcome == "WIN" ? QStringLiteral("✓  ") + winnerName + " won" : QStringLiteral("=  Draw, no winner");
        statusColor = outcome == "WIN" ? T::GREEN : T::AMBER;
        if (secondPending) {
            statusText += QStringLiteral("  ·  2nd place still to choose, points provisional");
            statusColor = T::AMBER;
        } else if (editable) {
            statusText += QStringLiteral("  ·  saved, provisional until the round is finalized");
        } else if (!legacy) {
            statusText += QStringLiteral("  ·  final");
        }
    }
    QLabel *status = T::lbl(statusText, statusColor, 12, 700, true);
    status->setObjectName("podStatus");
    v->addWidget(status);
    v->addWidget(T::hline());

    for (const QVariant &sv : seats) {
        const QVariantMap s = sv.toMap();
        const qint64 pid = s["player_id"].toLongLong();
        const QString name = s["display_name"].toString();
        const QString result = s["result"].toString();
        const int seat = s["seat"].toInt();
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        QLabel *badge = T::lbl(QStringLiteral("SEAT %1").arg(seat), seat == 1 ? T::PURPLE_LT : T::MUTED, 10, 700, false, true);
        badge->setFixedWidth(T::px(48));
        if (seat == 1)
            badge->setToolTip("Seat 1 takes the first turn.");
        row->addWidget(badge);
        auto *names = new QVBoxLayout;
        names->setSpacing(0);
        names->addWidget(T::lbl(name, reported && result == "LOSS" ? T::MUTED : T::TEXT, 14, 600, true));
        if (legacy) {
            if (s["playoff_seed"].toInt())
                names->addWidget(T::lbl(QStringLiteral("seed %1").arg(s["playoff_seed"].toInt()), T::MUTED, 11, 400, false, true));
        } else {
            names->addWidget(T::lbl(cmdr::pointsText(points.value(pid)) + " pts", T::MUTED, 11, 400, false, true));
        }
        // under the name, so a narrow window or large text never has to make the row wider
        if (reported && pid == pod["second_place_player_id"].toLongLong()) {
            QLabel *second = T::pill(scoring ? "2nd  +" + secondPoints : QStringLiteral("2nd"), T::PURPLE_LT, false);
            second->setToolTip(scoring ? "Clear second place: " + secondPoints + " points. It still counts as a loss."
                                       : QStringLiteral("Second place, recorded for recognition. No extra points."));
            second->setAccessibleName(QStringLiteral("Second place in pod %1").arg(podNumber));
            names->addSpacing(4);
            names->addWidget(second, 0, Qt::AlignLeft);
        }
        row->addLayout(names, 1);
        // in a very narrow window the action goes under the name, so the row never outgrows the card
        const auto placeAction = [this, row, names](QPushButton *b) {
            if (tiny) {
                names->addSpacing(4);
                names->addWidget(b, 0, Qt::AlignLeft);
            } else {
                row->addWidget(b);
            }
        };
        if (secondPending && editable && pid != winnerId) {
            QPushButton *second = smallButton("2nd  +" + secondPoints);
            second->setObjectName("second");
            second->setAccessibleName(QStringLiteral("%1 second in pod %2").arg(name).arg(podNumber));
            connect(second, &QPushButton::clicked, this, [this, podId, winnerId, pid, version] {
                saveFromCard(podId, [=] {
                    cdb::reportPodResult(podId, "WIN", winnerId, {}, version, cdb::SecondPlace(pid));
                }, "correct");
            });
            placeAction(second);
        } else if (reported) {
            if (result == "WIN")
                row->addWidget(T::lbl(legacy ? QString("WIN") : "WIN  +" + winPoints, T::GREEN, 11, 700));
            else if (result == "DRAW")
                row->addWidget(T::lbl(legacy ? QString("DRAW") : "DRAW  +" + cmdr::pointsText(settings["draw_points"].toDouble()),
                                      T::AMBER, 11, 700));
            else
                row->addWidget(T::lbl(outcome == "DRAW" ? "OUT" : "LOSS", T::MUTED, 11, 700));
        } else if (editable) {
            QPushButton *win = smallButton("Winner  +" + winPoints);
            win->setObjectName("winner");
            win->setAccessibleName(QStringLiteral("%1 wins pod %2").arg(name).arg(podNumber));
            connect(win, &QPushButton::clicked, this, [this, podId, pid, version, scoring] {
                saveFromCard(podId, [=] { cdb::reportPodResult(podId, "WIN", pid, {}, version); },
                             scoring ? "second" : "correct");
            });
            placeAction(win);
        }
        v->addLayout(row);
    }
    // a won pod's second place in an event that scores it: decided, or visibly still to decide
    if (secondPending) {
        QLabel *missing = T::lbl(QStringLiteral("2nd place not recorded yet. Choose 2nd above, or No clear second if the last "
                                                "players were knocked out at the same moment."),
                                 T::AMBER, 12, 700, true);
        missing->setAccessibleName(QStringLiteral("Second place not recorded yet in pod %1").arg(podNumber));
        v->addWidget(missing);
    } else if (scoring && reported && outcome == "WIN" && secondState == "NONE") {
        v->addWidget(T::lbl("No clear second place", T::MUTED, 12, 400, true));
    }

    // a draw is entered on the card too: who was already knocked out, then Save draw
    QFrame *drawBox = nullptr;
    QHash<qint64, QCheckBox *> outBoxes;
    if (editable && !reported) {
        drawBox = T::panel("pd", T::AMBER, 1, 10);
        auto *dv = new QVBoxLayout(drawBox);
        dv->setContentsMargins(12, 10, 12, 10);
        dv->setSpacing(6);
        dv->addWidget(T::lbl("Draw, not saved yet", T::AMBER, 12, 700, true));
        dv->addWidget(T::lbl(settings["draw_policy"].toString() == "ELIMINATED_LOSE"
                                 ? QStringLiteral("Tick anyone knocked out before the draw: they lose, the others draw.")
                                 : QStringLiteral("Tick anyone knocked out before the draw. Everyone still draws."),
                             T::MUTED, 12, 400, true));
        for (const QVariant &sv : seats) {
            const QVariantMap s = sv.toMap();
            auto *cb = new QCheckBox(buttonText(T::breakable(s["display_name"].toString())));
            cb->setAccessibleName(QStringLiteral("%1 was knocked out before the draw").arg(s["display_name"].toString()));
            outBoxes.insert(s["player_id"].toLongLong(), cb);
            dv->addWidget(cb);
        }
        drawBox->hide();
        v->addWidget(drawBox);
    }

    QLabel *error = T::lbl(podErrors_.contains(podId) ? "Not saved: " + podErrors_.value(podId) : QString(), T::RED, 12, 700, true);
    error->setObjectName("podError");
    error->setVisible(podErrors_.contains(podId));
    v->addWidget(error);

    if (legacy || terminated)
        return card;        // a read-only record: nothing can be reported or corrected
    auto *acts = new QWidget;
    auto *al = new T::FlowLayout(acts, 8);
    if (editable && !reported) {
        QPushButton *draw = smallButton(QStringLiteral("Draw…"));
        draw->setObjectName("draw");
        draw->setAccessibleName(QStringLiteral("Report a draw in pod %1").arg(podNumber));
        QPushButton *cancel = smallButton("Cancel draw");
        cancel->setAccessibleName(QStringLiteral("Cancel the draw in pod %1").arg(podNumber));
        QPushButton *save = smallButton("Save draw");
        save->setAccessibleName(QStringLiteral("Save the draw in pod %1").arg(podNumber));
        cancel->hide();
        save->hide();
        const auto show = [drawBox, draw, cancel, save, outBoxes](bool open) {
            drawBox->setVisible(open);
            draw->setVisible(!open);
            cancel->setVisible(open);
            save->setVisible(open);
            if (!open)
                for (QCheckBox *cb : outBoxes)
                    cb->setChecked(false);
            (open ? static_cast<QWidget *>(save) : draw)->setFocus();
        };
        connect(draw, &QPushButton::clicked, this, [show] { show(true); });
        connect(cancel, &QPushButton::clicked, this, [show] { show(false); });
        connect(save, &QPushButton::clicked, this, [this, podId, version, outBoxes] {
            QList<qint64> out;
            for (auto it = outBoxes.begin(); it != outBoxes.end(); ++it)
                if (it.value()->isChecked())
                    out << it.key();
            saveFromCard(podId, [=] { cdb::reportPodResult(podId, "DRAW", 0, out, version); }, "correct");
        });
        for (QPushButton *b : {draw, cancel, save})
            al->addWidget(b);
    } else if (editable) {
        if (secondPending) {
            QPushButton *none = smallButton("No clear second");
            none->setObjectName("nosecond");
            none->setAccessibleName(QStringLiteral("No clear second place in pod %1").arg(podNumber));
            connect(none, &QPushButton::clicked, this, [this, podId, winnerId, version] {
                saveFromCard(podId, [=] {
                    cdb::reportPodResult(podId, "WIN", winnerId, {}, version, cdb::SecondPlace::noClearSecond());
                }, "correct");
            });
            al->addWidget(none);
        }
        QPushButton *change = smallButton(QStringLiteral("Correct…"), false, "pencil");
        change->setObjectName("correct");
        change->setAccessibleName(QStringLiteral("Correct the result of pod %1").arg(podNumber));
        connect(change, &QPushButton::clicked, this, [this, pod] { correctLive(pod); });
        QPushButton *clearBtn = smallButton(QStringLiteral("Clear…"), true);
        clearBtn->setAccessibleName(QStringLiteral("Clear the result of pod %1").arg(podNumber));
        connect(clearBtn, &QPushButton::clicked, this, [this, pod] { clearPod(pod); });
        al->addWidget(change);
        al->addWidget(clearBtn);
    } else if (reported) {
        QPushButton *fix = smallButton(QStringLiteral("Correct result…"), false, "pencil");
        fix->setAccessibleName(QStringLiteral("Correct the result of pod %1").arg(podNumber));
        connect(fix, &QPushButton::clicked, this, [this, pod] { correct(pod); });
        al->addWidget(fix);
    }
    v->addWidget(acts);
    return card;
}

void CommanderEventScreen::saveFromCard(qint64 podId, const std::function<void()> &save, const QString &focusName)
{
    try {
        save();
    } catch (const cdb::ConflictError &e) {
        // the card no longer shows what is saved, so it is redrawn from the database
        podErrors_.insert(podId, e.message);
        updatePod(podId);
        return;
    } catch (const cdb::CommanderError &e) {
        // nothing was saved: the card keeps what was entered on it
        QWidget *card = podCards_.value(podId);
        QLabel *error = card ? card->findChild<QLabel *>("podError") : nullptr;
        if (!error) {
            warn(this, "Result", e.message);
            return;
        }
        error->setText(T::breakable("Not saved: " + e.message));
        error->show();
        return;
    }
    podErrors_.remove(podId);
    updatePod(podId, focusName);
}

void CommanderEventScreen::updatePod(qint64 podId, const QString &focusName)
{
    T::ScrollKeeper keep(this);
    const QPointer<QWidget> old = podCards_.value(podId);
    try {
        state = cdb::getState(tournamentId_);
    } catch (const cdb::CommanderError &) {
        refresh();
        return;
    }
    const QVariantMap rnd = currentRound();
    QVariantMap pod;
    for (const QVariant &p : rnd["pods"].toList())
        if (p.toMap()["pod_id"].toLongLong() == podId)
            pod = p.toMap();
    if (pod.isEmpty() || !old || showingFinal()) {
        refresh();
        return;
    }
    QWidget *fresh = podCard(pod, rnd, points());
    bool placed = false;
    for (auto &g : grids_) {
        const qsizetype i = g.second.indexOf(old.data());
        if (i < 0 || !g.first)
            continue;
        delete g.first->replaceWidget(old, fresh);
        g.second[i] = fresh;
        placed = true;
    }
    if (!placed) {
        delete fresh;
        refresh();
        return;
    }
    podCards_.insert(podId, fresh);
    fresh->show();
    // off the page before it goes, so focus and scroll are never put back onto it
    old->hide();
    old->setParent(nullptr);
    old->deleteLater();
    fillRoundStatus(rnd);
    mw_->updateTournamentTab(tournamentId_);
    if (!focusName.isEmpty())
        for (QPushButton *b : fresh->findChildren<QPushButton *>(focusName))
            if (b->isEnabled() && !b->isHidden()) {
                b->setFocus(Qt::OtherFocusReason);
                break;
            }
}

template <typename F> void CommanderEventScreen::guard(F fn, const QString &title)
{
    try {
        fn();
    } catch (const cdb::ConflictError &e) {
        warn(this, "Changed elsewhere", e.message);
    } catch (const cdb::CommanderError &e) {
        warn(this, title, e.message);
    }
    refresh();
}

// Runs something that ends the event, then shows the final standings.
template <typename F> void CommanderEventScreen::completeWith(F fn)
{
    view_ = 0;
    try {
        fn();
    } catch (const cdb::CommanderError &e) {
        warn(this, "Finish tournament", e.message);
        refresh();
        return;
    }
    refresh();
    if (state["event"].toMap()["stage"].toString() == "COMPLETE")
        mw_->navigateTo("standings", {{"tournament_id", tournamentId_}});
}

void CommanderEventScreen::report(const QVariantMap &pod, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminated,
                                  const cdb::SecondPlace &second)
{
    guard([&] {
        cdb::reportPodResult(pod["pod_id"].toLongLong(), outcome, winnerId, eliminated, pod["result_version"].toInt(), second);
    }, "Result");
}

// a saved result in a round still in play: the full choice, with the reason a correction needs
void CommanderEventScreen::correctLive(const QVariantMap &pod)
{
    const qint64 podId = pod["pod_id"].toLongLong();
    ResultDialog d(this, pod, QStringLiteral("Correct Pod %1").arg(pod["pod_number"].toInt()),
                   state["event"].toMap()["settings"].toMap()["draw_policy"].toString(), false, true,
                   state["scores_second"].toBool());
    while (d.exec() == QDialog::Accepted) {
        try {
            cdb::reportPodResult(podId, d.outcome(), d.winner(), d.eliminated(), pod["result_version"].toInt(),
                                 d.secondPlace(), d.reason());
        } catch (const cdb::ConflictError &e) {
            warn(this, "Changed elsewhere", e.message);
            break;
        } catch (const cdb::CommanderError &e) {
            d.showError(e.message);
            continue;
        }
        podErrors_.remove(podId);
        updatePod(podId, "correct");
        return;
    }
    updatePod(podId, "correct");
}

void CommanderEventScreen::clearPod(const QVariantMap &pod)
{
    const qint64 podId = pod["pod_id"].toLongLong();
    const int version = pod["result_version"].toInt();
    const int number = pod["pod_number"].toInt();
    QString reason;
    if (!askReason(this, QStringLiteral("Clear Pod %1").arg(number),
                   QStringLiteral("The saved result of pod %1 is removed and its points are taken back. The pod then "
                                  "waits for a new result.").arg(number),
                   "Clear result", &reason))
        return;
    saveFromCard(podId, [=] { cdb::clearPodResult(podId, version, reason); }, "winner");
}

void CommanderEventScreen::correct(const QVariantMap &pod)
{
    const bool scoring = state["scores_second"].toBool();
    ResultDialog d(this, pod, QStringLiteral("Correct Pod %1").arg(pod["pod_number"].toInt()),
                   state["event"].toMap()["settings"].toMap()["draw_policy"].toString(), false, true, scoring, scoring);
    const qint64 podId = pod["pod_id"].toLongLong();
    QHash<qint64, QString> names;
    for (const QVariant &p : state["players"].toList())
        names.insert(p.toMap()["player_id"].toLongLong(), p.toMap()["display_name"].toString());
    const auto namesOf = [&names](const QVariantList &ids) {
        QStringList out;
        for (const QVariant &id : ids)
            out << names.value(id.toLongLong(), "?");
        return out.join(", ");
    };
    // each answer the organizer gives is added and the correction tried again; nothing is saved before
    bool done = false;
    while (!done && d.exec() == QDialog::Accepted) {
        QString resolution;
        for (int asked = 0; asked < 3; ++asked) {
            try {
                cdb::correctResult(podId, d.outcome(), d.winner(), d.eliminated(), resolution, d.reason(), d.secondPlace());
                done = true;
                break;
            } catch (const cdb::ResolutionRequired &e) {
                QMessageBox box(this);
                box.setWindowTitle("Later rounds depend on this result");
                box.setTextFormat(Qt::PlainText);
                box.setText(e.message);
                box.setInformativeText("Keep: standings are recalculated, but everything already published stays as it is.\n"
                                       "Rebuild: later rounds are discarded and redone from the corrected result "
                                       "(only possible while they have no results).");
                QPushButton *keep = box.addButton("Keep later rounds", QMessageBox::AcceptRole);
                QPushButton *rebuild = box.addButton("Rebuild later rounds", QMessageBox::DestructiveRole);
                box.addButton(QMessageBox::Cancel);
                box.exec();
                resolution = box.clickedButton() == keep ? "KEEP" : box.clickedButton() == rebuild ? "REBUILD" : "";
                if (resolution.isEmpty())
                    break;
                if (resolution == "REBUILD")
                    view_ = 0;
            } catch (const cdb::FinalAffected &e) {
                if (!confirm(this, "The final would change",
                             e.message + "\n\nFinalists now: " + namesOf(e.seedsBefore) + "\nAfter the correction: "
                                 + namesOf(e.seedsAfter)
                                 + "\n\nDiscard the final and save the correction? Later qualifying rounds stay as published, "
                                   "and the final is then created again from the corrected standings."))
                    break;
                resolution = "REBUILD_FINAL";
                view_ = 0;
            } catch (const cdb::ConflictError &e) {
                warn(this, "Changed elsewhere", e.message);
                done = true;
                break;
            } catch (const cdb::CommanderError &e) {
                d.showError(e.message);         // the dialog opens again with everything as entered
                break;
            }
        }
    }
    refresh();
}

// after the last qualifying round of an event with a final: who plays it, or why nobody can yet.
void CommanderEventScreen::buildFinalCreate()
{
    const QVariantMap candidates = state["candidates"].toMap();
    QFrame *f = T::panel("fc", T::PURPLE_LT, 2);
    auto *v = new QVBoxLayout(f);
    v->setContentsMargins(18, 14, 18, 14);
    v->setSpacing(6);
    v->addWidget(T::lbl("TOP-FOUR FINAL", T::PURPLE_LT, 12, 700, false, false, 1.0));
    v->addWidget(T::lbl(QStringLiteral("All %1 qualifying rounds are final. The four best players in the standings who are "
                                       "checked in and have not dropped play one final pod, seated in seed order: seed 1 "
                                       "takes the first turn. The final adds no points.").arg(state["total_rounds"].toInt()),
                        T::TEXT, 13, 400, true));
    for (const QVariant &pv : candidates["players"].toList()) {
        const QVariantMap p = pv.toMap();
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        QLabel *seed = T::lbl(QStringLiteral("SEED %1").arg(p["seed"].toInt()), T::PURPLE_LT, 10, 700, false, true);
        seed->setFixedWidth(T::px(56));
        row->addWidget(seed);
        row->addWidget(T::lbl(p["display_name"].toString(), T::TEXT, 14, 600, true), 1);
        row->addWidget(T::lbl(cmdr::pointsText(p["points"].toDouble()) + " pts", T::MUTED, 12));
        v->addLayout(row);
    }
    body_->addWidget(f);
    if (!candidates["ready"].toBool()) {
        body_->addWidget(banner(candidates["reason"].toString(), T::RED));
        return;
    }
    QPushButton *create = T::button(QStringLiteral("Create final…"), "primary", "trophy");
    create->setAccessibleName("Create the top-four final");
    connect(create, &QPushButton::clicked, this, [this] { createFinal(); });
    footer_->addWidget(create);
}

void CommanderEventScreen::createFinal()
{
    const QVariantMap candidates = cdb::finalCandidates(tournamentId_);       // as saved now, not as last shown
    if (!candidates["ready"].toBool()) {
        warn(this, "Top-four final", candidates["reason"].toString());
        refresh();
        return;
    }
    QStringList lines;
    QList<qint64> seeds;
    for (const QVariant &pv : candidates["players"].toList()) {
        const QVariantMap p = pv.toMap();
        seeds << p["player_id"].toLongLong();
        lines << QStringLiteral("Seed %1, seat %1: %2 (%3 pts)").arg(p["seed"].toInt()).arg(p["display_name"].toString(),
                                                                     cmdr::pointsText(p["points"].toDouble()));
    }
    if (!confirm(this, "Create the top-four final",
                 "Create the final with these players?\n\n" + lines.join('\n')
                     + "\n\nThe players and their seeds are fixed once the final is created."))
        return;
    view_ = FINAL_VIEW;
    guard([&] { cdb::createFinal(tournamentId_, seeds); }, "Top-four final");
}

static QString placementRules()
{
    return QStringLiteral("The first players knocked out place 4th, the next 3rd, and so on; the winner is 1st. Players "
                          "knocked out together are placed by qualifying seed among themselves. With a winner, the "
                          "players still in when the game ended lost together and follow the winner. With no winner, the "
                          "players still in are placed by seed. The final adds no points, and the clock never decides "
                          "the result.");
}

// the final pod: its four players with their seeds, the knockouts so far, and how it ended
void CommanderEventScreen::buildFinal()
{
    const QVariantMap fin = state["final"].toMap();
    const bool confirmed = fin["status"].toString() == "CONFIRMED";
    const bool terminated = state["terminated"].toBool();
    const bool live = !confirmed && !terminated && state["event"].toMap()["stage"].toString() == "SWISS";

    QFrame *card = T::panel("fp", confirmed ? T::GREEN : T::PURPLE_LT, 2);
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(16, 13, 16, 13);
    v->setSpacing(8);
    auto *head = new QHBoxLayout;
    head->addWidget(T::lbl("TOP-FOUR FINAL", T::TEXT, 13, 700, false, false, 1.0));
    head->addWidget(T::lbl("4 players", T::MUTED, 11));
    head->addStretch();
    v->addLayout(head);

    if (live) {
        v->addWidget(T::lbl("Record each knockout as it happens. When the game ends, choose the winner, check the places "
                            "and confirm.", T::TEXT, 13, 400, true));
        auto *editor = new FinalEditor(tournamentId_, fin, FinalEditor::Live);
        finalEditor_ = editor;
        v->addWidget(editor);
        body_->addWidget(card);
        connect(editor, &FinalEditor::knockoutRequested, this, [this](const QList<QList<qint64>> &groups) { knockout(groups); });
        connect(editor, &FinalEditor::undoRequested, this, [this] { undoKnockout(); });
        connect(editor, &FinalEditor::confirmRequested, this, [this, editor] { confirmFinalFrom(editor); });
        return;
    }

    QVariantList players = fin["players"].toList();
    std::sort(players.begin(), players.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["seat"].toInt() < b.toMap()["seat"].toInt();
    });
    QString winnerName;
    for (const QVariant &p : players)
        if (p.toMap()["player_id"].toLongLong() == fin["winner_player_id"].toLongLong())
            winnerName = p.toMap()["display_name"].toString();
    const QString status = confirmed ? (fin["outcome"].toString() == "WIN"
                                            ? QStringLiteral("✓  Final confirmed: ") + winnerName + " won"
                                            : QStringLiteral("Final confirmed: no winner, so the players still in are placed by seed"))
                                     : QStringLiteral("Ended early: the final was not finished and nobody was placed");
    v->addWidget(T::lbl(status, confirmed ? T::GREEN : T::GREY, 12, 700, true));
    v->addWidget(T::hline());
    for (const QVariant &pv : players) {
        const QVariantMap p = pv.toMap();
        const int seat = p["seat"].toInt();
        const int group = p["elimination_group"].toInt();
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        QLabel *badge = T::lbl(QStringLiteral("SEAT %1").arg(seat), seat == 1 ? T::PURPLE_LT : T::MUTED, 10, 700, false, true);
        badge->setFixedWidth(T::px(48));
        row->addWidget(badge);
        auto *names = new QVBoxLayout;
        names->setSpacing(0);
        names->addWidget(T::lbl(p["display_name"].toString(), T::TEXT, 14, 600, true));
        names->addWidget(T::lbl(QStringLiteral("Seed %1  ·  #%2 after qualifying  ·  %3 pts").arg(p["seed"].toInt())
                                    .arg(p["standing"].toInt()).arg(cmdr::pointsText(p["points"].toDouble())),
                                T::MUTED, 11, 400, true, true));
        row->addLayout(names, 1);
        QString shown;
        if (confirmed) {
            const QString basis = p["placement_basis"].toString();
            shown = placeText(p["placement"].toInt()) + (basis == "WINNER" ? QStringLiteral("  ·  won")
                                                         : basis == "QUALIFYING_RANK" ? QStringLiteral("  ·  by seed")
                                                                                      : QString());
        } else {
            shown = group ? QStringLiteral("Out in knockout %1").arg(group) : QStringLiteral("Still in");
        }
        QLabel *stateLabel = T::lbl(shown, confirmed && p["placement"].toInt() == 1 ? T::GOLD : T::MUTED, 11, 700);
        stateLabel->setAccessibleName(p["display_name"].toString() + ": " + shown);
        row->addWidget(stateLabel);
        v->addLayout(row);
    }
    if (confirmed && !terminated && state["event"].toMap()["stage"].toString() != "REGISTRATION") {
        auto *acts = new QWidget;
        auto *al = new T::FlowLayout(acts, 8);
        QPushButton *fix = smallButton(QStringLiteral("Correct final…"), false, "pencil");
        fix->setAccessibleName("Correct the final results");
        connect(fix, &QPushButton::clicked, this, [this] { correctFinal(); });
        al->addWidget(fix);
        v->addWidget(acts);
    }
    body_->addWidget(card);
}

// a failure nobody chose (the database refused the write): logged with a reference, shown beside the control
static QString failureText(const std::exception &e)
{
    QString op;
    const QString ref = applog::errorReference(&op);
    applog::Fields fields{{"ref", ref}};
    if (!op.isEmpty())
        fields.append({"op", op});
    applog::error("final.save", fields, applog::scrub(QString::fromUtf8(e.what())));
    return QString::fromUtf8(e.what()) + QStringLiteral(" (reference %1)").arg(ref);
}

static QList<QList<qint64>> savedGroups(const QVariantMap &fin)
{
    QList<QList<qint64>> out;
    for (const QVariant &g : fin["groups"].toList()) {
        QList<qint64> group;
        for (const QVariant &p : g.toList())
            group << p.toLongLong();
        out << group;
    }
    return out;
}

// saves knockouts on the page and redraws only the final's editor, so a chosen ending stays chosen
void CommanderEventScreen::knockout(const QList<QList<qint64>> &groups)
{
    T::ScrollKeeper keep(this);
    const int version = state["final"].toMap()["result_version"].toInt();
    try {
        cdb::saveFinalEliminations(tournamentId_, groups, version);
    } catch (const cdb::ConflictError &e) {
        warn(this, "Changed elsewhere", e.message);
        refresh();
        return;
    } catch (const cdb::CommanderError &e) {
        if (finalEditor_)
            finalEditor_->setError(e.message, true);        // nothing saved; the selection stays
        return;
    } catch (const std::exception &e) {
        if (finalEditor_)
            finalEditor_->setError(failureText(e), true);
        return;
    }
    state = cdb::getState(tournamentId_);
    if (finalEditor_)
        finalEditor_->setFinal(state["final"].toMap());
    mw_->updateTournamentTab(tournamentId_);
}

void CommanderEventScreen::undoKnockout()
{
    const QVariantMap fin = state["final"].toMap();
    QList<QList<qint64>> groups = savedGroups(fin);
    if (groups.isEmpty())
        return;
    QStringList names;
    for (const QVariant &p : fin["players"].toList())
        if (groups.last().contains(p.toMap()["player_id"].toLongLong()))
            names << p.toMap()["display_name"].toString();
    QString reason;
    if (!askReason(this, "Undo last knockout",
                   "Take back the last knockout (" + names.join(", ") + ")? It is already saved, so this is a correction.",
                   "Undo knockout", &reason))
        return;
    groups.removeLast();
    T::ScrollKeeper keep(this);
    try {
        cdb::saveFinalEliminations(tournamentId_, groups, fin["result_version"].toInt(), reason);
    } catch (const cdb::ConflictError &e) {
        warn(this, "Changed elsewhere", e.message);
        refresh();
        return;
    } catch (const cdb::CommanderError &e) {
        if (finalEditor_)
            finalEditor_->setError(e.message, true);
        return;
    } catch (const std::exception &e) {
        if (finalEditor_)
            finalEditor_->setError(failureText(e), true);
        return;
    }
    state = cdb::getState(tournamentId_);
    if (finalEditor_)
        finalEditor_->setFinal(state["final"].toMap());
    mw_->updateTournamentTab(tournamentId_);
}

void CommanderEventScreen::confirmFinalFrom(FinalEditor *editor)
{
    if (!editor->ready())
        return;
    if (!confirm(this, "Confirm final results",
                 "Confirm these places and finish the tournament?\n\n" + editor->preview->text().remove(QChar(0x200B))))
        return;
    try {
        cdb::confirmFinal(tournamentId_, editor->groups(), editor->outcome(), editor->winner(),
                          state["final"].toMap()["result_version"].toInt());
    } catch (const cdb::ConflictError &e) {
        warn(this, "Changed elsewhere", e.message);
        refresh();
        return;
    } catch (const cdb::CommanderError &e) {
        editor->setError(e.message);        // nothing saved; the knockouts and ending stay as entered
        return;
    } catch (const std::exception &e) {
        editor->setError(failureText(e));
        return;
    }
    completeWith([] {});
}

void CommanderEventScreen::correctFinal()
{
    const QVariantMap fin = state["final"].toMap();
    FinalCorrectionDialog d(this, tournamentId_, fin);
    while (d.exec() == QDialog::Accepted) {
        try {
            cdb::confirmFinal(tournamentId_, d.editor->groups(), d.editor->outcome(), d.editor->winner(),
                              fin["result_version"].toInt(), d.reason());
        } catch (const cdb::ConflictError &e) {
            warn(this, "Changed elsewhere", e.message);
            break;
        } catch (const cdb::CommanderError &e) {
            d.showError(e.message);
            continue;
        } catch (const std::exception &e) {
            d.showError(failureText(e));
            continue;
        }
        break;
    }
    refresh();
}

// a pill naming a finalist and their seed; its label wraps, so a long name is never clipped
static QPushButton *finalistPill(const QString &name, int seed, const QString &accessible)
{
    auto *b = new T::WrapButton(QStringLiteral("%1  ·  seed %2").arg(name).arg(seed));
    b->makeChoice();
    b->setAccessibleName(accessible);
    return b;
}

static QFrame *finalSection(const QString &heading, const QString &intro, QVBoxLayout **box)
{
    QFrame *f = T::panel("fs", T::BORDER2, 1, T::RADIUS_SM);
    auto *v = new QVBoxLayout(f);
    v->setContentsMargins(12, 10, 12, 12);
    v->setSpacing(6);
    v->addWidget(T::caps(heading));
    v->addWidget(T::lbl(intro, T::MUTED, 12, 400, true));
    *box = v;
    return f;
}

FinalEditor::FinalEditor(qint64 tournamentId, const QVariantMap &final, Mode mode, QWidget *parent)
    : QWidget(parent), tournamentId_(tournamentId), mode_(mode)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    sections_ = new QBoxLayout(QBoxLayout::LeftToRight);
    sections_->setSpacing(12);
    v->addLayout(sections_);

    QVBoxLayout *left = nullptr;
    QFrame *knockoutBox = finalSection("Record knockout", "Select every player knocked out at the same moment.", &left);
    pickRows_ = new QVBoxLayout;
    pickRows_->setSpacing(6);
    left->addLayout(pickRows_);
    knockoutNote = T::lbl("", T::TEXT, 12, 700, true);
    knockoutNote->setAccessibleName("About the knockout");
    knockoutNote->hide();
    left->addWidget(knockoutNote);
    knockoutError = T::lbl("", T::RED, 12, 700, true);
    knockoutError->hide();
    left->addWidget(knockoutError);
    auto *acts = new QWidget;
    auto *al = new T::FlowLayout(acts, 8);
    knockoutBtn = T::button(mode == Live ? QStringLiteral("Confirm knockout") : QStringLiteral("Add knockout"), "primary");
    knockoutBtn->setAccessibleName(mode == Live ? QStringLiteral("Confirm the knockout of the selected players")
                                                : QStringLiteral("Add a knockout of the selected players"));
    undoBtn = smallButton(mode == Live ? QStringLiteral("Undo last knockout…") : QStringLiteral("Undo last knockout"), true);
    undoBtn->setAccessibleName("Undo the last knockout");
    al->addWidget(knockoutBtn);
    al->addWidget(undoBtn);
    left->addWidget(acts);
    history = T::lbl("", T::MUTED, 12, 400, true);
    history->setAccessibleName("Knockouts so far");
    left->addWidget(history);
    left->addStretch();
    sections_->addWidget(knockoutBox, 1);

    QVBoxLayout *right = nullptr;
    QFrame *winnerBox = finalSection("Determine winner", "Choose the player who won.", &right);
    winnerRows_ = new QVBoxLayout;
    winnerRows_->setSpacing(6);
    right->addLayout(winnerRows_);
    endingGroup_ = new QButtonGroup(this);
    endingGroup_->setExclusive(true);
    auto *nobody = new T::WrapButton("No winner / Draw");
    nobody->makeChoice();
    noWinner = nobody;
    noWinner->setAccessibleName("No winner or draw in the final");
    endingGroup_->addButton(noWinner);
    right->addSpacing(4);
    right->addWidget(noWinner);
    right->addWidget(T::lbl("Nobody won: the game was drawn, or everyone still in went out at the same moment. They "
                            "are placed by seed.", T::MUTED, 11, 400, true));
    right->addSpacing(4);
    right->addWidget(T::caps(mode == Live ? QStringLiteral("Places") : QStringLiteral("Places after the correction")));
    preview = T::lbl("", T::TEXT, 13, 400, true);
    preview->setAccessibleName("Places");
    right->addWidget(preview);
    problem = T::lbl("", T::RED, 12, 700, true);
    problem->hide();
    right->addWidget(problem);
    if (mode == Live) {
        right->addWidget(T::lbl("Nothing about the ending is saved until you confirm the final results.", T::MUTED, 11, 400, true));
        confirmBtn = new T::WrapButton("Confirm final results", "primary");     // wraps in a narrow window
        confirmBtn->setAccessibleName("Confirm the final results");
        right->addWidget(confirmBtn, 0, Qt::AlignLeft);
    }
    right->addStretch();
    sections_->addWidget(winnerBox, 1);

    howBtn = smallButton("How placements work");          // no icon, so it fits a narrow window at large text
    howBtn->setCheckable(true);
    howBtn->setAccessibleName("How placements work");
    howBtn->setAccessibleDescription("collapsed");
    howText = T::lbl(placementRules(), T::MUTED, 12, 400, true);
    howText->hide();
    connect(howBtn, &QPushButton::toggled, this, [this](bool open) {
        howText->setVisible(open);
        howBtn->setAccessibleDescription(open ? "expanded" : "collapsed");
    });
    v->addWidget(howBtn, 0, Qt::AlignLeft);
    v->addWidget(howText);

    connect(knockoutBtn, &QPushButton::clicked, this, [this] {
        QList<qint64> picked;
        for (qint64 p : seeds_)
            if (picks.contains(p) && picks.value(p)->isChecked())
                picked << p;
        if (saving_ || picked.isEmpty() || !knockoutBtn->isEnabled())
            return;
        if (mode_ == Live) {
            // one save per press, without disabling the pressed button: that would pass focus down the page
            const QPointer<FinalEditor> self(this);
            saving_ = true;
            emit knockoutRequested(groups_ + QList<QList<qint64>>{picked});
            if (self)
                saving_ = false;
            return;
        }
        groups_ << picked;
        rebuild(1);
        emit changed();
    });
    connect(undoBtn, &QPushButton::clicked, this, [this] {
        if (saving_ || groups_.isEmpty())
            return;
        if (mode_ == Live) {
            const QPointer<FinalEditor> self(this);
            saving_ = true;
            emit undoRequested();
            if (self)
                saving_ = false;
            return;
        }
        groups_.removeLast();
        rebuild(-1);
        emit changed();
    });
    connect(noWinner, &QPushButton::toggled, this, [this](bool on) {
        if (!on)
            return;
        chosenWinner_ = 0;
        chosenNoWinner_ = true;
        updatePreview();
        emit changed();
    });
    if (confirmBtn) {
        connect(confirmBtn, &QPushButton::clicked, this, [this] {
            if (saving_ || !ready())
                return;
            const QPointer<FinalEditor> self(this);
            saving_ = true;
            emit confirmRequested();
            if (self)
                saving_ = false;
        });
    }

    // a correction starts from the saved ending
    if (mode == Draft && final["status"].toString() == "CONFIRMED") {
        if (final["outcome"].toString() == "WIN")
            chosenWinner_ = final["winner_player_id"].toLongLong();
        else
            chosenNoWinner_ = true;
    }
    setFinal(final);
}

void FinalEditor::setFinal(const QVariantMap &final)
{
    QVariantList players = final["players"].toList();
    std::sort(players.begin(), players.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["seed"].toInt() < b.toMap()["seed"].toInt();
    });
    seeds_.clear();
    names_.clear();
    seats_.clear();
    for (const QVariant &pv : players) {
        const QVariantMap p = pv.toMap();
        const qint64 id = p["player_id"].toLongLong();
        seeds_ << id;
        names_.insert(id, p["display_name"].toString());
        seats_.insert(id, p["seat"].toInt());
    }
    const int before = int(groups_.size());
    groups_ = savedGroups(final);
    rebuild(int(groups_.size()) - before);
    emit changed();
}

QList<QList<qint64>> FinalEditor::groups() const
{
    QList<QList<qint64>> out = groups_;
    if (!chosenWinner_)
        return out;
    QList<qint64> last;
    for (qint64 p : seeds_) {
        bool gone = false;
        for (const QList<qint64> &g : groups_)
            gone = gone || g.contains(p);
        if (!gone && p != chosenWinner_)
            last << p;
    }
    if (!last.isEmpty())
        out << last;
    return out;
}

QString FinalEditor::outcome() const
{
    return chosenWinner_ ? QStringLiteral("WIN") : chosenNoWinner_ ? QStringLiteral("NO_WINNER") : QString();
}

qint64 FinalEditor::winner() const { return chosenWinner_; }

bool FinalEditor::ready() const { return !outcome().isEmpty() && previewOk_; }

bool FinalEditor::stacked() const { return sections_->direction() == QBoxLayout::TopToBottom; }

void FinalEditor::setError(const QString &text, bool knockout)
{
    QLabel *where = knockout ? knockoutError : problem;
    where->setText(T::breakable("Not saved: " + text));
    where->setVisible(!text.isEmpty());
}

void FinalEditor::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    // side by side once each section has room for a name; the gap between the two widths keeps
    // a scroll bar that comes and goes from flipping it back and forth
    const int w = width();
    if (!stacked() && w < T::px(540))
        sections_->setDirection(QBoxLayout::TopToBottom);
    else if (stacked() && w >= T::px(580))
        sections_->setDirection(QBoxLayout::LeftToRight);
}

// change: knockouts added (positive), taken back (negative) or neither
void FinalEditor::rebuild(int change)
{
    // focus on a control that is about to be removed or disabled would be passed down the page by
    // Qt, and the scroll area would follow it; hold it on the editor, which never scrolls
    QWidget *f = QApplication::focusWidget();
    const bool hadFocus = f && (f == this || isAncestorOf(f));
    if (hadFocus)
        setFocus(Qt::OtherFocusReason);

    QHash<qint64, int> knockedOut;
    for (int g = 0; g < groups_.size(); ++g)
        for (qint64 p : groups_[g])
            knockedOut.insert(p, g + 1);
    QList<qint64> still;
    for (qint64 p : seeds_)
        if (!knockedOut.contains(p))
            still << p;
    if (knockedOut.contains(chosenWinner_))
        chosenWinner_ = 0;
    if (still.size() < 2)
        chosenNoWinner_ = false;        // one player left has won

    for (QPushButton *b : std::as_const(winners))
        endingGroup_->removeButton(b);
    T::clear(pickRows_);
    T::clear(winnerRows_);
    picks.clear();
    winners.clear();
    for (qint64 p : still) {
        const QString name = names_.value(p);
        const int seed = int(seeds_.indexOf(p)) + 1;
        QPushButton *pick = finalistPill(name, seed, QStringLiteral("Knock out %1, seed %2").arg(name).arg(seed));
        pick->setEnabled(still.size() > 1);
        picks.insert(p, pick);
        pickRows_->addWidget(pick);
        connect(pick, &QPushButton::toggled, this, [this] {
            knockoutError->hide();
            updatePreview();
        });

        QPushButton *win = finalistPill(name, seed, name + " won the final");
        win->setChecked(chosenWinner_ == p);
        endingGroup_->addButton(win);
        winners.insert(p, win);
        winnerRows_->addWidget(win);
        connect(win, &QPushButton::toggled, this, [this, p](bool on) {
            if (!on)
                return;
            chosenWinner_ = p;
            chosenNoWinner_ = false;
            updatePreview();
            emit changed();
        });
    }
    noWinner->setEnabled(still.size() > 1);
    if (noWinner->isChecked() != chosenNoWinner_) {
        endingGroup_->setExclusive(false);      // an exclusive group cannot be left with nothing chosen
        noWinner->setChecked(chosenNoWinner_);
        endingGroup_->setExclusive(true);
    }

    QStringList lines;
    for (int g = 0; g < groups_.size(); ++g) {
        QStringList who;
        for (qint64 p : groups_[g])
            who << names_.value(p);
        lines << QStringLiteral("Knockout %1: %2").arg(g + 1).arg(who.join(", "))
                     + (groups_[g].size() > 1 ? QStringLiteral(" (together)") : QString());
    }
    history->setText(T::breakable(lines.isEmpty() ? QStringLiteral("No knockouts yet.") : lines.join('\n')));
    undoBtn->setEnabled(!groups_.isEmpty());
    knockoutError->hide();
    problem->hide();

    // tab order follows the page: knockouts, then the ending
    QList<QWidget *> order;
    for (qint64 p : still)
        order << picks.value(p);
    order << knockoutBtn << undoBtn;
    for (qint64 p : still)
        order << winners.value(p);
    order << noWinner;
    if (confirmBtn)
        order << confirmBtn;
    order << howBtn;
    for (int i = 1; i < order.size(); ++i)
        setTabOrder(order[i - 1], order[i]);
    updatePreview();

    if (hadFocus) {
        QList<QPointer<QWidget>> next;
        QPushButton *firstPick = still.size() > 1 ? picks.value(still.first()) : nullptr;
        if (change < 0)
            next << undoBtn << firstPick;
        else if (still.size() == 1)
            next << winners.value(still.first()) << undoBtn;        // the one left is the winner to confirm
        else
            next << firstPick << undoBtn;
        next << knockoutBtn;
        focusFirstShown(next);
    }
}

// once the new rows are laid out and the page's scroll position is back, focus goes to the
// first candidate the user can see; setFocus never scrolls, and nothing is chosen for them
void FinalEditor::focusFirstShown(const QList<QPointer<QWidget>> &candidates)
{
    QTimer::singleShot(0, this, [this, candidates] {
        QTimer::singleShot(0, this, [this, candidates] {
            if (QApplication::focusWidget() != this)        // the user has moved on
                return;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            for (const QPointer<QWidget> &w : candidates) {
                if (w && w->isVisible() && w->isEnabled() && !w->visibleRegion().isEmpty()) {
                    w->setFocus(Qt::OtherFocusReason);
                    return;
                }
            }
        });
    });
}

void FinalEditor::updatePreview()
{
    int still = 0, ticked = 0;
    for (QPushButton *b : std::as_const(picks)) {
        ++still;
        ticked += b->isChecked() ? 1 : 0;
    }
    // at least one player stays in: the last one won, or if the last ones went out together, nobody did
    knockoutBtn->setEnabled(ticked > 0 && ticked < still);
    QString note;
    if (still == 1)
        note = mode_ == Live ? QStringLiteral("One player remains. Confirm the winner to finish.")
                             : QStringLiteral("One player remains. Choose the winner, then save the correction.");
    else if (ticked > 0 && ticked == still)
        note = "Leave at least one player in. If everyone still in went out at the same moment, nobody won: choose "
               "No winner / Draw.";
    knockoutNote->setText(T::breakable(note));
    knockoutNote->setVisible(!note.isEmpty());

    QStringList lines;
    QString hint;
    previewOk_ = false;
    if (outcome().isEmpty()) {
        lines << "Choose the winner, or No winner / Draw, to see the places.";
    } else {
        try {
            for (const QVariant &pv : cdb::previewFinal(tournamentId_, groups(), outcome(), winner())) {
                const QVariantMap p = pv.toMap();
                const QString basis = p["basis"].toString();
                lines << placeText(p["place"].toInt()) + "   " + p["display_name"].toString() + "   ·   "
                             + (basis == "WINNER" ? QStringLiteral("won the final")
                                : basis == "QUALIFYING_RANK" ? QStringLiteral("still in at the end, placed by seed %1").arg(p["seed"].toInt())
                                                             : QStringLiteral("knocked out (seed %1)").arg(p["seed"].toInt()));
            }
            previewOk_ = true;
        } catch (const cdb::CommanderError &e) {
            hint = e.message;
        }
    }
    preview->setText(T::breakable(lines.join('\n')));
    problem->setText(T::breakable(hint));
    problem->setVisible(!hint.isEmpty());
    if (confirmBtn)
        confirmBtn->setEnabled(ready());
}

FinalCorrectionDialog::FinalCorrectionDialog(QWidget *parent, qint64 tournamentId, const QVariantMap &final)
    : QDialog(parent)
{
    setWindowTitle("Correct the final");
    fitDialog(this, parent, 720, 640);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(8);
    outer->addWidget(T::lbl("Correct the final", T::TEXT, 20, 700, true));
    QVBoxLayout *v = nullptr;
    outer->addWidget(T::scrollArea(&v), 1);
    v->setSpacing(8);

    QVariantList players = final["players"].toList();
    std::sort(players.begin(), players.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap()["placement"].toInt() < b.toMap()["placement"].toInt();
    });
    QStringList saved;
    for (const QVariant &pv : players) {
        const QVariantMap p = pv.toMap();
        const QString basis = p["placement_basis"].toString();
        saved << placeText(p["placement"].toInt()) + "   " + p["display_name"].toString()
                     + (basis == "WINNER" ? QStringLiteral("   ·   won the final")
                        : basis == "QUALIFYING_RANK" ? QStringLiteral("   ·   placed by seed %1").arg(p["seed"].toInt())
                                                     : QStringLiteral("   ·   knocked out (seed %1)").arg(p["seed"].toInt()));
    }
    v->addWidget(T::caps("Saved places"));
    before = T::lbl(saved.join('\n'), T::MUTED, 13, 400, true);
    before->setAccessibleName("Saved places");
    v->addWidget(before);
    v->addWidget(T::hline());
    editor = new FinalEditor(tournamentId, final, FinalEditor::Draft);
    v->addWidget(editor);
    v->addStretch();

    error = T::lbl("", T::RED, 12, 700, true);
    error->hide();
    outer->addWidget(error);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    reasonEdit = new QLineEdit;
    reasonEdit->setPlaceholderText("Reason for the correction (required)");
    reasonEdit->setAccessibleName("Reason for the correction");
    buttons->addWidget(reasonEdit, 1);
    QPushButton *cancel = T::button("Cancel", "ghost");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    saveBtn = T::button("Save correction", "primary");
    saveBtn->setDefault(true);
    connect(saveBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(saveBtn);
    outer->addLayout(buttons);
    const auto sync = [this] { saveBtn->setEnabled(editor->ready() && !reason().isEmpty()); };
    connect(editor, &FinalEditor::changed, this, sync);
    connect(reasonEdit, &QLineEdit::textChanged, this, sync);
    sync();
}

QString FinalCorrectionDialog::reason() const
{
    return reasonEdit->text().trimmed();
}

void FinalCorrectionDialog::showError(const QString &text)
{
    error->setText(T::breakable("Not saved: " + text));
    error->setVisible(!text.isEmpty());
}

void CommanderEventScreen::finalizeRound(const QVariantMap &rnd)
{
    const int n = rnd["round_number"].toInt();
    const bool last = n >= state["total_rounds"].toInt();
    if (!confirm(this, "Finalize round",
                 last ? QStringLiteral("Confirm all results for Round %1?\n\nThis is the last qualifying round. The "
                                       "top-four final is then created from the standings.").arg(n)
                      : QStringLiteral("Confirm all results for Round %1?\n\nRound %2 of %3 can then be started.")
                            .arg(n).arg(n + 1).arg(state["total_rounds"].toInt())))
        return;
    view_ = 0;
    guard([&] { cdb::finalizeRound(rnd["round_id"].toLongLong()); }, "Finalize");
}

// Last configured round: finalize it, save the final standings and show them.
void CommanderEventScreen::finish(const QVariantMap &rnd)
{
    const int total = state["total_rounds"].toInt();
    if (!confirm(this, "Finish tournament",
                 QStringLiteral("Finish %1?\n\nThis finalizes Round %2, the last of %3 round%4, and saves the final "
                                "standings. No further rounds are created.")
                     .arg(state["event"].toMap()["name"].toString()).arg(rnd["round_number"].toInt()).arg(total)
                     .arg(total == 1 ? "" : "s")))
        return;
    completeWith([this] { cdb::finishTournament(tournamentId_); });
}

void CommanderEventScreen::publish(int expectedRound)
{
    view_ = 0;
    guard([&] { cdb::publishNextRound(tournamentId_, expectedRound); }, "Next round");
}

void CommanderEventScreen::openHistory()
{
    QStringList lines;
    for (const QVariant &av : cdb::getAudit(tournamentId_)) {
        const QVariantMap a = av.toMap();
        QStringList detail;
        const QVariantMap d = a["detail"].toMap();
        for (auto it = d.begin(); it != d.end(); ++it)
            if (it.key() != "settings")
                detail << it.key() + "=" + QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
        lines << QStringLiteral("%1  %2  by %3\n    %4").arg(a["created_at"].toString(), a["action"].toString(),
                                                             a["actor"].toString(), detail.join(", "));
    }
    showText(this, "Event history (newest first)", lines);
}

void CommanderEventScreen::openDrops()
{
    QDialog d(this);
    d.setWindowTitle("Players & drops");
    fitDialog(&d, this, 540, 560);
    auto *v = new QVBoxLayout(&d);
    v->addWidget(T::lbl("Players & drops", T::TEXT, 20, 700));
    v->addWidget(T::lbl("Dropping removes a player from future rounds only. Their pods, results and standing "
                        "stay on record, and the number of rounds does not change.", T::MUTED, 12, 400, true));
    QVBoxLayout *rows = nullptr;
    v->addWidget(T::scrollArea(&rows), 1);
    rows->setSpacing(6);

    const qint64 tid = tournamentId_;
    std::function<void()> fill = [&] {
        T::ScrollKeeper keep(&d);
        T::clear(rows);
        const bool open = cdb::getEvent(tid)["stage"].toString() == "SWISS" && !tdb::isTerminated(tid);
        QSet<qint64> finalists;
        for (const QVariant &p : cdb::getFinal(tid)["players"].toList())
            finalists.insert(p.toMap()["player_id"].toLongLong());
        for (const QVariant &rv : cdb::getStandings(tid)) {
            const QVariantMap r = rv.toMap();
            const qint64 pid = r["player_id"].toLongLong();
            const bool dropped = r["dropped"].toInt() != 0;
            auto *h = new QHBoxLayout;
            auto *text = new QVBoxLayout;
            text->setSpacing(0);
            text->addWidget(T::lbl(r["display_name"].toString(), dropped ? T::MUTED : T::TEXT, 14, 600, true));
            text->addWidget(T::lbl(cmdr::pointsText(r["points"].toDouble()) + " pts"
                                       + (dropped ? QStringLiteral("  ·  dropped after round %1").arg(r["drop_round"].toInt()) : QString()),
                                   dropped ? T::RED : T::MUTED, 11));
            h->addLayout(text, 1);
            QPushButton *b = dropped ? smallButton("Undo drop") : smallButton("Drop", true);
            b->setAccessibleName(b->text() + " " + r["display_name"].toString());
            b->setEnabled(open && !finalists.contains(pid));
            if (finalists.contains(pid))
                b->setToolTip("A finalist cannot be dropped once the final is created.");
            QObject::connect(b, &QPushButton::clicked, &d, [&d, &fill, tid, pid, dropped] {
                try {
                    if (dropped)
                        cdb::reinstatePlayer(tid, pid);
                    else
                        cdb::dropPlayer(tid, pid);
                } catch (const cdb::CommanderError &e) {
                    warn(&d, "Players", e.message);
                }
                fill();
            });
            h->addWidget(b);
            rows->addLayout(h);
        }
        rows->addStretch();
    };
    fill();
    QPushButton *close = T::button("Done", "primary");
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::accept);
    v->addWidget(close, 0, Qt::AlignRight);
    d.exec();
    refresh();
}

namespace {
const int STATUS_GAP = 16;      // space between the last figure and the status text
}

CommanderStandingsScreen::CommanderStandingsScreen(MainWindow *mw, qint64 tournamentId) : mw_(mw), tournamentId_(tournamentId)
{
    state = cdb::getState(tournamentId);
    const QVariantMap ev = state["event"].toMap();
    const QVariantMap legacy = state["legacy"].toMap();
    const bool hasPlayoff = !legacy["playoff_rounds"].toList().isEmpty();
    const bool scoring = state["scores_second"].toBool();
    const QVariantMap fin = state["final"].toMap();
    const bool finalDone = fin["status"].toString() == "CONFIRMED";
    cols_ = {{"#", 46}, {"Player", 0}, {"Pts", 52}, {"W", 40}, {"L", 40}, {"D", 40}, {"Byes", 54}};
    if (scoring)
        cols_ << qMakePair(QString("2nd"), 48);
    cols_ << qMakePair(QString("OMW%"), 76) << qMakePair(QString("MW%"), 70) << qMakePair(QString("Status"), 196);

    QVBoxLayout *body = nullptr;
    root->addWidget(page(&body));       // the whole page scrolls; the table also scrolls sideways
    QBoxLayout *top = flip(T::row(12));
    auto *tv = new QVBoxLayout;
    tv->setSpacing(3);
    const bool final = ev["stage"].toString() == "COMPLETE";
    int done = 0;
    for (const QVariant &r : state["rounds"].toList())
        if (!r.toMap()["legacy"].toBool() && r.toMap()["status"].toString() == "FINALIZED")
            ++done;
    const int total = state["total_rounds"].toInt();
    tv->addWidget(T::lbl(final ? "Final Standings" : "Standings", T::TEXT, 26, 700, true));
    QString sub = ev["name"].toString() + QStringLiteral("  ·  Commander  ·  ")
                  + (final ? QStringLiteral("%1 round%2 played%3  ·  Tournament complete").arg(done).arg(done == 1 ? "" : "s")
                                 .arg(finalDone ? QStringLiteral(" and the top-four final") : QString())
                           : QStringLiteral("%1 of %2 round%3 complete").arg(done).arg(total).arg(total == 1 ? "" : "s"));
    if (final && !ev["champion_name"].toString().isEmpty())
        sub += QStringLiteral("  ·  1st place: ") + ev["champion_name"].toString();
    if (state["terminated"].toBool())
        sub += QStringLiteral("  ·  Terminated");
    tv->addWidget(T::lbl(sub, T::MUTED, 13, 400, true));
    top->addLayout(tv, 1);
    auto *actions = new QWidget;
    auto *al = new T::FlowLayout(actions, 8);
    QPushButton *roundsBtn = T::button("Rounds && pods");
    connect(roundsBtn, &QPushButton::clicked, this, [this] { mw_->navigateTo("round", {{"tournament_id", tournamentId_}}); });
    QPushButton *rulesBtn = T::button("Rules", "secondary", "circle-help");
    connect(rulesBtn, &QPushButton::clicked, this, [this] { showText(this, "Event rules", cdb::rulesText(state["event"].toMap())); });
    QPushButton *csvBtn = T::button("Export CSV");
    connect(csvBtn, &QPushButton::clicked, this, [this] { exportCsv(); });
    QPushButton *pdfBtn = T::button("Save PDF", "secondary", "printer");
    connect(pdfBtn, &QPushButton::clicked, this, [this] { savePdf(); });
    for (QPushButton *b : {roundsBtn, rulesBtn, csvBtn, pdfBtn})
        al->addWidget(b);
    top->addWidget(actions);
    body->addLayout(top);
    body->addSpacing(8);
    if (state["terminated"].toBool()) {
        // not final placings: the standings as they stood when the event was ended
        body->addWidget(terminatedNotice(tdb::tournamentById(tournamentId)["terminated_at"].toString()));
        body->addSpacing(8);
    }
    const QVariantMap s = ev["settings"].toMap();
    QString order = scoring
        ? QStringLiteral("Order: points (win %1, clear second %2, draw %3, bye %4), then opponents' match-win % (OMW), own "
                         "match-win % (MW). %5% floor; a bye counts as a win. Second place is still a loss.")
              .arg(cmdr::pointsText(s["win_points"].toDouble()), cmdr::pointsText(s["second_place_points"].toDouble()),
                   cmdr::pointsText(s["draw_points"].toDouble()), cmdr::pointsText(s["bye_points"].toDouble()))
              .arg(qRound(s["tiebreak_floor"].toDouble() * 100))
        : QStringLiteral("Order: points, opponents' match-win % (OMW), own match-win % (MW). %1% floor; a bye "
                         "counts as a win.").arg(qRound(s["tiebreak_floor"].toDouble() * 100));
    if (state["final_enabled"].toBool())
        order += finalDone ? " Places 1 to 4 are from the top-four final; everyone else follows in qualifying order. The "
                             "final adds no points."
                           : " The top-four final decides places 1 to 4; until it is confirmed these are the qualifying "
                             "standings.";
    if (hasPlayoff)
        order += " Placings here include a playoff played in an earlier version; the points and percentages are from "
                 "the rounds only.";
    body->addWidget(T::lbl(order, T::MUTED, 12, 400, true));
    body->addSpacing(12);

    QFrame *table = T::panel("st");
    int fixed = 200;
    for (const auto &c : cols_)
        fixed += T::px(c.second);
    table->setMinimumWidth(fixed);
    auto *tl = new QVBoxLayout(table);
    tl->setContentsMargins(1, 1, 1, 1);
    tl->setSpacing(0);
    auto *head = new QWidget;
    auto *hh = new QHBoxLayout(head);
    hh->setContentsMargins(20, 11, 20, 11);
    hh->setSpacing(0);
    for (int i = 0; i < cols_.size(); ++i) {
        const auto &c = cols_[i];
        QLabel *lb = T::lbl(c.first.toUpper(), T::MUTED, 10, 700, false, false, 1.0);
        if (c.second) {
            lb->setFixedWidth(T::px(c.second));
            const bool number = i > 1 && i < cols_.size() - 1;      // figures line up on the right
            lb->setAlignment((number ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            if (i == cols_.size() - 1)
                lb->setContentsMargins(T::px(STATUS_GAP), 0, 0, 0);
            hh->addWidget(lb);
        } else {
            hh->addWidget(lb, 1);                   // player names stay left-aligned
        }
    }
    tl->addWidget(head);
    tl->addWidget(T::hline());

    QHash<qint64, int> seeds;
    if (hasPlayoff)
        for (const QVariant &p : ev["playoff_seeds"].toList())
            seeds.insert(p.toMap()["player_id"].toLongLong(), p.toMap()["seed"].toInt());
    QHash<qint64, QVariantMap> finalists;
    for (const QVariant &p : fin["players"].toList())
        finalists.insert(p.toMap()["player_id"].toLongLong(), p.toMap());
    // a finished event shows the placings saved when it was finished
    const QVariantList finalRows = state["final_standings"].toList();
    for (const QVariant &rv : finalRows.isEmpty() ? state["standings"].toList() : finalRows) {
        QVariantMap r = rv.toMap();
        const qint64 pid = r["player_id"].toLongLong();
        r["place"] = finalRows.isEmpty() ? r["standing"] : r["final_placement"];
        QString note;
        if (finalists.contains(pid)) {
            const QVariantMap f = finalists.value(pid);
            const QString basis = f["placement_basis"].toString();
            note = finalDone ? QStringLiteral("Final: %1").arg(placeText(f["placement"].toInt()))
                                   + (basis == "WINNER" ? QStringLiteral(", won") : basis == "QUALIFYING_RANK"
                                                                                        ? QStringLiteral(", by seed")
                                                                                        : QString())
                             : QStringLiteral("Finalist, seed %1").arg(f["seed"].toInt());
        } else if (seeds.contains(pid)) {
            note = QStringLiteral("Playoff seed %1").arg(seeds.value(pid));
        }
        if (r["dropped"].toInt())
            note = (note.isEmpty() ? QString() : note + "; ") + QStringLiteral("Dropped after R%1").arg(r["drop_round"].toInt());
        r["note"] = note;
        rows << r;
    }
    for (int i = 0; i < rows.size(); ++i)
        tl->addWidget(standingRow(rows[i].toMap(), i == rows.size() - 1));
    if (rows.isEmpty())
        tl->addWidget(T::lbl("   No players yet", T::MUTED, 13));
    body->addWidget(new T::HScroll(table));     // sideways when the window is narrower than the columns
    body->addStretch();
}

QWidget *CommanderStandingsScreen::standingRow(const QVariantMap &r, bool last)
{
    const int place = r["place"].toInt();
    const QString pc = T::placeColor(place);
    const bool dropped = r["dropped"].toInt() != 0;
    auto *row = new QFrame;
    row->setObjectName("sr");
    row->setStyleSheet("#sr{background:transparent;border:none;border-bottom:" + (last ? QString("none") : "1px solid " + T::BORDER)
                       + ";border-left:4px solid " + (pc.isEmpty() ? QString("transparent") : pc) + ";}");
    auto *hl = new QHBoxLayout(row);
    hl->setContentsMargins(16, 11, 20, 11);
    hl->setSpacing(0);
    QList<QLabel *> cells{
        T::lbl(QString::number(place), pc.isEmpty() ? T::MUTED : pc, 13, 700, false, true),
        T::lbl(r["display_name"].toString(), dropped ? T::MUTED : T::TEXT, 14, 600, true),
        T::lbl(cmdr::pointsText(r["points"].toDouble()), T::TEXT, 15, 700),
        T::lbl(r["wins"].toString(), T::GREEN, 13, 600),
        T::lbl(r["losses"].toString(), T::RED, 13),
        T::lbl(r["draws"].toString(), T::AMBER, 13),
        T::lbl(r["byes"].toString(), T::GREY, 13),
    };
    if (state["scores_second"].toBool())
        cells << T::lbl(QString::number(r["seconds"].toInt()), T::PURPLE_LT, 13);
    cells << T::lbl(QString::number(r["omw_pct"].toDouble(), 'f', 3), T::MUTED, 12, 400, false, true)
          << T::lbl(QString::number(r["mw_pct"].toDouble(), 'f', 3), T::MUTED, 12, 400, false, true);
    for (int i = 0; i < cells.size(); ++i) {
        if (cols_[i].second) {
            cells[i]->setFixedWidth(T::px(cols_[i].second));
            cells[i]->setAlignment((i > 1 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            hl->addWidget(cells[i]);
        } else {
            hl->addWidget(cells[i], 1);
        }
    }
    const QString note = r["note"].toString();
    QLabel *status = T::lbl(note, dropped ? T::RED : note.startsWith("Final") ? T::GOLD : T::PURPLE_LT, 11, 700, true);
    status->setFixedWidth(T::px(cols_.last().second));
    status->setContentsMargins(T::px(STATUS_GAP), 0, 0, 0);
    hl->addWidget(status);
    return row;
}

printing::Doc CommanderStandingsScreen::pdfDoc() const
{
    const QVariantMap ev = state["event"].toMap();
    const bool scoring = state["scores_second"].toBool();
    printing::Doc d;
    d.title = ev["name"].toString();
    d.sub = (ev["stage"].toString() == "COMPLETE" ? QStringLiteral("Final standings") : QStringLiteral("Standings"))
            + QStringLiteral("  ·  MTG Commander  ·  ") + ev["tournament_date"].toString().left(10);
    d.columns = {{"#", 0.07}, {"Player", scoring ? 0.25 : 0.29}, {"Pts", 0.08}, {"W", 0.06}, {"L", 0.06}, {"D", 0.06}};
    if (scoring)
        d.columns << qMakePair(QString("2nd"), 0.06);
    d.columns << qMakePair(QString("OMW%"), 0.09) << qMakePair(QString("MW%"), 0.09)
              << qMakePair(QString("Notes"), scoring ? 0.18 : 0.20);
    d.bold = {0, 1, 2};
    d.empty = "No standings yet.";
    for (const QVariant &rv : rows) {
        const QVariantMap r = rv.toMap();
        QStringList row{r["place"].toString(), r["display_name"].toString(), cmdr::pointsText(r["points"].toDouble()),
                        r["wins"].toString(), r["losses"].toString(), r["draws"].toString()};
        if (scoring)
            row << QString::number(r["seconds"].toInt());
        row << QString::number(r["omw_pct"].toDouble(), 'f', 3) << QString::number(r["mw_pct"].toDouble(), 'f', 3)
            << r["note"].toString();
        d.rows << row;
    }
    return d;
}

void CommanderStandingsScreen::savePdf()
{
    const QString name = state["event"].toMap()["name"].toString();
    const QString path = QFileDialog::getSaveFileName(this, "Save PDF", QString(name).replace(" ", "_") + "_standings.pdf",
                                                      "PDF Files (*.pdf)");
    if (path.isEmpty())
        return;
    if (!printing::savePdf({pdfDoc()}, path)) {
        warn(this, "Failed", "The PDF could not be saved.");
        return;
    }
    inform(this, "Saved", "Saved to:\n" + path);
}

void CommanderStandingsScreen::exportCsv()
{
    const QString name = state["event"].toMap()["name"].toString();
    const QString path = QFileDialog::getSaveFileName(this, "Export CSV", QString(name).replace(" ", "_") + "_standings.csv",
                                                      "CSV Files (*.csv)");
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(this, "Failed", f.errorString());
        return;
    }
    const bool scoring = state["scores_second"].toBool();
    const bool withFinal = state["final_enabled"].toBool();
    QTextStream out(&f);
    out << "Place,Player,Points,Wins,Losses,Draws,Byes," << (scoring ? "Seconds," : "") << "OMW%,MW%,Dropped"
        << (withFinal ? ",Final" : "") << '\n';
    for (const QVariant &rv : rows) {
        const QVariantMap r = rv.toMap();
        const QString player = printing::csvCell(r["display_name"].toString());
        out << r["place"].toString() << ',' << player << ',' << cmdr::pointsText(r["points"].toDouble()) << ','
            << r["wins"].toString() << ',' << r["losses"].toString() << ',' << r["draws"].toString() << ','
            << r["byes"].toString() << ',';
        if (scoring)
            out << r["seconds"].toInt() << ',';
        out << QString::number(r["omw_pct"].toDouble(), 'f', 4) << ',' << QString::number(r["mw_pct"].toDouble(), 'f', 4) << ','
            << (r["dropped"].toInt() ? QStringLiteral("after round %1").arg(r["drop_round"].toInt()) : QString());
        if (withFinal) {
            const QString note = r["note"].toString();
            out << ',' << printing::csvCell(note.startsWith("Final") || note.startsWith("Finalist") ? note.section(';', 0, 0) : QString());
        }
        out << '\n';
    }
    f.close();
    inform(this, "Exported", "Saved to:\n" + path);
}
