#include "commander_db.h"

#include "commander.h"
#include "applog.h"
#include "players.h"
#include "tournaments.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <algorithm>
#include <optional>

using db::Row;
using db::Rows;

namespace cdb {

const char *const FORMAT = "Commander";
const char *const ORGANIZER = "organizer";

static const char *const LEGACY_ROUND = "This is a playoff round kept from an earlier version. It is a read-only record.";

static QString toJson(const QVariant &v)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));
}

static QVariantMap jsonObject(const QVariant &text)
{
    return QJsonDocument::fromJson(text.toString().toUtf8()).object().toVariantMap();
}

static QVariantList jsonArray(const QVariant &text)
{
    return QJsonDocument::fromJson(text.toString().toUtf8()).array().toVariantList();
}

static QString plural(int n, const QString &one = "", const QString &many = "s")
{
    return n == 1 ? one : many;
}

static QVariantMap loadEvent(qint64 tournamentId)
{
    Row ev = db::one("SELECT e.*, t.name, t.status AS tournament_status, t.tournament_date FROM commander_events e "
                     "JOIN tournaments t ON t.tournament_id = e.tournament_id WHERE e.tournament_id = ?", {tournamentId});
    if (ev.isEmpty())
        throw CommanderError("This tournament is not a Commander event.");
    ev["settings"] = jsonObject(ev["settings_json"]);
    ev["playoff_seeds"] = ev["playoff_seeds_json"].isNull() ? QVariantList() : jsonArray(ev["playoff_seeds_json"]);
    ev["legacy_playoff"] = ev["legacy_playoff"].toInt() != 0;
    ev["final_enabled"] = ev["final_enabled"].toInt() != 0;
    ev["scores_second"] = cmdr::scoresSecondPlace(ev["settings"].toMap());
    ev["total_rounds"] = ev["swiss_rounds"];
    return ev;
}

static QString idsText(const QList<qint64> &ids)
{
    QStringList out;
    for (qint64 id : ids)
        out << QString::number(id);
    return out.join(',');
}

static QVariantList idList(const QList<qint64> &ids)
{
    QVariantList out;
    for (qint64 id : ids)
        out << id;
    return out;
}

// elimination groups for the log: "7|3,5" is 7 out first, then 3 and 5 together
static QString groupsText(const QList<QList<qint64>> &groups)
{
    QStringList out;
    for (const QList<qint64> &g : groups)
        out << idsText(g);
    return out.join('|');
}

static QVariantList groupsList(const QList<QList<qint64>> &groups)
{
    QVariantList out;
    for (const QList<qint64> &g : groups)
        out << QVariant(idList(g));
    return out;
}

static Row loadFinalRow(qint64 tournamentId)
{
    return db::one("SELECT * FROM commander_finals WHERE tournament_id = ?", {tournamentId});
}

static Rows loadFinalPlayers(qint64 finalId)
{
    return db::query("SELECT * FROM commander_final_players WHERE final_id = ? ORDER BY seed", {finalId});
}

static QList<qint64> finalSeeds(const Rows &finalists)
{
    QList<qint64> out;
    for (const Row &f : finalists)
        out << f["player_id"].toLongLong();
    return out;
}

static QList<QList<qint64>> finalGroups(const Rows &finalists)
{
    QList<QList<qint64>> out;
    for (int g = 1; g <= 3; ++g) {
        QList<qint64> group;
        for (const Row &f : finalists)
            if (f["elimination_group"].toInt() == g)
                group << f["player_id"].toLongLong();
        if (!group.isEmpty())
            out << group;
    }
    return out;
}

// the final has begun once an elimination or its result is saved, or its clock was started
static bool finalBegun(const Row &fin, const Rows &finalists)
{
    if (fin["status"].toString() == "CONFIRMED" || fin["timer_state"].toString() != "STOPPED"
        || fin["timer_elapsed_secs"].toInt() > 0)
        return true;
    for (const Row &f : finalists)
        if (!f["elimination_group"].isNull())
            return true;
    return false;
}

// The event, for a change to it.  An event that was ended early is a read-only record.
static QVariantMap loadOpenEvent(qint64 tournamentId)
{
    const QVariantMap ev = loadEvent(tournamentId);
    if (ev["tournament_status"].toString() == tdb::TERMINATED)
        throw CommanderError("This tournament was ended early. Its rounds and results are a read-only record.");
    return ev;
}

static Rows loadPlayers(qint64 tournamentId)
{
    return db::query("SELECT e.player_id, p.display_name, p.deleted_at IS NOT NULL AS removed, e.checked_in, e.dropped, "
                     "e.drop_round, e.final_placement FROM enrollments e JOIN players p ON p.player_id = e.player_id "
                     "WHERE e.tournament_id = ? ORDER BY p.display_name COLLATE NOCASE, p.player_id", {tournamentId});
}

// The players as the screens and printouts show them: a name another participant also has
// carries the player's id.  What is saved (results, placings, the final snapshot) uses loadPlayers.
static Rows shownPlayers(qint64 tournamentId)
{
    Rows players = loadPlayers(tournamentId);
    const QSet<qint64> shared = pdb::tournamentDuplicates(tournamentId);
    if (!shared.isEmpty())
        for (Row &p : players)
            p["display_name"] = pdb::label(p["display_name"].toString(), p["player_id"].toLongLong(), shared);
    return players;
}

// Every round with its pods and seats.  "byes" holds player ids.
static QVariantList loadRounds(qint64 tournamentId)
{
    QHash<qint64, QVariantList> seatsByPod, podsByRound, byesByRound;
    for (const Row &s : db::query("SELECT pod_id, player_id, seat_number AS seat, result, eliminated, playoff_seed "
                                  "FROM commander_seats WHERE tournament_id = ? ORDER BY pod_id, seat_number", {tournamentId})) {
        Row seat = s;
        const qint64 pod = seat.take("pod_id").toLongLong();
        seatsByPod[pod] << seat;
    }
    for (const Row &p : db::query("SELECT * FROM commander_pods WHERE tournament_id = ? ORDER BY round_id, pod_number",
                                  {tournamentId})) {
        Row pod = p;
        pod["seats"] = seatsByPod.value(pod["pod_id"].toLongLong());
        pod["reported"] = pod["status"].toString() == "REPORTED";
        podsByRound[pod["round_id"].toLongLong()] << pod;
    }
    for (const Row &b : db::query("SELECT round_id, player_id FROM commander_byes WHERE tournament_id = ? ORDER BY bye_id",
                                  {tournamentId}))
        byesByRound[b["round_id"].toLongLong()] << b["player_id"];

    QVariantList rounds;
    for (const Row &r : db::query("SELECT * FROM commander_rounds WHERE tournament_id = ? ORDER BY round_number", {tournamentId})) {
        Row rnd = r;
        const qint64 id = rnd["round_id"].toLongLong();
        rnd["pods"] = podsByRound.value(id);
        rnd["byes"] = byesByRound.value(id);
        rounds << rnd;
    }
    return rounds;
}

static QVariantList swissOnly(const QVariantList &rounds)
{
    QVariantList out;
    for (const QVariant &r : rounds)
        if (r.toMap().value("stage").toString() == "SWISS")
            out << r;
    return out;
}

static QVariantList computeTable(const QVariantMap &ev, const Rows &players, const QVariantList &rounds)
{
    QList<qint64> field;
    QHash<qint64, Row> info;
    for (const Row &p : players) {
        if (p["checked_in"].toInt()) {
            field << p["player_id"].toLongLong();
            info.insert(p["player_id"].toLongLong(), p);
        }
    }
    QVariantList table = cmdr::computeStandings(field, swissOnly(rounds), ev["settings"].toMap(), ev["base_seed"].toLongLong());
    for (QVariant &v : table) {
        QVariantMap row = v.toMap();
        const Row p = info.value(row["player_id"].toLongLong());
        row["display_name"] = p["display_name"];
        row["dropped"] = p["dropped"].toInt();
        row["drop_round"] = p["drop_round"];
        row["final_placement"] = p["final_placement"];
        v = row;
    }
    return table;
}

// Mirror the recomputed numbers into enrollments for the hub and player profiles.
static void cacheStandings(qint64 tournamentId, const QVariantList &table)
{
    for (const QVariant &v : table) {
        const QVariantMap r = v.toMap();
        db::exec("UPDATE enrollments SET match_points = ?, match_wins = ?, match_losses = ?, match_draws = ?, "
                 "omw_pct = ?, gw_pct = ? WHERE tournament_id = ? AND player_id = ?",
                 {r["points"], r["wins"].toInt() + r["byes"].toInt(), r["losses"], r["draws"],
                  QString::number(r["omw_pct"].toDouble(), 'f', 4).toDouble(),
                  QString::number(r["mw_pct"].toDouble(), 'f', 4).toDouble(), tournamentId, r["player_id"]});
    }
}

static void audit(qint64 tournamentId, const QString &action, const QVariantMap &detail = {},
                  const QVariant &roundId = {}, const QVariant &podId = {})
{
    db::exec("INSERT INTO commander_audit (tournament_id, round_id, pod_id, action, actor, detail_json) "
             "VALUES (?, ?, ?, ?, ?, ?)", {tournamentId, roundId, podId, action, QString(ORGANIZER), toJson(detail)});
}

static int activeCount(const Rows &players)
{
    int n = 0;
    for (const Row &p : players)
        if (p["checked_in"].toInt() && !p["dropped"].toInt())
            ++n;
    return n;
}

static int checkedInCount(const Rows &players)
{
    int n = 0;
    for (const Row &p : players)
        if (p["checked_in"].toInt())
            ++n;
    return n;
}

bool isCommander(qint64 tournamentId)
{
    return db::value("SELECT 1 FROM commander_events WHERE tournament_id = ?", {tournamentId}).isValid();
}

