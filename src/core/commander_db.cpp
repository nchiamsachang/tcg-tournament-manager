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
    Row ev = db::one("SELECT e.*, t.name, t.status AS tournament_status FROM commander_events e "
                     "JOIN tournaments t ON t.tournament_id = e.tournament_id WHERE e.tournament_id = ?", {tournamentId});
    if (ev.isEmpty())
        throw CommanderError("This tournament is not a Commander event.");
    ev["settings"] = jsonObject(ev["settings_json"]);
    ev["playoff_seeds"] = ev["playoff_seeds_json"].isNull() ? QVariantList() : jsonArray(ev["playoff_seeds_json"]);
    ev["legacy_playoff"] = ev["legacy_playoff"].toInt() != 0;
    ev["total_rounds"] = ev["swiss_rounds"];
    return ev;
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
                                 qint64 baseSeed, const QString &tournamentDate, int roundTimeMins)
{
    applog::Action log("tournament.create", {{"game", "MTG"}, {"format", "Commander"}, {"rounds", swissRounds}});
    QVariantMap cfg = cmdr::defaultSettings();
    for (auto it = settings.begin(); it != settings.end(); ++it)
        cfg[it.key()] = it.value();
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
        "VALUES (?, 'MTG', ?, ?, ?, 0, ?, 'PENDING')", {name, QString(FORMAT), date, rounds, roundTimeMins}).lastId;
    db::exec("INSERT INTO commander_events (tournament_id, rules_version, settings_json, recommended_rounds, "
             "swiss_rounds, recommended_cut, playoff_cut, base_seed) VALUES (?, ?, ?, ?, ?, 0, 0, ?)",
             {tid, QString(cmdr::RULES_VERSION), toJson(cfg), recRounds, rounds, seed});
    audit(tid, "EVENT_CREATED", {{"rounds", rounds}, {"base_seed", seed}});
    tx.commit();
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

