#include "swiss.h"

#include "applog.h"

#include "tournaments.h"

#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>
#include <random>
#include <tuple>

using db::Row;
using db::Rows;

namespace swiss {

Points points(const QString &game)
{
    if (game == "MTG")
        return {3, 1, 0};
    if (game == "POKEMON")
        return {1, 0, 0};
    return {3, 0, 0};       // ONEPIECE
}

double omwFloor(const QString &game)
{
    return game == "POKEMON" ? 0.25 : 0.33;
}

int suggestedRounds(int n)
{
    if (n <= 4)   return 2;
    if (n <= 8)   return 3;
    if (n <= 16)  return 4;
    if (n <= 32)  return 5;
    if (n <= 64)  return 6;
    if (n <= 128) return 7;
    return 8;
}

QList<QPair<QString, QString>> tiebreakColumns(const QString &game)
{
    if (game == "ONEPIECE")
        return {{"omw_pct", "OMW%"}, {"gw_pct", "GW%"}};
    if (game == "POKEMON")
        return {{"omw_pct", "Opp Win%"}, {"ogw_pct", "Opp Opp Win%"}};      // club policy; see figuresFromMatches
    return {{"omw_pct", "OMW%"}, {"gw_pct", "GW%"}, {"ogw_pct", "OGW%"}};      // MTG
}

QString tiebreakText(const QString &game, const QString &column, double value)
{
    // Pokémon's second tiebreaker is never below the 25% floor once it has been worked out,
    // so 0 means a tournament finished before the figure existed.  Its saved placings stand.
    if (game == "POKEMON" && column == "ogw_pct" && value <= 0.0)
        return QStringLiteral("—");
    return QString::number(value, 'f', 3);
}

// Every player's record and tiebreakers, worked out from the saved matches.  Nothing is written.
static QHash<qint64, QVariantMap> figuresFromMatches(qint64 tournamentId, const QString &game)
{
    QHash<qint64, QVariantMap> out;
    const double floor = omwFloor(game);
    const Rows matches = tdb::allMatches(tournamentId);
    const Rows players = tdb::enrolledPlayers(tournamentId, true);

    QList<qint64> ids;
    QHash<qint64, int> wins, losses, draws, byes;
    QHash<qint64, QList<qint64>> opponents;
    for (const Row &p : players) {
        const qint64 id = p["player_id"].toLongLong();
        ids << id;
        wins[id] = losses[id] = draws[id] = byes[id] = 0;
        opponents[id] = {};
    }

    for (const Row &m : matches) {
        const qint64 p1 = m["player1_id"].toLongLong();
        const bool has2 = !m["player2_id"].isNull();
        const qint64 p2 = m["player2_id"].toLongLong();
        const QString w = m["winner"].toString();
        if (w == "BYE") {
            byes[p1] += 1;
            wins[p1] += 1;
        } else if (w == "PLAYER1") {
            wins[p1] += 1;
            if (has2) losses[p2] += 1;
        } else if (w == "PLAYER2") {
            if (has2) wins[p2] += 1;
            losses[p1] += 1;
        } else if (w == "DRAW") {
            draws[p1] += 1;
            if (has2) draws[p2] += 1;
        }
        if (w != "BYE" && has2) {
            opponents[p1] << p2;
            opponents[p2] << p1;
        }
    }

    const Points pts = points(game);
    auto matchWinPct = [&](qint64 id) {
        const int w = wins.value(id) - byes.value(id);
        const int total = w + losses.value(id) + draws.value(id);
        return total > 0 ? std::max(double(w) / total, floor) : floor;
    };

    // Opponents' match-win percentage for everybody first, at full double precision:
    // Pokémon's second tiebreaker is worked out from these.
    //   - A bye is not an opponent and does not count in anybody's win rate.
    //   - An opponent met twice counts twice, once per match.
    //   - Each opponent counts as at least the floor.  A player with no real opponent
    //     (only byes, or nothing played yet) is given the floor.
    //   - A player who has dropped is treated like anyone else: their record stands as it is.
    QHash<qint64, double> oppWin;
    for (qint64 id : ids) {
        const QList<qint64> &opp = opponents[id];
        double omw = floor;
        if (!opp.isEmpty()) {
            double sum = 0;
            for (qint64 o : opp) sum += matchWinPct(o);
            omw = sum / opp.size();
        }
        oppWin.insert(id, omw);
    }

    for (qint64 id : ids) {
        const QList<qint64> &opp = opponents[id];
        const double omw = oppWin.value(id);
        const int total = wins[id] + losses[id] + draws[id];
        const double gw = total > 0 ? double(wins[id]) / total : 0.0;
        // The third stored figure (column ogw_pct) is a different thing per game.
        double ogw = 0.0;
        int ogwDigits = 4;
        if (game == "POKEMON") {
            // Club policy: opponents' opponents' win percentage, the average of each opponent's
            // own Opp Win% above (same opponents, same counting).  It cannot fall below the
            // floor, since every Opp Win% is at least the floor; with no real opponent it is
            // the floor.  Both Pokémon figures are saved and compared at nine-decimal
            // precision (rounded to nine places: floating-point noise goes, and two values
            // compare equal only if they agree to nine places); screens show three.
            ogw = floor;
            if (!opp.isEmpty()) {
                double sum = 0;
                for (qint64 o : opp) sum += oppWin.value(o, floor);
                ogw = sum / opp.size();
            }
            ogwDigits = 9;
        } else if (game == "MTG") {
            ogw = floor;
            if (!opp.isEmpty()) {
                double sum = 0;
                for (qint64 o : opp) {
                    const int t = wins.value(o) + losses.value(o) + draws.value(o);
                    sum += t > 0 ? std::max(double(wins.value(o)) / t, floor) : floor;
                }
                ogw = sum / opp.size();
            }
        }
        out.insert(id, {
            {"match_points", wins[id] * pts.win + draws[id] * pts.draw},
            {"match_wins", wins[id]}, {"match_losses", losses[id]}, {"match_draws", draws[id]},
            {"omw_pct", db::roundTo(omw, game == "POKEMON" ? 9 : 4)}, {"gw_pct", db::roundTo(gw, 4)}, {"ogw_pct", db::roundTo(ogw, ogwDigits)},
        });
    }
    return out;
}

void calculateTiebreakers(qint64 tournamentId, const QString &game)
{
    db::Tx tx;      // every player's figures are replaced together
    const QHash<qint64, QVariantMap> figures = figuresFromMatches(tournamentId, game);
    for (auto it = figures.begin(); it != figures.end(); ++it)
        tdb::updateEnrollmentStats(tournamentId, it.key(), it.value());
    tx.commit();
}

static Rows ranked(Rows players, const QString &game)
{
    auto key = [&](const Row &p) {
        const double third = game == "POKEMON" ? p["ogw_pct"].toDouble() : p["gw_pct"].toDouble();
        const double fourth = game == "MTG" ? p["ogw_pct"].toDouble() : 0.0;
        return std::make_tuple(-p["match_points"].toInt(), -p["omw_pct"].toDouble(), -third, -fourth);
    };
    std::stable_sort(players.begin(), players.end(), [&](const Row &a, const Row &b) { return key(a) < key(b); });
    for (int i = 0; i < players.size(); ++i)
        players[i]["standing"] = i + 1;
    return players;
}

Rows standings(qint64 tournamentId, const QString &game)
{
    return ranked(tdb::enrolledPlayers(tournamentId, true), game);
}

Rows viewStandings(qint64 tournamentId, const QString &game)
{
    db::Tx tx;      // one consistent read; nothing is written, so there is nothing to commit
    Rows players = tdb::enrolledPlayers(tournamentId, true);
    if (tdb::tournamentById(tournamentId)["status"].toString() == tdb::COMPLETED) {
        // finished: the figures and placings saved when it was finalized are the result
        Rows table = ranked(players, game);
        bool placed = !table.isEmpty();
        for (const Row &p : table)
            placed = placed && p["final_placement"].toInt() > 0;
        if (placed) {
            std::stable_sort(table.begin(), table.end(), [](const Row &a, const Row &b) {
                return a["final_placement"].toInt() < b["final_placement"].toInt();
            });
            for (Row &p : table)
                p["standing"] = p["final_placement"];
        }
        return table;
    }
    // still being played (or ended early): today's standings from the matches, in memory only
    const QHash<qint64, QVariantMap> figures = figuresFromMatches(tournamentId, game);
    for (Row &p : players) {
        const QVariantMap f = figures.value(p["player_id"].toLongLong());
        for (auto it = f.begin(); it != f.end(); ++it)
            p[it.key()] = it.value();
    }
    return ranked(players, game);
}

Rows currentStandings(qint64 tournamentId, const QString &game)
{
    db::Tx tx;
    calculateTiebreakers(tournamentId, game);
    const Rows table = standings(tournamentId, game);
    tx.commit();
    return table;
}

// Two players who have already met, lower id first.
using Met = QPair<qint64, qint64>;
static Met metKey(qint64 a, qint64 b) { return {qMin(a, b), qMax(a, b)}; }

// How many partial pairings pairWithoutRematch may try before giving up.  A club-sized field
// is searched in full well inside this; it only bounds the time spent on a very large one.
static const int PAIRING_SEARCH_STEPS = 200000;

// Pairs `unpaired` (in standings order) so that nobody meets an opponent again, when such a
// round exists.  Each player takes the highest-placed opponent they have not met, and a
// choice that would leave the players below it with no new opponent is taken back and the
// next one tried.  Returns false, leaving `out` unfinished, when there is no such round.
static bool pairWithoutRematch(const QList<qint64> &unpaired, const QSet<Met> &met, QList<QPair<qint64, qint64>> &out,
                               int &steps)
{
    if (unpaired.size() < 2)
        return true;
    if (--steps < 0)
        return false;
    const qint64 p1 = unpaired.first();
    for (int i = 1; i < unpaired.size(); ++i) {
        if (met.contains(metKey(p1, unpaired[i])))
            continue;
        QList<qint64> rest = unpaired.mid(1);
        rest.removeAt(i - 1);
        out.append({p1, unpaired[i]});
        if (pairWithoutRematch(rest, met, out, steps))
            return true;
        out.removeLast();
    }
    return false;
}

qint64 generatePairings(qint64 tournamentId, int roundNumber)
{
    db::Tx tx;      // the round and all of its matches, or nothing
    if (tdb::isTerminated(tournamentId))
        throw RuleError("This tournament was ended early; no further round can be paired.");
    Rows players = tdb::enrolledPlayers(tournamentId, false);
    if (players.isEmpty())
        throw std::runtime_error("No active players to pair.");

    // sort by points, shuffling players tied on points
    std::stable_sort(players.begin(), players.end(), [](const Row &a, const Row &b) {
        return a["match_points"].toInt() > b["match_points"].toInt();
    });
    std::mt19937 rng(QRandomGenerator::global()->generate());
    for (int i = 0; i < players.size();) {
        int j = i;
        while (j < players.size() && players[j]["match_points"].toInt() == players[i]["match_points"].toInt())
            ++j;
        std::shuffle(players.begin() + i, players.begin() + j, rng);
        i = j;
    }

    QSet<qint64> hadBye;
    QSet<Met> met;
    for (const Row &m : tdb::allMatches(tournamentId)) {
        if (m["winner"].toString() == "BYE")
            hadBye.insert(m["player1_id"].toLongLong());
        if (!m["player2_id"].isNull())
            met.insert(metKey(m["player1_id"].toLongLong(), m["player2_id"].toLongLong()));
    }

    QVariant byePlayer;
    if (players.size() % 2 == 1) {
        qint64 chosen = players.last()["player_id"].toLongLong();
        for (int i = players.size() - 1; i >= 0; --i) {        // lowest standing without a bye yet
            const qint64 id = players[i]["player_id"].toLongLong();
            if (!hadBye.contains(id)) {
                chosen = id;
                break;
            }
        }
        byePlayer = chosen;
        players.erase(std::remove_if(players.begin(), players.end(),
                                     [&](const Row &p) { return p["player_id"].toLongLong() == chosen; }),
                      players.end());
    }

    QList<QPair<qint64, qint64>> pairings;
    QList<qint64> unpaired;
    for (const Row &p : players)
        unpaired << p["player_id"].toLongLong();
    int steps = PAIRING_SEARCH_STEPS;
    if (!pairWithoutRematch(unpaired, met, pairings, steps)) {
        // every possible round repeats a match (or the field is too large to search in
        // full): go down the standings, repeating a match only for a player with nobody new left
        pairings.clear();
        while (unpaired.size() >= 2) {
            const qint64 p1 = unpaired.takeFirst();
            bool paired = false;
            for (int i = 0; i < unpaired.size(); ++i) {
                if (!met.contains(metKey(p1, unpaired[i]))) {
                    pairings.append({p1, unpaired.takeAt(i)});
                    paired = true;
                    break;
                }
            }
            if (!paired)
                pairings.append({p1, unpaired.takeFirst()});
        }
    }

    const qint64 roundId = tdb::createRound(tournamentId, roundNumber);
    for (int i = 0; i < pairings.size(); ++i)
        tdb::createMatch(roundId, tournamentId, pairings[i].first, pairings[i].second, i + 1);
    if (!byePlayer.isNull())
        tdb::createMatch(roundId, tournamentId, byePlayer.toLongLong());
    tx.commit();
    return roundId;
}

// The tournament row, or a RuleError when the id is unknown.
static Row requireTournament(qint64 tournamentId)
{
    const Row t = tdb::tournamentById(tournamentId);
    if (t.isEmpty())
        throw RuleError("That tournament does not exist.");
    return t;
}

static void requireAllReported(qint64 roundId, int roundNumber)
{
    const int pending = tdb::pendingMatchCount(roundId);
    if (pending > 0)
        throw RuleError(QStringLiteral("Round %1 still has %2 result%3 to report.")
                            .arg(roundNumber).arg(pending).arg(pending == 1 ? "" : "s"));
}

void reportResult(qint64 matchId, const QString &result)
{
    applog::Action log("result.report", {{"match", matchId}, {"match_result", result.isEmpty() ? QStringLiteral("CLEARED") : result}});
    db::Tx tx;      // owns the transaction; the two steps below join it
    const qint64 tournamentId = tdb::writeMatchResult(matchId, result);
    log.set("tournament", tournamentId);
    calculateTiebreakers(tournamentId, tdb::tournamentById(tournamentId)["game"].toString());
    tx.commit();
}

void startTournament(qint64 tournamentId)
{
    applog::Action log("tournament.start", {{"tournament", tournamentId}});
    db::Tx tx;
    const Row t = requireTournament(tournamentId);
    if (t["status"].toString() != tdb::PENDING)
        throw RuleError("This tournament has already been started.");
    tdb::updateTournamentStatus(tournamentId, tdb::IN_PROGRESS);
    generatePairings(tournamentId, 1);
    tx.commit();
}

qint64 advanceToNextRound(qint64 tournamentId, int fromRound)
{
    applog::Action log("round.end", {{"tournament", tournamentId}, {"round_number", fromRound}});
    db::Tx tx;
    const Row t = requireTournament(tournamentId);
    if (t["status"].toString() != tdb::IN_PROGRESS)
        throw RuleError("This tournament is not running.");
    const Row current = tdb::currentRound(tournamentId);
    if (current.isEmpty() || current["round_number"].toInt() != fromRound)
        throw RuleError(QStringLiteral("Round %1 is not the round being played; it may already have been ended.")
                            .arg(fromRound));
    if (fromRound >= t["total_rounds"].toInt())
        throw RuleError(QStringLiteral("Round %1 is the last round of this tournament.").arg(fromRound));
    const qint64 roundId = current["round_id"].toLongLong();
    requireAllReported(roundId, fromRound);

    db::exec("UPDATE rounds SET ended_at = DATETIME('now') WHERE round_id = ?", {roundId});
    calculateTiebreakers(tournamentId, t["game"].toString());       // the next pairing reads the new points
    const qint64 nextRoundId = generatePairings(tournamentId, fromRound + 1);
    tx.commit();
    log.set("next_round", nextRoundId);
    return nextRoundId;
}

Rows finalizeTournament(qint64 tournamentId, const QString &game)
{
    applog::Action log("tournament.finalize", {{"tournament", tournamentId}});
    db::Tx tx;
    const Row t = requireTournament(tournamentId);
    if (t["status"].toString() == tdb::COMPLETED)
        return standings(tournamentId, game);       // already finished: the saved placings stand
    if (t["status"].toString() == tdb::TERMINATED)
        throw RuleError("This tournament was ended early; it has no final standings to save.");
    const Row current = tdb::currentRound(tournamentId);
    if (!current.isEmpty())
        requireAllReported(current["round_id"].toLongLong(), current["round_number"].toInt());

    calculateTiebreakers(tournamentId, game);
    for (const Row &p : standings(tournamentId, game))
        tdb::updateEnrollmentStats(tournamentId, p["player_id"].toLongLong(), {{"final_placement", p["standing"]}});
    tdb::updateTournamentStatus(tournamentId, tdb::COMPLETED);
    const Rows table = standings(tournamentId, game);       // as saved, placings included
    tx.commit();
    return table;
}

} // namespace swiss
