// Commander (multiplayer) screens: rounds of pods and results, and standings.
#include "commander_db.h"
#include "printing.h"
#include "screens.h"

#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTextStream>

static QPushButton *smallButton(const QString &text, bool danger = false)
{
    return T::button(text, danger ? "danger" : "ghost");
}

static QList<qint64> seatIds(const QVariantMap &pod)
{
    QList<qint64> out;
    for (const QVariant &s : pod["seats"].toList())
        out << s.toMap()["player_id"].toLongLong();
    return out;
}

ResultDialog::ResultDialog(QWidget *parent, const QVariantMap &pod, const QString &title, const QString &drawPolicy,
                           bool startAsDraw, bool askReason)
    : QDialog(parent)
{
    setWindowTitle(title);
    fitDialog(this, parent, 460, 520);
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
    outcomeCombo->addItem(QStringLiteral("Draw — no winner"), qint64(0));
    v->addWidget(outcomeCombo);

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

    if (askReason) {
        v->addWidget(T::caps("Reason for the correction"));
        reason_ = new QLineEdit;
        reason_->setPlaceholderText("e.g. slip was misread");
        reason_->setAccessibleName("Reason for the correction");
        v->addWidget(reason_);
    }
    v->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    QPushButton *cancel = T::button("Cancel", "ghost");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    QPushButton *save = T::button("Save result", "primary");
    save->setDefault(true);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    outer->addLayout(buttons);

    if (startAsDraw || wasDraw)
        outcomeCombo->setCurrentIndex(outcomeCombo->count() - 1);
    else if (savedWinner)
        outcomeCombo->setCurrentIndex(outcomeCombo->findData(savedWinner));
    auto syncElim = [this] { elimBox_->setVisible(outcomeCombo->currentData().toLongLong() == 0); };
    connect(outcomeCombo, &QComboBox::currentIndexChanged, this, syncElim);
    syncElim();
    outcomeCombo->setFocus();
}

QString ResultDialog::outcome() const { return winner() ? "WIN" : "DRAW"; }
qint64 ResultDialog::winner() const { return outcomeCombo->currentData().toLongLong(); }
QString ResultDialog::reason() const { return reason_ ? reason_->text().trimmed() : QString(); }

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
    root->addWidget(T::scrollArea(&body), 1);   // header and pods scroll together; actions stay put
    QBoxLayout *top = flip(T::row(12));
    auto *info = new QVBoxLayout;
    info->setSpacing(3);
    title = T::lbl("", T::TEXT, 24, 700, true);
    sub = T::lbl("", T::MUTED, 13, 400, true);
    info->addWidget(title);
    info->addWidget(sub);
    top->addLayout(info, 1);
    timerSlot_ = new QVBoxLayout;
    top->addLayout(timerSlot_);
    body->addLayout(top);
    body->addSpacing(10);

    auto *tools = new QWidget;                   // wraps onto more lines instead of running off the edge
    auto *tl = new T::FlowLayout(tools, 8);
    printBtn = smallButton(QStringLiteral("🖨  Print"));
    printBtn->setToolTip(QStringLiteral("Print this round’s pairings or pod signs"));
    connect(printBtn, &QPushButton::clicked, this, [this] {
        const QVariantMap rnd = currentRound();
        if (rnd.isEmpty())
            return;
        printing::PrintDialog(this, printing::commanderRoundData(state["event"].toMap()["name"].toString(),
                                                                 cdb::stageLabel(rnd), rnd)).exec();
    });
    tl->addWidget(printBtn);
    QPushButton *standings = smallButton("Standings");
    connect(standings, &QPushButton::clicked, this, [this] { mw_->navigateTo("standings", {{"tournament_id", tournamentId_}}); });
    QPushButton *drops = smallButton("Players && drops");
    connect(drops, &QPushButton::clicked, this, [this] { openDrops(); });
    QPushButton *rules = smallButton("Rules");
    connect(rules, &QPushButton::clicked, this, [this] { showText(this, "Event rules", cdb::rulesText(state["event"].toMap())); });
    QPushButton *history = smallButton("History");
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
    sub->setText(QStringLiteral("Commander  ·  %1 active players  ·  %2 of %3 round%4 complete")
                     .arg(state["active_count"].toInt()).arg(done).arg(total).arg(total == 1 ? "" : "s"));
    title->setText(T::breakable(current.isEmpty() ? name : cdb::stageLabel(current) + QStringLiteral(" — ") + name));
    printBtn->setEnabled(!current.isEmpty());

    // the clock belongs to the round in play and is saved with it
    const bool active = !current.isEmpty() && current["status"].toString() == "ACTIVE" && !current["legacy"].toBool()
                        && stage == "SWISS";
    const qint64 wanted = active ? current["round_id"].toLongLong() : 0;
    if (wanted != timerRound_) {
        T::clear(timerSlot_);
        if (active) {
            MainWindow *mw = mw_;
            const QString message = QStringLiteral("Time expired — %1, %2. Results are not finalized automatically.")
                                        .arg(name, cdb::stageLabel(current));
            timerSlot_->addWidget(new TimerWidget(timerdb::Kind::Commander, wanted, [mw, message] { mw->roundAlert(message); }));
        }
        timerRound_ = wanted;
    }

    T::ScrollKeeper keep(this);         // reporting a result must not move the page
    grids_.clear();
    buildStrip();
    T::clear(body_);
    T::clear(footer_);
    if (stage == "COMPLETE") {
        const QString first = ev["champion_name"].toString();
        body_->addWidget(banner(QStringLiteral("🏆  Tournament complete.") + (first.isEmpty() ? QString() : "  1st place: " + first),
                                T::GOLD));
    }
    if (state["legacy"].toMap()["flagged"].toBool())
        buildLegacyNotice();
    if (!current.isEmpty())
        buildRound(current);
    if (stage == "COMPLETE") {
        QPushButton *results = T::button(QStringLiteral("View final standings  →"), "primary");
        connect(results, &QPushButton::clicked, this, [this] { mw_->navigateTo("standings", {{"tournament_id", tournamentId_}}); });
        footer_->addWidget(results);
    }
    footerHolder_->setVisible(footer_->count() > 0);

    if (!rounds.isEmpty() && stage != "COMPLETE") {
        mw_->addTournamentTab(tournamentId_, "MTG", name);
        mw_->updateTournamentTab(tournamentId_);
    } else if (stage == "COMPLETE") {
        mw_->removeTournamentTab(tournamentId_);
    }
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
    const int shown = currentRound()["round_number"].toInt();
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

