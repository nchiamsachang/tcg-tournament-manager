// timings of the core operations a screen runs, on generated data in a temporary folder
//
// not part of the test suites: run build\bench_core.exe by hand and compare before and after a change

#include "applog.h"
#include "db.h"
#include "players.h"
#include "round_status.h"
#include "swiss.h"
#include "tournaments.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <random>

using db::Row;
using db::Rows;

namespace {

struct Size {
    const char *name;
    int players, pastEvents, pastField, pastRounds, liveField, liveRounds;
    bool roundIndex = true;
};

std::mt19937 rng(20261011);

void playRound(qint64 tid, bool recalculate)
{
    for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong())) {
        if (m["player2_id"].isNull())
            continue;
        const QString result = rng() % 2 ? "PLAYER1" : "PLAYER2";
        if (recalculate)
            swiss::reportResult(m["match_id"].toLongLong(), result);
        else
            tdb::writeMatchResult(m["match_id"].toLongLong(), result);
    }
}

// median and worst time of `reps` calls, in milliseconds
void measure(const char *scenario, const char *what, int reps, const std::function<void()> &call)
{
    QList<double> ms;
    for (int i = 0; i < reps; ++i) {
        QElapsedTimer t;
        t.start();
        call();
        ms << t.nsecsElapsed() / 1e6;
    }
    std::sort(ms.begin(), ms.end());
    std::printf("%-6s | %-46s | %9.3f | %9.3f | %d\n", scenario, what, ms[ms.size() / 2], ms.last(), reps);
    std::fflush(stdout);
}

void run(const Size &s)
{
    QTemporaryDir dir;
    db::closeThreadConnection();
    db::setPath(dir.filePath("bench.db"));
    db::initialize();
    if (!s.roundIndex)
        db::exec("DROP INDEX idx_matches_round");       // as the schema was before update 6

    QList<qint64> players;
    qint64 live = 0;
    double lastPairingMs = 0;
    QElapsedTimer build;
    build.start();
    {
        db::Tx all;     // one commit for the generated history
        for (int i = 0; i < s.players; ++i)
            players << pdb::addPlayer(QStringLiteral("Player %1").arg(i + 1, 4, 10, QChar('0')));
        const QStringList games = tdb::supportedGames();
        for (int e = 0; e <= s.pastEvents; ++e) {
            const bool isLive = e == s.pastEvents;
            const int field = isLive ? s.liveField : s.pastField, rounds = isLive ? s.liveRounds : s.pastRounds;
            const qint64 tid = tdb::createTournament(QStringLiteral("Event %1").arg(e + 1), games[e % games.size()], rounds);
            std::shuffle(players.begin(), players.end(), rng);
            for (int i = 0; i < field; ++i)
                tdb::enrollPlayer(tid, players[i]);
            swiss::startTournament(tid);
            for (int r = 1; r <= rounds; ++r) {
                if (isLive && r == rounds)
                    break;      // the last round stays open
                playRound(tid, false);
                if (r < rounds) {
                    QElapsedTimer t;
                    t.start();
                    swiss::advanceToNextRound(tid, r);
                    if (isLive)
                        lastPairingMs = t.nsecsElapsed() / 1e6;
                } else {
                    swiss::finalizeTournament(tid, games[e % games.size()]);
                }
            }
            if (isLive)
                live = tid;
        }
        all.commit();
    }
    const qint64 roundId = tdb::currentRound(live)["round_id"].toLongLong();
    const QString game = tdb::tournamentById(live)["game"].toString();
    const int matches = db::value("SELECT COUNT(*) FROM matches").toInt();
    std::printf("%-6s | data: %d players, %d finished events of %d x %d rounds, live event %d x %d rounds, %d matches, "
                "file %lld KB, built in %.1f s\n",
                s.name, s.players, s.pastEvents, s.pastField, s.pastRounds, s.liveField, s.liveRounds, matches,
                QFileInfo(db::path()).size() / 1024, build.elapsed() / 1000.0);
    std::printf("%-6s | %-46s | %9.3f | %9s | 1\n", s.name, "end round: tiebreakers + pair the last round", lastPairingMs, "");

    qint64 match = 0;
    for (const Row &m : tdb::roundPairings(roundId))
        if (!m["player2_id"].isNull())
            match = m["match_id"].toLongLong();
    int flip = 0;
    measure(s.name, "report a result (saved, standings rewritten)", 30,
            [&] { swiss::reportResult(match, ++flip % 2 ? "PLAYER1" : "PLAYER2"); });
    measure(s.name, "round screen: pairings of the round", 200, [&] { tdb::roundPairings(roundId); });
    measure(s.name, "round screen: results still pending", 200, [&] { tdb::pendingMatchCount(roundId); });
    measure(s.name, "standings of the live event", 50, [&] { swiss::viewStandings(live, game); });
    measure(s.name, "hub: tournaments and club stats of a game", 50, [&] {
        tdb::tournamentsByGame("MTG");
        tdb::gameStats("MTG");
    });
    measure(s.name, "players page: directory, game tags, same names", 50, [&] {
        pdb::allPlayers();
        pdb::gamesPlayed();
        pdb::directoryDuplicates();
    });
    measure(s.name, "player search", 200, [&] { pdb::searchPlayers("1"); });
    measure(s.name, "player profile: tournament history", 100, [&] { pdb::tournamentHistory(players.first()); });
    measure(s.name, "tab bar poll: live round status", 200, [&] { live::roundStatus(live); });
    db::closeThreadConnection();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    db::requireChosenPath();
    applog::setDirectory(QString());        // no log file: entries are held in memory and dropped
    std::printf("%-6s | %-46s | %9s | %9s | calls\n", "data", "operation", "median ms", "worst ms");
    // "noindex" times the schema without the index on matches(round_id), for comparison
    const bool index = !(argc > 1 && QByteArray(argv[1]) == "noindex");
    run({"club", 40, 25, 12, 3, 16, 4, index});
    run({"large", 2000, 300, 64, 6, 256, 8, index});
    return 0;
}
