// Persistence and workflow for Commander (multiplayer) events.
//
// The database is the only source of truth: every screen re-reads from here,
// and every state change runs in one BEGIN IMMEDIATE transaction that
// re-validates the current state, so repeated or concurrent requests cannot
// create duplicate rounds, double assignments or conflicting results.
// Rules and pairing maths live in commander.h.
//
// an event is its configured number of qualifying rounds, optionally followed by one
// top-four final pod (chosen before the start, commander-rules-4).  without a final,
// finalizing the last round completes the event and its standings are the final
// placings.  with one, the four best eligible players are seeded into the final once the
// last round is finalized, and confirming the final's result completes the event.
// events that reached a playoff under the earlier rules are flagged `legacy_playoff`;
// their playoff rounds are kept as a read-only record and nothing new can be added to
// them.  the new final is stored apart from those (commander_finals) and never mixed up
// with them.
#pragma once

#include "db.h"

#include <QStringList>
#include <stdexcept>

namespace cdb {

extern const char *const FORMAT;        // "Commander"
extern const char *const ORGANIZER;     // single local operator; recorded as the actor in the audit log

// A request that the current tournament state does not allow.
struct CommanderError : std::runtime_error {
    explicit CommanderError(const QString &m) : std::runtime_error(m.toStdString()), message(m) {}
    QString message;
};
// The request raced with, or contradicts, something already saved.
struct ConflictError : CommanderError {
    using CommanderError::CommanderError;
};
// A correction touches rounds that were already published after it.
struct ResolutionRequired : CommanderError {
    ResolutionRequired(const QString &m, const QVariantList &later) : CommanderError(m), laterRounds(later) {}
    QVariantList laterRounds;
};
// a correction would change who plays the final, or their seeds.  the final has not begun,
// so the correction can still be saved with the resolution "REBUILD_FINAL", which discards
// the final (it is then created again from the corrected standings).
struct FinalAffected : CommanderError {
    FinalAffected(const QString &m, const QVariantList &before, const QVariantList &after)
        : CommanderError(m), seedsBefore(before), seedsAfter(after) {}
    QVariantList seedsBefore, seedsAfter;       // player ids, best seed first
};

const int NONE = -1;                    // "not given" for expected versions and round numbers

// the organizer's decision on a won pod's second place.  made from a player id, so 0 is
// "not recorded yet" and any other id names that player; noClearSecond() is the confirmed
// decision that nobody was a clear second (the last opponents lost at the same moment).
// in events that score second place (commander-rules-4) the three are saved apart.  in
// earlier events second place is recognition only and "no clear second" is saved as nothing.
struct SecondPlace {
    enum Kind { NotRecorded, Player, NoClearSecond };
    Kind kind = NotRecorded;
    qint64 playerId = 0;
    SecondPlace(qint64 id = 0) : kind(id ? Player : NotRecorded), playerId(id) {}
    static SecondPlace noClearSecond()
    {
        SecondPlace s;
        s.kind = NoClearSecond;
        return s;
    }
    bool operator==(const SecondPlace &o) const { return kind == o.kind && playerId == o.playerId; }
    bool operator!=(const SecondPlace &o) const { return !(*this == o); }
};

bool isCommander(qint64 tournamentId);
// swissRounds is the number of qualifying rounds (0 = recommended); topFourFinal adds one
// final pod after them.  new events use the current club rules (cmdr::RULES_VERSION); the
// settings may change the draw and pod options but not the scoring.  baseSeed < 0 picks a
// random seed.  returns tournament_id.
qint64 createCommanderTournament(const QString &name, int expectedPlayers = 8, int swissRounds = 0,
                                 const QVariantMap &settings = {}, qint64 baseSeed = -1,
                                 const QString &tournamentDate = {}, int roundTimeMins = 50, bool topFourFinal = false);
QVariantMap getEvent(qint64 tournamentId);
db::Rows getRegistrations(qint64 tournamentId);
// Setup advice for a checked-in player count: {players, rounds, pods, byes, error}.
QVariantMap recommendation(int playerCount);
void setCheckedIn(qint64 tournamentId, qint64 playerId, bool checkedIn);
// organizer overrides, only before the event starts.  pass 0 / empty / -1 to leave a value alone.
void updateEventConfig(qint64 tournamentId, int swissRounds = 0, const QString &drawPolicy = {},
                       const QString &threePodPlacement = {}, int topFourFinal = -1);
QVariantMap syncRecommendation(qint64 tournamentId);
qint64 startEvent(qint64 tournamentId);                 // closes registration, publishes round 1

// Generates and publishes the next round.  Pass the round number you expect to
// create so a repeated click returns the existing round instead of creating
// another.  Refused once the configured number of rounds exists.
qint64 publishNextRound(qint64 tournamentId, int expectedRoundNumber = NONE);

// outcome "WIN" + winnerId, or "DRAW" + eliminatedIds.  Returns the pod's new result_version.
// second: another player of a won pod as its second place, or the decision that there was
// none.  it earns second_place_points in events that score it (still a loss) and is
// recognition only in earlier events.  a draw has none.  while it is not recorded the round
// cannot be finalized.
int reportPodResult(qint64 podId, const QString &outcome, qint64 winnerId = 0, const QList<qint64> &eliminatedIds = {},
                    int expectedVersion = NONE, const SecondPlace &second = {});
int clearPodResult(qint64 podId, int expectedVersion = NONE);
bool finalizeRound(qint64 roundId);                     // false if it was already final
// The organizer's "Finish tournament": finalizes the last configured round and
// saves the completed status and final standings.  false if already finished.  an event
// with a top-four final finishes by confirming the final instead.
bool finishTournament(qint64 tournamentId);
// resolution: "" | "KEEP" | "REBUILD" | "REBUILD_FINAL".  returns true if anything changed.
// in a finalized round of an event that scores second place, a won pod's second place
// must be decided (a player, or no clear second).  REBUILD_FINAL keeps later qualifying
// rounds as published and discards a final that has not begun (see FinalAffected).
bool correctResult(qint64 podId, const QString &outcome, qint64 winnerId = 0, const QList<qint64> &eliminatedIds = {},
                   const QString &resolution = {}, const QString &reason = {}, const SecondPlace &second = {});

// the top-four final.  groups are elimination groups in the order they happened, each the
// players eliminated at the same moment.  see cmdr::placeFinal for how places follow.
//
// who would play the final now: {ready, reason, eligible, players: [{player_id,
// display_name, seed, standing, points}]}.  read-only.
QVariantMap finalCandidates(qint64 tournamentId);
// creates the final pod from the qualifying standings, once every qualifying round is
// finalized.  expectedSeeds are the player ids the organizer confirmed, best seed first;
// if the qualifiers are different now nothing is created (ConflictError).  seats follow
// the seeds, seed 1 taking the first turn.  A repeated request returns the same final.
qint64 createFinal(qint64 tournamentId, const QList<qint64> &expectedSeeds);
// saves the eliminations recorded so far, while the final is being played.  returns the
// final's new result_version.
int saveFinalEliminations(qint64 tournamentId, const QList<QList<qint64>> &groups, int expectedVersion = NONE);
// the places a result would give, without saving anything: [{player_id, display_name,
// seed, place, basis}].  throws CommanderError for a result that cannot be confirmed.
QVariantList previewFinal(qint64 tournamentId, const QList<QList<qint64>> &groups, const QString &outcome,
                          qint64 winnerId = 0);
// saves the eliminations and the ending (outcome "WIN" with winnerId, or "NO_WINNER" for a
// drawn final or one ended without a winner), places the four finalists and completes the
// tournament.  on a confirmed final this is a correction.  returns the new result_version.
int confirmFinal(qint64 tournamentId, const QList<QList<qint64>> &groups, const QString &outcome, qint64 winnerId = 0,
                 int expectedVersion = NONE, const QString &reason = {});
// the final as saved, or an empty map: final_id, status (PLAYING | CONFIRMED), outcome,
// winner_player_id, result_version, begun, groups, qualifying (the standings it was seeded
// from), players: [{player_id, display_name, seed, seat, standing, points,
// elimination_group, placement, placement_basis}] by seed.
QVariantMap getFinal(qint64 tournamentId);

void dropPlayer(qint64 tournamentId, qint64 playerId);
void reinstatePlayer(qint64 tournamentId, qint64 playerId);
void endSwissEarly(qint64 tournamentId);                // fewer than three active players left
bool finishLegacyPlayoff(qint64 tournamentId);          // closes an event that was mid-playoff when playoffs were removed

QVariantList getStandings(qint64 tournamentId);
QVariantList getRounds(qint64 tournamentId);
// One consistent snapshot for the UI: event, players, rounds, standings,
// final_standings, next (SWISS | WAIT | FINAL | FINAL_PLAYING | NONE), active_count,
// rounds_created, total_rounds, can_pair, legacy, final_enabled, scores_second, final,
// candidates (while next is FINAL), second_pending (won pods whose second place is not
// recorded yet: {pod_id, pod_number, round_number}).
QVariantMap getState(qint64 tournamentId);
QVariantList getAudit(qint64 tournamentId, int limit = 200);
QString stageLabel(const QVariantMap &round);
QVariantMap progress(qint64 tournamentId);              // labels and counts for the hub row and live tab
QStringList rulesText(const QVariantMap &event);

} // namespace cdb
