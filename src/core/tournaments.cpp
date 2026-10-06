#include "tournaments.h"

#include <QDate>
#include <stdexcept>

using db::Row;
using db::Rows;

namespace tdb {

const QString PENDING = QStringLiteral("PENDING");
const QString IN_PROGRESS = QStringLiteral("IN_PROGRESS");
const QString COMPLETED = QStringLiteral("COMPLETED");
const QString CANCELLED = QStringLiteral("CANCELLED");

bool isValidRoundMinutes(int minutes)
{
    return minutes >= MIN_ROUND_MINUTES && minutes <= MAX_ROUND_MINUTES;
}

qint64 createTournament(const QString &name, const QString &game, int totalRounds, const QString &format,
                        const QString &tournamentDate, int roundTimeMins, int topCut)
{
    if (name.trimmed().isEmpty())
        throw std::invalid_argument("A tournament needs a name.");
    if (totalRounds < 1)
        throw std::invalid_argument("A tournament needs at least one round.");
    if (!isValidRoundMinutes(roundTimeMins))
        throw std::invalid_argument("Round length must be from 1 to 240 minutes.");
    const QString date = tournamentDate.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : tournamentDate;
    return db::exec("INSERT INTO tournaments (name, game, format, tournament_date, location, total_rounds, top_cut, "
                    "round_time_mins, status, notes) VALUES (?, ?, ?, ?, NULL, ?, ?, ?, 'PENDING', NULL)",
                    {name, game, format.isEmpty() ? QVariant() : QVariant(format), date, totalRounds, topCut,
                     roundTimeMins}).lastId;
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
        "SELECT p.display_name, SUM(e.match_wins) AS wins FROM enrollments e "
        "JOIN tournaments t ON t.tournament_id = e.tournament_id JOIN players p ON p.player_id = e.player_id "
        "WHERE t.game = ? GROUP BY e.player_id HAVING wins > 0 ORDER BY wins DESC, p.display_name COLLATE NOCASE LIMIT 4",
        {game});
    return stats;
}

qint64 enrollPlayer(qint64 tournamentId, qint64 playerId)
{
    try {
        return db::exec("INSERT INTO enrollments (tournament_id, player_id, deck_name) VALUES (?, ?, NULL)",
                        {tournamentId, playerId}).lastId;
    } catch (const db::Error &) {
        return 0;       // player already enrolled
    }
}

void unenrollPlayer(qint64 tournamentId, qint64 playerId)
{
    db::exec("DELETE FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tournamentId, playerId});
}

Rows enrolledPlayers(qint64 tournamentId, bool includeDropped)
{
    return db::query(
        QStringLiteral(
            "SELECT p.player_id, p.display_name, e.enrollment_id, e.deck_name, e.match_points, e.match_wins, "
            "e.match_losses, e.match_draws, e.omw_pct, e.gw_pct, e.ogw_pct, e.final_placement, e.dropped, e.drop_round "
            "FROM enrollments e JOIN players p ON p.player_id = e.player_id WHERE e.tournament_id = ? %1 "
            "ORDER BY e.match_points DESC, e.omw_pct DESC, p.display_name COLLATE NOCASE")
            .arg(includeDropped ? "" : "AND e.dropped = 0"),
        {tournamentId});
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
        "SELECT m.match_id, m.table_number, m.player1_id, m.player2_id, m.winner, m.player1_game_wins, "
        "m.player2_game_wins, p1.display_name AS player1_name, p2.display_name AS player2_name "
        "FROM matches m JOIN players p1 ON p1.player_id = m.player1_id "
        "LEFT JOIN players p2 ON p2.player_id = m.player2_id WHERE m.round_id = ? ORDER BY m.table_number ASC",
        {roundId});
    for (Row &r : rows)
        r["result"] = r["winner"].toString() == "PENDING" ? QVariant() : r["winner"];
    return rows;
}

void reportMatchResult(qint64 matchId, const QString &result)
{
    static const QStringList reportable = {"PLAYER1", "PLAYER2", "DRAW"};
    if (!result.isEmpty() && !reportable.contains(result))
        throw std::invalid_argument(("Unknown match result: " + result).toStdString());

    db::Tx tx;      // the checks and the write see the same state
    const Row match = db::one(
        "SELECT m.player2_id, m.round_id, t.status, "
        "(SELECT MAX(r.round_id) FROM rounds r WHERE r.tournament_id = m.tournament_id "
        "  AND r.round_number = (SELECT MAX(round_number) FROM rounds WHERE tournament_id = m.tournament_id)) AS latest_round "
        "FROM matches m JOIN tournaments t ON t.tournament_id = m.tournament_id WHERE m.match_id = ?", {matchId});
    if (match.isEmpty())
        throw std::invalid_argument("That match does not exist.");
    if (match["player2_id"].isNull())
        throw std::logic_error("A bye has no result to report.");
    if (match["status"].toString() == COMPLETED)
        throw std::logic_error("This tournament is finished; its results can no longer be changed.");
    if (match["round_id"] != match["latest_round"])
        throw std::logic_error("Only results in the current round can be changed.");
    db::exec("UPDATE matches SET winner = ? WHERE match_id = ?",
             {result.isEmpty() ? QStringLiteral("PENDING") : result, matchId});
    tx.commit();
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
