// The player directory and each player's history.
#pragma once

#include "db.h"

#include <QHash>
#include <QStringList>

namespace pdb {

qint64 addPlayer(const QString &displayName);
db::Rows allPlayers();
db::Row playerById(qint64 playerId);
db::Rows searchPlayers(const QString &text);
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
// Games are in a fixed order: POKEMON, ONEPIECE, MTG.
QHash<qint64, QStringList> gamesPlayed();
void renamePlayer(qint64 playerId, const QString &displayName);
// Removes the player with their one-on-one matches and registrations, all or nothing.
// Throws db::Error (constraint) and removes nothing when other records, such as Commander
// pods, still refer to the player.
void deletePlayer(qint64 playerId);

} // namespace pdb
