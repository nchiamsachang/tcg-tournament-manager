// The player directory and each player's history.
#pragma once

#include "db.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <stdexcept>

namespace pdb {

// A player is identified by player_id, a number the database assigns once and never reuses
// (the column is AUTOINCREMENT), not by name: two people may have the same name, and a
// rename changes nothing else.  Everything a player did refers to that id.

qint64 addPlayer(const QString &displayName);            // always a new player with a new id
db::Rows allPlayers();                                   // the directory: players not removed
db::Row playerById(qint64 playerId);                     // removed or not; deleted_at tells which
db::Rows searchPlayers(const QString &text);             // within the directory
bool isRemoved(qint64 playerId);

// "#0042": at least four digits, more when the number needs them.
QString formatId(qint64 playerId);
// How two names are compared: case-insensitively, ignoring spaces at the ends and runs of
// spaces inside.  Names are stored in one field, so this is an exact match of the whole
// name, not a guess at first and last names: "Alex Smith" matches "alex  smith", and
// neither "Alex Jones" nor "Jordan Smith".
QString normalizedName(const QString &name);
// The ids, among `players` (rows with player_id and display_name), whose name another
// player in the same list also has.  The list must be the whole set being shown, taken
// before any search or filter, or a needed id could go missing.
QSet<qint64> sharedNameIds(const db::Rows &players);
QSet<qint64> directoryDuplicates();                      // among players not removed
QSet<qint64> tournamentDuplicates(qint64 tournamentId);  // among everyone who took part, removed or not
// The name as shown outside a profile: just the name, or "Alex Smith · #0042" when the id is
// needed to tell two players apart.
QString label(const QString &name, qint64 playerId, const QSet<qint64> &shared);
// Players in the directory with exactly this name (see normalizedName), for the "is this the
// same person?" question when one is added.  Removed players are never offered.
db::Rows playersNamed(const QString &name);

db::Rows tournamentHistory(qint64 playerId);
// One row per one-on-one match the player had in a tournament, in round order:
// round_number, table_number, opponent_name and result (WIN, LOSS, DRAW, BYE or PENDING).
db::Rows matchHistory(qint64 playerId, qint64 tournamentId);
// One row per Commander pod the player sat in, in round order: stage, stage_round,
// pod_number, seat_number and result (WIN, LOSS, DRAW, or empty while unreported).
db::Rows podHistory(qint64 playerId, qint64 tournamentId);
db::Rows lifetimeStats(qint64 playerId);
// The distinct games each player has actually played, for every player in one query.
// "Played" means the player was paired in at least one round (a match, a pod seat
// or a bye) of a tournament that was not cancelled.  Being registered is not enough.
// A tournament that was ended early still counts.  Games are in tdb::supportedGames() order.
QHash<qint64, QStringList> gamesPlayed();
// Throws std::invalid_argument for a blank name or a removed player.  The id does not change.
void renamePlayer(qint64 playerId, const QString &displayName);

// Why a player cannot be removed yet: they are still part of a tournament that is being set
// up or played.  `reasons` has one sentence per tournament, saying what to do there first.
struct StillPlaying : std::runtime_error {
    explicit StillPlaying(const QStringList &r) : std::runtime_error(r.join(" ").toStdString()), reasons(r) {}
    QStringList reasons;
};
// Removes a player from the directory.  Only deleted_at is set: the player's id, name,
// registrations, matches, pod seats, byes, points and placings all stay exactly as they
// are, so nobody else's record changes.  Afterwards the player is left out of the
// directory, searches and new registrations, and cannot be renamed or enrolled.
// Returns false, changing nothing, when the player was already removed.  Throws
// StillPlaying, changing nothing, while they are registered for a tournament that has not
// started, are still in one being played, or have a result outstanding there; and
// std::invalid_argument for an unknown player.
bool removePlayer(qint64 playerId);

} // namespace pdb
