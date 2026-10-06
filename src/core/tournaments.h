// Tournament records: tournaments, enrollments, one-on-one rounds and matches.
// These functions read and write rows.  Running an event (starting it, moving to the next
// round, finishing it) is in swiss.h for one-on-one formats and commander_db.h for Commander.
#pragma once

#include "db.h"

namespace tdb {

// Round length in minutes.  The same limits apply wherever a length is entered or loaded.
constexpr int MIN_ROUND_MINUTES = 1;
constexpr int MAX_ROUND_MINUTES = 240;
constexpr int DEFAULT_ROUND_MINUTES = 50;
bool isValidRoundMinutes(int minutes);

// Tournament status values, as stored in tournaments.status.
extern const QString PENDING;           // created, players registering
extern const QString IN_PROGRESS;
extern const QString COMPLETED;
extern const QString CANCELLED;

// Throws std::invalid_argument for an empty name, fewer than one round or a round length
// outside the limits above.
qint64 createTournament(const QString &name, const QString &game, int totalRounds, const QString &format = {},
                        const QString &tournamentDate = {}, int roundTimeMins = DEFAULT_ROUND_MINUTES, int topCut = 0);
db::Rows tournamentsByGame(const QString &game);                 // most recent first, with player_count
db::Row tournamentById(qint64 tournamentId);                     // empty when not found
db::Rows activeTournaments();
void updateTournamentStatus(qint64 tournamentId, const QString &status);

// What a game's hub page shows beside its tournament list.
struct GameStats {
    int players = 0;            // distinct players registered in any tournament of this game
    int gamesPlayed = 0;        // reported one-on-one matches plus reported Commander pods
    db::Rows topPlayers;        // up to four rows of display_name, wins
};
GameStats gameStats(const QString &game);

qint64 enrollPlayer(qint64 tournamentId, qint64 playerId);       // 0 if already enrolled
void unenrollPlayer(qint64 tournamentId, qint64 playerId);
db::Rows enrolledPlayers(qint64 tournamentId, bool includeDropped = false);
// Only the keys present in `stats` are written.
void updateEnrollmentStats(qint64 tournamentId, qint64 playerId, const QVariantMap &stats);

qint64 createRound(qint64 tournamentId, int roundNumber);
db::Rows rounds(qint64 tournamentId);                            // in round order
db::Row currentRound(qint64 tournamentId);                       // the latest round; empty before round 1
db::Row roundByNumber(qint64 tournamentId, int roundNumber);

qint64 createMatch(qint64 roundId, qint64 tournamentId, qint64 player1, const QVariant &player2 = {},
                   const QVariant &tableNumber = {});
db::Rows roundPairings(qint64 roundId);                          // with names; "result" is null while pending
// `result` is PLAYER1, PLAYER2 or DRAW; an empty string puts the match back to "not reported".
// Throws std::invalid_argument for anything else, and std::logic_error when the match is a
// bye, is not in the tournament's current round, or the tournament is finished.
void reportMatchResult(qint64 matchId, const QString &result);
db::Rows allMatches(qint64 tournamentId);
bool havePlayedBefore(qint64 tournamentId, qint64 a, qint64 b);
int pendingMatchCount(qint64 roundId);

} // namespace tdb
