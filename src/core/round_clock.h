// Round countdowns, saved as timestamps.
// Nothing counts down in memory: a round stores when its clock was started and how many
// seconds had already run, so the time left is always
//     limit - (elapsed + (now - started_at))
// Closing the window or restarting the app cannot reset it or make it drift.
// All timestamps come from the database clock.
#pragma once

#include "db.h"

namespace timerdb {

// Which table the round is in: one-on-one rounds and Commander rounds are stored separately.
enum class Kind { OneOnOne, Commander };
enum class State { Stopped, Running, Paused };

struct Timer {
    bool valid = false;             // false when the round does not exist
    State state = State::Stopped;
    int elapsed = 0;                // seconds run before the last start
    bool hasStart = false;
    double startedAt = 0;           // database clock, seconds since 1970
    int limit = 0;                  // the tournament's round length in seconds
    double now = 0;                 // database clock when this was read
    bool closed = false;            // the tournament is finished or was ended early: the clock is stopped for good
};

double clockOffset();               // seconds to add to this machine's clock to get the database clock
double remaining(int limitSecs, State state, int elapsedSecs, bool hasStart, double startedAt, double now);
double remaining(const Timer &t, double now);
Timer get(Kind kind, qint64 roundId);
// Start, pause and reset change nothing once the clock is closed.
Timer start(Kind kind, qint64 roundId);     // starting a running clock changes nothing
Timer pause(Kind kind, qint64 roundId);
Timer reset(Kind kind, qint64 roundId);

// A saved clock together with the difference between the database's clock and this
// machine's, taken when it was read.  A screen keeps one and asks it for the time left on
// every tick, without touching the database; it re-reads the saved clock every few seconds.
class Reading {
public:
    Reading() = default;
    explicit Reading(const Timer &timer);
    bool valid() const { return timer_.valid; }
    State state() const { return timer_.state; }
    const Timer &timer() const { return timer_; }
    double remaining() const;       // seconds, never below zero
    int secondsLeft() const;        // rounded up: 49:59.2 is shown as 50:00 until a full second has passed

private:
    Timer timer_;
    double offset_ = 0;             // database clock minus this machine's clock
};
Reading read(Kind kind, qint64 roundId);

QString clockText(int seconds);     // "24:18"

} // namespace timerdb