QWidget *CommanderEventScreen::banner(const QString &text, const QString &color)
{
    QFrame *f = T::panel("bn", color, 2);
    auto *h = new QHBoxLayout(f);
    h->setContentsMargins(18, 12, 18, 12);
    h->addWidget(T::lbl(text, color, 15, 700, true));
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
    const QVariantList rounds = state["rounds"].toList();
    int latestNumber = 0;
    for (const QVariant &r : rounds)
        if (!r.toMap()["legacy"].toBool())
            latestNumber = r.toMap()["round_number"].toInt();
    const int n = rnd["round_number"].toInt();
    const bool latest = !legacy && n == latestNumber;
    const bool active = rnd["status"].toString() == "ACTIVE" && !legacy && ev["stage"].toString() == "SWISS";
    const QVariantList pods = rnd["pods"].toList();
    const int total = int(pods.size());
    const int pending = rnd["pending"].toInt();
    const int done = total - pending;

    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    for (int i = 0; i < qMin(total, 60); ++i) {
        auto *seg = new QFrame;
        seg->setFixedHeight(5);
        seg->setStyleSheet("background:" + (i < done ? T::GREEN : T::BORDER2) + ";border:none;border-radius:2px;");
        bar->addWidget(seg, 1);
    }
    if (legacy)
        bar->addWidget(T::lbl(QStringLiteral("Read-only record · not counted"), T::GREY, 12));
    else
        bar->addWidget(T::lbl(active ? QStringLiteral("%1 / %2 pods reported").arg(done).arg(total) : QString("Results final"),
                              active ? T::MUTED : T::GREEN, 12));
    body_->addLayout(bar);

    QHash<qint64, int> points;
    for (const QVariant &r : state["standings"].toList())
        points.insert(r.toMap()["player_id"].toLongLong(), r.toMap()["points"].toInt());
    QList<QWidget *> cards;
    for (const QVariant &pod : pods)
        cards << podCard(pod.toMap(), rnd, points);
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

    const int roundsTotal = state["total_rounds"].toInt();
    if (active) {
        const bool ready = pending == 0;
        QPushButton *fin;
        if (n >= roundsTotal) {
            // the last configured round: finalizing it finishes the tournament — nothing follows
            body_->addWidget(T::lbl(QStringLiteral("Round %1 is the last round. Finishing saves the final standings.").arg(n),
                                    T::MUTED, 12, 400, true));
            fin = T::button("Finish tournament", "primary");
            connect(fin, &QPushButton::clicked, this, [this, rnd] { finish(rnd); });
        } else {
            body_->addWidget(T::lbl("Results are provisional until the round is finalized.", T::MUTED, 12, 400, true));
            fin = T::button(QStringLiteral("Finalize Round %1  →").arg(n), "primary");
            connect(fin, &QPushButton::clicked, this, [this, rnd] { finalizeRound(rnd); });
        }
        fin->setEnabled(ready);
        fin->setToolTip(ready ? "" : "Every pod needs a result first.");
        footer_->addWidget(fin);
    } else if (latest && state["next"].toString() == "SWISS") {
        if (state["can_pair"].toBool()) {
            body_->addWidget(T::lbl(QStringLiteral("Round %1 is final. Drops made now apply to Round %2.").arg(n).arg(n + 1),
                                    T::MUTED, 12, 400, true));
            QPushButton *next = T::button(QStringLiteral("Start Round %1  →").arg(n + 1), "primary");
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
}

QWidget *CommanderEventScreen::podCard(const QVariantMap &pod, const QVariantMap &rnd, const QHash<qint64, int> &points)
{
    const bool reported = pod["reported"].toBool();
    const QString outcome = pod["outcome"].toString();
    const bool legacy = rnd["legacy"].toBool();
    const QVariantMap ev = state["event"].toMap();
    const bool editable = rnd["status"].toString() == "ACTIVE" && !legacy && ev["stage"].toString() == "SWISS";
    const QVariantList seats = pod["seats"].toList();
    const int podNumber = pod["pod_number"].toInt();

    QFrame *card = T::panel("pc", !reported ? T::BORDER : (outcome == "WIN" ? T::GREEN : T::AMBER), 2);
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(16, 13, 16, 13);
    v->setSpacing(8);

    auto *head = new QHBoxLayout;
    head->addWidget(T::lbl(QStringLiteral("POD %1").arg(podNumber), T::TEXT, 13, 700, false, false, 1.0));
    head->addWidget(T::lbl(QStringLiteral("%1 players").arg(seats.size()), T::MUTED, 11));
    head->addStretch();
    v->addLayout(head);
    // status label on its own line so a long winner name wraps instead of being cut off
    QLabel *status;
    if (!reported) {
        status = editable ? T::lbl(QStringLiteral("●  Awaiting result"), T::PURPLE_LT, 12, 700, true)
                          : T::lbl("No result recorded", T::GREY, 12, 700, true);
    } else if (outcome == "WIN") {
        QString winner;
        for (const QVariant &s : seats)
            if (s.toMap()["result"].toString() == "WIN")
                winner = s.toMap()["display_name"].toString();
        status = T::lbl(QStringLiteral("✓  ") + winner + " wins", T::GREEN, 12, 700, true);
    } else {
        status = T::lbl(QStringLiteral("=  Draw — no winner"), T::AMBER, 12, 700, true);
    }
    v->addWidget(status);
    v->addWidget(T::hline());

    const QVariantMap settings = ev["settings"].toMap();
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
            names->addWidget(T::lbl(QStringLiteral("%1 pts").arg(points.value(pid)), T::MUTED, 11, 400, false, true));
        }
        row->addLayout(names, 1);
        if (reported) {
            if (result == "WIN")
                row->addWidget(T::lbl(legacy ? QString("WIN") : QStringLiteral("WIN  +%1").arg(settings["win_points"].toInt()),
                                      T::GREEN, 11, 700));
            else if (result == "DRAW")
                row->addWidget(T::lbl(legacy ? QString("DRAW") : QStringLiteral("DRAW  +%1").arg(settings["draw_points"].toInt()),
                                      T::AMBER, 11, 700));
            else
                row->addWidget(T::lbl(outcome == "DRAW" ? "OUT" : "LOSS", T::MUTED, 11, 700));
        } else if (editable) {
            QPushButton *win = smallButton("Wins");
            win->setAccessibleName(QStringLiteral("%1 wins pod %2").arg(name).arg(podNumber));
            connect(win, &QPushButton::clicked, this, [this, pod, pid] { report(pod, "WIN", pid, {}); });
            row->addWidget(win);
        }
        v->addLayout(row);
    }

    if (legacy)
        return card;
    auto *acts = new QWidget;
    auto *al = new T::FlowLayout(acts, 8);
    if (editable && !reported) {
        QPushButton *draw = smallButton(QStringLiteral("Draw…"));
        draw->setAccessibleName(QStringLiteral("Report a draw in pod %1").arg(podNumber));
        connect(draw, &QPushButton::clicked, this, [this, pod] { dialogReport(pod, true); });
        al->addWidget(draw);
    } else if (editable) {
        QPushButton *change = smallButton("Change");
        change->setAccessibleName(QStringLiteral("Change the result of pod %1").arg(podNumber));
        connect(change, &QPushButton::clicked, this, [this, pod] { dialogReport(pod, false); });
        QPushButton *clearBtn = smallButton("Clear", true);
        clearBtn->setAccessibleName(QStringLiteral("Clear the result of pod %1").arg(podNumber));
        connect(clearBtn, &QPushButton::clicked, this, [this, pod] {
            guard([&] { cdb::clearPodResult(pod["pod_id"].toLongLong(), pod["result_version"].toInt()); }, "Result");
        });
        al->addWidget(change);
        al->addWidget(clearBtn);
    } else if (reported) {
        QPushButton *fix = smallButton(QStringLiteral("Correct result…"));
        fix->setAccessibleName(QStringLiteral("Correct the result of pod %1").arg(podNumber));
        connect(fix, &QPushButton::clicked, this, [this, pod] { correct(pod); });
        al->addWidget(fix);
    }
    v->addWidget(acts);
    return card;
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

void CommanderEventScreen::report(const QVariantMap &pod, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminated)
{
    guard([&] { cdb::reportPodResult(pod["pod_id"].toLongLong(), outcome, winnerId, eliminated, pod["result_version"].toInt()); },
          "Result");
}

void CommanderEventScreen::dialogReport(const QVariantMap &pod, bool startAsDraw)
{
    ResultDialog d(this, pod, QStringLiteral("Pod %1 result").arg(pod["pod_number"].toInt()),
                   state["event"].toMap()["settings"].toMap()["draw_policy"].toString(), startAsDraw);
    if (d.exec() == QDialog::Accepted)
        report(pod, d.outcome(), d.winner(), d.eliminated());
}

void CommanderEventScreen::correct(const QVariantMap &pod)
{
    ResultDialog d(this, pod, QStringLiteral("Correct Pod %1").arg(pod["pod_number"].toInt()),
                   state["event"].toMap()["settings"].toMap()["draw_policy"].toString(), false, true);
    if (d.exec() != QDialog::Accepted)
        return;
    const qint64 podId = pod["pod_id"].toLongLong();
    const QString outcome = d.outcome(), reason = d.reason();
    const qint64 winner = d.winner();
    const QList<qint64> eliminated = d.eliminated();
    try {
        cdb::correctResult(podId, outcome, winner, eliminated, {}, reason);
    } catch (const cdb::ResolutionRequired &e) {
        QMessageBox box(this);
        box.setWindowTitle("Later rounds depend on this result");
        box.setText(e.message);
        box.setInformativeText("Keep: standings are recalculated, but everything already published stays as it is.\n"
                               "Rebuild: later rounds are discarded and redone from the corrected result "
                               "(only possible while they have no results).");
        QPushButton *keep = box.addButton("Keep later rounds", QMessageBox::AcceptRole);
        QPushButton *rebuild = box.addButton("Rebuild later rounds", QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        const QString choice = box.clickedButton() == keep ? "KEEP" : box.clickedButton() == rebuild ? "REBUILD" : "";
        if (!choice.isEmpty()) {
            if (choice == "REBUILD")
                view_ = 0;
            guard([&] { cdb::correctResult(podId, outcome, winner, eliminated, choice, reason); }, "Correction");
        }
        return;
    } catch (const cdb::CommanderError &e) {
        warn(this, "Correction", e.message);
    }
    refresh();
}

void CommanderEventScreen::finalizeRound(const QVariantMap &rnd)
{
    const int n = rnd["round_number"].toInt();
    if (!confirm(this, "Finalize round",
                 QStringLiteral("Confirm all results for Round %1?\n\nRound %2 of %3 can then be started.")
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
        const bool open = cdb::getEvent(tid)["stage"].toString() == "SWISS";
        for (const QVariant &rv : cdb::getStandings(tid)) {
            const QVariantMap r = rv.toMap();
            const qint64 pid = r["player_id"].toLongLong();
            const bool dropped = r["dropped"].toInt() != 0;
            auto *h = new QHBoxLayout;
            auto *text = new QVBoxLayout;
            text->setSpacing(0);
            text->addWidget(T::lbl(r["display_name"].toString(), dropped ? T::MUTED : T::TEXT, 14, 600, true));
            text->addWidget(T::lbl(QStringLiteral("%1 pts").arg(r["points"].toInt())
                                       + (dropped ? QStringLiteral("  ·  dropped after round %1").arg(r["drop_round"].toInt()) : QString()),
                                   dropped ? T::RED : T::MUTED, 11));
            h->addLayout(text, 1);
            QPushButton *b = dropped ? smallButton("Undo drop") : smallButton("Drop", true);
            b->setAccessibleName(b->text() + " " + r["display_name"].toString());
            b->setEnabled(open);
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
struct Column {
    QString label;
    int width;      // 0: the stretching player column
};
const QList<Column> COLS{{"#", 46}, {"Player", 0}, {"Pts", 52}, {"W", 40}, {"L", 40}, {"D", 40}, {"Byes", 54},
                         {"OMW%", 76}, {"MW%", 70}, {"Status", 166}};
const int STATUS_GAP = 16;      // space between the last figure and the status text
}

CommanderStandingsScreen::CommanderStandingsScreen(MainWindow *mw, qint64 tournamentId) : mw_(mw), tournamentId_(tournamentId)
{
    state = cdb::getState(tournamentId);
    const QVariantMap ev = state["event"].toMap();
    const QVariantMap legacy = state["legacy"].toMap();
    const bool hasPlayoff = !legacy["playoff_rounds"].toList().isEmpty();

    QVBoxLayout *body = nullptr;
    root->addWidget(T::scrollArea(&body));       // the whole page scrolls; the table also scrolls sideways
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
                  + (final ? QStringLiteral("%1 round%2 played  ·  Tournament complete").arg(done).arg(done == 1 ? "" : "s")
                           : QStringLiteral("%1 of %2 round%3 complete").arg(done).arg(total).arg(total == 1 ? "" : "s"));
    if (final && !ev["champion_name"].toString().isEmpty())
        sub += QStringLiteral("  ·  1st place: ") + ev["champion_name"].toString();
    tv->addWidget(T::lbl(sub, T::MUTED, 13, 400, true));
    top->addLayout(tv, 1);
    auto *actions = new QWidget;
    auto *al = new T::FlowLayout(actions, 8);
    QPushButton *roundsBtn = T::button("Rounds && pods");
    connect(roundsBtn, &QPushButton::clicked, this, [this] { mw_->navigateTo("round", {{"tournament_id", tournamentId_}}); });
    QPushButton *rulesBtn = T::button("Rules");
    connect(rulesBtn, &QPushButton::clicked, this, [this] { showText(this, "Event rules", cdb::rulesText(state["event"].toMap())); });
    QPushButton *csvBtn = T::button("Export CSV");
    connect(csvBtn, &QPushButton::clicked, this, [this] { exportCsv(); });
    for (QPushButton *b : {roundsBtn, rulesBtn, csvBtn})
        al->addWidget(b);
    top->addWidget(actions);
    body->addLayout(top);
    body->addSpacing(8);
    QString order = QStringLiteral("Order: points, opponents' match-win % (OMW), own match-win % (MW). %1% floor; a bye "
                                   "counts as a win.").arg(qRound(ev["settings"].toMap()["tiebreak_floor"].toDouble() * 100));
    if (hasPlayoff)
        order += " Placings here include a playoff played in an earlier version; the points and percentages are from "
                 "the rounds only.";
    body->addWidget(T::lbl(order, T::MUTED, 12, 400, true));
    body->addSpacing(12);

    QFrame *table = T::panel("st");
    int fixed = 200;
    for (const Column &c : COLS)
        fixed += T::px(c.width);
    table->setMinimumWidth(fixed);
    auto *tl = new QVBoxLayout(table);
    tl->setContentsMargins(1, 1, 1, 1);
    tl->setSpacing(0);
    auto *head = new QWidget;
    auto *hh = new QHBoxLayout(head);
    hh->setContentsMargins(20, 11, 20, 11);
    hh->setSpacing(0);
    for (int i = 0; i < COLS.size(); ++i) {
        const Column &c = COLS[i];
        QLabel *lb = T::lbl(c.label.toUpper(), T::MUTED, 10, 700, false, false, 1.0);
        if (c.width) {
            lb->setFixedWidth(T::px(c.width));
            const bool number = i > 1 && i < COLS.size() - 1;      // figures line up on the right
            lb->setAlignment((number ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            if (i == COLS.size() - 1)
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
        for (const QVariant &s : ev["playoff_seeds"].toList())
            seeds.insert(s.toMap()["player_id"].toLongLong(), s.toMap()["seed"].toInt());
    // a finished event shows the placings saved when it was finished
    const QVariantList finalRows = state["final_standings"].toList();
    for (const QVariant &rv : finalRows.isEmpty() ? state["standings"].toList() : finalRows) {
        QVariantMap r = rv.toMap();
        r["place"] = finalRows.isEmpty() ? r["standing"] : r["final_placement"];
        rows << r;
    }
    for (int i = 0; i < rows.size(); ++i)
        tl->addWidget(standingRow(rows[i].toMap(), seeds, i == rows.size() - 1));
    if (rows.isEmpty())
        tl->addWidget(T::lbl("   No players yet", T::MUTED, 13));
    body->addWidget(new T::HScroll(table));     // sideways when the window is narrower than the columns
    body->addStretch();
}

QWidget *CommanderStandingsScreen::standingRow(const QVariantMap &r, const QHash<qint64, int> &seeds, bool last)
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
    const QList<QLabel *> cells{
        T::lbl(QString::number(place), pc.isEmpty() ? T::MUTED : pc, 13, 700, false, true),
        T::lbl(r["display_name"].toString(), dropped ? T::MUTED : T::TEXT, 14, 600, true),
        T::lbl(r["points"].toString(), T::TEXT, 15, 700),
        T::lbl(r["wins"].toString(), T::GREEN, 13, 600),
        T::lbl(r["losses"].toString(), T::RED, 13),
        T::lbl(r["draws"].toString(), T::AMBER, 13),
        T::lbl(r["byes"].toString(), T::GREY, 13),
        T::lbl(QString::number(r["omw_pct"].toDouble(), 'f', 3), T::MUTED, 12, 400, false, true),
        T::lbl(QString::number(r["mw_pct"].toDouble(), 'f', 3), T::MUTED, 12, 400, false, true),
    };
    for (int i = 0; i < cells.size(); ++i) {
        if (COLS[i].width) {
            cells[i]->setFixedWidth(T::px(COLS[i].width));
            cells[i]->setAlignment((i > 1 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
            hl->addWidget(cells[i]);
        } else {
            hl->addWidget(cells[i], 1);
        }
    }
    const qint64 pid = r["player_id"].toLongLong();
    QLabel *status = dropped ? T::lbl(QStringLiteral("Dropped after R%1").arg(r["drop_round"].toInt()), T::RED, 11, 700)
                   : seeds.contains(pid) ? T::lbl(QStringLiteral("Playoff seed %1").arg(seeds.value(pid)), T::PURPLE_LT, 11, 700)
                   : T::lbl("", T::MUTED, 11);
    status->setFixedWidth(T::px(COLS.last().width));
    status->setContentsMargins(T::px(STATUS_GAP), 0, 0, 0);
    hl->addWidget(status);
    return row;
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
    QTextStream out(&f);
    out << "Place,Player,Points,Wins,Losses,Draws,Byes,OMW%,MW%,Dropped\n";
    for (const QVariant &rv : rows) {
        const QVariantMap r = rv.toMap();
        const QString player = printing::csvCell(r["display_name"].toString());
        out << r["place"].toString() << ',' << player << ',' << r["points"].toString() << ',' << r["wins"].toString() << ','
            << r["losses"].toString() << ',' << r["draws"].toString() << ',' << r["byes"].toString() << ','
            << QString::number(r["omw_pct"].toDouble(), 'f', 4) << ',' << QString::number(r["mw_pct"].toDouble(), 'f', 4) << ','
            << (r["dropped"].toInt() ? QStringLiteral("after round %1").arg(r["drop_round"].toInt()) : QString()) << '\n';
    }
    f.close();
    inform(this, "Exported", "Saved to:\n" + path);
}
