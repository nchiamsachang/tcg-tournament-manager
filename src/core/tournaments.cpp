#include "tournaments.h"

#include "players.h"
#include "applog.h"

#include <QDate>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

using db::Row;
using db::Rows;

namespace tdb {

const QString PENDING = QStringLiteral("PENDING");
const QString IN_PROGRESS = QStringLiteral("IN_PROGRESS");
const QString COMPLETED = QStringLiteral("COMPLETED");
const QString CANCELLED = QStringLiteral("CANCELLED");
const QString TERMINATED = QStringLiteral("TERMINATED");

const QStringList &supportedGames()
{
    static const QStringList games{"ONEPIECE", "POKEMON", "MTG"};
    return games;
}

bool isValidRoundMinutes(int minutes)
{
    return minutes >= MIN_ROUND_MINUTES && minutes <= MAX_ROUND_MINUTES;
}

qint64 createTournament(const QString &name, const QString &game, int totalRounds, const QString &format,
                        const QString &tournamentDate, int roundTimeMins, int topCut)
{
    applog::Action log("tournament.create", {{"game", game}, {"format", format}, {"rounds", totalRounds}});
    if (name.trimmed().isEmpty())
        throw std::invalid_argument("A tournament needs a name.");
    if (name.size() > MAX_NAME_LENGTH)
        throw std::invalid_argument("A tournament name can be at most 100 characters.");
    if (totalRounds < 1)
        throw std::invalid_argument("A tournament needs at least one round.");
    if (!isValidRoundMinutes(roundTimeMins))
        throw std::invalid_argument("Round length must be from 1 to 240 minutes.");
    const QString date = tournamentDate.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : tournamentDate;
    const qint64 id = db::exec("INSERT INTO tournaments (name, game, format, tournament_date, location, total_rounds, top_cut, "
                               "round_time_mins, status, notes) VALUES (?, ?, ?, ?, NULL, ?, ?, ?, 'PENDING', NULL)",
                               {name, game, format.isEmpty() ? QVariant() : QVariant(format), date, totalRounds, topCut,
                                roundTimeMins}).lastId;
    log.set("tournament", id);
    return id;
}

static const char *TOURNAMENT_SELECT =
    "SELECT t.*, COUNT(e.enrollment_id) AS player_count FROM tournaments t "
    "LEFT JOIN enrollments e ON e.tournament_id = t.tournament_id ";

Rows tournamentsByGame(const QString &game)
{
    return db::query(QString(TOURNAMENT_SELECT) + "WHERE t.game = ? GROUP BY t.tournament_id "
                     "ORDER BY t.tournament_date DESC, t.tournament_id DESC", {game});
}

Row tournamentById(qint64 tournamentId)
{
    return db::one(QString(TOURNAMENT_SELECT) + "WHERE t.tournament_id = ? GROUP BY t.tournament_id", {tournamentId});
}

Rows activeTournaments()
{
    return db::query(QString(TOURNAMENT_SELECT) + "WHERE t.status = 'IN_PROGRESS' GROUP BY t.tournament_id "
                     "ORDER BY t.tournament_date DESC");
}

void updateTournamentStatus(qint64 tournamentId, const QString &status)
{
    db::exec("UPDATE tournaments SET status = ? WHERE tournament_id = ?", {status, tournamentId});
}

bool renameTournament(qint64 tournamentId, const QString &name)
{
    applog::Action log("tournament.rename", {{"tournament", tournamentId}});
    const QString wanted = name.trimmed();
    if (wanted.isEmpty())
        throw std::invalid_argument("A tournament needs a name.");
    if (wanted.size() > MAX_NAME_LENGTH)
        throw std::invalid_argument("A tournament name can be at most 100 characters.");
    db::Tx tx;
    const Row t = db::one("SELECT name FROM tournaments WHERE tournament_id = ?", {tournamentId});
    if (t.isEmpty())
        throw std::invalid_argument("That tournament does not exist.");
    const QString previous = t["name"].toString();
    if (previous == wanted) {
        log.outcome("no-change");
        return false;
    }
    db::exec("UPDATE tournaments SET name = ? WHERE tournament_id = ?", {wanted, tournamentId});
    // Commander events keep an audit trail; other formats have none
    const QString detail = QString::fromUtf8(
        QJsonDocument(QJsonObject{{"from", previous}, {"to", wanted}}).toJson(QJsonDocument::Compact));
    db::exec("INSERT INTO commander_audit (tournament_id, action, actor, detail_json) "
             "SELECT tournament_id, 'EVENT_RENAMED', 'organizer', ? FROM commander_events WHERE tournament_id = ?",
             {detail, tournamentId});
    tx.commit();
    return true;
}

static const char *ENDED_EARLY = "This tournament was ended early. Its rounds and results are a read-only record.";

bool isTerminated(qint64 tournamentId)
{
    return db::value("SELECT status FROM tournaments WHERE tournament_id = ?", {tournamentId}).toString() == TERMINATED;
}

void requireNotTerminated(qint64 tournamentId)
{
    if (isTerminated(tournamentId))
        throw std::logic_error(ENDED_EARLY);
}

bool terminateTournament(qint64 tournamentId)
{
    applog::Action log("tournament.end_early", {{"tournament", tournamentId}});
    db::Tx tx;      // the status, the time and the clocks change together or not at all
    const QString status = db::value("SELECT status FROM tournaments WHERE tournament_id = ?", {tournamentId}).toString();
    if (status.isEmpty())
        throw std::invalid_argument("That tournament does not exist.");
    if (status == TERMINATED) {
        log.outcome("no-change");
        return false;       // a second click, or another window got there first
    }
    if (status == COMPLETED)
        throw std::logic_error("This tournament is already finished.");

    // a running clock keeps the seconds it had run; no clock of this tournament runs again
    static const char *DB_NOW = "(julianday('now') - 2440587.5) * 86400.0";
    for (const char *table : {"rounds", "commander_rounds"}) {
        db::exec(QStringLiteral("UPDATE %1 SET timer_elapsed_secs = COALESCE(timer_elapsed_secs, 0) "
                                "+ CAST(ROUND(MAX(0, %2 - timer_started_at)) AS INTEGER) "
                                "WHERE tournament_id = ? AND timer_state = 'RUNNING' AND timer_started_at IS NOT NULL")
                     .arg(table, DB_NOW), {tournamentId});
        db::exec(QStringLiteral("UPDATE %1 SET timer_state = 'ENDED', timer_started_at = NULL WHERE tournament_id = ?")
                     .arg(table), {tournamentId});
    }
    db::exec("UPDATE tournaments SET status = 'TERMINATED', terminated_at = DATETIME('now') WHERE tournament_id = ?",
             {tournamentId});
    // Commander events keep an audit trail; other formats have none
    db::exec("INSERT INTO commander_audit (tournament_id, action, actor, detail_json) "
             "SELECT tournament_id, 'EVENT_TERMINATED', 'organizer', '{\"stage\": \"' || stage || '\"}' "
             "FROM commander_events WHERE tournament_id = ?", {tournamentId});
    tx.commit();
    return true;
}

GameStats gameStats(const QString &game)
{
    GameStats stats;
    stats.players = db::value("SELECT COUNT(DISTINCT e.player_id) FROM enrollments e "
                              "JOIN tournaments t ON t.tournament_id = e.tournament_id WHERE t.game = ?", {game}).toInt();
    stats.gamesPlayed = db::value("SELECT COUNT(*) FROM matches m JOIN tournaments t ON t.tournament_id = m.tournament_id "
                                  "WHERE t.game = ? AND m.winner NOT IN ('PENDING', 'BYE')", {game}).toInt();
    // Commander results live in pod tables; each reported pod is one game played
    stats.gamesPlayed += db::value("SELECT COUNT(*) FROM commander_pods p "
                                   "JOIN tournaments t ON t.tournament_id = p.tournament_id "
                                   "WHERE t.game = ? AND p.status = 'REPORTED'", {game}).toInt();
    stats.topPlayers = db::query(
        "SELECT p.player_id, p.display_name, SUM(e.match_wins) AS wins FROM enrollments e "
        "JOIN tournaments t ON t.tournament_id = e.tournament_id JOIN players p ON p.player_id = e.player_id "
        "WHERE t.game = ? GROUP BY e.player_id HAVING wins > 0 ORDER BY wins DESC, p.display_name COLLATE NOCASE LIMIT 4",
        {game});
    // two leaders with the same name are told apart by id
    const QSet<qint64> shared = pdb::sharedNameIds(stats.topPlayers);
    for (Row &r : stats.topPlayers)
        r["display_name"] = pdb::label(r["display_name"].toString(), r["player_id"].toLongLong(), shared);
    return stats;
}

qint64 enrollPlayer(qint64 tournamentId, qint64 playerId)
{
    applog::Action log("player.enroll", {{"tournament", tournamentId}, {"player", playerId}});
    db::Tx tx;      // the checks and the write see the same state
    const QVariant status = db::value("SELECT status FROM tournaments WHERE tournament_id = ?", {tournamentId});
    if (!status.isValid())
        throw std::invalid_argument("That tournament does not exist.");
    if (status.toString() == TERMINATED)
        throw std::logic_error(ENDED_EARLY);
    const Row player = db::one("SELECT deleted_at FROM players WHERE player_id = ?", {playerId});
    if (player.isEmpty())
        throw std::invalid_argument("That player does not exist.");
    if (!player["deleted_at"].isNull())
        throw std::logic_error("This player was removed from the directory and cannot be registered.");
    // Already registered is an answer, not an error; it is the only case reported as 0.
    if (db::value("SELECT 1 FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tournamentId, playerId}).isValid()) {
        log.outcome("already-enrolled");
        return 0;
    }
    qint64 id = 0;
    try {
        id = db::exec("INSERT INTO enrollments (tournament_id, player_id, deck_name) VALUES (?, ?, NULL)",
                      {tournamentId, playerId}).lastId;
    } catch (const db::Error &e) {
        // Another connection registered them between the check and the write: the table's
        // UNIQUE (tournament_id, player_id) rule refused the second row.  Every other failure
        // (a missing row, a locked or unwritable database, any other rule) is a real error.
        if (!e.unique())
            throw;
        log.outcome("already-enrolled");
        return 0;
    }
    tx.commit();
    return id;
}

qint64 addAndEnrollPlayer(qint64 tournamentId, const QString &displayName)
{
    applog::Action log("player.add_and_enroll", {{"tournament", tournamentId}});
    if (displayName.trimmed().isEmpty())
        throw std::invalid_argument("A player needs a name.");
    db::Tx tx;      // owns the transaction: the new player and their registration, or neither
    const qint64 playerId = pdb::addPlayer(displayName.trimmed());
    log.set("player", playerId);
    if (enrollPlayer(tournamentId, playerId) == 0)
        throw std::logic_error("The new player could not be registered.");     // cannot happen for a new id
    tx.commit();
    return playerId;
}

void unenrollPlayer(qint64 tournamentId, qint64 playerId)
{
    applog::Action log("player.unenroll", {{"tournament", tournamentId}, {"player", playerId}});
    requireNotTerminated(tournamentId);
    db::exec("DELETE FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tournamentId, playerId});
}

Rows enrolledPlayers(qint64 tournamentId, bool includeDropped)
{
    Rows rows = db::query(
        QStringLiteral(
            "SELECT p.player_id, p.display_name, p.deleted_at IS NOT NULL AS removed, e.enrollment_id, e.deck_name, "
            "e.match_points, e.match_wins, "
            "e.match_losses, e.match_draws, e.omw_pct, e.gw_pct, e.ogw_pct, e.final_placement, e.dropped, e.drop_round "
            "FROM enrollments e JOIN players p ON p.player_id = e.player_id WHERE e.tournament_id = ? %1 "
            "ORDER BY e.match_points DESC, e.omw_pct DESC, p.display_name COLLATE NOCASE")
            .arg(includeDropped ? "" : "AND e.dropped = 0"),
        {tournamentId});
    // the name as shown: with the id when another participant has the same name
    const QSet<qint64> shared = pdb::tournamentDuplicates(tournamentId);
    if (!shared.isEmpty())
        for (Row &r : rows)
            r["display_name"] = pdb::label(r["display_name"].toString(), r["player_id"].toLongLong(), shared);
    return rows;
}

void updateEnrollmentStats(qint64 tournamentId, qint64 playerId, const QVariantMap &stats)
{
    static const QStringList allowed = {"match_points", "match_wins", "match_losses", "match_draws", "game_wins",
                                        "game_losses", "omw_pct", "gw_pct", "ogw_pct", "final_placement"};
    QStringList fields;
    QVariantList values;
    for (const QString &key : allowed) {
        if (stats.contains(key)) {
            fields << key + " = ?";
            values << stats.value(key);
        }
    }
    if (fields.isEmpty())
        return;
    values << tournamentId << playerId;
    db::exec(QStringLiteral("UPDATE enrollments SET %1 WHERE tournament_id = ? AND player_id = ?").arg(fields.join(", ")),
             values);
}

qint64 createRound(qint64 tournamentId, int roundNumber)
{
    return db::exec("INSERT INTO rounds (tournament_id, round_number, round_type) VALUES (?, ?, 'SWISS')",
                    {tournamentId, roundNumber}).lastId;
}

Rows rounds(qint64 tournamentId)
{
    return db::query("SELECT * FROM rounds WHERE tournament_id = ? ORDER BY round_number ASC", {tournamentId});
}

Row currentRound(qint64 tournamentId)
{
    return db::one("SELECT * FROM rounds WHERE tournament_id = ? ORDER BY round_number DESC LIMIT 1", {tournamentId});
}

Row roundByNumber(qint64 tournamentId, int roundNumber)
{
    return db::one("SELECT * FROM rounds WHERE tournament_id = ? AND round_number = ?", {tournamentId, roundNumber});
}

qint64 createMatch(qint64 roundId, qint64 tournamentId, qint64 player1, const QVariant &player2, const QVariant &tableNumber)
{
    // byes are auto-resolved immediately
    const QString winner = player2.isNull() ? "BYE" : "PENDING";
    return db::exec("INSERT INTO matches (round_id, tournament_id, player1_id, player2_id, table_number, winner) "
                    "VALUES (?, ?, ?, ?, ?, ?)",
                    {roundId, tournamentId, player1, player2, tableNumber, winner}).lastId;
}

Rows roundPairings(qint64 roundId)
{
    Rows rows = db::query(
        "SELECT m.match_id, m.tournament_id, m.table_number, m.player1_id, m.player2_id, m.winner, m.player1_game_wins, "
        "m.player2_game_wins, p1.display_name AS player1_name, p2.display_name AS player2_name, "
        "p1.deleted_at IS NOT NULL AS player1_removed, p2.deleted_at IS NOT NULL AS player2_removed "
        "FROM matches m JOIN players p1 ON p1.player_id = m.player1_id "
        "LEFT JOIN players p2 ON p2.player_id = m.player2_id WHERE m.round_id = ? ORDER BY m.table_number ASC",
        {roundId});
    const QSet<qint64> shared = rows.isEmpty() ? QSet<qint64>() : pdb::tournamentDuplicates(rows.first()["tournament_id"].toLongLong());
    for (Row &r : rows) {
        r["result"] = r["winner"].toString() == "PENDING" ? QVariant() : r["winner"];
        r["player1_name"] = pdb::label(r["player1_name"].toString(), r["player1_id"].toLongLong(), shared);
        if (!r["player2_name"].isNull())
            r["player2_name"] = pdb::label(r["player2_name"].toString(), r["player2_id"].toLongLong(), shared);
    }
    return rows;
}

qint64 writeMatchResult(qint64 matchId, const QString &result)
{
    static const QStringList reportable = {"PLAYER1", "PLAYER2", "DRAW"};
    if (!result.isEmpty() && !reportable.contains(result))
        throw std::invalid_argument(("Unknown match result: " + result).toStdString());

    db::Tx tx;      // the checks and the write see the same state
    const Row match = db::one(
        "SELECT m.player2_id, m.round_id, m.tournament_id, t.status, "
        "(SELECT MAX(r.round_id) FROM rounds r WHERE r.tournament_id = m.tournament_id "
        "  AND r.round_number = (SELECT MAX(round_number) FROM rounds WHERE tournament_id = m.tournament_id)) AS latest_round "
        "FROM matches m JOIN tournaments t ON t.tournament_id = m.tournament_id WHERE m.match_id = ?", {matchId});
    if (match.isEmpty())
        throw std::invalid_argument("That match does not exist.");
    if (match["player2_id"].isNull())
        throw std::logic_error("A bye has no result to report.");
    if (match["status"].toString() == COMPLETED)
        throw std::logic_error("This tournament is finished; its results can no longer be changed.");
    if (match["status"].toString() == TERMINATED)
        throw std::logic_error(ENDED_EARLY);
    if (match["round_id"] != match["latest_round"])
        throw std::logic_error("Only results in the current round can be changed.");
    db::exec("UPDATE matches SET winner = ? WHERE match_id = ?",
             {result.isEmpty() ? QStringLiteral("PENDING") : result, matchId});
    tx.commit();
    return match["tournament_id"].toLongLong();
}

void reportMatchResult(qint64 matchId, const QString &result)
{
    applog::Action log("result.report", {{"match", matchId}, {"match_result", result.isEmpty() ? QStringLiteral("CLEARED") : result}});
    writeMatchResult(matchId, result);
}

Rows allMatches(qint64 tournamentId)
{
    return db::query("SELECT m.*, r.round_number, r.round_type FROM matches m JOIN rounds r ON r.round_id = m.round_id "
                     "WHERE m.tournament_id = ? ORDER BY r.round_number ASC", {tournamentId});
}

bool havePlayedBefore(qint64 tournamentId, qint64 a, qint64 b)
{
    return db::value("SELECT COUNT(*) FROM matches WHERE tournament_id = ? AND ((player1_id = ? AND player2_id = ?) "
                     "OR (player1_id = ? AND player2_id = ?))", {tournamentId, a, b, b, a}).toInt() > 0;
}

int pendingMatchCount(qint64 roundId)
{
    return db::value("SELECT COUNT(*) FROM matches WHERE round_id = ? AND winner = 'PENDING'", {roundId}).toInt();
}

} // namespace tdb
