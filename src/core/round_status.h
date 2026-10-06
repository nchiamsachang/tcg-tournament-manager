// "Which round is this tournament on, and is it being played right now?" for any format.
// The window's tournament pills and the hub's tournament rows both show this, so it is
// answered in one place instead of each screen working it out from the tables.
#pragma once

#include "round_clock.h"

namespace live {

struct RoundStatus {
    timerdb::Kind kind = timerdb::Kind::OneOnOne;   // where the round and its clock are stored
    bool playing = false;       // a round is open: results are being entered and its clock applies
    qint64 roundId = 0;         // the latest round; 0 before round 1
    int roundNumber = 1;
    int totalRounds = 0;        // the configured number of rounds, which is the whole tournament
    bool needsFinish = false;   // every round is played but the event was never closed (earlier-version playoffs)
};

RoundStatus roundStatus(qint64 tournamentId);

} // namespace live
