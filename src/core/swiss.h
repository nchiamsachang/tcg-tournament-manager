// One-on-one Swiss (Modern, One Piece, Pokémon): scoring, tiebreakers, pairing, and the
// three steps that move an event along: start it, go to the next round, finish it.
#pragma once

#include "db.h"

#include <stdexcept>

namespace swiss {

// An action the rules do not allow right now (results still missing, no round left, the
// round already ended).  Nothing has been changed when this is thrown.
struct RuleError : std::runtime_error {
    explicit RuleError(const QString &message) : std::runtime_error(message.toStdString()) {}
};

struct Points { int win, draw, loss; };
Points points(const QString &game);
double omwFloor(const QString &game);
int suggestedRounds(int playerCount);
// The tiebreakers a game uses after match points, in the order standings() applies them:
// the stored column and the heading shown for it.
QList<QPair<QString, QString>> tiebreakColumns(const QString &game);
// A tiebreaker as the screens, exports and printouts show it: three decimal places, or a
// dash for a figure that was never worked out (see the .cpp).
QString tiebreakText(const QString &game, const QString &column, double value);

// The tiebreakers, per game (club policy; not a claim to follow any publisher's full rules):
//   MTG        opponents' match-win % (OMW%), own win rate (GW%), opponents' win rate (OGW%); floor 33%
//   One Piece  OMW%, GW%; floor 33%
//   Pokémon    opponents' match-win % ("Opp Win%"), then opponents' opponents' win %
//              ("Opp Opp Win%": the average of each opponent's Opp Win%); floor 25%.
//              Points stay 1 per win and 0 for a draw or loss.  Both figures are
//              saved and compared at nine-decimal precision, and shown to three.
// Pokémon's second figure is stored in the ogw_pct column, which holds a different figure
// for MTG.  Players level on every tiebreaker keep the order the database returns them in.

// Recomputes every player's record and tiebreakers from the saved match results and stores
// them with the registrations.  The matches are the source of truth; these stored figures
// are derived from them and can be rebuilt at any time.
void calculateTiebreakers(qint64 tournamentId, const QString &game);
db::Rows standings(qint64 tournamentId, const QString &game);     // from the stored figures; each row gains "standing"
db::Rows currentStandings(qint64 tournamentId, const QString &game);   // calculateTiebreakers, then standings
// The standings to show or print.  It never writes: looking at a table cannot change one.
// A finished tournament shows the figures and placings saved when it was finalized; one
// still being played, or ended early, shows them worked out from its matches in memory.
db::Rows viewStandings(qint64 tournamentId, const QString &game);

// Saves a match result (PLAYER1, PLAYER2, DRAW, or empty to clear it) together with the
// standings that depend on it: the match row and every player's stored record and
// tiebreakers change in one transaction, or not at all.  This is what the round screen
// calls.  Throws as tdb::writeMatchResult does; a failure at any step leaves the previous
// result and the previous figures in place.
void reportResult(qint64 matchId, const QString &result);

// Pairs and saves one round.  Used by the functions below; throws when there is nobody to pair.
qint64 generatePairings(qint64 tournamentId, int roundNumber);

// Marks the tournament as running and pairs round 1.  Throws RuleError if it already started.
void startTournament(qint64 tournamentId);
// Ends round `fromRound` and pairs the next one; returns the new round's id.  Throws
// RuleError when `fromRound` is not the round in play (for instance a second click after the
// round was already ended), when results are missing, or when `fromRound` is the last of the
// configured rounds: the configured number of rounds is the whole tournament.
qint64 advanceToNextRound(qint64 tournamentId, int fromRound);
// Saves final placings and marks the tournament completed; returns the final standings.
// Finishing a tournament that is already completed changes nothing.  Throws RuleError
// while results are missing.
db::Rows finalizeTournament(qint64 tournamentId, const QString &game);

} // namespace swiss