qint64 createCommanderTournament(const QString &name, int expectedPlayers, int swissRounds, const QVariantMap &settings,
                                 qint64 baseSeed, const QString &tournamentDate, int roundTimeMins, bool topFourFinal)
{
    applog::Action log("tournament.create", {{"game", "MTG"}, {"format", "Commander"}, {"rounds", swissRounds},
                                             {"rules", QString(cmdr::RULES_VERSION)}, {"top_four_final", topFourFinal}});
    const QString wanted = tdb::checkedName(name, QStringLiteral("tournament"));
    if (!tdb::isValidRoundMinutes(roundTimeMins))
        throw CommanderError("Round length must be from 1 to 240 minutes.");
    QVariantMap cfg = cmdr::defaultSettings();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        // the scoring is the club's rules for every new event, not a per-event choice
        for (const char *fixed : {"rules_version", "win_points", "loss_points", "draw_points", "bye_points",
                                  "second_place_points"})
            if (it.key() == fixed && it.value() != cfg.value(fixed))
                throw CommanderError(QStringLiteral("The %1 setting is fixed by the club's rules.").arg(it.key()));
        cfg[it.key()] = it.value();
    }
    if (!cmdr::isDrawPolicy(cfg["draw_policy"].toString()))
        throw CommanderError("Unknown draw policy: " + cfg["draw_policy"].toString());
    if (!cmdr::isThreePodPlacement(cfg["three_pod_placement"].toString()))
        throw CommanderError("Unknown three-player pod placement: " + cfg["three_pod_placement"].toString());
    const int recRounds = cmdr::recommendedRounds(expectedPlayers);
    const int rounds = swissRounds == 0 ? recRounds : swissRounds;
    if (rounds < 1)
        throw CommanderError("A Commander event needs at least one round.");
    const qint64 seed = baseSeed < 0 ? qint64(QRandomGenerator::global()->generate() & 0x7fffffffU) : baseSeed;
    const QString date = tournamentDate.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : tournamentDate;

    db::Tx tx;
    const qint64 tid = db::exec(
        "INSERT INTO tournaments (name, game, format, tournament_date, total_rounds, top_cut, round_time_mins, status) "
        "VALUES (?, 'MTG', ?, ?, ?, 0, ?, 'PENDING')", {wanted, QString(FORMAT), date, rounds, roundTimeMins}).lastId;
    db::exec("INSERT INTO commander_events (tournament_id, rules_version, settings_json, recommended_rounds, "
             "swiss_rounds, recommended_cut, playoff_cut, base_seed, final_enabled) VALUES (?, ?, ?, ?, ?, 0, 0, ?, ?)",
             {tid, QString(cmdr::RULES_VERSION), toJson(cfg), recRounds, rounds, seed, topFourFinal ? 1 : 0});
    audit(tid, "EVENT_CREATED", {{"rounds", rounds}, {"base_seed", seed}, {"rules_version", QString(cmdr::RULES_VERSION)},
                                 {"top_four_final", topFourFinal}});
    tx.commit();
    log.set("tournament", tid);
    return tid;
}

QVariantMap getEvent(qint64 tournamentId)
{
    QVariantMap ev = loadEvent(tournamentId);
    const Rows players = loadPlayers(tournamentId);
    ev["checked_in_count"] = checkedInCount(players);
    ev["active_count"] = activeCount(players);
    ev["enrolled_count"] = int(players.size());
    return ev;
}

Rows getRegistrations(qint64 tournamentId)
{
    return shownPlayers(tournamentId);
}

QVariantMap recommendation(int playerCount)
{
    QVariantMap out{{"players", playerCount}, {"rounds", cmdr::recommendedRounds(playerCount)},
                    {"pods", QVariantList()}, {"byes", 0}, {"error", QString()}};
    try {
        const cmdr::PodSizes sizes = cmdr::podSizes(playerCount);
        QVariantList pods;
        for (int s : sizes.sizes)
            pods << s;
        out["pods"] = pods;
        out["byes"] = sizes.byes;
    } catch (const cmdr::PairingError &e) {
        out["error"] = QString::fromStdString(e.what());
    }
    return out;
}

