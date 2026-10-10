// Tournament records: tournaments, enrollments, one-on-one rounds and matches.
// These functions read and write rows.  Running an event (starting it, moving to the next
// round, finishing it) is in swiss.h for one-on-one formats and commander_db.h for Commander.
#pragma once

#include "db.h"

#include <QStringList>

namespace tdb {

// Round length in minutes.  The same limits apply wherever a length is entered or loaded.
constexpr int MIN_ROUND_MINUTES = 1;
constexpr int MAX_ROUND_MINUTES = 240;
constexpr int DEFAULT_ROUND_MINUTES = 50;
bool isValidRoundMinutes(int minutes);
// The longest tournament name, in characters, when one is created or renamed.
constexpr int MAX_NAME_LENGTH = 100;

// The games the app runs, by the id stored in tournaments.game, in the order they are
// listed everywhere (home page, game tags, the players filter).  Modern and Commander are
// formats of MTG, not games.  A new game is added here and to the CHECK rule on that column.
const QStringList &supportedGames();

// Tournament status values, as stored in tournaments.status.
extern const QString PENDING;           // created, players registering
extern const QString IN_PROGRESS;
extern const QString COMPLETED;
extern const QString CANCELLED;
extern const QString TERMINATED;        // ended early by the organizer; kept as a read-only record

// Throws std::invalid_argument for an empty or over-long name, fewer than one round or a
// round length outside the limits above.
qint64 createTournament(const QString &name, const QString &game, int totalRounds, const QString &format = {},
                        const QString &tournamentDate = {}, int roundTimeMins = DEFAULT_ROUND_MINUTES, int topCut = 0);
db::Rows tournamentsByGame(const QString &game);                 // most recent first, with player_count
db::Row tournamentById(qint64 tournamentId);                     // empty when not found
db::Rows activeTournaments();
void updateTournamentStatus(qint64 tournamentId, const QString &status);
// Renames a tournament in any state: registering, running, completed or ended early.  Only
// the name of that tournament's own row changes (spaces around it are removed first); its
// status, players, rounds, results, settings, clocks and dates are not touched.  Returns
// false, changing nothing, when that is already its name.  Throws std::invalid_argument for a
// blank name, one longer than MAX_NAME_LENGTH, or an unknown tournament.
bool renameTournament(qint64 tournamentId, const QString &name);

// Ends a tournament early, for any format.  In one transaction: the status becomes
// TERMINATED, the time is recorded in terminated_at and every round clock is stopped where
// it stood.  Players, rounds, pairings and reported results are kept exactly as they are;
// unreported matches stay unreported, no placings are saved and nobody is declared the
// winner.  Returns false, changing nothing, when the tournament was already ended early.
// Throws std::invalid_argument for an unknown tournament and std::logic_error for one that
// is already completed.
bool terminateTournament(qint64 tournamentId);
bool isTerminated(qint64 tournamentId);
// Throws std::logic_error when the tournament was ended early.  The functions that enrol
// players, pair rounds and save results call it first.
void requireNotTerminated(qint64 tournamentId);

// What a game's hub page shows beside its tournament list.
struct GameStats {
    int players = 0;            // distinct players registered in any tournament of this game
    int gamesPlayed = 0;        // reported one-on-one matches plus reported Commander pods
    db::Rows topPlayers;        // up to four rows of display_name, wins
};
GameStats gameStats(const QString &game);

// Registers a player.  Returns the new enrollment's id, or 0 when that player is already
// registered for that tournament (the one case that is not an error).  Anything else that
// stops it is thrown: std::invalid_argument for an unknown tournament or player,
// std::logic_error for a tournament that was ended early or a player who was removed, and
// db::Error for a database failure (locked, could not write, a rule of the schema).
qint64 enrollPlayer(qint64 tournamentId, qint64 playerId);
// "Add & enroll": creates a new player with this name and registers them, in one
// transaction.  If the registration cannot be made, the player is not created either.
// Returns the new player's id.  Throws as enrollPlayer does, and std::invalid_argument for
// a blank name.  Any question about an existing player with the same name is asked by the
// caller beforehand; no dialog is open while this runs.
qint64 addAndEnrollPlayer(qint64 tournamentId, const QString &displayName);
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
//
// writeMatchResult is the step: it checks and writes the match row, joining the caller's
// transaction, and returns the tournament's id.  It does not touch the standings kept with
// the registrations.  The app saves a result through swiss::reportResult, which does both in
// one transaction.  reportMatchResult is that step on its own, as a logged action.
qint64 writeMatchResult(qint64 matchId, const QString &result);
void reportMatchResult(qint64 matchId, const QString &result);
db::Rows allMatches(qint64 tournamentId);
bool havePlayedBefore(qint64 tournamentId, qint64 a, qint64 b);
int pendingMatchCount(qint64 roundId);

} // namespace tdb