void updateEventConfig(qint64 tournamentId, int swissRounds, const QString &drawPolicy, const QString &threePodPlacement)
{
    applog::Action log("event.configure", {{"tournament", tournamentId}});
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
    db::exec("UPDATE commander_events SET swiss_rounds = ?, settings_json = ? WHERE tournament_id = ?",
             {rounds, toJson(settings), tournamentId});
    db::exec("UPDATE tournaments SET total_rounds = ? WHERE tournament_id = ?", {rounds, tournamentId});
    audit(tournamentId, "CONFIG_UPDATED",
          {{"rounds", rounds}, {"draw_policy", settings["draw_policy"]},
           {"three_pod_placement", settings.value("three_pod_placement", cmdr::LEGACY_THREE_POD_PLACEMENT)}});
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

// What the organizer can publish next: SWISS (the next round), WAIT (a round
// is in play) or NONE.  Once the configured number of rounds exists there is
// nothing further — no playoff, semifinal or final is ever generated.
static QString nextStep(const QVariantMap &ev, const QVariantList &rounds)
{
    if (ev["stage"].toString() != "SWISS" || ev["tournament_status"].toString() == tdb::TERMINATED)
        return "NONE";
    if (!rounds.isEmpty() && rounds.last().toMap().value("status").toString() == "ACTIVE")
        return "WAIT";
    return rounds.size() < ev["swiss_rounds"].toInt() ? "SWISS" : "NONE";
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
    db::exec("UPDATE commander_events SET stage = 'SWISS', started_at = DATETIME('now'), started_player_count = ?, "
             "recommended_rounds = ? WHERE tournament_id = ?", {n, cmdr::recommendedRounds(n), tournamentId});
    db::exec("UPDATE tournaments SET status = 'IN_PROGRESS', total_rounds = ?, top_cut = 0 WHERE tournament_id = ?",
             {ev["swiss_rounds"], tournamentId});
    audit(tournamentId, "EVENT_STARTED", {{"players", n}, {"rounds", ev["swiss_rounds"]}, {"settings", ev["settings"]}});
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

static void applyResults(const QVariantMap &pod, const QString &outcome, const Results &results)
{
    for (auto it = results.begin(); it != results.end(); ++it)
        db::exec("UPDATE commander_seats SET result = ?, eliminated = ? WHERE pod_id = ? AND player_id = ?",
                 {it.value().result, it.value().eliminated, pod["pod_id"], it.key()});
    db::exec("UPDATE commander_pods SET status = 'REPORTED', outcome = ?, result_version = result_version + 1, "
             "reported_at = DATETIME('now') WHERE pod_id = ?", {outcome, pod["pod_id"]});
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
                    int expectedVersion)
{
    applog::Action log("result.report", {{"pod", podId}, {"pod_result", outcome}, {"winner", winnerId}});
    db::Tx tx;
    const QVariantMap pod = loadPod(podId);
    if (pod["stage"].toString() != "SWISS")
        throw CommanderError(LEGACY_ROUND);
    if (pod["round_status"].toString() != "ACTIVE")
        throw CommanderError("This round is finalized. Use a correction to change its results.");
    const qint64 tid = pod["tournament_id"].toLongLong();
    const QVariantMap ev = loadOpenEvent(tid);
    const Results results = resolve(pod, ev["settings"].toMap(), outcome, winnerId, eliminatedIds);
    const std::optional<Results> before = currentResults(pod);
    const int version = pod["result_version"].toInt();
    if (before && *before == results)
        return version;
    if (expectedVersion != NONE && expectedVersion != version)
        throw ConflictError("This pod's result was changed elsewhere. Reload it and try again.");
    if (before && expectedVersion == NONE)
        throw ConflictError("This pod already has a different result. Reload it to correct that result.");
    applyResults(pod, outcome, results);
    audit(tid, before ? "RESULT_CORRECTED" : "RESULT_REPORTED",
          {{"before", resultsDetail(before)}, {"after", resultsDetail(results)}, {"outcome", outcome}},
          pod["round_id"], podId);
    refreshCache(tid);
    tx.commit();
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
             "result_version = result_version + 1, reported_at = NULL WHERE pod_id = ?", {podId});
    audit(tid, "RESULT_CLEARED", {{"before", resultsDetail(currentResults(pod))}, {"after", QVariant()}},
          pod["round_id"], podId);
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

// Saves the completed status, each player's final placing and a snapshot of the final standings.
static void complete(qint64 tournamentId, const QString &action = "EVENT_COMPLETED")
{
    const QVariantMap ev = loadEvent(tournamentId);
    const QVariantList table = finalTable(computeTable(ev, loadPlayers(tournamentId), loadRounds(tournamentId)));
    cacheStandings(tournamentId, table);
    QVariantList snapshot;
    for (int i = 0; i < table.size(); ++i) {
        const QVariantMap r = table[i].toMap();
        snapshot << QVariantMap{{"place", i + 1}, {"player_id", r["player_id"]}, {"display_name", r["display_name"]},
                                {"points", r["points"]}, {"wins", r["wins"]}, {"losses", r["losses"]},
                                {"draws", r["draws"]}, {"byes", r["byes"]}, {"omw_pct", r["omw_pct"]},
                                {"mw_pct", r["mw_pct"]}, {"dropped", r["dropped"].toInt() != 0}};
        db::exec("UPDATE enrollments SET final_placement = ? WHERE tournament_id = ? AND player_id = ?",
                 {i + 1, tournamentId, r["player_id"]});
    }
    const QVariant winner = snapshot.isEmpty() ? QVariant() : snapshot.first().toMap().value("player_id");
    db::exec("UPDATE commander_events SET stage = 'COMPLETE', champion_player_id = ?, final_standings_json = ?, "
             "completed_at = COALESCE(completed_at, DATETIME('now')) WHERE tournament_id = ?",
             {winner, toJson(snapshot), tournamentId});
    db::exec("UPDATE tournaments SET status = 'COMPLETED' WHERE tournament_id = ?", {tournamentId});
    audit(tournamentId, action, {{"winner_player_id", winner}, {"players", int(snapshot.size())}});
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
    db::exec("UPDATE commander_rounds SET status = 'FINALIZED', finalized_at = DATETIME('now') WHERE round_id = ?", {roundId});
    db::exec("UPDATE commander_pods SET confirmed_at = DATETIME('now') WHERE round_id = ?", {roundId});
    audit(tid, "ROUND_FINALIZED", {{"round_number", rnd["round_number"]}}, roundId);
    QVariantMap ev;
    refreshCache(tid, &ev);
    if (rnd["round_number"].toInt() >= ev["swiss_rounds"].toInt())
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

bool correctResult(qint64 podId, const QString &outcome, qint64 winnerId, const QList<qint64> &eliminatedIds,
                   const QString &resolution, const QString &reason)
{
    applog::Action log("result.correct", {{"pod", podId}, {"pod_result", outcome}, {"winner", winnerId}, {"resolution", resolution}});
    if (!resolution.isEmpty() && resolution != "KEEP" && resolution != "REBUILD")
        throw CommanderError("Resolution must be KEEP or REBUILD.");
    db::Tx tx;
    const QVariantMap pod = loadPod(podId);
    const qint64 tid = pod["tournament_id"].toLongLong();
    QVariantMap ev = loadOpenEvent(tid);
    const Results results = resolve(pod, ev["settings"].toMap(), outcome, winnerId, eliminatedIds);
    const std::optional<Results> before = currentResults(pod);
    if (before && *before == results)
        return false;
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
    const bool finalized = pod["round_status"].toString() == "FINALIZED";
    if (finalized && !later.isEmpty() && resolution.isEmpty())
        throw ResolutionRequired(
            QStringLiteral("Round %1 is finalized and round%2 %3 %4 already published from it. "
                           "Choose KEEP (leave later rounds as published) or REBUILD (discard and redo them).")
                .arg(roundNumber).arg(later.size() > 1 ? "s" : "").arg(laterText.join(", "))
                .arg(later.size() > 1 ? "were" : "was"),
            later);

    if (finalized && !later.isEmpty() && resolution == "REBUILD") {
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

    applyResults(pod, outcome, results);
    audit(tid, "RESULT_CORRECTED",
          {{"before", resultsDetail(before)}, {"after", resultsDetail(results)}, {"outcome", outcome}, {"reason", reason},
           {"resolution", resolution.isEmpty() ? QVariant() : QVariant(resolution)}, {"later_rounds", laterNumbers}},
          pod["round_id"], podId);

    refreshCache(tid, &ev);
    // a finished event's placings are its standings, so they follow the correction
    // (unless they came from a legacy playoff, which is left exactly as it was played)
    if (ev["stage"].toString() == "COMPLETE" && !hasPlayoffRounds(tid))
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
    QVariantMap ev;
    Rows players;
    QVariantList rounds, table;
    {
        db::Tx tx;      // one consistent snapshot
        ev = loadEvent(tournamentId);
        players = shownPlayers(tournamentId);
        rounds = loadRounds(tournamentId);
        table = computeTable(ev, players, rounds);
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
    QVariantList legacyRounds;
    for (const QVariant &r : shown)
        if (r.toMap().value("legacy").toBool())
            legacyRounds << r.toMap().value("round_number");

    return {
        {"event", ev}, {"players", playerList}, {"rounds", shown}, {"standings", table},
        {"final_standings", finalStandings}, {"next", nextStep(ev, swiss)}, {"active_count", active},
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
    };
    if (stage == "PLAYOFF")
        out["long"] = QStringLiteral("All %1 rounds played · needs finishing").arg(ev["swiss_rounds"].toInt());
    if (ev["tournament_status"].toString() == tdb::TERMINATED) {
        out["active"] = false;
        out["needs_finish"] = false;
        out["long"] = QStringLiteral("Ended early in round %1 of %2").arg(number).arg(ev["swiss_rounds"].toInt());
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
        QStringLiteral("Rounds: %1 in total. The tournament ends when Round %1 is finalized; the standings after that "
                       "round are the final placings. There is no playoff, top cut or separate final.").arg(n),
        "Pods: four players by default; three-player pods are used so everyone plays (never 1, 2 or 5). "
        "With exactly five players, one four-player pod and one bye.",
        "Three-player pods: " + shortPods,
        QStringLiteral("Scoring: win %1, loss %2, draw %3, bye %4.")
            .arg(s["win_points"].toInt()).arg(s["loss_points"].toInt()).arg(s["draw_points"].toInt())
            .arg(s["bye_points"].toInt()),
        QStringLiteral("Draws: %1.").arg(draw),
        QStringLiteral("Tiebreakers: match points, then opponents' match-win %, then own match-win %, then a random "
                       "draw fixed at event start. Percentages have a %1% floor; a bye counts as a win and adds %2 "
                       "opponents at the floor.")
            .arg(qRound(s["tiebreak_floor"].toDouble() * 100)).arg(s["bye_phantom_opponents"].toInt()),
    };
    if (ev.value("legacy_playoff").toBool())
        lines << QStringLiteral("Earlier version: this event was set up with a Top %1 playoff, which the app no longer "
                                "runs. Playoff rounds that were already created are kept as a read-only record.")
                     .arg(ev["playoff_cut"].toInt());
    lines << "Pod policy and pairing weights are this club's own rules; scoring, the 20% floor, bye handling "
             "and recommended round counts follow the TopDeck.gg multiplayer addendum. "
             "This is not an official Wizards of the Coast pairing algorithm.";
    return lines;
}

} // namespace cdb
