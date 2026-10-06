// Persistence and workflow for Commander (multiplayer) events.
//
// The database is the only source of truth: every screen re-reads from here,
// and every state change runs in one BEGIN IMMEDIATE transaction that
// re-validates the current state, so repeated or concurrent requests cannot
// create duplicate rounds, double assignments or conflicting results.
// Rules and pairing maths live in commander.h.
//
// An event is exactly its configured number of rounds.  There is no playoff,
// semifinal or final stage: finalizing the last configured round completes the
// event and its standings are the final placings.  Events that reached a playoff
// under the earlier rules are flagged `legacy_playoff`; their playoff rounds are
// kept as a read-only record and nothing new can be added to them.
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

const int NONE = -1;                    // "not given" for expected versions and round numbers

bool isCommander(qint64 tournamentId);
// swissRounds is the total number of rounds the event will have (0 = recommended).
// baseSeed < 0 picks a random seed.  Returns tournament_id.
qint64 createCommanderTournament(const QString &name, int expectedPlayers = 8, int swissRounds = 0,
                                 const QVariantMap &settings = {}, qint64 baseSeed = -1,
                                 const QString &tournamentDate = {}, int roundTimeMins = 50);
QVariantMap getEvent(qint64 tournamentId);
db::Rows getRegistrations(qint64 tournamentId);
// Setup advice for a checked-in player count: {players, rounds, pods, byes, error}.
QVariantMap recommendation(int playerCount);
void setCheckedIn(qint64 tournamentId, qint64 playerId, bool checkedIn);
// Organizer overrides, only before the event starts.  Pass 0 / empty to leave a value alone.
void updateEventConfig(qint64 tournamentId, int swissRounds = 0, const QString &drawPolicy = {},
                       const QString &threePodPlacement = {});
QVariantMap syncRecommendation(qint64 tournamentId);
qint64 startEvent(qint64 tournamentId);                 // closes registration, publishes round 1

// Generates and publishes the next round.  Pass the round number you expect to
// create so a repeated click returns the existing round instead of creating
// another.  Refused once the configured number of rounds exists.
qint64 publishNextRound(qint64 tournamentId, int expectedRoundNumber = NONE);

// outcome "WIN" + winnerId, or "DRAW" + eliminatedIds.  Returns the pod's new result_version.
int reportPodResult(qint64 podId, const QString &outcome, qint64 winnerId = 0, const QList<qint64> &eliminatedIds = {},
                    int expectedVersion = NONE);
int clearPodResult(qint64 podId, int expectedVersion = NONE);
bool finalizeRound(qint64 roundId);                     // false if it was already final
// The organizer's "Finish tournament": finalizes the last configured round and
// saves the completed status and final standings.  False if already finished.
bool finishTournament(qint64 tournamentId);
// resolution: "" | "KEEP" | "REBUILD".  Returns true if anything changed.
bool correctResult(qint64 podId, const QString &outcome, qint64 winnerId = 0, const QList<qint64> &eliminatedIds = {},
                   const QString &resolution = {}, const QString &reason = {});

void dropPlayer(qint64 tournamentId, qint64 playerId);
void reinstatePlayer(qint64 tournamentId, qint64 playerId);
void endSwissEarly(qint64 tournamentId);                // fewer than three active players left
bool finishLegacyPlayoff(qint64 tournamentId);          // closes an event that was mid-playoff when playoffs were removed

QVariantList getStandings(qint64 tournamentId);
QVariantList getRounds(qint64 tournamentId);
// One consistent snapshot for the UI: event, players, rounds, standings,
// final_standings, next, active_count, rounds_created, total_rounds, can_pair, legacy.
QVariantMap getState(qint64 tournamentId);
QVariantList getAudit(qint64 tournamentId, int limit = 200);
QString stageLabel(const QVariantMap &round);
QVariantMap progress(qint64 tournamentId);              // labels and counts for the hub row and live tab
QStringList rulesText(const QVariantMap &event);

} // namespace cdb
