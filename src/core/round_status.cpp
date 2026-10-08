#include "round_status.h"

#include "commander_db.h"
#include "tournaments.h"

namespace live {

RoundStatus roundStatus(qint64 tournamentId)
{
    RoundStatus status;
    const db::Row tournament = tdb::tournamentById(tournamentId);
    status.totalRounds = tournament["total_rounds"].toInt();
    status.terminated = tournament["status"].toString() == tdb::TERMINATED;

    if (cdb::isCommander(tournamentId)) {
        const QVariantMap progress = cdb::progress(tournamentId);
        status.kind = timerdb::Kind::Commander;
        status.playing = progress["active"].toBool();
        status.roundId = progress["round_id"].toLongLong();
        status.roundNumber = progress["round_number"].toInt();
        status.needsFinish = progress["needs_finish"].toBool();
        return status;
    }

    // one-on-one: the latest round stays open until the next one is paired or the event is finished
    const db::Row current = tdb::currentRound(tournamentId);
    if (current.isEmpty())
        return status;
    status.playing = !status.terminated;
    status.roundId = current["round_id"].toLongLong();
    status.roundNumber = current["round_number"].toInt();
    return status;
}

} // namespace live
