#include "round_clock.h"

#include "tournaments.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>

using db::Row;

namespace timerdb {

// the database clock as seconds since 1970, with fractions
static const char *DB_NOW = "(julianday('now') - 2440587.5) * 86400.0";

static QString tableFor(Kind kind)
{
    return kind == Kind::Commander ? "commander_rounds" : "rounds";
}

static double localNow()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

// Saved as text.  Anything else (including the unused 'ENDED' the schema allows) is a stopped clock.
static State stateFromText(const QString &text)
{
    if (text == "RUNNING") return State::Running;
    if (text == "PAUSED") return State::Paused;
    return State::Stopped;
}

static QString stateText(State state)
{
    switch (state) {
    case State::Running: return "RUNNING";
    case State::Paused: return "PAUSED";
    case State::Stopped: break;
    }
    return "STOPPED";
}

double clockOffset()
{
    return db::value(QStringLiteral("SELECT %1").arg(DB_NOW)).toDouble() - localNow();
}

double remaining(int limitSecs, State state, int elapsedSecs, bool hasStart, double startedAt, double now)
{
    double run = elapsedSecs;
    if (state == State::Running && hasStart)
        run += std::max(0.0, now - startedAt);
    return std::max(0.0, double(limitSecs) - run);
}

double remaining(const Timer &t, double now)
{
    return remaining(t.limit, t.state, t.elapsed, t.hasStart, t.startedAt, now);
}

static bool hasValue(const QVariant &v)
{
    return !v.isNull() && !(v.typeId() == QMetaType::QString && v.toString().isEmpty());
}

Timer get(Kind kind, qint64 roundId)
{
    const Row row = db::one(
        QStringLiteral("SELECT r.timer_state, r.timer_elapsed_secs, r.timer_started_at, t.round_time_mins, %1 AS now "
                       "FROM %2 r JOIN tournaments t ON t.tournament_id = r.tournament_id WHERE r.round_id = ?")
            .arg(DB_NOW, tableFor(kind)),
        {roundId});
    Timer t;
    if (row.isEmpty())
        return t;
    t.valid = true;
    t.state = stateFromText(row["timer_state"].toString());
    t.elapsed = row["timer_elapsed_secs"].toInt();
    t.hasStart = hasValue(row["timer_started_at"]);
    t.startedAt = row["timer_started_at"].toDouble();
    const int mins = row["round_time_mins"].toInt();
    t.limit = (mins ? mins : tdb::DEFAULT_ROUND_MINUTES) * 60;
    t.now = row["now"].toDouble();
    return t;
}

enum class Change { Start, Pause, Reset };

// Reads the saved clock, applies one change and writes it back in a single transaction,
// so two windows pressing Start and Pause at once cannot interleave.
static Timer change(Kind kind, qint64 roundId, Change what)
{
    db::Tx tx;
    Timer t = get(kind, roundId);
    if (!t.valid)
        return t;
    const bool running = t.state == State::Running && t.hasStart;
    switch (what) {
    case Change::Start:
        if (!running) {
            t.state = State::Running;
            t.hasStart = true;
            t.startedAt = t.now;
        }
        break;
    case Change::Pause:
        if (running) {
            t.state = State::Paused;
            t.elapsed += int(std::lround(std::max(0.0, t.now - t.startedAt)));
            t.hasStart = false;
        }
        break;
    case Change::Reset:
        t.state = State::Stopped;
        t.elapsed = 0;
        t.hasStart = false;
        break;
    }
    db::exec(QStringLiteral("UPDATE %1 SET timer_state = ?, timer_elapsed_secs = ?, timer_started_at = ? "
                            "WHERE round_id = ?").arg(tableFor(kind)),
             {stateText(t.state), t.elapsed, t.hasStart ? QVariant(t.startedAt) : QVariant(), roundId});
    tx.commit();
    return t;
}

Timer start(Kind kind, qint64 roundId) { return change(kind, roundId, Change::Start); }
Timer pause(Kind kind, qint64 roundId) { return change(kind, roundId, Change::Pause); }
Timer reset(Kind kind, qint64 roundId) { return change(kind, roundId, Change::Reset); }

Reading::Reading(const Timer &timer) : timer_(timer), offset_(timer.valid ? timer.now - localNow() : 0.0) {}

double Reading::remaining() const
{
    return timer_.valid ? timerdb::remaining(timer_, localNow() + offset_) : 0.0;
}

int Reading::secondsLeft() const
{
    return int(std::ceil(remaining()));
}

Reading read(Kind kind, qint64 roundId)
{
    return Reading(get(kind, roundId));
}

QString clockText(int seconds)
{
    return QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}

} // namespace timerdb
