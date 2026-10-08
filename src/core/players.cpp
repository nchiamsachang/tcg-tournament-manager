#include "players.h"

#include "tournaments.h"

#include <algorithm>

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
    return db::query("SELECT * FROM players WHERE deleted_at IS NULL ORDER BY display_name COLLATE NOCASE, player_id");
}

bool isRemoved(qint64 playerId)
{
    return !db::value("SELECT deleted_at FROM players WHERE player_id = ?", {playerId}).isNull();
}

QString formatId(qint64 playerId)
{
    return QStringLiteral("#%1").arg(playerId, 4, 10, QChar('0'));
}

QString normalizedName(const QString &name)
{
    return name.simplified().toCaseFolded();
}

QSet<qint64> sharedNameIds(const Rows &players)
{
    QHash<QString, QSet<qint64>> byName;
    for (const Row &p : players)
        byName[normalizedName(p["display_name"].toString())].insert(p["player_id"].toLongLong());
    QSet<qint64> shared;
    for (const QSet<qint64> &ids : byName)
        if (ids.size() > 1)
            shared.unite(ids);
    return shared;
}

QSet<qint64> directoryDuplicates()
{
    return sharedNameIds(db::query("SELECT player_id, display_name FROM players WHERE deleted_at IS NULL"));
}

QSet<qint64> tournamentDuplicates(qint64 tournamentId)
{
    // everyone with any record in the tournament, whether or not they are still in the directory
    return sharedNameIds(db::query(R"sql(
        SELECT player_id, display_name FROM players WHERE player_id IN (
            SELECT player_id FROM enrollments WHERE tournament_id = ?
            UNION SELECT player1_id FROM matches WHERE tournament_id = ?
            UNION SELECT player2_id FROM matches WHERE tournament_id = ? AND player2_id IS NOT NULL
            UNION SELECT player_id FROM commander_seats WHERE tournament_id = ?
            UNION SELECT player_id FROM commander_byes WHERE tournament_id = ?)
    )sql", {tournamentId, tournamentId, tournamentId, tournamentId, tournamentId}));
}

QString label(const QString &name, qint64 playerId, const QSet<qint64> &shared)
{
    return shared.contains(playerId) ? name + QStringLiteral(" · ") + formatId(playerId) : name;
}

Rows playersNamed(const QString &name)
{
    const QString wanted = normalizedName(name);
    Rows out;
    for (const Row &p : allPlayers())
        if (normalizedName(p["display_name"].toString()) == wanted)
            out << p;
    return out;
}

Row playerById(qint64 playerId)
{
    return db::one("SELECT * FROM players WHERE player_id = ?", {playerId});
}

Rows searchPlayers(const QString &text)
{
    return db::query("SELECT * FROM players WHERE deleted_at IS NULL AND display_name LIKE ? "
                     "ORDER BY display_name COLLATE NOCASE, player_id", {"%" + text + "%"});
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
    Rows rows = db::query(R"sql(
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
    // an opponent who shares a name with another participant is shown with their id
    const QSet<qint64> shared = tournamentDuplicates(tournamentId);
    if (!shared.isEmpty())
        for (Row &r : rows) {
            const qint64 opponent = r["player1_id"].toLongLong() == playerId ? r["player2_id"].toLongLong()
                                                                             : r["player1_id"].toLongLong();
            if (!r["opponent_name"].isNull())
                r["opponent_name"] = label(r["opponent_name"].toString(), opponent, shared);
        }
    return rows;
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
    )sql"))
        out[r["player_id"].toLongLong()] << r["game"].toString();
    const QStringList &order = tdb::supportedGames();
    for (QStringList &games : out)
        std::sort(games.begin(), games.end(), [&order](const QString &a, const QString &b) {
            return order.indexOf(a) < order.indexOf(b);
        });
    return out;
}

void renamePlayer(qint64 playerId, const QString &displayName)
{
    if (displayName.trimmed().isEmpty())
        throw std::invalid_argument("A player needs a name.");
    if (isRemoved(playerId))
        throw std::invalid_argument("This player was removed from the directory and can no longer be renamed.");
    db::exec("UPDATE players SET display_name = ? WHERE player_id = ?", {displayName.trimmed(), playerId});
}

bool removePlayer(qint64 playerId)
{
    db::Tx tx;      // the checks and the change see the same state
    const Row player = db::one("SELECT deleted_at FROM players WHERE player_id = ?", {playerId});
    if (player.isEmpty())
        throw std::invalid_argument("That player does not exist.");
    if (!player["deleted_at"].isNull())
        return false;       // a second click, or another window got there first

    // Tournaments being set up or played in which this player still has a part.  Nothing is
    // changed there on their behalf: the organizer resolves it in that tournament first.
    QStringList reasons;
    for (const Row &t : db::query(R"sql(
        SELECT t.name, t.status, t.format, e.dropped,
               EXISTS (SELECT 1 FROM matches m WHERE m.tournament_id = t.tournament_id AND m.winner = 'PENDING'
                          AND (m.player1_id = e.player_id OR m.player2_id = e.player_id))
            OR EXISTS (SELECT 1 FROM commander_seats s JOIN commander_rounds r ON r.round_id = s.round_id
                        WHERE s.tournament_id = t.tournament_id AND s.player_id = e.player_id AND r.status = 'ACTIVE')
               AS unresolved
        FROM enrollments e JOIN tournaments t ON t.tournament_id = e.tournament_id
        WHERE e.player_id = ? AND t.status IN ('PENDING', 'IN_PROGRESS')
        ORDER BY t.tournament_id
    )sql", {playerId})) {
        const QString name = t["name"].toString();
        const bool commander = t["format"].toString() == "Commander";
        if (t["status"].toString() == tdb::PENDING)
            reasons << QStringLiteral("They are registered for %1, which has not started: remove them from its player list first.").arg(name);
        else if (t["unresolved"].toInt())
            reasons << (commander ? QStringLiteral("They are in a round of %1 that is not finalized yet: finalize that round first.").arg(name)
                                  : QStringLiteral("They have a match in %1 that is not reported yet: report it first.").arg(name));
        else if (!t["dropped"].toInt())
            reasons << (commander ? QStringLiteral("They are still playing in %1: drop them there first (Players & drops).").arg(name)
                                  : QStringLiteral("They are still playing in %1: finish that tournament, or end it early, first.").arg(name));
    }
    if (!reasons.isEmpty())
        throw StillPlaying(reasons);

    db::exec("UPDATE players SET deleted_at = DATETIME('now') WHERE player_id = ?", {playerId});
    tx.commit();
    return true;
}

} // namespace pdb