void setCheckedIn(qint64 tournamentId, qint64 playerId, bool checkedIn)
{
    applog::Action log("player.check_in", {{"tournament", tournamentId}, {"player", playerId}, {"checked_in", checkedIn}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (ev["stage"].toString() != "REGISTRATION")
        throw CommanderError("Check-in closes when the event starts. Use a drop instead.");
    const int changed = db::exec("UPDATE enrollments SET checked_in = ? WHERE tournament_id = ? AND player_id = ?",
                                 {checkedIn ? 1 : 0, tournamentId, playerId}).affected;
    if (changed != 1)
        throw CommanderError("That player is not registered for this event.");
    audit(tournamentId, checkedIn ? "CHECK_IN" : "CHECK_IN_REMOVED", {{"player_id", playerId}});
    tx.commit();
}

void updateEventConfig(qint64 tournamentId, int swissRounds, const QString &drawPolicy, const QString &threePodPlacement,
                       int topFourFinal)
{
    applog::Action log("event.configure", {{"tournament", tournamentId}, {"top_four_final", topFourFinal}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (ev["stage"].toString() != "REGISTRATION")
        throw CommanderError("The number of rounds and the scoring are frozen once the event has started.");
    const int rounds = swissRounds == 0 ? ev["swiss_rounds"].toInt() : swissRounds;
    QVariantMap settings = ev["settings"].toMap();
    if (!drawPolicy.isEmpty()) {
        if (!cmdr::isDrawPolicy(drawPolicy))
            throw CommanderError("Unknown draw policy: " + drawPolicy);
        settings["draw_policy"] = drawPolicy;
    }
    if (!threePodPlacement.isEmpty()) {
        if (!cmdr::isThreePodPlacement(threePodPlacement))
            throw CommanderError("Unknown three-player pod placement: " + threePodPlacement);
        settings["three_pod_placement"] = threePodPlacement;
        QVariantMap pairing = settings["pairing"].toMap();
        if (!pairing.contains("placement"))
            pairing["placement"] = cmdr::defaultSettings()["pairing"].toMap()["placement"];
        settings["pairing"] = pairing;
    }
    if (rounds < 1)
        throw CommanderError("A Commander event needs at least one round.");
    const bool final = topFourFinal < 0 ? ev["final_enabled"].toBool() : topFourFinal != 0;
    if (final && !ev["final_enabled"].toBool() && !ev["scores_second"].toBool())
        throw CommanderError("This event was created with earlier rules, which have no top-four final.");
    db::exec("UPDATE commander_events SET swiss_rounds = ?, settings_json = ?, final_enabled = ? WHERE tournament_id = ?",
             {rounds, toJson(settings), final ? 1 : 0, tournamentId});
    db::exec("UPDATE tournaments SET total_rounds = ? WHERE tournament_id = ?", {rounds, tournamentId});
    audit(tournamentId, "CONFIG_UPDATED",
          {{"rounds", rounds}, {"draw_policy", settings["draw_policy"]},
           {"three_pod_placement", settings.value("three_pod_placement", cmdr::LEGACY_THREE_POD_PLACEMENT)},
           {"top_four_final", final}});
    tx.commit();
}

QVariantMap syncRecommendation(qint64 tournamentId)
{
    db::Tx tx;
    QVariantMap ev = loadEvent(tournamentId);
    if (ev["stage"].toString() != "REGISTRATION" || ev["tournament_status"].toString() == tdb::TERMINATED)
        return ev;
    const int n = checkedInCount(loadPlayers(tournamentId));
    const int recRounds = cmdr::recommendedRounds(n);
    const int current = ev["swiss_rounds"].toInt();
    const int rounds = current == ev["recommended_rounds"].toInt() ? recRounds : current;
    if (rounds != current || recRounds != ev["recommended_rounds"].toInt()) {
        db::exec("UPDATE commander_events SET swiss_rounds = ?, recommended_rounds = ? WHERE tournament_id = ?",
                 {rounds, recRounds, tournamentId});
        db::exec("UPDATE tournaments SET total_rounds = ? WHERE tournament_id = ?", {rounds, tournamentId});
    }
    ev = loadEvent(tournamentId);
    tx.commit();
    return ev;
}

// what the organizer does next: SWISS (publish the next round), WAIT (a round is in
// play), FINAL (create the top-four final), FINAL_PLAYING (report it) or NONE.  nothing
// follows the configured rounds except the one final of an event that chose it.
static QString nextStep(const QVariantMap &ev, const QVariantList &rounds)
{
    if (ev["stage"].toString() != "SWISS" || ev["tournament_status"].toString() == tdb::TERMINATED)
        return "NONE";
    if (!rounds.isEmpty() && rounds.last().toMap().value("status").toString() == "ACTIVE")
        return "WAIT";
    if (rounds.size() < ev["swiss_rounds"].toInt())
        return "SWISS";
    if (!ev["final_enabled"].toBool())
        return "NONE";
    return loadFinalRow(ev["tournament_id"].toLongLong()).isEmpty() ? "FINAL" : "FINAL_PLAYING";
}

static qint64 publish(qint64 tournamentId, int expectedRoundNumber)
{
    const QVariantMap ev = loadOpenEvent(tournamentId);
    const Rows players = loadPlayers(tournamentId);
    const QVariantList rounds = loadRounds(tournamentId);
    const int total = ev["swiss_rounds"].toInt();
    const int number = int(rounds.size()) + 1;
    if (expectedRoundNumber != NONE && expectedRoundNumber != number) {
        for (const QVariant &r : rounds)
            if (r.toMap().value("round_number").toInt() == expectedRoundNumber)
                return r.toMap().value("round_id").toLongLong();      // repeated request: the round is already published
        if (expectedRoundNumber > total)
            throw CommanderError(QStringLiteral("This event has %1 round%2; Round %3 cannot be created.")
                                     .arg(total).arg(plural(total)).arg(expectedRoundNumber));
        throw ConflictError(QStringLiteral("Round %1 cannot be published yet.").arg(expectedRoundNumber));
    }
    const QString kind = nextStep(ev, rounds);
    const QString stage = ev["stage"].toString();
    if (kind == "WAIT")
        throw CommanderError("Finalize the current round's results before publishing the next round.");
    if (kind == "NONE") {
        if (stage == "COMPLETE")
            throw CommanderError("This tournament is finished. No further round can be created.");
        if (stage == "PLAYOFF")
            throw CommanderError("Playoff rounds are no longer supported. Finish this tournament on its standings.");
        if (stage == "REGISTRATION")
            throw CommanderError("This tournament has not started.");
        throw CommanderError(QStringLiteral("All %1 rounds of this event have been created. Finish the tournament instead.")
                                 .arg(total));
    }
    if (kind == "FINAL" || kind == "FINAL_PLAYING")
        throw CommanderError(QStringLiteral("All %1 qualifying rounds have been played. The top-four final follows; "
                                            "no further round can be created.").arg(total));

    QVariantList active;
    for (const QVariant &r : computeTable(ev, players, rounds))
        if (!r.toMap().value("dropped").toInt())
            active << r;
    const QVariantMap inputs = cmdr::pairingInputs(active, rounds, number, ev["settings"].toMap());
    QVariantMap pairing;
    try {
        pairing = cmdr::pairRound(inputs, cmdr::deriveSeed(ev["base_seed"].toLongLong(), number));
    } catch (const cmdr::PairingError &e) {
        throw CommanderError(QString::fromStdString(e.what()));
    }

    const qint64 roundId = db::exec(
        "INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round, pairing_algorithm, "
        "pairing_seed, pairing_inputs_json, pairing_cost_json) VALUES (?, 'SWISS', ?, ?, ?, ?, ?, ?)",
        {tournamentId, number, number, pairing["algorithm"], pairing["seed"], toJson(inputs), toJson(pairing["cost"])}).lastId;
    const QVariantList pods = pairing["pods"].toList();
    for (int i = 0; i < pods.size(); ++i) {
        const qint64 podId = db::exec("INSERT INTO commander_pods (round_id, tournament_id, pod_number) VALUES (?, ?, ?)",
                                      {roundId, tournamentId, i + 1}).lastId;
        const QVariantList seats = pods[i].toList();
        for (int s = 0; s < seats.size(); ++s)
            db::exec("INSERT INTO commander_seats (pod_id, round_id, tournament_id, player_id, seat_number) "
                     "VALUES (?, ?, ?, ?, ?)", {podId, roundId, tournamentId, seats[s], s + 1});
    }
    for (const QVariant &pid : pairing["byes"].toList())
        db::exec("INSERT INTO commander_byes (round_id, tournament_id, player_id) VALUES (?, ?, ?)",
                 {roundId, tournamentId, pid});
    audit(tournamentId, "ROUND_PUBLISHED",
          {{"round_number", number}, {"of", total}, {"pods", int(pods.size())}, {"byes", pairing["byes"]},
           {"seed", pairing["seed"]}}, roundId);
    return roundId;
}

qint64 startEvent(qint64 tournamentId)
{
    applog::Action log("tournament.start", {{"tournament", tournamentId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (ev["stage"].toString() != "REGISTRATION")
        throw ConflictError("This event has already started.");
    const int n = checkedInCount(loadPlayers(tournamentId));
    try {
        cmdr::podSizes(n);
    } catch (const cmdr::PairingError &e) {
        throw CommanderError(QString::fromStdString(e.what()));
    }
    if (ev["final_enabled"].toBool() && n < 4)
        throw CommanderError(QStringLiteral("This event ends with a top-four final, which needs at least four "
                                            "checked-in players; %1 %2 checked in.").arg(n).arg(n == 1 ? "is" : "are"));
    db::exec("UPDATE commander_events SET stage = 'SWISS', started_at = DATETIME('now'), started_player_count = ?, "
             "recommended_rounds = ? WHERE tournament_id = ?", {n, cmdr::recommendedRounds(n), tournamentId});
    db::exec("UPDATE tournaments SET status = 'IN_PROGRESS', total_rounds = ?, top_cut = 0 WHERE tournament_id = ?",
             {ev["swiss_rounds"], tournamentId});
    audit(tournamentId, "EVENT_STARTED", {{"players", n}, {"rounds", ev["swiss_rounds"]}, {"settings", ev["settings"]},
                                          {"top_four_final", ev["final_enabled"]}});
    const qint64 roundId = publish(tournamentId, NONE);
    tx.commit();
    return roundId;
}

qint64 publishNextRound(qint64 tournamentId, int expectedRoundNumber)
{
    applog::Action log("round.publish", {{"tournament", tournamentId}, {"round_number", expectedRoundNumber}});
    try {
        db::Tx tx;
        const qint64 roundId = publish(tournamentId, expectedRoundNumber);
        tx.commit();
        return roundId;
    } catch (const db::Error &e) {
        if (e.constraint)
            throw ConflictError(QStringLiteral("That round cannot be created (%1).").arg(e.message));
        throw;
    }
}

using Results = QMap<qint64, cmdr::SeatResult>;

static QVariantMap loadPod(qint64 podId)
{
    Row pod = db::one("SELECT p.*, r.stage, r.status AS round_status, r.round_number, r.stage_round "
                      "FROM commander_pods p JOIN commander_rounds r ON r.round_id = p.round_id WHERE p.pod_id = ?", {podId});
    if (pod.isEmpty())
        throw CommanderError("That pod does not exist.");
    QVariantList seats;
    for (const Row &s : db::query("SELECT player_id, seat_number AS seat, result, eliminated, playoff_seed "
                                  "FROM commander_seats WHERE pod_id = ? ORDER BY seat_number", {podId}))
        seats << s;
    pod["seats"] = seats;
    return pod;
}

static std::optional<Results> currentResults(const QVariantMap &pod)
{
    if (pod["status"].toString() != "REPORTED")
        return std::nullopt;
    Results out;
    for (const QVariant &sv : pod["seats"].toList()) {
        const QVariantMap s = sv.toMap();
        out[s["player_id"].toLongLong()] = {s["result"].toString(), s["eliminated"].toInt()};
    }
    return out;
}

static Results resolve(const QVariantMap &pod, const QVariantMap &settings, const QString &outcome, qint64 winnerId,
                       const QList<qint64> &eliminatedIds)
{
    if (pod["stage"].toString() != "SWISS")
        throw CommanderError(LEGACY_ROUND);
    QList<qint64> ids;
    for (const QVariant &s : pod["seats"].toList())
        ids << s.toMap().value("player_id").toLongLong();
    try {
        return cmdr::seatResults(ids, outcome, winnerId, eliminatedIds, settings);
    } catch (const std::invalid_argument &e) {
        throw CommanderError(QString::fromStdString(e.what()));
    }
}

// the second place as it will be saved: an event that scores it keeps all three decisions
// apart; an earlier event keeps only a named player, as before
static SecondPlace checkSecondPlace(const QVariantMap &pod, bool scoring, const QString &outcome, qint64 winnerId,
                                    const SecondPlace &second)
{
    if (second.kind == SecondPlace::NotRecorded)
        return second;
    if (outcome != "WIN")
        throw CommanderError("Second place can only be recorded when the pod has a winner.");
    if (second.kind == SecondPlace::NoClearSecond)
        return scoring ? second : SecondPlace();
    if (second.playerId == winnerId)
        throw CommanderError("The winner cannot also be second.");
    for (const QVariant &s : pod["seats"].toList())
        if (s.toMap().value("player_id").toLongLong() == second.playerId)
            return second;
    throw CommanderError("Second place must be a player in this pod.");
}

static SecondPlace savedSecondPlace(const QVariantMap &pod)
{
    if (pod["second_place_status"].toString() == "NONE")
        return SecondPlace::noClearSecond();
    return SecondPlace(pod["second_place_player_id"].toLongLong());
}

static QVariant idOrNull(qint64 id)
{
    return id ? QVariant(id) : QVariant();
}

// PENDING, PLAYER or NONE in an event that scores second place, for a won pod; null otherwise
static QVariant secondState(bool scoring, const QString &outcome, const SecondPlace &second)
{
    if (!scoring || outcome != "WIN")
        return QVariant();
    return second.kind == SecondPlace::Player ? QString("PLAYER") : second.kind == SecondPlace::NoClearSecond
        ? QString("NONE") : QString("PENDING");
}

static QString secondText(const QVariant &state, qint64 id)
{
    return state.isNull() ? (id ? QStringLiteral("player") : QStringLiteral("none")) : state.toString().toLower();
}

static void applyResults(const QVariantMap &pod, const QString &outcome, const Results &results, bool scoring,
                         const SecondPlace &second)
{
    for (auto it = results.begin(); it != results.end(); ++it)
        db::exec("UPDATE commander_seats SET result = ?, eliminated = ? WHERE pod_id = ? AND player_id = ?",
                 {it.value().result, it.value().eliminated, pod["pod_id"], it.key()});
    db::exec("UPDATE commander_pods SET status = 'REPORTED', outcome = ?, second_place_player_id = ?, second_place_status = ?, "
             "result_version = result_version + 1, reported_at = DATETIME('now') WHERE pod_id = ?",
             {outcome, idOrNull(second.playerId), secondState(scoring, outcome, second), pod["pod_id"]});
}

// what an audit entry says about a pod's second place: the player and the decision
static QVariantMap secondDetail(const QString &prefix, bool scoring, const QString &outcome, const SecondPlace &second)
{
    const QVariant state = secondState(scoring, outcome, second);
    return {{prefix, idOrNull(second.playerId)}, {prefix + "_state", state}};
}

static void addDetail(QVariantMap &into, const QVariantMap &more)
{
    for (auto it = more.begin(); it != more.end(); ++it)
        into.insert(it.key(), it.value());
}

static QVariant resultsDetail(const std::optional<Results> &r)
{
    if (!r)
        return QVariant();
    QVariantMap out;
    for (auto it = r->begin(); it != r->end(); ++it)
        out[QString::number(it.key())] = QVariantList{it.value().result, it.value().eliminated};
    return out;
}

static QVariantList refreshCache(qint64 tournamentId, QVariantMap *evOut = nullptr)
{
    const QVariantMap ev = loadEvent(tournamentId);
    const QVariantList table = computeTable(ev, loadPlayers(tournamentId), loadRounds(tournamentId));
    cacheStandings(tournamentId, table);
    if (evOut)
        *evOut = ev;
    return table;
}

int reportPodResult(qint64 podId, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminatedIds,
                    int expectedVersion, const SecondPlace &requested)
{
    applog::Action log("result.report", {{"pod", podId}, {"pod_result", outcome}, {"winner", winnerId},
                                         {"second", requested.playerId}, {"second_decision", int(requested.kind)}});
    db::Tx tx;
    const QVariantMap pod = loadPod(podId);
    if (pod["stage"].toString() != "SWISS")
        throw CommanderError(LEGACY_ROUND);
    if (pod["round_status"].toString() != "ACTIVE")
        throw CommanderError("This round is finalized. Use a correction to change its results.");
    const qint64 tid = pod["tournament_id"].toLongLong();
    const QVariantMap ev = loadOpenEvent(tid);
    const bool scoring = ev["scores_second"].toBool();
    const Results results = resolve(pod, ev["settings"].toMap(), outcome, winnerId, eliminatedIds);
    const SecondPlace second = checkSecondPlace(pod, scoring, outcome, winnerId, requested);
    const std::optional<Results> before = currentResults(pod);
    const SecondPlace previous = savedSecondPlace(pod);
    const int version = pod["result_version"].toInt();
    if (before && *before == results && previous == second) {
        log.outcome("no-change");
        return version;
    }
    if (expectedVersion != NONE && expectedVersion != version)
        throw ConflictError("This pod's result was changed elsewhere. Reload it and try again.");
    if (before && expectedVersion == NONE)
        throw ConflictError("This pod already has a different result. Reload it to correct that result.");
    applyResults(pod, outcome, results, scoring, second);
    QVariantMap detail{{"before", resultsDetail(before)}, {"after", resultsDetail(results)}, {"outcome", outcome}};
    addDetail(detail, secondDetail("second_place_before", scoring, pod["outcome"].toString(), before ? previous : SecondPlace()));
    addDetail(detail, secondDetail("second_place", scoring, outcome, second));
    audit(tid, before ? "RESULT_CORRECTED" : "RESULT_REPORTED", detail, pod["round_id"], podId);
    refreshCache(tid);
    tx.commit();
    log.set("second_state", secondText(secondState(scoring, outcome, second), second.playerId));
    return version + 1;
}

int clearPodResult(qint64 podId, int expectedVersion)
{
    applog::Action log("result.clear", {{"pod", podId}});
    db::Tx tx;
    const QVariantMap pod = loadPod(podId);
    if (pod["stage"].toString() != "SWISS")
        throw CommanderError(LEGACY_ROUND);
    if (pod["round_status"].toString() != "ACTIVE")
        throw CommanderError("This round is finalized. Use a correction to change its results.");
    loadOpenEvent(pod["tournament_id"].toLongLong());
    const int version = pod["result_version"].toInt();
    if (pod["status"].toString() == "PENDING")
        return version;
    if (expectedVersion != NONE && expectedVersion != version)
        throw ConflictError("This pod's result was changed elsewhere. Reload it and try again.");
    const qint64 tid = pod["tournament_id"].toLongLong();
    db::exec("UPDATE commander_seats SET result = NULL, eliminated = 0 WHERE pod_id = ?", {podId});
    db::exec("UPDATE commander_pods SET status = 'PENDING', outcome = NULL, advancing_player_id = NULL, "
             "second_place_player_id = NULL, second_place_status = NULL, result_version = result_version + 1, "
             "reported_at = NULL WHERE pod_id = ?", {podId});
    const bool scoring = loadEvent(tid)["scores_second"].toBool();
    QVariantMap detail{{"before", resultsDetail(currentResults(pod))}, {"after", QVariant()}};
    addDetail(detail, secondDetail("second_place_before", scoring, pod["outcome"].toString(), savedSecondPlace(pod)));
    audit(tid, "RESULT_CLEARED", detail, pod["round_id"], podId);
    refreshCache(tid);
    tx.commit();
    return version + 1;
}

// Final placings: the standings order, with the best player still in the event first.
static QVariantList finalTable(const QVariantList &table)
{
    if (table.isEmpty())
        return {};
    int first = 0;
    for (int i = 0; i < table.size(); ++i) {
        if (!table[i].toMap().value("dropped").toInt()) {
            first = i;
            break;
        }
    }
    QVariantList out{table[first]};
    for (int i = 0; i < table.size(); ++i)
        if (i != first)
            out << table[i];
    return out;
}

// final placings of an event with a confirmed final: the four finalists in the places the
// final gave them, then everyone else in qualifying order.  nothing else moves them.
static QVariantList finalPlacings(const QVariantList &table, const Rows &finalists)
{
    QHash<qint64, QVariantMap> rows;
    for (const QVariant &v : table)
        rows.insert(v.toMap().value("player_id").toLongLong(), v.toMap());
    QVariantList out;
    for (int place = 1; place <= 4; ++place) {
        for (const Row &f : finalists) {
            if (f["placement"].toInt() != place)
                continue;
            QVariantMap r = rows.value(f["player_id"].toLongLong());
            r["final_seed"] = f["seed"];
            r["final_basis"] = f["placement_basis"];
            r["final_group"] = f["elimination_group"];
            out << r;
        }
    }
    QSet<qint64> placed;
    for (const Row &f : finalists)
        placed.insert(f["player_id"].toLongLong());
    for (const QVariant &v : table)
        if (!placed.contains(v.toMap().value("player_id").toLongLong()))
            out << v;
    return out;
}

// Saves the completed status, each player's final placing and a snapshot of the final standings.
static void complete(qint64 tournamentId, const QString &action = "EVENT_COMPLETED")
{
    const QVariantMap ev = loadEvent(tournamentId);
    const QVariantList qualifying = computeTable(ev, loadPlayers(tournamentId), loadRounds(tournamentId));
    QVariantList table;
    if (ev["final_enabled"].toBool()) {
        const Row fin = loadFinalRow(tournamentId);
        if (fin.isEmpty() || fin["status"].toString() != "CONFIRMED")
            throw CommanderError("The top-four final has no confirmed result, so the tournament cannot be completed.");
        table = finalPlacings(qualifying, loadFinalPlayers(fin["final_id"].toLongLong()));
    } else {
        table = finalTable(qualifying);
    }
    cacheStandings(tournamentId, table);
    QVariantList snapshot;
    for (int i = 0; i < table.size(); ++i) {
        const QVariantMap r = table[i].toMap();
        QVariantMap row{{"place", i + 1}, {"player_id", r["player_id"]}, {"display_name", r["display_name"]},
                        {"points", r["points"]}, {"wins", r["wins"]}, {"losses", r["losses"]},
                        {"draws", r["draws"]}, {"byes", r["byes"]}, {"omw_pct", r["omw_pct"]},
                        {"mw_pct", r["mw_pct"]}, {"dropped", r["dropped"].toInt() != 0}};
        if (ev["scores_second"].toBool())
            row["seconds"] = r["seconds"];
        if (ev["final_enabled"].toBool()) {
            row["qualifying_standing"] = r["standing"];
            if (r.contains("final_seed")) {
                row["final_seed"] = r["final_seed"];
                row["final_basis"] = r["final_basis"];
            }
        }
        snapshot << row;
        db::exec("UPDATE enrollments SET final_placement = ? WHERE tournament_id = ? AND player_id = ?",
                 {i + 1, tournamentId, r["player_id"]});
    }
    const QVariant winner = snapshot.isEmpty() ? QVariant() : snapshot.first().toMap().value("player_id");
    db::exec("UPDATE commander_events SET stage = 'COMPLETE', champion_player_id = ?, final_standings_json = ?, "
             "completed_at = COALESCE(completed_at, DATETIME('now')) WHERE tournament_id = ?",
             {winner, toJson(snapshot), tournamentId});
    db::exec("UPDATE tournaments SET status = 'COMPLETED' WHERE tournament_id = ?", {tournamentId});
    QVariantMap detail{{"winner_player_id", winner}, {"players", int(snapshot.size())}};
    if (ev["final_enabled"].toBool())
        detail["first_place_basis"] = snapshot.first().toMap().value("final_basis");
    audit(tournamentId, action, detail);
}

// won pods whose second place is still to be decided, in an event that scores it
static Rows pendingSeconds(qint64 roundId)
{
    return db::query("SELECT pod_id, pod_number FROM commander_pods WHERE round_id = ? AND second_place_status = 'PENDING' "
                     "ORDER BY pod_number", {roundId});
}

// Returns true if the round was finalized now, false if it already was.
static bool finalize(const Row &rnd)
{
    if (rnd["stage"].toString() != "SWISS")
        throw CommanderError(LEGACY_ROUND);
    if (rnd["status"].toString() == "FINALIZED")
        return false;
    const qint64 roundId = rnd["round_id"].toLongLong();
    const qint64 tid = rnd["tournament_id"].toLongLong();
    loadOpenEvent(tid);
    const int pending = db::value("SELECT COUNT(*) FROM commander_pods WHERE round_id = ? AND status = 'PENDING'",
                                  {roundId}).toInt();
    if (pending)
        throw CommanderError(QStringLiteral("%1 pod%2 still %3 no result.")
                                 .arg(pending).arg(plural(pending)).arg(pending == 1 ? "has" : "have"));
    const Rows seconds = pendingSeconds(roundId);
    if (!seconds.isEmpty()) {
        QStringList pods;
        for (const Row &p : seconds)
            pods << p["pod_number"].toString();
        QList<qint64> podIds;
        for (const Row &p : seconds)
            podIds << p["pod_id"].toLongLong();
        // the operation then ends as failed; this says why, by pod id
        applog::info("round.blocked", {{"round", roundId}, {"reason", "second_place_pending"}, {"pods", idsText(podIds)}});
        throw CommanderError(QStringLiteral("Second place is not recorded yet in pod%1 %2. Choose the second-place "
                                            "player, or confirm there was no clear second, before finalizing.")
                                 .arg(plural(int(seconds.size()))).arg(pods.join(", ")));
    }
    db::exec("UPDATE commander_rounds SET status = 'FINALIZED', finalized_at = DATETIME('now') WHERE round_id = ?", {roundId});
    db::exec("UPDATE commander_pods SET confirmed_at = DATETIME('now') WHERE round_id = ?", {roundId});
    QVariantMap ev = loadEvent(tid);
    const bool last = rnd["round_number"].toInt() >= ev["swiss_rounds"].toInt();
    audit(tid, "ROUND_FINALIZED", {{"round_number", rnd["round_number"]},
                                   {"qualifying_complete", last && ev["final_enabled"].toBool()}}, roundId);
    refreshCache(tid, &ev);
    // with a top-four final the event goes on: the final is created from these standings
    if (last && !ev["final_enabled"].toBool())
        complete(tid);
    return true;
}

bool finalizeRound(qint64 roundId)
{
    applog::Action log("round.finalize", {{"round", roundId}});
    db::Tx tx;
    const Row rnd = db::one("SELECT * FROM commander_rounds WHERE round_id = ?", {roundId});
    if (rnd.isEmpty())
        throw CommanderError("That round does not exist.");
    const bool changed = finalize(rnd);
    tx.commit();
    return changed;
}

bool finishTournament(qint64 tournamentId)
{
    applog::Action log("tournament.finalize", {{"tournament", tournamentId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    const QString stage = ev["stage"].toString();
    if (stage == "COMPLETE")
        return false;
    if (stage == "PLAYOFF")
        throw CommanderError("This event was in a playoff from an earlier version. "
                             "Close it on its standings instead.");
    if (ev["final_enabled"].toBool())
        throw CommanderError("This event ends with a top-four final. Finalize the last round, then create the final "
                             "and confirm its result.");
    const Row last = db::one("SELECT * FROM commander_rounds WHERE tournament_id = ? ORDER BY round_number DESC LIMIT 1",
                             {tournamentId});
    if (stage != "SWISS" || last.isEmpty())
        throw CommanderError("This tournament has not started.");
    const int total = ev["swiss_rounds"].toInt();
    if (last["round_number"].toInt() < total)
        throw CommanderError(QStringLiteral("Round %1 of %2 is the latest round. The tournament can be finished after Round %2.")
                                 .arg(last["round_number"].toInt()).arg(total));
    finalize(last);
    tx.commit();
    return true;
}

static bool hasPlayoffRounds(qint64 tournamentId)
{
    return db::value("SELECT 1 FROM commander_rounds WHERE tournament_id = ? AND stage <> 'SWISS' LIMIT 1",
                     {tournamentId}).isValid();
}

// the players who would play the final: the best four in the qualifying standings who are
// checked in and have not dropped
struct Candidates {
    QVariantList eligible;
    QList<qint64> seeds;
};

static Candidates candidatesFrom(const QVariantList &table)
{
    Candidates c;
    for (const QVariant &v : table)
        if (!v.toMap().value("dropped").toInt())
            c.eligible << v;
    for (int i = 0; i < c.eligible.size() && i < 4; ++i)
        c.seeds << c.eligible[i].toMap().value("player_id").toLongLong();
    return c;
}

bool correctResult(qint64 podId, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminatedIds,
                   const QString &resolution, const QString &reason, const SecondPlace &requested)
{
    applog::Action log("result.correct", {{"pod", podId}, {"pod_result", outcome}, {"winner", winnerId},
                                          {"second", requested.playerId}, {"second_decision", int(requested.kind)},
                                          {"resolution", resolution}});
    if (!resolution.isEmpty() && resolution != "KEEP" && resolution != "REBUILD" && resolution != "REBUILD_FINAL")
        throw CommanderError("Resolution must be KEEP, REBUILD or REBUILD_FINAL.");
    db::Tx tx;
    const QVariantMap pod = loadPod(podId);
    const qint64 tid = pod["tournament_id"].toLongLong();
    QVariantMap ev = loadOpenEvent(tid);
    const bool scoring = ev["scores_second"].toBool();
    const Results results = resolve(pod, ev["settings"].toMap(), outcome, winnerId, eliminatedIds);
    const SecondPlace second = checkSecondPlace(pod, scoring, outcome, winnerId, requested);
    const bool finalized = pod["round_status"].toString() == "FINALIZED";
    if (scoring && finalized && outcome == "WIN" && second.kind == SecondPlace::NotRecorded)
        throw CommanderError("This round is finalized, so its second places are decided: choose the second-place "
                             "player, or confirm there was no clear second.");
    const std::optional<Results> before = currentResults(pod);
    const SecondPlace previous = savedSecondPlace(pod);
    const bool resultsChanged = !before || !(*before == results);
    // second place is part of the score where it earns points; in an earlier event it changes none
    const bool scoreChanged = resultsChanged || (scoring && previous != second);
    if (!resultsChanged && previous == second) {
        log.outcome("no-change");
        return false;
    }
    const int roundNumber = pod["round_number"].toInt();
    QVariantList later, laterNumbers;
    QStringList laterText;
    bool laterPlayoff = false;
    for (const Row &r : db::query("SELECT round_id, round_number, stage FROM commander_rounds WHERE tournament_id = ? "
                                  "AND round_number > ? ORDER BY round_number", {tid, roundNumber})) {
        later << r;
        laterNumbers << r["round_number"];
        laterText << r["round_number"].toString();
        laterPlayoff = laterPlayoff || r["stage"].toString() != "SWISS";
    }
    if (finalized && !later.isEmpty() && scoreChanged && resolution.isEmpty())
        throw ResolutionRequired(
            QStringLiteral("Round %1 is finalized and round%2 %3 %4 already published from it. "
                           "Choose KEEP (leave later rounds as published) or REBUILD (discard and redo them).")
                .arg(roundNumber).arg(later.size() > 1 ? "s" : "").arg(laterText.join(", "))
                .arg(later.size() > 1 ? "were" : "was"),
            later);

    if (finalized && !later.isEmpty() && scoreChanged && resolution == "REBUILD") {
        if (laterPlayoff)
            throw CommanderError("Later rounds include a playoff kept from an earlier version, "
                                 "which is never deleted. Use KEEP.");
        const int played = db::value(
            "SELECT COUNT(*) FROM commander_pods p JOIN commander_rounds r ON r.round_id = p.round_id "
            "WHERE p.tournament_id = ? AND r.round_number > ? AND p.status = 'REPORTED'", {tid, roundNumber}).toInt();
        if (played)
            throw CommanderError("Later rounds already have results, so they cannot be rebuilt. "
                                 "Use KEEP, or clear those results first.");
        db::exec("DELETE FROM commander_rounds WHERE tournament_id = ? AND round_number > ?", {tid, roundNumber});
        db::exec("UPDATE commander_events SET stage = 'SWISS', champion_player_id = NULL, final_standings_json = NULL, "
                 "completed_at = NULL WHERE tournament_id = ?", {tid});
        db::exec("UPDATE tournaments SET status = 'IN_PROGRESS' WHERE tournament_id = ?", {tid});
        db::exec("UPDATE enrollments SET final_placement = NULL WHERE tournament_id = ?", {tid});
    }

    applyResults(pod, outcome, results, scoring, second);
    QVariantMap detail{{"before", resultsDetail(before)}, {"after", resultsDetail(results)}, {"outcome", outcome},
                       {"reason", reason}, {"resolution", resolution.isEmpty() ? QVariant() : QVariant(resolution)},
                       {"later_rounds", laterNumbers}};
    addDetail(detail, secondDetail("second_place_before", scoring, pod["outcome"].toString(), before ? previous : SecondPlace()));
    addDetail(detail, secondDetail("second_place", scoring, outcome, second));

    // a final is seeded from the qualifying standings: it is never reseeded quietly
    const Row fin = loadFinalRow(tid);
    QVariantList seedsBefore, seedsAfter;
    if (!fin.isEmpty() && scoreChanged) {
        const Rows finalists = loadFinalPlayers(fin["final_id"].toLongLong());
        const QList<qint64> saved = finalSeeds(finalists);
        const Candidates now = candidatesFrom(computeTable(ev, loadPlayers(tid), loadRounds(tid)));
        if (now.seeds != saved) {
            if (finalBegun(fin, finalists))
                throw CommanderError(QStringLiteral("This correction would change who plays the final or their seeds, "
                                                    "and the final has already %1. The correction cannot be saved.")
                                         .arg(fin["status"].toString() == "CONFIRMED" ? "been played" : "begun"));
            if (resolution != "REBUILD_FINAL")
                throw FinalAffected("This correction changes who qualifies for the final or their seeds. The final has "
                                    "not begun, so it can be discarded and created again from the corrected standings.",
                                    idList(saved), idList(now.seeds));
            db::exec("DELETE FROM commander_finals WHERE final_id = ?", {fin["final_id"]});
            seedsBefore = idList(saved);
            seedsAfter = idList(now.seeds);
            detail["final_discarded"] = fin["final_id"];
            log.set("final_discarded", fin["final_id"]);
        }
    }
    audit(tid, "RESULT_CORRECTED", detail, pod["round_id"], podId);
    if (!seedsBefore.isEmpty())
        audit(tid, "FINAL_DISCARDED", {{"final_id", fin["final_id"]}, {"seeds_before", seedsBefore},
                                       {"seeds_after", seedsAfter}, {"reason", reason}, {"pod_id", podId}});

    refreshCache(tid, &ev);
    // a finished event's placings follow the correction (a final keeps the places it gave;
    // a legacy playoff is left exactly as it was played)
    if (scoreChanged && ev["stage"].toString() == "COMPLETE" && !hasPlayoffRounds(tid))
        complete(tid, "FINAL_STANDINGS_UPDATED");
    tx.commit();
    return true;
}

static int latestRoundNumber(qint64 tournamentId)
{
    return db::value("SELECT COALESCE(MAX(round_number), 0) FROM commander_rounds WHERE tournament_id = ?",
                     {tournamentId}).toInt();
}

void dropPlayer(qint64 tournamentId, qint64 playerId)
{
    applog::Action log("player.drop", {{"tournament", tournamentId}, {"player", playerId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (ev["stage"].toString() != "SWISS")
        throw CommanderError("Players can only be dropped while the event is being played.");
    const Row fin = loadFinalRow(tournamentId);
    if (!fin.isEmpty() && finalSeeds(loadFinalPlayers(fin["final_id"].toLongLong())).contains(playerId))
        // the club's rules do not say how a finalist who leaves is placed, so nothing is assumed
        throw CommanderError("A finalist cannot be dropped while the final is being played: the club's rules do not "
                             "say how a finalist who leaves is placed. Record the final as it was played, or end the "
                             "tournament early.");
    const int latest = latestRoundNumber(tournamentId);
    const int changed = db::exec("UPDATE enrollments SET dropped = 1, drop_round = ? WHERE tournament_id = ? "
                                 "AND player_id = ? AND checked_in = 1 AND dropped = 0",
                                 {latest, tournamentId, playerId}).affected;
    if (changed != 1)
        throw CommanderError("That player is not active in this event.");
    audit(tournamentId, "PLAYER_DROPPED", {{"player_id", playerId}, {"after_round", latest}});
    tx.commit();
}

void reinstatePlayer(qint64 tournamentId, qint64 playerId)
{
    applog::Action log("player.reinstate", {{"tournament", tournamentId}, {"player", playerId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (!loadFinalRow(tournamentId).isEmpty())
        throw CommanderError("The final has been created from the players who were still in the event, so a drop "
                             "can no longer be undone.");
    const int latest = latestRoundNumber(tournamentId);
    const int changed = db::exec("UPDATE enrollments SET dropped = 0, drop_round = NULL WHERE tournament_id = ? "
                                 "AND player_id = ? AND dropped = 1 AND drop_round = ?",
                                 {tournamentId, playerId, latest}).affected;
    if (ev["stage"].toString() != "SWISS" || changed != 1)
        throw CommanderError("A drop can only be undone before the next round is published.");
    audit(tournamentId, "PLAYER_REINSTATED", {{"player_id", playerId}});
    tx.commit();
}

void endSwissEarly(qint64 tournamentId)
{
    applog::Action log("tournament.finish_short", {{"tournament", tournamentId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    const Rows players = loadPlayers(tournamentId);
    const QVariantList rounds = loadRounds(tournamentId);
    const int active = activeCount(players);
    if (nextStep(ev, rounds) != "SWISS" || active >= 3)
        throw CommanderError("An event can only be finished early between rounds when fewer than "
                             "three active players remain.");
    if (ev["final_enabled"].toBool())
        throw CommanderError("This event ends with a top-four final, which needs four players, so it cannot be "
                             "finished on its standings. Undo a drop if that player is still here, or end the "
                             "tournament early.");
    audit(tournamentId, "ENDED_EARLY",
          {{"active_players", active}, {"rounds_played", int(rounds.size())}, {"configured", ev["swiss_rounds"]}});
    complete(tournamentId);
    tx.commit();
}

bool finishLegacyPlayoff(qint64 tournamentId)
{
    applog::Action log("tournament.finish_legacy", {{"tournament", tournamentId}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    if (ev["stage"].toString() == "COMPLETE")
        return false;
    if (ev["stage"].toString() != "PLAYOFF")
        throw CommanderError("This event is not waiting on a playoff.");
    QVariantList kept;
    for (const Row &r : db::query("SELECT round_number FROM commander_rounds WHERE tournament_id = ? AND stage <> 'SWISS'",
                                  {tournamentId}))
        kept << r["round_number"];
    audit(tournamentId, "LEGACY_PLAYOFF_CLOSED", {{"playoff_rounds_kept", kept}});
    complete(tournamentId);
    tx.commit();
    return true;
}

// why the final cannot be created now, or empty when it can.
static QString finalUnavailable(const QVariantMap &ev, const QVariantList &rounds, const Candidates &c)
{
    const int total = ev["swiss_rounds"].toInt();
    if (!ev["final_enabled"].toBool())
        return "This event was set up without a top-four final.";
    if (ev["tournament_status"].toString() == tdb::TERMINATED)
        return "This tournament was ended early. Its rounds and results are a read-only record.";
    const QString stage = ev["stage"].toString();
    if (stage == "REGISTRATION")
        return "This tournament has not started.";
    if (stage != "SWISS")
        return "This tournament is finished.";
    if (!loadFinalRow(ev["tournament_id"].toLongLong()).isEmpty())
        return "The final has already been created.";
    int finalized = 0;
    for (const QVariant &r : swissOnly(rounds))
        if (r.toMap().value("status").toString() == "FINALIZED")
            ++finalized;
    if (finalized < total)
        return QStringLiteral("The final is created once Round %1, the last qualifying round, is finalized.").arg(total);
    const int n = int(c.eligible.size());
    if (n < 4)
        return QStringLiteral("Only %1 player%2 who %3 not dropped %4 left, and the final needs four. Undo a drop made "
                              "after the last round if that player is still here, or end the tournament early.")
            .arg(n).arg(plural(n)).arg(n == 1 ? "has" : "have").arg(n == 1 ? "is" : "are");
    return {};
}

QVariantMap finalCandidates(qint64 tournamentId)
{
    QVariantMap ev;
    QVariantList rounds, table;
    {
        db::Tx tx;      // one consistent snapshot
        ev = loadEvent(tournamentId);
        rounds = loadRounds(tournamentId);
        table = computeTable(ev, shownPlayers(tournamentId), rounds);
    }
    const Candidates c = candidatesFrom(table);
    QVariantList players;
    for (int i = 0; i < c.seeds.size(); ++i) {
        const QVariantMap r = c.eligible[i].toMap();
        players << QVariantMap{{"player_id", r["player_id"]}, {"display_name", r["display_name"]}, {"seed", i + 1},
                               {"standing", r["standing"]}, {"points", r["points"]}};
    }
    const QString reason = finalUnavailable(ev, rounds, c);
    return {{"ready", reason.isEmpty()}, {"reason", reason}, {"eligible", int(c.eligible.size())}, {"players", players}};
}

qint64 createFinal(qint64 tournamentId, const QList<qint64> &expectedSeeds)
{
    applog::Action log("final.create", {{"tournament", tournamentId}, {"seeds", idsText(expectedSeeds)}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    const Row existing = loadFinalRow(tournamentId);
    if (!existing.isEmpty()) {
        const QList<qint64> seeds = finalSeeds(loadFinalPlayers(existing["final_id"].toLongLong()));
        if (seeds != expectedSeeds)
            throw ConflictError("The final has already been created with other players. Reload the page to see it.");
        log.outcome("no-change");      // a repeated click, or another window got there first
        log.set("final", existing["final_id"]);
        return existing["final_id"].toLongLong();
    }
    const QVariantList rounds = loadRounds(tournamentId);
    const QVariantList table = computeTable(ev, loadPlayers(tournamentId), rounds);
    const Candidates c = candidatesFrom(table);
    const QString reason = finalUnavailable(ev, rounds, c);
    if (!reason.isEmpty())
        throw CommanderError(reason);
    if (c.seeds != expectedSeeds)
        throw ConflictError("The qualifiers have changed since they were shown. Review the final's players again.");

    QVariantList snapshot;
    for (const QVariant &v : table) {
        const QVariantMap r = v.toMap();
        snapshot << QVariantMap{{"standing", r["standing"]}, {"player_id", r["player_id"]}, {"display_name", r["display_name"]},
                                {"points", r["points"]}, {"wins", r["wins"]}, {"losses", r["losses"]}, {"draws", r["draws"]},
                                {"byes", r["byes"]}, {"seconds", r["seconds"]}, {"omw_pct", r["omw_pct"]},
                                {"mw_pct", r["mw_pct"]}, {"dropped", r["dropped"].toInt() != 0}};
    }
    const qint64 finalId = db::exec("INSERT INTO commander_finals (tournament_id, qualifying_json) VALUES (?, ?)",
                                    {tournamentId, toJson(snapshot)}).lastId;
    QVariantList ranks;
    for (int i = 0; i < c.seeds.size(); ++i) {
        const QVariantMap r = c.eligible[i].toMap();
        // seats follow the seeds, seed 1 taking the first turn (TopDeck multiplayer addendum)
        db::exec("INSERT INTO commander_final_players (final_id, tournament_id, player_id, seed, seat_number, qualifying_rank) "
                 "VALUES (?, ?, ?, ?, ?, ?)", {finalId, tournamentId, c.seeds[i], i + 1, i + 1, r["standing"]});
        ranks << r["standing"];
    }
    audit(tournamentId, "FINAL_CREATED", {{"final_id", finalId}, {"seeds", idList(c.seeds)}, {"qualifying_ranks", ranks},
                                          {"eligible", int(c.eligible.size())}});
    tx.commit();
    log.set("final", finalId);
    return finalId;
}

// the final being played or corrected, checked against the expected version
static Row finalForChange(qint64 tournamentId, int expectedVersion)
{
    const Row fin = loadFinalRow(tournamentId);
    if (fin.isEmpty())
        throw CommanderError("This event has no final yet.");
    if (expectedVersion != NONE && expectedVersion != fin["result_version"].toInt())
        throw ConflictError("The final was changed elsewhere. Reload it and try again.");
    return fin;
}

static void checkedFinal(const QList<qint64> &seeds, const QList<QList<qint64>> &groups)
{
    try {
        cmdr::checkFinalProgress(seeds, groups);
    } catch (const std::invalid_argument &e) {
        throw CommanderError(QString::fromStdString(e.what()));
    }
}

static QVariantList placed(const QList<qint64> &seeds, const QList<QList<qint64>> &groups, const QString &outcome,
                           qint64 winnerId)
{
    try {
        return cmdr::placeFinal(seeds, groups, outcome, winnerId);
    } catch (const std::invalid_argument &e) {
        throw CommanderError(QString::fromStdString(e.what()));
    }
}

// each group in seed order, so one result is always saved and compared the same way
static QList<QList<qint64>> inSeedOrder(QList<QList<qint64>> groups, const QList<qint64> &seeds)
{
    for (QList<qint64> &g : groups)
        std::sort(g.begin(), g.end(), [&seeds](qint64 a, qint64 b) { return seeds.indexOf(a) < seeds.indexOf(b); });
    return groups;
}

static void saveGroups(qint64 finalId, const QList<QList<qint64>> &groups)
{
    db::exec("UPDATE commander_final_players SET elimination_group = NULL WHERE final_id = ?", {finalId});
    for (int g = 0; g < groups.size(); ++g)
        for (qint64 p : groups[g])
            db::exec("UPDATE commander_final_players SET elimination_group = ? WHERE final_id = ? AND player_id = ?",
                     {g + 1, finalId, p});
}

int saveFinalEliminations(qint64 tournamentId, const QList<QList<qint64>> &requested, int expectedVersion)
{
    applog::Action log("final.eliminations", {{"tournament", tournamentId}, {"groups", groupsText(requested)}});
    db::Tx tx;
    loadOpenEvent(tournamentId);
    const Row fin = finalForChange(tournamentId, expectedVersion);
    if (fin["status"].toString() == "CONFIRMED")
        throw CommanderError("The final's result is confirmed. Use Correct final to change it.");
    const qint64 finalId = fin["final_id"].toLongLong();
    const Rows finalists = loadFinalPlayers(finalId);
    checkedFinal(finalSeeds(finalists), requested);
    const QList<QList<qint64>> groups = inSeedOrder(requested, finalSeeds(finalists));
    const QList<QList<qint64>> before = finalGroups(finalists);
    const int version = fin["result_version"].toInt();
    if (before == groups) {
        log.outcome("no-change");
        return version;
    }
    saveGroups(finalId, groups);
    db::exec("UPDATE commander_finals SET result_version = result_version + 1 WHERE final_id = ?", {finalId});
    audit(tournamentId, "FINAL_ELIMINATIONS_RECORDED", {{"final_id", finalId}, {"groups_before", groupsList(before)},
                                                        {"groups", groupsList(groups)}});
    tx.commit();
    return version + 1;
}

QVariantList previewFinal(qint64 tournamentId, const QList<QList<qint64>> &groups, const QString &outcome, qint64 winnerId)
{
    const Row fin = loadFinalRow(tournamentId);
    if (fin.isEmpty())
        throw CommanderError("This event has no final yet.");
    const Rows finalists = loadFinalPlayers(fin["final_id"].toLongLong());
    QHash<qint64, QString> names;
    for (const Row &p : shownPlayers(tournamentId))
        names.insert(p["player_id"].toLongLong(), p["display_name"].toString());
    const QList<qint64> seeds = finalSeeds(finalists);
    QVariantList out;
    for (const QVariant &v : placed(seeds, groups, outcome, winnerId)) {
        QVariantMap p = v.toMap();
        const qint64 id = p["player_id"].toLongLong();
        p["display_name"] = names.value(id);
        p["seed"] = int(seeds.indexOf(id)) + 1;
        out << p;
    }
    return out;
}

int confirmFinal(qint64 tournamentId, const QList<QList<qint64>> &requested, const QString &outcome, qint64 winnerId,
                 int expectedVersion, const QString &reason)
{
    applog::Action log("final.confirm", {{"tournament", tournamentId}, {"final_result", outcome}, {"winner", winnerId},
                                         {"groups", groupsText(requested)}});
    db::Tx tx;
    const QVariantMap ev = loadOpenEvent(tournamentId);
    const Row fin = finalForChange(tournamentId, expectedVersion);
    const qint64 finalId = fin["final_id"].toLongLong();
    const bool correction = fin["status"].toString() == "CONFIRMED";
    if (!correction && ev["stage"].toString() != "SWISS")
        throw CommanderError("This tournament is finished.");
    const Rows finalists = loadFinalPlayers(finalId);
    const QList<qint64> seeds = finalSeeds(finalists);
    const QVariantList places = placed(seeds, requested, outcome, winnerId);
    const QList<QList<qint64>> groups = inSeedOrder(requested, seeds);
    const QList<QList<qint64>> before = finalGroups(finalists);
    const int version = fin["result_version"].toInt();
    if (correction && before == groups && fin["outcome"].toString() == outcome
        && fin["winner_player_id"].toLongLong() == winnerId) {
        log.outcome("no-change");
        return version;
    }
    QVariantList previousPlaces;
    for (int place = 1; place <= 4; ++place)
        for (const Row &f : finalists)
            if (f["placement"].toInt() == place)
                previousPlaces << f["player_id"];

    saveGroups(finalId, groups);
    db::exec("UPDATE commander_final_players SET placement = NULL, placement_basis = NULL WHERE final_id = ?", {finalId});
    QVariantList order, bases;
    for (const QVariant &v : places) {
        const QVariantMap p = v.toMap();
        db::exec("UPDATE commander_final_players SET placement = ?, placement_basis = ? WHERE final_id = ? AND player_id = ?",
                 {p["place"], p["basis"], finalId, p["player_id"]});
        order << p["player_id"];
        bases << p["basis"];
    }
    db::exec("UPDATE commander_finals SET status = 'CONFIRMED', outcome = ?, winner_player_id = ?, "
             "result_version = result_version + 1, confirmed_at = DATETIME('now') WHERE final_id = ?",
             {outcome, idOrNull(winnerId), finalId});
    QVariantMap detail{{"final_id", finalId}, {"outcome", outcome}, {"winner_player_id", idOrNull(winnerId)},
                       {"groups", groupsList(groups)}, {"places", order}, {"bases", bases}};
    if (correction) {
        detail["outcome_before"] = fin["outcome"];
        detail["winner_before"] = fin["winner_player_id"];
        detail["groups_before"] = groupsList(before);
        detail["places_before"] = previousPlaces;
        detail["reason"] = reason;
    }
    audit(tournamentId, correction ? "FINAL_CORRECTED" : "FINAL_CONFIRMED", detail);
    complete(tournamentId, correction ? "FINAL_STANDINGS_UPDATED" : "EVENT_COMPLETED");
    tx.commit();
    log.set("first", order.value(0));
    return version + 1;
}

static QVariantMap finalView(qint64 tournamentId, const Rows &shown)
{
    const Row fin = loadFinalRow(tournamentId);
    if (fin.isEmpty())
        return {};
    const Rows finalists = loadFinalPlayers(fin["final_id"].toLongLong());
    QHash<qint64, QString> names;
    for (const Row &p : shown)
        names.insert(p["player_id"].toLongLong(), p["display_name"].toString());
    const QVariantList qualifying = jsonArray(fin["qualifying_json"]);
    QHash<qint64, QVariantMap> snapshot;
    for (const QVariant &v : qualifying)
        snapshot.insert(v.toMap().value("player_id").toLongLong(), v.toMap());
    QVariantList players;
    for (const Row &f : finalists) {
        const qint64 id = f["player_id"].toLongLong();
        players << QVariantMap{{"player_id", id}, {"display_name", names.value(id, "?")}, {"seed", f["seed"]},
                               {"seat", f["seat_number"]}, {"standing", f["qualifying_rank"]},
                               {"points", snapshot.value(id).value("points")}, {"elimination_group", f["elimination_group"]},
                               {"placement", f["placement"]}, {"placement_basis", f["placement_basis"]}};
    }
    return {{"final_id", fin["final_id"]}, {"status", fin["status"]}, {"outcome", fin["outcome"]},
            {"winner_player_id", fin["winner_player_id"]}, {"result_version", fin["result_version"]},
            {"begun", finalBegun(fin, finalists)}, {"groups", groupsList(finalGroups(finalists))},
            {"qualifying", qualifying}, {"players", players}, {"created_at", fin["created_at"]},
            {"confirmed_at", fin["confirmed_at"]}};
}

QVariantMap getFinal(qint64 tournamentId)
{
    return finalView(tournamentId, shownPlayers(tournamentId));
}

QVariantList getStandings(qint64 tournamentId)
{
    const QVariantMap ev = loadEvent(tournamentId);
    return computeTable(ev, shownPlayers(tournamentId), loadRounds(tournamentId));
}

// Adds names, pending counts and the legacy flag for display.
static QVariantList decorate(const QVariantList &rounds, const Rows &players)
{
    QHash<qint64, QString> names;
    for (const Row &p : players)
        names.insert(p["player_id"].toLongLong(), p["display_name"].toString());
    QVariantList out;
    for (const QVariant &rv : rounds) {
        QVariantMap r = rv.toMap();
        QVariantList pods;
        int pending = 0;
        for (const QVariant &pv : r["pods"].toList()) {
            QVariantMap pod = pv.toMap();
            QVariantList seats;
            for (const QVariant &sv : pod["seats"].toList()) {
                QVariantMap s = sv.toMap();
                s["display_name"] = names.value(s["player_id"].toLongLong(), "?");
                seats << s;
            }
            pod["seats"] = seats;
            if (!pod["reported"].toBool())
                ++pending;
            pods << pod;
        }
        QVariantList byes;
        for (const QVariant &b : r["byes"].toList())
            byes << QVariantMap{{"player_id", b}, {"display_name", names.value(b.toLongLong(), "?")}};
        r["pods"] = pods;
        r["byes"] = byes;
        r["pending"] = pending;
        r["legacy"] = r["stage"].toString() != "SWISS";
        out << r;
    }
    return out;
}

QVariantList getRounds(qint64 tournamentId)
{
    return decorate(loadRounds(tournamentId), shownPlayers(tournamentId));
}

QVariantMap getState(qint64 tournamentId)
{
    QVariantMap ev, fin;
    Rows players;
    QVariantList rounds, table;
    QString next;
    {
        db::Tx tx;      // one consistent snapshot
        ev = loadEvent(tournamentId);
        players = shownPlayers(tournamentId);
        rounds = loadRounds(tournamentId);
        table = computeTable(ev, players, rounds);
        fin = finalView(tournamentId, players);
        next = nextStep(ev, swissOnly(rounds));
    }
    QHash<qint64, QString> names;
    QVariantList playerList;
    for (const Row &p : players) {
        names.insert(p["player_id"].toLongLong(), p["display_name"].toString());
        playerList << p;
    }
    ev["champion_name"] = ev["champion_player_id"].isNull()
                              ? QVariant() : QVariant(names.value(ev["champion_player_id"].toLongLong()));
    const QVariantList swiss = swissOnly(rounds);
    const int active = activeCount(players);

    QVariant finalStandings;
    if (ev["stage"].toString() == "COMPLETE") {
        // placings exactly as saved when the event was finished
        QVariantList placed;
        for (const QVariant &r : table)
            if (!r.toMap().value("final_placement").isNull())
                placed << r;
        std::stable_sort(placed.begin(), placed.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("final_placement").toInt() < b.toMap().value("final_placement").toInt();
        });
        finalStandings = placed;
    }
    const QVariantList shown = decorate(rounds, players);
    QVariantList legacyRounds, secondPending;
    for (const QVariant &r : shown) {
        if (r.toMap().value("legacy").toBool())
            legacyRounds << r.toMap().value("round_number");
        for (const QVariant &p : r.toMap().value("pods").toList())
            if (p.toMap().value("second_place_status").toString() == "PENDING")
                secondPending << QVariantMap{{"pod_id", p.toMap().value("pod_id")}, {"pod_number", p.toMap().value("pod_number")},
                                             {"round_number", r.toMap().value("round_number")}};
    }

    return {
        {"event", ev}, {"players", playerList}, {"rounds", shown}, {"standings", table},
        {"final_enabled", ev["final_enabled"]}, {"scores_second", ev["scores_second"]}, {"final", fin},
        {"candidates", next == "FINAL" ? finalCandidates(tournamentId) : QVariant()}, {"second_pending", secondPending},
        {"final_standings", finalStandings}, {"next", next}, {"active_count", active},
        {"rounds_created", int(swiss.size())}, {"total_rounds", ev["swiss_rounds"]}, {"can_pair", active >= 3},
        // ended early by the organizer: everything below is a read-only record
        {"terminated", ev["tournament_status"].toString() == tdb::TERMINATED},
        // flagged by migration 3: the event reached a playoff under the earlier rules
        {"legacy", QVariantMap{{"flagged", ev["legacy_playoff"]}, {"cut", ev["playoff_cut"]},
                               {"playoff_rounds", legacyRounds},
                               {"needs_finish", ev["stage"].toString() == "PLAYOFF"}}},
    };
}

QVariantList getAudit(qint64 tournamentId, int limit)
{
    QVariantList out;
    for (const Row &r : db::query("SELECT * FROM commander_audit WHERE tournament_id = ? ORDER BY audit_id DESC LIMIT ?",
                                  {tournamentId, limit})) {
        Row row = r;
        row["detail"] = jsonObject(row["detail_json"].isNull() ? QVariant("{}") : row["detail_json"]);
        out << row;
    }
    return out;
}

QString stageLabel(const QVariantMap &round)
{
    const QString stage = round["stage"].toString();
    if (stage == "SWISS")
        return QStringLiteral("Round %1").arg(round["stage_round"].toInt());
    return stage == "SEMIFINAL" ? "Semifinals (earlier version)" : "Final pod (earlier version)";
}

QVariantMap progress(qint64 tournamentId)
{
    const QVariantMap ev = loadEvent(tournamentId);
    const Row rnd = db::one(
        "SELECT r.*, (SELECT COUNT(*) FROM commander_pods p WHERE p.round_id = r.round_id) AS pods, "
        "(SELECT COUNT(*) FROM commander_pods p WHERE p.round_id = r.round_id AND p.status = 'REPORTED') AS reported "
        "FROM commander_rounds r WHERE r.tournament_id = ? AND r.stage = 'SWISS' "
        "ORDER BY r.round_number DESC LIMIT 1", {tournamentId});
    if (rnd.isEmpty())
        return {{"long", "Not started"}, {"short", "R1"}, {"round_number", 1}, {"round_id", QVariant()},
                {"active", false}, {"pods", 0}, {"reported", 0}, {"needs_finish", false}};
    const int number = rnd["round_number"].toInt();
    const QString stage = ev["stage"].toString();
    QVariantMap out{
        {"long", QStringLiteral("Round %1 of %2").arg(number).arg(ev["swiss_rounds"].toInt())},
        {"short", QStringLiteral("R%1").arg(number)}, {"round_number", number}, {"round_id", rnd["round_id"]},
        {"active", rnd["status"].toString() == "ACTIVE" && stage == "SWISS"},
        {"pods", rnd["pods"].toInt()}, {"reported", rnd["reported"].toInt()}, {"needs_finish", stage == "PLAYOFF"},
        {"final", false},
    };
    if (stage == "PLAYOFF")
        out["long"] = QStringLiteral("All %1 rounds played · needs finishing").arg(ev["swiss_rounds"].toInt());
    // an event with a final: the final pod is played after the qualifying rounds
    const Row fin = ev["final_enabled"].toBool() ? loadFinalRow(tournamentId) : Row();
    const bool qualifyingDone = number >= ev["swiss_rounds"].toInt() && rnd["status"].toString() == "FINALIZED";
    if (stage == "SWISS" && !fin.isEmpty()) {
        out["long"] = QStringLiteral("Top-four final");
        out["short"] = QStringLiteral("Final");
        out["final"] = true;
        out["final_id"] = fin["final_id"];
        out["active"] = true;
    } else if (stage == "SWISS" && ev["final_enabled"].toBool() && qualifyingDone) {
        out["long"] = QStringLiteral("All %1 rounds played · final next").arg(ev["swiss_rounds"].toInt());
    }
    if (ev["tournament_status"].toString() == tdb::TERMINATED) {
        out["active"] = false;
        out["needs_finish"] = false;
        out["long"] = out["final"].toBool() ? QStringLiteral("Ended early during the top-four final")
                                            : QStringLiteral("Ended early in round %1 of %2").arg(number).arg(ev["swiss_rounds"].toInt());
    }
    return out;
}

QStringList rulesText(const QVariantMap &ev)
{
    const QVariantMap s = ev["settings"].toMap();
    const int n = ev["swiss_rounds"].toInt();
    const QString draw = s["draw_policy"].toString() == "ELIMINATED_LOSE"
        ? "players still in the game each get the draw; players already eliminated take a loss"
        : "every player seated in the pod gets the draw";
    const QString placement = s.value("three_pod_placement", cmdr::LEGACY_THREE_POD_PLACEMENT).toString();
    const QString shortPods =
        placement == "LOW" ? "seated from the lowest match-point groups, so the leaders play four-player pods."
        : placement == "HIGH" ? "seated from the highest match-point groups."
        : "placed wherever players' match points line up best, which can be the top pod.";
    QStringList lines{
        QStringLiteral("Format: Commander, one game per pod per round.  Rules set %1, pairing %2.")
            .arg(s["rules_version"].toString(), cmdr::PAIRING_ALGORITHM),
        ev.value("final_enabled").toBool()
            ? QStringLiteral("Rounds: %1 qualifying round%2, then one top-four final pod. The four best players in the "
                             "qualifying standings who are checked in and have not dropped play it, seated in seed order "
                             "(seed 1 takes the first turn). The final adds no points.").arg(n).arg(n == 1 ? "" : "s")
            : QStringLiteral("Rounds: %1 in total. The tournament ends when Round %1 is finalized; the standings after "
                             "that round are the final placings. There is no playoff, top cut or separate final.").arg(n),
        "Pods: four players by default; three-player pods are used so everyone plays (never 1, 2 or 5). "
        "With exactly five players, one four-player pod and one bye.",
        "Three-player pods: " + shortPods,
        cmdr::scoresSecondPlace(s)
            ? QStringLiteral("Scoring: win %1, clear second place %2, other losses %3, draw %4, bye %5. Second place "
                             "is another player of a won pod, chosen by the organizer; it still counts as a loss. When "
                             "the last players lost at the same moment there is no clear second and no second-place "
                             "points.")
                  .arg(cmdr::pointsText(s["win_points"].toDouble()), cmdr::pointsText(s["second_place_points"].toDouble()),
                       cmdr::pointsText(s["loss_points"].toDouble()), cmdr::pointsText(s["draw_points"].toDouble()),
                       cmdr::pointsText(s["bye_points"].toDouble()))
            : QStringLiteral("Scoring: win %1, loss %2, draw %3, bye %4.")
                  .arg(s["win_points"].toInt()).arg(s["loss_points"].toInt()).arg(s["draw_points"].toInt())
                  .arg(s["bye_points"].toInt()),
        QStringLiteral("Draws: %1.").arg(draw),
        QStringLiteral("Tiebreakers: match points, then opponents' match-win %, then own match-win %, then a random "
                       "draw fixed at event start. Percentages have a %1% floor; a bye counts as a win and adds %2 "
                       "opponents at the floor.")
            .arg(qRound(s["tiebreak_floor"].toDouble() * 100)).arg(s["bye_phantom_opponents"].toInt()),
    };
    if (ev.value("final_enabled").toBool())
        lines << "Final places: the first player eliminated is 4th, the next 3rd, the last opponent eliminated 2nd and "
                 "the winner 1st. Players eliminated at the same moment are ordered by their qualifying seed, and only "
                 "against each other. If the final is drawn or ends without a winner, players already eliminated keep "
                 "the lower places and the players still in are placed by qualifying seed. The round clock only "
                 "announces the end of time; the organizer records how the final ended.";
    if (ev.value("legacy_playoff").toBool())
        lines << QStringLiteral("Earlier version: this event was set up with a Top %1 playoff, which the app no longer "
                                "runs. Playoff rounds that were already created are kept as a read-only record.")
                     .arg(ev["playoff_cut"].toInt());
    lines << (cmdr::scoresSecondPlace(s)
                  ? "Second-place points, the optional final and its placing order, pod policy and pairing weights are "
                    "this club's own rules. Win, draw and bye points, the 20% floor, bye handling and recommended round "
                    "counts follow the TopDeck.gg multiplayer addendum; the event as a whole is not TopDeck's method. "
                    "This is not an official Wizards of the Coast pairing algorithm. Announce these rules before the "
                    "tournament starts."
                  : "Pod policy and pairing weights are this club's own rules; scoring, the 20% floor, bye handling "
                    "and recommended round counts follow the TopDeck.gg multiplayer addendum. "
                    "This is not an official Wizards of the Coast pairing algorithm.");
    return lines;
}

} // namespace cdb
