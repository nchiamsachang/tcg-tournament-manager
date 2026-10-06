#include "players.h"

using db::Row;
using db::Rows;

namespace pdb {

qint64 addPlayer(const QString &displayName)
{
    return db::exec("INSERT INTO players (display_name, email, phone, plays_mtg, plays_onepiece, plays_pokemon, notes) "
                    "VALUES (?, NULL, NULL, 0, 0, 0, NULL)", {displayName}).lastId;
}

Rows allPlayers()
{
    return db::query("SELECT * FROM players ORDER BY display_name COLLATE NOCASE");
}

Row playerById(qint64 playerId)
{
    return db::one("SELECT * FROM players WHERE player_id = ?", {playerId});
}

Rows searchPlayers(const QString &text)
{
    return db::query("SELECT * FROM players WHERE display_name LIKE ? ORDER BY display_name COLLATE NOCASE",
                     {"%" + text + "%"});
}

Rows tournamentHistory(qint64 playerId)
{
    return db::query(
        "SELECT t.tournament_id, t.name AS tournament_name, t.game, t.format, t.tournament_date, t.status, "
        "e.match_wins, e.match_losses, e.match_draws, e.match_points, e.final_placement, e.deck_name "
        "FROM enrollments e JOIN tournaments t ON t.tournament_id = e.tournament_id "
        "WHERE e.player_id = ? ORDER BY t.tournament_date DESC", {playerId});
}

Rows matchHistory(qint64 playerId, qint64 tournamentId)
{
    return db::query(R"sql(
        SELECT r.round_number, r.round_type, m.match_id, m.table_number, m.winner, m.player1_id, m.player2_id,
               p_opp.display_name AS opponent_name,
               CASE
                   WHEN m.player2_id IS NULL THEN 'BYE'
                   WHEN m.winner = 'DRAW'    THEN 'DRAW'
                   WHEN (m.winner = 'PLAYER1' AND m.player1_id = ?)
                     OR (m.winner = 'PLAYER2' AND m.player2_id = ?) THEN 'WIN'
                   WHEN m.winner = 'PENDING' THEN 'PENDING'
                   ELSE 'LOSS'
               END AS result
        FROM matches m
        JOIN rounds r ON r.round_id = m.round_id
        LEFT JOIN players p_opp
            ON p_opp.player_id = CASE WHEN m.player1_id = ? THEN m.player2_id ELSE m.player1_id END
        WHERE m.tournament_id = ? AND (m.player1_id = ? OR m.player2_id = ?)
        ORDER BY r.round_number ASC
    )sql", {playerId, playerId, playerId, tournamentId, playerId, playerId});
}

Rows podHistory(qint64 playerId, qint64 tournamentId)
{
    return db::query(
        "SELECT r.stage, r.stage_round, p.pod_number, s.seat_number, s.result, p.status FROM commander_seats s "
        "JOIN commander_pods p ON p.pod_id = s.pod_id JOIN commander_rounds r ON r.round_id = s.round_id "
        "WHERE s.tournament_id = ? AND s.player_id = ? ORDER BY r.round_number ASC", {tournamentId, playerId});
}

Rows lifetimeStats(qint64 playerId)
{
    return db::query(R"sql(
        SELECT t.game,
               COUNT(DISTINCT e.tournament_id)                    AS tournaments_played,
               SUM(e.match_wins)                                  AS total_wins,
               SUM(e.match_losses)                                AS total_losses,
               SUM(e.match_draws)                                 AS total_draws,
               MIN(e.final_placement)                             AS best_placement,
               COUNT(CASE WHEN e.final_placement = 1 THEN 1 END)  AS championships
        FROM enrollments e
        JOIN tournaments t ON t.tournament_id = e.tournament_id
        WHERE e.player_id = ? AND t.status = 'COMPLETED'
        GROUP BY t.game
    )sql", {playerId});
}

QHash<qint64, QStringList> gamesPlayed()
{
    QHash<qint64, QStringList> out;
    // UNION removes repeats, so a player gets each game once however many tournaments they played
    for (const Row &r : db::query(R"sql(
        SELECT player_id, game FROM (
            SELECT m.player1_id AS player_id, t.game AS game FROM matches m
              JOIN tournaments t ON t.tournament_id = m.tournament_id WHERE t.status <> 'CANCELLED'
            UNION
            SELECT m.player2_id, t.game FROM matches m
              JOIN tournaments t ON t.tournament_id = m.tournament_id
             WHERE t.status <> 'CANCELLED' AND m.player2_id IS NOT NULL
            UNION
            SELECT s.player_id, t.game FROM commander_seats s
              JOIN tournaments t ON t.tournament_id = s.tournament_id WHERE t.status <> 'CANCELLED'
            UNION
            SELECT b.player_id, t.game FROM commander_byes b
              JOIN tournaments t ON t.tournament_id = b.tournament_id WHERE t.status <> 'CANCELLED'
        )
        ORDER BY player_id, CASE game WHEN 'POKEMON' THEN 1 WHEN 'ONEPIECE' THEN 2 ELSE 3 END
    )sql"))
        out[r["player_id"].toLongLong()] << r["game"].toString();
    return out;
}

void renamePlayer(qint64 playerId, const QString &displayName)
{
    db::exec("UPDATE players SET display_name = ? WHERE player_id = ?", {displayName, playerId});
}

void deletePlayer(qint64 playerId)
{
    db::Tx tx;
    // children first, so the foreign keys are satisfied at every step
    db::exec("DELETE FROM matches WHERE player1_id = ? OR player2_id = ?", {playerId, playerId});
    db::exec("DELETE FROM enrollments WHERE player_id = ?", {playerId});
    db::exec("DELETE FROM players WHERE player_id = ?", {playerId});
    tx.commit();
}

} // namespace pdb
