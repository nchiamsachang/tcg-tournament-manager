// Tests for the database, rules and workflow layer.
//
// reference.json holds recorded events; the replay tests require the engine
// to produce exactly the same pods, seats, byes and standings from the same
// inputs and seeds.
#include "commander.h"
#include "commander_db.h"
#include "db.h"
#include "round_status.h"
#include "store.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

using db::Row;
using db::Rows;

namespace {

QList<qint64> ids(const QVariantList &seats)
{
    QList<qint64> out;
    for (const QVariant &s : seats)
        out << s.toMap().value("player_id").toLongLong();
    return out;
}

QVariantMap podAt(const QVariantMap &round, int i) { return round["pods"].toList()[i].toMap(); }
qint64 seatPlayer(const QVariantMap &pod, int i) { return pod["seats"].toList()[i].toMap()["player_id"].toLongLong(); }
qint64 podId(const QVariantMap &pod) { return pod["pod_id"].toLongLong(); }

QStringList auditActions(qint64 tid)
{
    QStringList out;
    for (const QVariant &a : cdb::getAudit(tid))
        out << a.toMap()["action"].toString();
    return out;
}

struct Race {
    QList<qint64> results;
    int conflicts = 0, refusals = 0, other = 0;
};

// Runs fn on n threads at once.
template <typename F> Race race(F fn, int n = 8)
{
    Race out;
    std::mutex lock;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load())
                std::this_thread::yield();
            try {
                const qint64 r = qint64(fn(i));
                std::lock_guard<std::mutex> g(lock);
                out.results << r;
            } catch (const cdb::ConflictError &) {
                std::lock_guard<std::mutex> g(lock);
                ++out.conflicts;
            } catch (const cdb::CommanderError &) {
                std::lock_guard<std::mutex> g(lock);
                ++out.refusals;
            } catch (const std::exception &) {
                std::lock_guard<std::mutex> g(lock);
                ++out.other;
            }
            db::closeThreadConnection();
        });
    }
    go.store(true);
    for (std::thread &t : threads)
        t.join();
    return out;
}

} // namespace

class CoreTests : public QObject {
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> tmp_;

    QList<qint64> addPlayers(int n, const QString &prefix = "P")
    {
        QList<qint64> out;
        for (int i = 0; i < n; ++i)
            out << pdb::addPlayer(QStringLiteral("%1%2").arg(prefix).arg(i + 1, 3, 10, QChar('0')));
        return out;
    }

    QPair<qint64, QList<qint64>> makeEvent(int n, int rounds = 0, bool start = true, qint64 seed = 12345,
                                           const QVariantMap &settings = {})
    {
        const qint64 tid = cdb::createCommanderTournament("Test Commander", n, rounds, settings, seed);
        const QList<qint64> pids = addPlayers(n);
        for (qint64 p : pids)
            tdb::enrollPlayer(tid, p);
        if (start)
            cdb::startEvent(tid);
        return {tid, pids};
    }

    QVariantMap currentRound(qint64 tid) { return cdb::getRounds(tid).last().toMap(); }

    QVariantMap playRound(qint64 tid, bool finalize = true)
    {
        const QVariantMap rnd = currentRound(tid);
        for (const QVariant &pv : rnd["pods"].toList()) {
            const QVariantMap pod = pv.toMap();
            if (!pod["reported"].toBool())
                cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));
        }
        if (finalize)
            cdb::finalizeRound(rnd["round_id"].toLongLong());
        return rnd;
    }

    void assertValidRound(const QVariantMap &rnd, QList<qint64> active)
    {
        QList<qint64> assigned;
        for (const QVariant &pv : rnd["pods"].toList()) {
            const QVariantList seats = pv.toMap()["seats"].toList();
            QVERIFY(seats.size() == 3 || seats.size() == 4);
            for (int i = 0; i < seats.size(); ++i)
                QCOMPARE(seats[i].toMap()["seat"].toInt(), i + 1);
            assigned << ids(seats);
        }
        for (const QVariant &b : rnd["byes"].toList())
            assigned << b.toMap()["player_id"].toLongLong();
        std::sort(assigned.begin(), assigned.end());
        std::sort(active.begin(), active.end());
        QCOMPARE(assigned, active);      // every active player assigned exactly once
    }

    // Plays `rounds` rounds, leaving the last one reported but not finalized.
    QPair<qint64, QList<qint64>> runEvent(int n, int rounds)
    {
        auto [tid, pids] = makeEvent(n, rounds);
        for (int number = 1; number <= rounds; ++number) {
            const QVariantMap rnd = currentRound(tid);
            [&] { QCOMPARE(rnd["round_number"].toInt(), number); QCOMPARE(rnd["stage"].toString(), QString("SWISS")); }();
            playRound(tid, false);
            if (number < rounds) {
                [&] { QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finishTournament(tid)); }();   // not the last round yet
                cdb::finalizeRound(rnd["round_id"].toLongLong());
                cdb::publishNextRound(tid, number + 1);
            }
        }
        return {tid, pids};
    }

    void restart()
    {
        db::closeThreadConnection();     // a fresh start only has the file
        db::initialize();
        db::initialize();
    }

    static QString stateJson(qint64 tid)
    {
        return QString::fromUtf8(QJsonDocument::fromVariant(cdb::getState(tid)).toJson());
    }

private slots:
    void init()
    {
        tmp_ = std::make_unique<QTemporaryDir>();
        db::closeThreadConnection();
        db::setPath(tmp_->filePath("test.db"));
        db::initialize();
    }

    void cleanup()
    {
        db::closeThreadConnection();
        tmp_.reset();
    }

    void podSizes()
    {
        const QMap<int, QList<int>> expected{{6, {3, 3}}, {7, {4, 3}}, {8, {4, 4}}, {9, {3, 3, 3}}, {10, {4, 3, 3}},
                                             {11, {4, 4, 3}}, {13, {4, 3, 3, 3}}, {14, {4, 4, 3, 3}}};
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            QCOMPARE(cmdr::podSizes(it.key()).sizes, it.value());
            QCOMPARE(cmdr::podSizes(it.key()).byes, 0);
        }
        QCOMPARE(cmdr::podSizes(3).sizes, QList<int>{3});
        QCOMPARE(cmdr::podSizes(4).sizes, QList<int>{4});
        QCOMPARE(cmdr::podSizes(5).sizes, QList<int>{4});
        QCOMPARE(cmdr::podSizes(5).byes, 1);
        for (int n : {0, 1, 2}) {
            try {
                cmdr::podSizes(n);
                QFAIL("fewer than three players must be refused");
            } catch (const cmdr::PairingError &e) {
                QVERIFY(QString(e.what()).contains("at least 3"));
            }
        }
        for (int n = 6; n < 400; ++n) {
            const cmdr::PodSizes s = cmdr::podSizes(n);
            QCOMPARE(s.byes, 0);
            QCOMPARE(std::accumulate(s.sizes.begin(), s.sizes.end(), 0), n);
            for (int size : s.sizes)
                QVERIFY(size == 3 || size == 4);
            QVERIFY2(s.sizes.count(3) <= 3, "more fours were possible");
        }
    }

    void roundCountBoundaries()
    {
        const QMap<int, int> table{{3, 3}, {16, 3}, {17, 4}, {34, 4}, {35, 5}, {64, 5}, {65, 5}, {128, 5}, {129, 6},
                                   {208, 6}, {209, 7}, {304, 7}, {305, 8}, {540, 8}, {541, 9}, {960, 9}, {961, 10},
                                   {5000, 10}};
        for (auto it = table.begin(); it != table.end(); ++it)
            QCOMPARE(cmdr::recommendedRounds(it.key()), it.value());
        QVERIFY(!cmdr::defaultSettings().contains("playoff_draw_rule"));
        QVERIFY(!cdb::recommendation(40).contains("cut"));
    }

    void scoring()
    {
        const QVariantMap s = cmdr::defaultSettings();
        auto res = cmdr::seatResults({1, 2, 3, 4}, "WIN", 3, {}, s);
        QCOMPARE(res[3], (cmdr::SeatResult{"WIN", 0}));
        for (qint64 p : {1, 2, 4})
            QCOMPARE(res[p], (cmdr::SeatResult{"LOSS", 1}));

        res = cmdr::seatResults({1, 2, 3, 4}, "DRAW", 0, {2}, s);        // already eliminated players lose
        QCOMPARE(res[2], (cmdr::SeatResult{"LOSS", 1}));
        for (qint64 p : {1, 3, 4})
            QCOMPARE(res[p], (cmdr::SeatResult{"DRAW", 0}));

        QVariantMap all = s;
        all["draw_policy"] = "ALL_DRAW";
        res = cmdr::seatResults({1, 2, 3}, "DRAW", 0, {2}, all);
        for (qint64 p : {1, 2, 3})
            QCOMPARE(res[p].result, QString("DRAW"));

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, cmdr::seatResults({1, 2, 3}, "WIN", 9, {}, s));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, cmdr::seatResults({1, 2, 3}, "DRAW", 0, {1, 2}, s));  // one left = a win
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, cmdr::seatResults({1, 2, 3}, "DRAW", 1, {}, s));
    }

    void seededRandomIsReproducedExactly()
    {
        // seed 12345: first 32-bit outputs, and a shuffle of 0..9
        cmdr::SeededRandom a(12345);
        QCOMPARE(a.next32(), quint32(1789368711));
        QCOMPARE(a.next32(), quint32(3146859322));
        cmdr::SeededRandom b(12345);
        QList<int> seq{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        b.shuffle(seq);
        QCOMPARE(seq, (QList<int>{8, 7, 3, 5, 1, 2, 9, 4, 0, 6}));
    }

    void referencePairingsAndStandingsAreReproduced()
    {
        QFile f(QStringLiteral(FIXTURE_DIR) + "/reference.json");
        QVERIFY2(f.open(QIODevice::ReadOnly), "reference.json is missing");
        const QVariantMap doc = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
        QCOMPARE(doc["algorithm"].toString(), QString(cmdr::PAIRING_ALGORITHM));
        int rounds = 0;
        for (const QVariant &cv : doc["cases"].toList()) {
            const QVariantMap c = cv.toMap();
            const qint64 baseSeed = c["base_seed"].toLongLong();
            QList<qint64> players;
            for (const QVariant &p : c["player_ids"].toList())
                players << p.toLongLong();
            const QByteArray tag = QStringLiteral("%1 players, seed %2").arg(players.size()).arg(baseSeed).toUtf8();
            for (const QVariant &sv : c["steps"].toList()) {
                const QVariantMap step = sv.toMap();
                const qint64 seed = step["seed"].toLongLong();
                QVERIFY2(cmdr::deriveSeed(baseSeed, step["round_number"].toInt()) == seed, tag);

                const QVariantMap got = cmdr::pairRound(step["inputs"].toMap(), seed);
                const auto asIds = [](const QVariant &v) {
                    QList<QList<qint64>> out;
                    for (const QVariant &pod : v.toList()) {
                        QList<qint64> one;
                        for (const QVariant &p : pod.toList())
                            one << p.toLongLong();
                        out << one;
                    }
                    return out;
                };
                QVERIFY2(asIds(got["pods"]) == asIds(step["pods"]), tag);
                QVERIFY2(asIds(QVariantList{got["byes"]}) == asIds(QVariantList{step["byes"]}), tag);
                const QVariantMap cost = got["cost"].toMap(), want = step["cost"].toMap();
                for (const char *key : {"total", "score", "seat", "rematch_pairs", "three_player_pods"})
                    QVERIFY2(qAbs(cost[key].toDouble() - want[key].toDouble()) < 1e-6, tag);

                const QVariantList table = cmdr::computeStandings(players, step["rounds_so_far"].toList(),
                                                                  c["settings"].toMap(), baseSeed);
                const QVariantList expected = step["standings"].toList();
                QVERIFY2(table.size() == expected.size(), tag);
                for (int i = 0; i < table.size(); ++i) {
                    const QVariantMap a = table[i].toMap(), b = expected[i].toMap();
                    for (const char *key : {"player_id", "points", "wins", "losses", "draws", "byes", "played",
                                            "random_key", "standing"})
                        QVERIFY2(a[key].toLongLong() == b[key].toLongLong(), tag + " " + key);
                    for (const char *key : {"mw_pct", "omw_pct"})
                        QVERIFY2(a[key].toDouble() == b[key].toDouble(), tag + " " + key);
                }
                ++rounds;
            }
        }
        QVERIFY(rounds >= 70);
    }

    void everyRoundIsValidForManyFieldSizes()
    {
        for (int n : {3, 4, 5, 6, 7, 9, 10, 11, 13, 14, 18, 23, 31}) {
            auto [tid, pids] = makeEvent(n, 3);
            for (int r = 1; r <= 3; ++r) {
                assertValidRound(currentRound(tid), pids);
                playRound(tid);
                if (r < 3)
                    cdb::publishNextRound(tid);
            }
        }
    }

    void fivePlayersShareTheBye()
    {
        auto [tid, pids] = makeEvent(5, 4);
        QList<qint64> byes;
        for (int r = 1; r <= 4; ++r) {
            const QVariantMap rnd = currentRound(tid);
            QCOMPARE(rnd["pods"].toList().size(), 1);
            QCOMPARE(rnd["byes"].toList().size(), 1);
            byes << rnd["byes"].toList()[0].toMap()["player_id"].toLongLong();
            playRound(tid);
            if (r < 4)
                cdb::publishNextRound(tid);
        }
        QCOMPARE(QSet<qint64>(byes.begin(), byes.end()).size(), 4);     // nobody gets a second bye first
    }

    void threePlayerPodsGoToTheLowestStandingsByDefault()
    {
        auto [tid, pids] = makeEvent(7, 3);
        playRound(tid);
        cdb::publishNextRound(tid);
        QHash<qint64, int> points;
        for (const QVariant &r : cdb::getStandings(tid))
            points.insert(r.toMap()["player_id"].toLongLong(), r.toMap()["points"].toInt());
        for (const QVariant &pv : currentRound(tid)["pods"].toList()) {
            const QList<qint64> seated = ids(pv.toMap()["seats"].toList());
            if (seated.size() == 3)
                for (qint64 p : seated)
                    QCOMPARE(points[p], 0);       // the two winners play the four-player pod
        }
    }

    void largeFieldIsFastAndValid()
    {
        QElapsedTimer t;
        t.start();
        auto [tid, pids] = makeEvent(203, 3);
        for (int r = 1; r <= 3; ++r) {
            assertValidRound(currentRound(tid), pids);
            playRound(tid);
            if (r < 3)
                cdb::publishNextRound(tid);
        }
        QVERIFY2(t.elapsed() < 60000, "three rounds of 203 players took over a minute");
    }

    void startRequiresThreeCheckedInPlayers()
    {
        auto [tid, pids] = makeEvent(3, 0, false);
        cdb::setCheckedIn(tid, pids[0], false);
        try {
            cdb::startEvent(tid);
            QFAIL("two players cannot start an event");
        } catch (const cdb::CommanderError &e) {
            QVERIFY(e.message.contains("at least 3"));
        }
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("REGISTRATION"));
        cdb::setCheckedIn(tid, pids[0], true);
        cdb::startEvent(tid);
        QCOMPARE(podAt(currentRound(tid), 0)["seats"].toList().size(), 3);
    }

    void playersNotCheckedInAreLeftOut()
    {
        auto [tid, pids] = makeEvent(9, 0, false);
        cdb::setCheckedIn(tid, pids[0], false);
        cdb::startEvent(tid);
        assertValidRound(currentRound(tid), pids.mid(1));
        QCOMPARE(cdb::getStandings(tid).size(), 8);
    }

    void configFreezesAtStart()
    {
        auto [tid, pids] = makeEvent(8, 0, false);
        cdb::updateEventConfig(tid, 5, "ALL_DRAW");
        cdb::startEvent(tid);
        const QVariantMap ev = cdb::getEvent(tid);
        QCOMPARE(ev["swiss_rounds"].toInt(), 5);
        QCOMPARE(ev["playoff_cut"].toInt(), 0);
        QCOMPARE(ev["settings"].toMap()["draw_policy"].toString(), QString("ALL_DRAW"));
        QCOMPARE(ev["recommended_rounds"].toInt(), 3);
        QCOMPARE(ev["started_player_count"].toInt(), 8);
        const Row t = tdb::tournamentById(tid);
        QCOMPARE(t["total_rounds"].toInt(), 5);
        QCOMPARE(t["top_cut"].toInt(), 0);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::updateEventConfig(tid, 2));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::setCheckedIn(tid, pids[0], false));
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::startEvent(tid));
    }

    void aPlayoffCannotBeConfigured()
    {
        db::exec("INSERT INTO tournaments (name, game, format, tournament_date, total_rounds) "
                 "VALUES ('raw', 'MTG', 'Commander', '2026-01-01', 3)");
        const qint64 tid = db::value("SELECT MAX(tournament_id) FROM tournaments").toLongLong();
        try {
            db::exec("INSERT INTO commander_events (tournament_id, rules_version, settings_json, swiss_rounds, "
                     "playoff_cut, base_seed) VALUES (?, 'x', '{}', 3, 4, 1)", {tid});
            QFAIL("the database must refuse a playoff");
        } catch (const db::Error &e) {
            QVERIFY(e.constraint);
        }
    }

    void nextRoundWaitsForFinalizedResults()
    {
        auto [tid, pids] = makeEvent(8, 3);
        const qint64 rid = currentRound(tid)["round_id"].toLongLong();
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));
        try {
            cdb::finalizeRound(rid);
            QFAIL("a round with no results cannot be finalized");
        } catch (const cdb::CommanderError &e) {
            QVERIFY(e.message.contains("no result"));
        }
        playRound(tid, false);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));   // reported but not finalized
        QVERIFY(cdb::finalizeRound(rid));
        QVERIFY2(!cdb::finalizeRound(rid), "finalizing twice is a no-op");
        cdb::publishNextRound(tid);
        QCOMPARE(cdb::getRounds(tid).size(), 2);
    }

    void drawResultsAndStandings()
    {
        auto [tid, pids] = makeEvent(4, 1);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        cdb::reportPodResult(podId(pod), "DRAW", 0, {seated[3]});
        const QVariantList seats = podAt(currentRound(tid), 0)["seats"].toList();
        QStringList results;
        for (const QVariant &s : seats)
            results << s.toMap()["result"].toString();
        QCOMPARE(results, (QStringList{"DRAW", "DRAW", "DRAW", "LOSS"}));
        QHash<qint64, QVariantMap> table;
        for (const QVariant &r : cdb::getStandings(tid))
            table.insert(r.toMap()["player_id"].toLongLong(), r.toMap());
        QCOMPARE(table[seated[0]]["points"].toInt(), 1);
        QCOMPARE(table[seated[3]]["points"].toInt(), 0);
        QCOMPARE(table[seated[3]]["losses"].toInt(), 1);
        QCOMPARE(table[seated[0]]["draws"].toInt(), 1);
    }

    void dropsChangeFuturePodsButKeepHistoryAndRoundCount()
    {
        auto [tid, pids] = makeEvent(8, 3);
        playRound(tid);
        const qint64 gone = pids[0];
        cdb::dropPlayer(tid, gone);
        QCOMPARE(cdb::getState(tid)["active_count"].toInt(), 7);
        cdb::publishNextRound(tid);
        const QVariantMap r2 = currentRound(tid);
        assertValidRound(r2, pids.mid(1));
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 3);
        QList<qint64> history;
        for (const QVariant &pv : cdb::getRounds(tid)[0].toMap()["pods"].toList())
            history << ids(pv.toMap()["seats"].toList());
        QVERIFY2(history.contains(gone), "round one still shows the dropped player");
        for (const QVariant &r : cdb::getStandings(tid)) {
            if (r.toMap()["player_id"].toLongLong() == gone) {
                QCOMPARE(r.toMap()["dropped"].toInt(), 1);
                QCOMPARE(r.toMap()["drop_round"].toInt(), 1);
            }
        }
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reinstatePlayer(tid, gone));   // a round was published since
    }

    void dropCanBeUndoneBeforeTheNextRound()
    {
        auto [tid, pids] = makeEvent(8, 3);
        playRound(tid);
        cdb::dropPlayer(tid, pids[0]);
        cdb::reinstatePlayer(tid, pids[0]);
        cdb::publishNextRound(tid);
        assertValidRound(currentRound(tid), pids);
    }

    void tooManyDropsBlockPairingWithAnExplanation()
    {
        auto [tid, pids] = makeEvent(4, 3);
        playRound(tid);
        cdb::dropPlayer(tid, pids[0]);
        cdb::dropPlayer(tid, pids[1]);
        QVariantMap state = cdb::getState(tid);
        QCOMPARE(state["next"].toString(), QString("SWISS"));
        QVERIFY(!state["can_pair"].toBool());
        try {
            cdb::publishNextRound(tid);
            QFAIL("two players cannot be paired");
        } catch (const cdb::CommanderError &e) {
            QVERIFY(e.message.contains("at least 3"));
        }
        cdb::endSwissEarly(tid);
        state = cdb::getState(tid);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QVERIFY2(state["rounds"].toList().size() == 1, "finishing early never adds a round");
        QCOMPARE(state["final_standings"].toList().size(), 4);
    }

    void publishingIsRepeatableAndSavesItsInputs()
    {
        auto [tid, pids] = makeEvent(13, 3);
        playRound(tid);
        const qint64 rid = cdb::publishNextRound(tid, 2);
        QVERIFY2(cdb::publishNextRound(tid, 2) == rid, "a repeated click is the same round");
        const Row row = db::one("SELECT pairing_inputs_json, pairing_seed FROM commander_rounds WHERE round_id = ?", {rid});
        const QVariantMap inputs = QJsonDocument::fromJson(row["pairing_inputs_json"].toString().toUtf8()).object().toVariantMap();
        const QVariantMap again = cmdr::pairRound(inputs, row["pairing_seed"].toLongLong());
        const QVariantList pods = currentRound(tid)["pods"].toList();
        for (int i = 0; i < pods.size(); ++i) {
            QList<qint64> rerun;
            for (const QVariant &p : again["pods"].toList()[i].toList())
                rerun << p.toLongLong();
            QCOMPARE(rerun, ids(pods[i].toMap()["seats"].toList()));
        }
    }

    void changingALiveResultNeedsTheCurrentVersion()
    {
        auto [tid, pids] = makeEvent(4, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        const int v1 = cdb::reportPodResult(podId(pod), "WIN", seated[0]);
        QVERIFY2(cdb::reportPodResult(podId(pod), "WIN", seated[0]) == v1, "repeat is a no-op");
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::reportPodResult(podId(pod), "WIN", seated[1]));
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::reportPodResult(podId(pod), "WIN", seated[1], {}, v1 - 1));
        const int v2 = cdb::reportPodResult(podId(pod), "WIN", seated[1], {}, v1);
        QCOMPARE(v2, v1 + 1);
        QHash<qint64, int> table;
        for (const QVariant &r : cdb::getStandings(tid))
            table.insert(r.toMap()["player_id"].toLongLong(), r.toMap()["points"].toInt());
        QCOMPARE(table[seated[0]], 0);
        QCOMPARE(table[seated[1]], 5);
        const int v3 = cdb::clearPodResult(podId(pod), v2);
        QCOMPARE(v3, v2 + 1);
        QCOMPARE(podAt(currentRound(tid), 0)["status"].toString(), QString("PENDING"));
        const QStringList actions = auditActions(tid);
        for (const char *expected : {"RESULT_REPORTED", "RESULT_CORRECTED", "RESULT_CLEARED"})
            QVERIFY(actions.contains(expected));
    }

    void correctionBehindAPublishedRoundNeedsAResolution()
    {
        auto [tid, pids] = makeEvent(8, 3);
        const QVariantMap r1 = playRound(tid);
        const QVariantMap pod = podAt(r1, 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        // nothing published after it yet: corrects directly
        QVERIFY(cdb::correctResult(podId(pod), "WIN", seated[2], {}, {}, "misreported"));
        QVERIFY2(!cdb::correctResult(podId(pod), "WIN", seated[2]), "no change, nothing to do");
        const QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["reason"].toString(), QString("misreported"));

        cdb::publishNextRound(tid);
        const QString before = QString::fromUtf8(QJsonDocument::fromVariant(currentRound(tid)["pods"]).toJson());
        try {
            cdb::correctResult(podId(pod), "WIN", seated[1]);
            QFAIL("a resolution is required");
        } catch (const cdb::ResolutionRequired &e) {
            QCOMPARE(e.laterRounds.size(), 1);
            QCOMPARE(e.laterRounds[0].toMap()["round_number"].toInt(), 2);
        }
        cdb::correctResult(podId(pod), "WIN", seated[1], {}, "KEEP");
        QCOMPARE(QString::fromUtf8(QJsonDocument::fromVariant(currentRound(tid)["pods"]).toJson()), before);

        cdb::correctResult(podId(pod), "WIN", seated[3], {}, "REBUILD");
        QCOMPARE(cdb::getRounds(tid).size(), 1);
        QCOMPARE(cdb::getState(tid)["next"].toString(), QString("SWISS"));
        cdb::publishNextRound(tid);
        const QVariantMap r2 = currentRound(tid);
        cdb::reportPodResult(podId(podAt(r2, 0)), "WIN", seatPlayer(podAt(r2, 0), 0));
        try {
            cdb::correctResult(podId(pod), "WIN", seated[0], {}, "REBUILD");
            QFAIL("played rounds cannot be rebuilt");
        } catch (const cdb::CommanderError &e) {
            QVERIFY(e.message.contains("already have results"));
        }
    }

    void correctingTheLastRoundUpdatesTheSavedFinalStandings()
    {
        auto [tid, pids] = makeEvent(8, 1);
        const QVariantMap r1 = playRound(tid);
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));
        const QVariantMap pod = podAt(r1, 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        cdb::correctResult(podId(pod), "DRAW", 0, {}, {}, "it was a draw");
        const QVariantMap after = cdb::getState(tid);
        QVERIFY2(after["event"].toMap()["stage"].toString() == "COMPLETE", "a correction never reopens or extends the event");
        QCOMPARE(after["rounds"].toList().size(), 1);
        const QVariantList snapshot = QJsonDocument::fromJson(
            after["event"].toMap()["final_standings_json"].toString().toUtf8()).array().toVariantList();
        for (const QVariant &s : snapshot)
            if (seated.contains(s.toMap()["player_id"].toLongLong()))
                QCOMPARE(s.toMap()["points"].toInt(), 1);
        QCOMPARE(ids(after["final_standings"].toList()), ids(snapshot));
    }

    // The configured round count is the whole tournament.

    void threeRoundEventIsExactlyThreeRounds()
    {
        auto [tid, pids] = runEvent(8, 3);
        QVariantMap state = cdb::getState(tid);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("SWISS"));   // not finished by itself
        QCOMPARE(state["next"].toString(), QString("WAIT"));
        QVERIFY(state["final_standings"].isNull());
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));

        QVERIFY(cdb::finishTournament(tid));
        state = cdb::getState(tid);
        const QVariantMap ev = state["event"].toMap();
        const QVariantList rounds = state["rounds"].toList();
        QCOMPARE(rounds.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(rounds[i].toMap()["round_number"].toInt(), i + 1);
            QCOMPARE(rounds[i].toMap()["stage"].toString(), QString("SWISS"));
            QCOMPARE(rounds[i].toMap()["status"].toString(), QString("FINALIZED"));
        }
        QCOMPARE(ev["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(state["next"].toString(), QString("NONE"));
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));
        QVERIFY(!state.contains("advancements"));
        QVERIFY(ev["playoff_seeds"].toList().isEmpty());

        // final standings: computed with the event's scoring and tiebreakers, and saved
        const QVariantList table = cdb::getStandings(tid);
        QCOMPARE(ids(state["final_standings"].toList()), ids(table));
        int total = 0;
        for (const QVariant &r : table)
            total += r.toMap()["points"].toInt();
        QCOMPARE(total, 3 * 2 * 5);                 // two pods a round, five points a win
        const QVariantList snapshot = QJsonDocument::fromJson(ev["final_standings_json"].toString().toUtf8()).array().toVariantList();
        QCOMPARE(snapshot.size(), 8);
        for (int i = 0; i < snapshot.size(); ++i) {
            QCOMPARE(snapshot[i].toMap()["place"].toInt(), i + 1);
            QCOMPARE(snapshot[i].toMap()["player_id"].toLongLong(), table[i].toMap()["player_id"].toLongLong());
            QCOMPARE(snapshot[i].toMap()["points"].toInt(), table[i].toMap()["points"].toInt());
        }
        QCOMPARE(ev["champion_player_id"].toLongLong(), table[0].toMap()["player_id"].toLongLong());
        QVERIFY(!ev["completed_at"].isNull());
        const Rows placed = db::query("SELECT player_id, final_placement, match_points FROM enrollments "
                                      "WHERE tournament_id = ? ORDER BY final_placement", {tid});
        for (int i = 0; i < placed.size(); ++i) {
            QCOMPARE(placed[i]["player_id"].toLongLong(), table[i].toMap()["player_id"].toLongLong());
            QCOMPARE(placed[i]["final_placement"].toInt(), i + 1);
            QCOMPARE(placed[i]["match_points"].toInt(), table[i].toMap()["points"].toInt());
        }
    }

    void roundFourIsRejectedEveryWay()
    {
        auto [tid, pids] = runEvent(8, 3);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid, 4));     // before finishing
        try {
            db::exec("INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round) "
                     "VALUES (?, 'SWISS', 4, 4)", {tid});
            QFAIL("round four must be refused by the database");
        } catch (const db::Error &e) {
            QVERIFY(e.constraint);
        }
        cdb::finishTournament(tid);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid, 4));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::startEvent(tid));
        QVERIFY2(!cdb::finishTournament(tid), "finishing twice changes nothing");
        QVERIFY(!cdb::finalizeRound(currentRound(tid)["round_id"].toLongLong()));
        for (const char *stage : {"SWISS", "FINAL", "SEMIFINAL"}) {
            try {                                                      // even a direct write is refused
                db::exec("INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round) "
                         "VALUES (?, ?, 4, 1)", {tid, QString(stage)});
                QFAIL("a fourth round must be refused by the database");
            } catch (const db::Error &e) {
                QVERIFY(e.constraint);
            }
        }
        try {                                                          // and the total cannot be raised afterwards
            db::exec("UPDATE commander_events SET swiss_rounds = 4 WHERE tournament_id = ?", {tid});
            QFAIL("the round count is frozen");
        } catch (const db::Error &e) {
            QVERIFY(e.constraint);
        }
        QCOMPARE(cdb::getRounds(tid).size(), 3);
        QCOMPARE(auditActions(tid).count("EVENT_COMPLETED"), 1);
    }

    void finishNeedsEveryResultOfTheLastRound()
    {
        auto [tid, pids] = makeEvent(8, 1);
        try {
            cdb::finishTournament(tid);
            QFAIL("pods still have no result");
        } catch (const cdb::CommanderError &e) {
            QVERIFY(e.message.contains("no result"));
        }
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("SWISS"));
        playRound(tid, false);
        QVERIFY(cdb::finishTournament(tid));
    }

    void completionSurvivesARestart()
    {
        auto [tid, pids] = runEvent(9, 3);
        cdb::finishTournament(tid);
        const QString before = stateJson(tid);
        restart();
        QCOMPARE(stateJson(tid), before);
        const QVariantMap after = cdb::getState(tid);
        QCOMPARE(after["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(after["rounds"].toList().size(), 3);
        QCOMPARE(after["next"].toString(), QString("NONE"));
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));
    }

    void fourAndFiveRoundEventsEndOnTheirLastRound()
    {
        for (int rounds : {4, 5}) {
            auto [tid, pids] = runEvent(8, rounds);
            cdb::finishTournament(tid);
            const QVariantMap state = cdb::getState(tid);
            QCOMPARE(state["rounds"].toList().size(), rounds);
            QCOMPARE(state["event"].toMap()["stage"].toString(), QString("COMPLETE"));
            for (const QVariant &r : state["rounds"].toList())
                QCOMPARE(r.toMap()["stage"].toString(), QString("SWISS"));
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));
        }
    }

    void aDroppedLeaderIsNotPlacedFirst()
    {
        auto [tid, pids] = makeEvent(4, 1);
        const QVariantMap rnd = playRound(tid, false);
        const qint64 winner = seatPlayer(podAt(rnd, 0), 0);
        cdb::dropPlayer(tid, winner);
        cdb::finishTournament(tid);
        const QVariantMap state = cdb::getState(tid);
        QVERIFY(state["event"].toMap()["champion_player_id"].toLongLong() != winner);
        QList<int> places;
        for (const QVariant &r : state["final_standings"].toList())
            places << r.toMap()["final_placement"].toInt();
        QCOMPARE(places, (QList<int>{1, 2, 3, 4}));
    }

    // Events that reached a playoff in the earlier version.

    // Rebuilds what the earlier version left behind: rounds done, Top 4 seeded, a final pod published.
    qint64 legacyEvent(bool finalReported, QList<qint64> *top4Out, qint64 *podOut)
    {
        auto [tid, pids] = makeEvent(8, 2);
        playRound(tid);
        cdb::publishNextRound(tid);
        playRound(tid);
        QList<qint64> top4 = ids(cdb::getStandings(tid)).mid(0, 4);
        for (const char *trigger : {"commander_round_limit", "commander_round_count_frozen", "commander_no_new_playoff"})
            db::exec(QStringLiteral("DROP TRIGGER %1").arg(trigger));
        db::exec("DELETE FROM schema_migrations WHERE version = 3");
        QVariantList seeds;
        for (int i = 0; i < 4; ++i)
            seeds << QVariantMap{{"seed", i + 1}, {"player_id", top4[i]}};
        db::exec("UPDATE commander_events SET stage = 'PLAYOFF', playoff_cut = 4, playoff_seeds_json = ?, "
                 "champion_player_id = NULL, final_standings_json = NULL, completed_at = NULL, legacy_playoff = 0 "
                 "WHERE tournament_id = ?", {QString::fromUtf8(QJsonDocument::fromVariant(seeds).toJson()), tid});
        db::exec("UPDATE tournaments SET status = 'IN_PROGRESS', top_cut = 4 WHERE tournament_id = ?", {tid});
        db::exec("UPDATE enrollments SET final_placement = NULL WHERE tournament_id = ?", {tid});
        const qint64 rid = db::exec("INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round, "
                                    "pairing_algorithm) VALUES (?, 'FINAL', 3, 1, 'playoff-seed-order-1')", {tid}).lastId;
        const qint64 pod = db::exec("INSERT INTO commander_pods (round_id, tournament_id, pod_number) VALUES (?, ?, 1)",
                                    {rid, tid}).lastId;
        for (int seat = 1; seat <= 4; ++seat)
            db::exec("INSERT INTO commander_seats (pod_id, round_id, tournament_id, player_id, seat_number, playoff_seed) "
                     "VALUES (?, ?, ?, ?, ?, ?)", {pod, rid, tid, top4[seat - 1], seat, seat});
        if (finalReported) {
            db::exec("UPDATE commander_seats SET result = CASE seat_number WHEN 3 THEN 'WIN' ELSE 'LOSS' END "
                     "WHERE pod_id = ?", {pod});
            db::exec("UPDATE commander_pods SET status = 'REPORTED', outcome = 'WIN', advancing_player_id = ? "
                     "WHERE pod_id = ?", {top4[2], pod});
            db::exec("UPDATE commander_rounds SET status = 'FINALIZED' WHERE round_id = ?", {rid});
            db::exec("UPDATE commander_events SET stage = 'COMPLETE', champion_player_id = ? WHERE tournament_id = ?",
                     {top4[2], tid});
            db::exec("UPDATE tournaments SET status = 'COMPLETED' WHERE tournament_id = ?", {tid});
            const QList<qint64> order{top4[2], top4[0], top4[1], top4[3]};
            for (int i = 0; i < 4; ++i)
                db::exec("UPDATE enrollments SET final_placement = ? WHERE tournament_id = ? AND player_id = ?",
                         {i + 1, tid, order[i]});
        }
        db::initialize();       // the upgrade runs
        *top4Out = top4;
        *podOut = pod;
        return tid;
    }

    void unfinishedPlayoffIsFlaggedKeptAndClosedOnItsStandings()
    {
        QList<qint64> top4;
        qint64 pod = 0;
        const qint64 tid = legacyEvent(false, &top4, &pod);
        QVariantMap state = cdb::getState(tid);
        const QVariantMap legacy = state["legacy"].toMap();
        QVERIFY(legacy["flagged"].toBool());
        QVERIFY(legacy["needs_finish"].toBool());
        QCOMPARE(legacy["cut"].toInt(), 4);
        QCOMPARE(legacy["playoff_rounds"].toList(), QVariantList{3});
        QCOMPARE(state["next"].toString(), QString("NONE"));
        QVERIFY(auditActions(tid).contains("LEGACY_PLAYOFF_FLAGGED"));
        QVERIFY(cdb::progress(tid)["long"].toString().contains("needs finishing"));
        // nothing can be added to, or played in, the old playoff
        const qint64 finalRound = state["rounds"].toList().last().toMap()["round_id"].toLongLong();
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(pod, "WIN", top4[0]));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(pod, "WIN", top4[0]));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finalizeRound(finalRound));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finishTournament(tid));

        QVERIFY(cdb::finishLegacyPlayoff(tid));
        QVERIFY(!cdb::finishLegacyPlayoff(tid));
        state = cdb::getState(tid);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("COMPLETE"));
        QStringList stages;
        for (const QVariant &r : state["rounds"].toList())
            stages << r.toMap()["stage"].toString();
        QCOMPARE(stages, (QStringList{"SWISS", "SWISS", "FINAL"}));      // the playoff round is kept
        QCOMPARE(ids(state["final_standings"].toList()), ids(cdb::getStandings(tid)));   // placings from the two rounds
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));
    }

    void finishedPlayoffKeepsItsResultUntouched()
    {
        QList<qint64> top4;
        qint64 pod = 0;
        const qint64 tid = legacyEvent(true, &top4, &pod);
        const QList<qint64> placed{top4[2], top4[0], top4[1], top4[3]};
        const QVariantMap state = cdb::getState(tid);
        QVERIFY(state["legacy"].toMap()["flagged"].toBool());
        QVERIFY(!state["legacy"].toMap()["needs_finish"].toBool());
        QCOMPARE(state["event"].toMap()["champion_player_id"].toLongLong(), top4[2]);
        QCOMPARE(ids(state["final_standings"].toList()).mid(0, 4), placed);
        // correcting an earlier round changes the numbers but never the playoff or its placings
        const qint64 r2pod = podId(podAt(state["rounds"].toList()[1].toMap(), 0));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(r2pod, "DRAW", 0, {}, "REBUILD"));
        cdb::correctResult(r2pod, "DRAW", 0, {}, "KEEP");
        const QVariantMap after = cdb::getState(tid);
        QCOMPARE(after["rounds"].toList().size(), 3);
        QCOMPARE(ids(after["final_standings"].toList()).mid(0, 4), placed);
    }

    void playoffThatWasOnlyConfiguredIsSimplyDropped()
    {
        auto [tid, pids] = makeEvent(8, 2);
        db::exec("DROP TRIGGER commander_no_new_playoff");
        db::exec("DELETE FROM schema_migrations WHERE version = 3");
        db::exec("UPDATE commander_events SET playoff_cut = 4 WHERE tournament_id = ?", {tid});
        db::exec("UPDATE tournaments SET top_cut = 4 WHERE tournament_id = ?", {tid});
        db::initialize();
        const QVariantMap ev = cdb::getEvent(tid);
        QCOMPARE(ev["playoff_cut"].toInt(), 0);
        QVERIFY(!ev["legacy_playoff"].toBool());
        QCOMPARE(tdb::tournamentById(tid)["top_cut"].toInt(), 0);
        QVERIFY(auditActions(tid).contains("PLAYOFF_SETTING_REMOVED"));
        playRound(tid);
        cdb::publishNextRound(tid);
        playRound(tid, false);
        cdb::finishTournament(tid);
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(cdb::getRounds(tid).size(), 2);
    }

    void databaseConstraintsBlockDuplicates()
    {
        auto [tid, pids] = makeEvent(8, 3);
        const QVariantMap rnd = currentRound(tid);
        const qint64 rid = rnd["round_id"].toLongLong();
        const qint64 otherPod = podId(podAt(rnd, 1));
        const qint64 seated = seatPlayer(podAt(rnd, 0), 0);
        const QList<QPair<QString, QVariantList>> writes{
            {"INSERT INTO commander_rounds (tournament_id, stage, round_number, stage_round) VALUES (?, 'SWISS', 1, 1)", {tid}},
            {"INSERT INTO commander_seats (pod_id, round_id, tournament_id, player_id, seat_number) VALUES (?, ?, ?, ?, 4)",
             {otherPod, rid, tid, seated}},                                    // same player in two pods of one round
            {"INSERT INTO commander_byes (round_id, tournament_id, player_id) VALUES (?, ?, ?)", {rid, tid, seated}},
            {"UPDATE commander_pods SET status = 'REPORTED' WHERE pod_id = ?", {otherPod}},   // needs an outcome
        };
        for (const auto &w : writes) {
            try {
                db::exec(w.first, w.second);
                QFAIL(qPrintable("should have been refused: " + w.first));
            } catch (const db::Error &e) {
                QVERIFY2(e.constraint, qPrintable(e.message));
            }
        }
    }

    void concurrentStartCreatesOneRound()
    {
        auto [tid, pids] = makeEvent(8, 0, false);
        const qint64 t = tid;
        const Race r = race([t](int) { return cdb::startEvent(t); });
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.conflicts, 7);
        QCOMPARE(cdb::getRounds(tid).size(), 1);
    }

    void concurrentPublishCreatesOneRound()
    {
        auto [tid, pids] = makeEvent(12, 3);
        playRound(tid);
        const qint64 t = tid;
        const Race r = race([t](int) { return cdb::publishNextRound(t, 2); });
        QCOMPARE(r.results.size(), 8);
        QVERIFY2(QSet<qint64>(r.results.begin(), r.results.end()).size() == 1, "every caller gets the same round");
        const QVariantList rounds = cdb::getRounds(tid);
        QCOMPARE(rounds.size(), 2);
        assertValidRound(rounds[1].toMap(), pids);
    }

    void concurrentConflictingResultsKeepOne()
    {
        auto [tid, pids] = makeEvent(4, 1);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        const qint64 pid = podId(pod);
        const Race r = race([pid, seated](int i) { return cdb::reportPodResult(pid, "WIN", seated[i % 4]); });
        const QVariantMap saved = podAt(currentRound(tid), 0);
        QVERIFY2(saved["result_version"].toInt() == 1, "exactly one write landed");
        QCOMPARE(r.results.size(), 2);          // the winning report and its identical repeat
        QCOMPARE(r.conflicts, 6);
        int total = 0;
        for (const QVariant &row : cdb::getStandings(tid))
            total += row.toMap()["points"].toInt();
        QCOMPARE(total, 5);
    }

    void concurrentFinishCompletesOnce()
    {
        auto [tid, pids] = runEvent(8, 3);
        const qint64 t = tid;
        Race r = race([t](int) { return cdb::finishTournament(t) ? 1 : 0; });
        QCOMPARE(r.results.size(), 8);
        QCOMPARE(r.results.count(1), 1);
        r = race([t](int) { return cdb::publishNextRound(t, 4); });
        QVERIFY(r.results.isEmpty());
        QCOMPARE(r.refusals + r.conflicts, 8);
        QCOMPARE(cdb::getRounds(tid).size(), 3);
    }

    void stateSurvivesRestart()
    {
        auto [tid, pids] = makeEvent(10, 3);
        playRound(tid);
        cdb::publishNextRound(tid);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        cdb::reportPodResult(podId(pod), "DRAW", 0, {seatPlayer(pod, 1)});
        const QString before = stateJson(tid);
        restart();
        QCOMPARE(stateJson(tid), before);
        QCOMPARE(db::value("SELECT COUNT(*) FROM schema_migrations").toInt(), int(db::migrations().size()));
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());
    }

    // Modern is untouched.

    void modernEventStillRunsOneOnOne()
    {
        const qint64 tid = tdb::createTournament("Modern Monday", "MTG", 2, "Modern");
        const QList<qint64> pids = addPlayers(5, "M");
        for (qint64 p : pids)
            tdb::enrollPlayer(tid, p);
        QVERIFY(!cdb::isCommander(tid));
        swiss::startTournament(tid);
        const Row rnd = tdb::currentRound(tid);
        const Rows pairings = tdb::roundPairings(rnd["round_id"].toLongLong());
        QCOMPARE(pairings.size(), 3);
        Rows played;
        QList<qint64> everyone;
        qint64 bye = 0;
        for (const Row &p : pairings) {
            everyone << p["player1_id"].toLongLong();
            if (p["player2_id"].isNull()) {
                bye = p["player1_id"].toLongLong();
                QCOMPARE(p["result"].toString(), QString("BYE"));
            } else {
                everyone << p["player2_id"].toLongLong();
                played << p;
                QVERIFY(p["result"].isNull());
            }
        }
        QCOMPARE(played.size(), 2);              // one bye for five players
        std::sort(everyone.begin(), everyone.end());
        QCOMPARE(everyone, pids);
        tdb::reportMatchResult(played[0]["match_id"].toLongLong(), "PLAYER1");
        tdb::reportMatchResult(played[1]["match_id"].toLongLong(), "DRAW");
        swiss::calculateTiebreakers(tid, "MTG");
        QHash<qint64, Row> table;
        for (const Row &p : swiss::standings(tid, "MTG"))
            table.insert(p["player_id"].toLongLong(), p);
        QVERIFY2(table[played[0]["player1_id"].toLongLong()]["match_points"].toInt() == 3, "Modern win is still 3 points");
        QCOMPARE(table[played[0]["player2_id"].toLongLong()]["match_points"].toInt(), 0);
        QCOMPARE(table[played[1]["player1_id"].toLongLong()]["match_points"].toInt(), 1);
        QCOMPARE(table[bye]["match_points"].toInt(), 3);
        QCOMPARE(table[played[0]["player2_id"].toLongLong()]["omw_pct"].toDouble(), 1.0);
        QVERIFY2(table[played[0]["player1_id"].toLongLong()]["omw_pct"].toDouble() == 0.33, "Modern keeps its 33% floor");

        swiss::advanceToNextRound(tid, 1);
        QCOMPARE(tdb::currentRound(tid)["round_number"].toInt(), 2);
        for (const char *table_name : {"commander_events", "commander_rounds", "commander_pods", "commander_seats"})
            QCOMPARE(db::value(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table_name)).toInt(), 0);

        // finishing saves placings and the completed status
        for (const Row &p : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!p["player2_id"].isNull())
                tdb::reportMatchResult(p["match_id"].toLongLong(), "PLAYER2");
        const Rows final = swiss::finalizeTournament(tid, "MTG");
        QCOMPARE(final.size(), 5);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("COMPLETED"));
        QCOMPARE(db::value("SELECT COUNT(*) FROM enrollments WHERE tournament_id = ? AND final_placement IS NOT NULL",
                           {tid}).toInt(), 5);
    }

    void modernRulesAreUnchanged()
    {
        QList<int> got;
        for (int n : {4, 8, 16, 32, 64, 128, 129})
            got << swiss::suggestedRounds(n);
        QCOMPARE(got, (QList<int>{2, 3, 4, 5, 6, 7, 8}));
        const swiss::Points p = swiss::points("MTG");
        QCOMPARE(p.win, 3);
        QCOMPARE(p.draw, 1);
        QCOMPARE(p.loss, 0);
        QCOMPARE(swiss::points("POKEMON").win, 1);
        QCOMPARE(swiss::omwFloor("POKEMON"), 0.25);
    }

    void modernNeverRematchesWhileItCanAvoidIt()
    {
        const qint64 tid = tdb::createTournament("Modern", "MTG", 3, "Modern");
        for (qint64 p : addPlayers(8, "M"))
            tdb::enrollPlayer(tid, p);
        swiss::startTournament(tid);
        QSet<QPair<qint64, qint64>> seen;
        for (int r = 1; r <= 3; ++r) {
            const Row rnd = tdb::currentRound(tid);
            for (const Row &m : tdb::roundPairings(rnd["round_id"].toLongLong())) {
                const qint64 a = m["player1_id"].toLongLong(), b = m["player2_id"].toLongLong();
                const QPair<qint64, qint64> key{qMin(a, b), qMax(a, b)};
                QVERIFY2(!seen.contains(key), "two players met twice");
                seen.insert(key);
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
            }
            if (r < 3)
                swiss::advanceToNextRound(tid, r);
        }
    }

    void commanderAndModernCoexist()
    {
        const qint64 modern = tdb::createTournament("Modern", "MTG", 1, "Modern");
        auto [cmd, pids] = makeEvent(8, 1);
        QList<qint64> found;
        for (const Row &t : tdb::tournamentsByGame("MTG"))
            found << t["tournament_id"].toLongLong();
        std::sort(found.begin(), found.end());
        QCOMPARE(found, (QList<qint64>{modern, cmd}));
        QCOMPARE(tdb::tournamentById(cmd)["format"].toString(), QString("Commander"));
        QCOMPARE(tdb::tournamentById(cmd)["player_count"].toInt(), 8);
        QCOMPARE(tdb::activeTournaments().size(), 1);
    }

    void playersAndHistory()
    {
        const qint64 a = pdb::addPlayer("Ana"), b = pdb::addPlayer("bo");
        QCOMPARE(pdb::allPlayers().size(), 2);
        QCOMPARE(pdb::allPlayers()[0]["display_name"].toString(), QString("Ana"));     // case-insensitive order
        QCOMPARE(pdb::searchPlayers("B").size(), 1);
        pdb::renamePlayer(b, "Bo");
        QCOMPARE(pdb::playerById(b)["display_name"].toString(), QString("Bo"));
        const qint64 tid = tdb::createTournament("T", "POKEMON", 1, "Standard");
        QVERIFY(tdb::enrollPlayer(tid, a) > 0);
        QCOMPARE(tdb::enrollPlayer(tid, a), qint64(0));                                 // already enrolled
        tdb::enrollPlayer(tid, b);
        swiss::startTournament(tid);
        const Row m = tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong())[0];
        tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(tid, "POKEMON");
        const qint64 winner = m["player1_id"].toLongLong();
        QCOMPARE(pdb::matchHistory(winner, tid)[0]["result"].toString(), QString("WIN"));
        QCOMPARE(pdb::tournamentHistory(winner)[0]["final_placement"].toInt(), 1);
        QCOMPARE(pdb::lifetimeStats(winner)[0]["championships"].toInt(), 1);
        pdb::removePlayer(pdb::addPlayer("temp"));
        QCOMPARE(pdb::allPlayers().size(), 2);
    }

    void gameTagsComeFromRoundsActuallyPlayed()
    {
        const QList<qint64> p = addPlayers(6, "G");
        // registered for a tournament that has not started: no rounds, so nothing was played
        const qint64 pending = tdb::createTournament("Pending", "MTG", 2, "Modern");
        tdb::enrollPlayer(pending, p[0]);
        tdb::enrollPlayer(pending, p[1]);
        QVERIFY(pdb::gamesPlayed().isEmpty());

        // Pokémon, three players: two play each other and the third has the bye — all three took part
        const qint64 poke = tdb::createTournament("Poke", "POKEMON", 1, "Standard");
        for (int i : {0, 1, 2})
            tdb::enrollPlayer(poke, p[i]);
        swiss::startTournament(poke);
        QHash<qint64, QStringList> played = pdb::gamesPlayed();
        for (int i : {0, 1, 2})
            QCOMPARE(played.value(p[i]), QStringList{"POKEMON"});

        // Modern and Commander are both Magic: one tag, however many events
        const qint64 modern = tdb::createTournament("Modern", "MTG", 1, "Modern");
        tdb::enrollPlayer(modern, p[0]);
        tdb::enrollPlayer(modern, p[1]);
        swiss::startTournament(modern);
        const qint64 cmd = cdb::createCommanderTournament("Cmd", 4, 1, {}, 3);
        for (int i : {0, 1, 2, 3, 4})
            tdb::enrollPlayer(cmd, p[i]);
        cdb::setCheckedIn(cmd, p[4], false);            // registered but never checked in: not seated
        cdb::startEvent(cmd);
        // a One Piece event that was cancelled does not count, even though a round was paired
        const qint64 op = tdb::createTournament("OP", "ONEPIECE", 1, "Standard");
        tdb::enrollPlayer(op, p[0]);
        tdb::enrollPlayer(op, p[5]);
        swiss::startTournament(op);
        tdb::updateTournamentStatus(op, "CANCELLED");

        played = pdb::gamesPlayed();
        QCOMPARE(played.value(p[0]), (QStringList{"POKEMON", "MTG"}));
        QCOMPARE(played.value(p[2]), (QStringList{"POKEMON", "MTG"}));
        QCOMPARE(played.value(p[3]), QStringList{"MTG"});
        QVERIFY2(!played.contains(p[4]), "registered but not checked in is not participation");
        QVERIFY2(!played.contains(p[5]), "a cancelled event is not participation");

        // the same event, once it runs, does count — and finishing a tournament keeps its tags
        tdb::updateTournamentStatus(op, "IN_PROGRESS");
        for (const Row &m : tdb::roundPairings(tdb::currentRound(poke)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(poke, "POKEMON");
        played = pdb::gamesPlayed();
        QCOMPARE(played.value(p[0]), (QStringList{"ONEPIECE", "POKEMON", "MTG"}));      // tdb::supportedGames() order
        QCOMPARE(played.value(p[5]), QStringList{"ONEPIECE"});
        restart();
        QCOMPARE(pdb::gamesPlayed(), played);           // read from saved history, nothing kept in memory
    }

    void existingDatabaseIsUpgradedInPlace()
    {
        // rewind to the pre-Commander shape: no migration record, no new tables or column
        for (const char *t : {"commander_audit", "commander_advancements", "commander_byes", "commander_seats",
                              "commander_pods", "commander_rounds", "commander_events", "schema_migrations"})
            db::exec(QStringLiteral("DROP TABLE %1").arg(t));
        db::exec("ALTER TABLE enrollments DROP COLUMN checked_in");
        db::exec("INSERT INTO players (display_name) VALUES ('Legacy')");
        db::exec("INSERT INTO tournaments (name, game, format, tournament_date, total_rounds, status) "
                 "VALUES ('Old Modern', 'MTG', 'Modern', '2026-01-01', 4, 'COMPLETED')");
        db::exec("INSERT INTO enrollments (tournament_id, player_id, match_points) VALUES (1, 1, 9)");
        restart();
        const Rows versions = db::query("SELECT version, name FROM schema_migrations ORDER BY version");
        QCOMPARE(versions.size(), 5);
        QCOMPARE(versions[4]["name"].toString(), QString("player_removed"));
        QCOMPARE(versions[2]["name"].toString(), QString("commander_fixed_rounds"));
        QCOMPARE(versions[3]["name"].toString(), QString("tournament_terminated"));
        QSet<QString> triggers;
        for (const Row &r : db::query("SELECT name FROM sqlite_master WHERE type = 'trigger'"))
            triggers.insert(r["name"].toString());
        QVERIFY(triggers.contains("commander_round_limit") && triggers.contains("commander_round_count_frozen"));
        const Row e = db::one("SELECT match_points, checked_in FROM enrollments");
        QCOMPARE(e["match_points"].toInt(), 9);
        QCOMPARE(e["checked_in"].toInt(), 1);
        const Row t = db::one("SELECT name, format, status FROM tournaments");
        QCOMPARE(t["name"].toString(), QString("Old Modern"));
        QCOMPARE(t["status"].toString(), QString("COMPLETED"));
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_events").toInt(), 0);
    }

    void remainingTimeIsComputedFromTimestamps()
    {
        QCOMPARE(timerdb::remaining(3000, timerdb::State::Stopped, 0, false, 0, 500.0), 3000.0);
        QCOMPARE(timerdb::remaining(3000, timerdb::State::Running, 0, true, 100.0, 160.0), 2940.0);
        QCOMPARE(timerdb::remaining(3000, timerdb::State::Running, 600, true, 100.0, 160.0), 2340.0);
        QCOMPARE(timerdb::remaining(3000, timerdb::State::Paused, 600, false, 0, 99999.0), 2400.0);
        QCOMPARE(timerdb::remaining(3000, timerdb::State::Running, 0, true, 100.0, 99999.0), 0.0);       // never below zero
    }

    void clockIsSavedWithTheRoundAndSurvivesARestart()
    {
        using timerdb::Kind;
        using timerdb::State;
        for (const Kind kind : {Kind::OneOnOne, Kind::Commander}) {
            qint64 rid = 0, tid = 0;
            if (kind == Kind::OneOnOne) {
                tid = tdb::createTournament("Modern", "MTG", 2, "Modern", {}, 45);
                for (qint64 p : addPlayers(4, "T"))
                    tdb::enrollPlayer(tid, p);
                swiss::startTournament(tid);
                rid = tdb::currentRound(tid)["round_id"].toLongLong();
            } else {
                tid = cdb::createCommanderTournament("Cmd", 4, 2, {}, 7, {}, 80);
                for (qint64 p : addPlayers(4, "C"))
                    tdb::enrollPlayer(tid, p);
                rid = cdb::startEvent(tid);
            }
            const QString table = kind == Kind::OneOnOne ? "rounds" : "commander_rounds";
            timerdb::Timer t = timerdb::get(kind, rid);
            QVERIFY(t.valid);
            QCOMPARE(t.limit, (kind == Kind::OneOnOne ? 45 : 80) * 60);         // the tournament's configured length
            QVERIFY(t.state == State::Stopped);
            t = timerdb::start(kind, rid);
            QVERIFY(t.state == State::Running);
            // ten minutes pass (and the app restarts)
            db::exec(QStringLiteral("UPDATE %1 SET timer_started_at = timer_started_at - 600 WHERE round_id = ?").arg(table), {rid});
            restart();
            t = timerdb::get(kind, rid);
            QVERIFY(qAbs(timerdb::remaining(t, t.now) - (t.limit - 600)) < 2.0);
            const double startedAt = t.startedAt;
            QVERIFY2(timerdb::start(kind, rid).startedAt == startedAt, "starting a running clock does not reset it");
            t = timerdb::pause(kind, rid);
            QVERIFY(t.state == State::Paused);
            QVERIFY(qAbs(t.elapsed - 600) <= 1);
            QVERIFY(qAbs(timerdb::remaining(t, t.now + 5000) - (t.limit - t.elapsed)) < 0.001);   // paused time stands still
            t = timerdb::start(kind, rid);
            // run past the end: "time expired" changes nothing else
            db::exec(QStringLiteral("UPDATE %1 SET timer_started_at = timer_started_at - 99999 WHERE round_id = ?").arg(table), {rid});
            t = timerdb::get(kind, rid);
            QCOMPARE(timerdb::remaining(t, t.now), 0.0);
            if (kind == Kind::Commander) {
                QCOMPARE(cdb::getState(tid)["rounds"].toList().last().toMap()["status"].toString(), QString("ACTIVE"));
                QCOMPARE(cdb::getRounds(tid).size(), 1);
            } else {
                QCOMPARE(tdb::pendingMatchCount(rid), 2);
                QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("IN_PROGRESS"));
            }
            t = timerdb::reset(kind, rid);
            QVERIFY(t.state == State::Stopped);
            QCOMPARE(t.elapsed, 0);
            // what a screen holds between reads gives the same answer without another query
            const timerdb::Reading reading = timerdb::read(kind, rid);
            QVERIFY(reading.valid());
            QCOMPARE(reading.secondsLeft(), t.limit);
        }
        QCOMPARE(timerdb::clockText(24 * 60 + 18), QString("24:18"));
        QCOMPARE(timerdb::clockText(5), QString("00:05"));
        QVERIFY(!timerdb::read(Kind::Commander, 987654).valid());
        QVERIFY(!timerdb::get(timerdb::Kind::OneOnOne, 987654).valid);
    }

private:
    // Reports PLAYER1 as the winner of every match still open in the current round.
    void reportWholeRound(qint64 tid)
    {
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
    }

    int count(const QString &table, qint64 tid)
    {
        return db::value(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE tournament_id = ?").arg(table), {tid}).toInt();
    }

private slots:
    void oneOnOneEventStopsAtItsConfiguredRoundsHoweverOftenActionsAreRepeated()
    {
        const qint64 tid = tdb::createTournament("Two rounds", "MTG", 2, "Modern");
        for (qint64 p : addPlayers(4, "R"))
            tdb::enrollPlayer(tid, p);
        swiss::startTournament(tid);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::startTournament(tid));       // a second click on Start
        QCOMPARE(count("rounds", tid), 1);
        QCOMPARE(count("matches", tid), 2);

        // results are missing: the round cannot be ended and nothing is half-done
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 1));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::finalizeTournament(tid, "MTG"));
        QCOMPARE(count("rounds", tid), 1);
        QVERIFY(tdb::currentRound(tid)["ended_at"].isNull());
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);

        reportWholeRound(tid);
        const qint64 second = swiss::advanceToNextRound(tid, 1);
        QCOMPARE(tdb::currentRound(tid)["round_id"].toLongLong(), second);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 1));  // a second click on Next round
        QCOMPARE(count("rounds", tid), 2);
        QCOMPARE(count("matches", tid), 4);

        // round 2 of 2 is the last: there is no round 3, with or without results
        reportWholeRound(tid);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 2));
        QCOMPARE(count("rounds", tid), 2);

        const Rows final = swiss::finalizeTournament(tid, "MTG");
        QCOMPARE(final.size(), 4);
        QCOMPARE(final[0]["match_points"].toInt(), 6);
        QCOMPARE(swiss::finalizeTournament(tid, "MTG"), final);                        // finishing twice changes nothing
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 2));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::startTournament(tid));

        restart();
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);
        QCOMPARE(tdb::rounds(tid).size(), 2);
        QCOMPARE(swiss::standings(tid, "MTG"), final);
        QCOMPARE(db::value("SELECT COUNT(DISTINCT final_placement) FROM enrollments WHERE tournament_id = ?", {tid}).toInt(), 4);
    }

    void invalidOneOnOneResultsAreRejectedAndChangeNothing()
    {
        const qint64 tid = tdb::createTournament("Results", "MTG", 2, "Modern");
        for (qint64 p : addPlayers(5, "V"))
            tdb::enrollPlayer(tid, p);
        swiss::startTournament(tid);
        qint64 played = 0, bye = 0;
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            (m["player2_id"].isNull() ? bye : played) = m["match_id"].toLongLong();
        auto winner = [](qint64 match) { return db::value("SELECT winner FROM matches WHERE match_id = ?", {match}).toString(); };

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::reportMatchResult(played, "PLAYER3"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::reportMatchResult(played, "BYE"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::reportMatchResult(played, "player1"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::reportMatchResult(987654, "PLAYER1"));
        QCOMPARE(winner(played), QString("PENDING"));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(bye, "PLAYER2"));    // a bye stays a bye
        QCOMPARE(winner(bye), QString("BYE"));

        // a result can be entered, changed and cleared while its round is the one in play
        tdb::reportMatchResult(played, "PLAYER1");
        tdb::reportMatchResult(played, "DRAW");
        QCOMPARE(winner(played), QString("DRAW"));
        tdb::reportMatchResult(played, "");
        QCOMPARE(winner(played), QString("PENDING"));

        // once the next round is paired from it, it is fixed
        reportWholeRound(tid);
        swiss::advanceToNextRound(tid, 1);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(played, "PLAYER2"));
        QCOMPARE(winner(played), QString("PLAYER1"));
        reportWholeRound(tid);
        swiss::finalizeTournament(tid, "MTG");
        qint64 last = 0;
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                last = m["match_id"].toLongLong();
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(last, "PLAYER2"));
        QCOMPARE(winner(last), QString("PLAYER1"));
    }

    // A save that touches several records either happens completely or not at all.  The
    // failures are forced with triggers that exist only in this disposable database.
    void aFailedSaveLeavesNoPartialRecords()
    {
        // one-on-one: the second table of a round cannot be saved
        db::exec("CREATE TRIGGER test_fail_match BEFORE INSERT ON matches WHEN NEW.table_number = 2 "
                 "BEGIN SELECT RAISE(ABORT, 'forced failure'); END");
        const qint64 tid = tdb::createTournament("Fails", "MTG", 3, "Modern");
        for (qint64 p : addPlayers(4, "F"))
            tdb::enrollPlayer(tid, p);
        QVERIFY_THROWS_EXCEPTION(db::Error, swiss::startTournament(tid));
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::PENDING);         // not half-started
        QCOMPARE(count("rounds", tid), 0);
        QCOMPARE(count("matches", tid), 0);

        db::exec("DROP TRIGGER test_fail_match");
        swiss::startTournament(tid);
        reportWholeRound(tid);
        db::exec("CREATE TRIGGER test_fail_match BEFORE INSERT ON matches WHEN NEW.table_number = 2 "
                 "BEGIN SELECT RAISE(ABORT, 'forced failure'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, swiss::advanceToNextRound(tid, 1));
        QCOMPARE(count("rounds", tid), 1);                                              // no empty round 2
        QCOMPARE(count("matches", tid), 2);
        QVERIFY2(tdb::currentRound(tid)["ended_at"].isNull(), "round 1 is still the open round");
        QCOMPARE(db::value("SELECT COALESCE(SUM(match_points), 0) FROM enrollments WHERE tournament_id = ?", {tid}).toInt(), 0);
        db::exec("DROP TRIGGER test_fail_match");
        swiss::advanceToNextRound(tid, 1);                                              // and the retry works
        QCOMPARE(count("rounds", tid), 2);
        QCOMPARE(db::value("SELECT SUM(match_points) FROM enrollments WHERE tournament_id = ?", {tid}).toInt(), 6);

        // Commander: the third seat of a pod cannot be saved
        db::exec("CREATE TRIGGER test_fail_seat BEFORE INSERT ON commander_seats WHEN NEW.seat_number = 3 "
                 "BEGIN SELECT RAISE(ABORT, 'forced failure'); END");
        const qint64 cmd = makeEvent(8, 2, false).first;
        QVERIFY_THROWS_EXCEPTION(std::exception, cdb::startEvent(cmd));
        for (const char *table : {"commander_rounds", "commander_pods", "commander_seats", "commander_byes"})
            QCOMPARE(count(table, cmd), 0);
        QCOMPARE(tdb::tournamentById(cmd)["status"].toString(), tdb::PENDING);
        db::exec("DROP TRIGGER test_fail_seat");
        cdb::startEvent(cmd);
        QCOMPARE(count("commander_seats", cmd), 8);
    }

    void nestedTransactionsSucceedOrFailAsOne()
    {
        const auto players = [] { return db::value("SELECT COUNT(*) FROM players").toInt(); };
        {
            db::Tx outer;
            pdb::addPlayer("outer");
            {
                db::Tx inner;
                pdb::addPlayer("inner");
                inner.commit();             // joins the outer transaction; nothing is final yet
            }
            // outer is never committed
        }
        QCOMPARE(players(), 0);

        try {
            db::Tx outer;
            pdb::addPlayer("outer");
            {
                db::Tx inner;
                pdb::addPlayer("inner");
                throw std::runtime_error("failure inside the inner step");
            }
        } catch (const std::runtime_error &) {
        }
        QCOMPARE(players(), 0);

        {
            db::Tx outer;
            pdb::addPlayer("outer");
            {
                db::Tx inner;
                pdb::addPlayer("inner");
                inner.commit();
            }
            outer.commit();
        }
        QCOMPARE(players(), 2);
        db::Tx again;                       // and a new transaction can start afterwards
        again.commit();
    }

    // Player ids, names that repeat, and removing a player from the directory

    void playerIdsArePermanentAndNamesMayRepeat()
    {
        const qint64 a = pdb::addPlayer("Alex Smith"), b = pdb::addPlayer("Alex Smith");
        const qint64 jones = pdb::addPlayer("Alex Jones"), jordan = pdb::addPlayer("Jordan Smith");
        const qint64 spaced = pdb::addPlayer("alex   SMITH");
        QVERIFY(a > 0 && b > a && jones > b && jordan > jones && spaced > jordan);     // each new player a new id
        // shown with at least four digits; more when the number needs them
        QCOMPARE(pdb::formatId(1), QString("#0001"));
        QCOMPARE(pdb::formatId(42), QString("#0042"));
        QCOMPARE(pdb::formatId(9999), QString("#9999"));
        QCOMPARE(pdb::formatId(10000), QString("#10000"));
        QCOMPARE(pdb::formatId(123456), QString("#123456"));
        // names are compared whole: case and spacing aside, exactly
        QCOMPARE(pdb::normalizedName("  Alex   SMITH "), pdb::normalizedName("alex smith"));
        QVERIFY(pdb::normalizedName("Alex Smith") != pdb::normalizedName("Alex Smyth"));
        QVERIFY(pdb::normalizedName("Alex Smith") != pdb::normalizedName("AlexSmith"));
        QCOMPARE(pdb::directoryDuplicates(), (QSet<qint64>{a, b, spaced}));     // a shared first or last name alone is not a match
        const QSet<qint64> shared = pdb::directoryDuplicates();
        QCOMPARE(pdb::label("Alex Smith", a, shared), QStringLiteral("Alex Smith · ") + pdb::formatId(a));
        QCOMPARE(pdb::label("alex   SMITH", spaced, shared), QStringLiteral("alex   SMITH · ") + pdb::formatId(spaced));   // spelling kept
        QCOMPARE(pdb::label("Alex Jones", jones, shared), QString("Alex Jones"));
        QCOMPARE(pdb::label("Jordan Smith", jordan, shared), QString("Jordan Smith"));
        QCOMPARE(pdb::playersNamed(" ALEX smith ").size(), 3);
        QVERIFY(pdb::playersNamed("Alex").isEmpty() && pdb::playersNamed("Smith").isEmpty());
        // a search narrows the list, but which names need an id was decided from everybody
        const Rows found = pdb::searchPlayers("SMITH");
        QCOMPARE(found.size(), 4);
        QCOMPARE(pdb::searchPlayers("alex   s").size(), 1);
        QVERIFY(pdb::sharedNameIds(pdb::searchPlayers("alex   s")).isEmpty());   // judged from the results alone it would be missed
        QVERIFY(shared.contains(pdb::searchPlayers("alex   s").first()["player_id"].toLongLong()));

        // renaming changes the name and the labels, never the id
        pdb::renamePlayer(b, "  Alexander Smith ");
        QCOMPARE(pdb::playerById(b)["display_name"].toString(), QString("Alexander Smith"));
        QCOMPARE(pdb::playerById(b)["player_id"].toLongLong(), b);
        QCOMPARE(pdb::directoryDuplicates(), (QSet<qint64>{a, spaced}));
        pdb::renamePlayer(spaced, "Sam Lee");
        QVERIFY(pdb::directoryDuplicates().isEmpty());
        pdb::renamePlayer(b, "Alex Smith");
        QCOMPARE(pdb::directoryDuplicates(), (QSet<qint64>{a, b}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::renamePlayer(b, "   "));

        // a removed player's id is never given to anyone else, also after a restart
        QVERIFY(pdb::removePlayer(spaced));
        const qint64 next = pdb::addPlayer("Sam Lee");
        QVERIFY(next > spaced);
        restart();
        const qint64 later = pdb::addPlayer("Alex Smith");
        QVERIFY(later > next);
        QCOMPARE(pdb::playerById(spaced)["display_name"].toString(), QString("Sam Lee"));     // the removed record is still its own
        QCOMPARE(pdb::playersNamed("sam lee").size(), 1);                 // removed players are not offered as a match
        QCOMPARE(pdb::playersNamed("sam lee").first()["player_id"].toLongLong(), next);
        QCOMPARE(db::value("SELECT COUNT(DISTINCT player_id) FROM players").toInt(), 7);
    }

    void removingAPlayerHidesThemAndKeepsEveryRecord()
    {
        // a finished one-on-one tournament (one match and a bye) and a finished Commander event
        const QList<qint64> p = addPlayers(8, "R");
        const qint64 target = p[0];
        const qint64 swissTid = tdb::createTournament("Old Cup", "POKEMON", 1, "Standard");
        for (int i : {0, 1, 2})
            tdb::enrollPlayer(swissTid, p[i]);
        swiss::startTournament(swissTid);
        const qint64 rid = tdb::currentRound(swissTid)["round_id"].toLongLong();
        qint64 opponent = 0;
        for (const Row &m : tdb::roundPairings(rid)) {
            if (m["player2_id"].isNull())
                continue;
            tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
            if (m["player1_id"].toLongLong() == target || m["player2_id"].toLongLong() == target)
                opponent = m["player1_id"].toLongLong() == target ? m["player2_id"].toLongLong() : m["player1_id"].toLongLong();
        }
        if (!opponent) {        // the target drew the bye: use the tournament again with the pairing known
            opponent = p[1];
        }
        swiss::finalizeTournament(swissTid, "POKEMON");
        const qint64 cmd = cdb::createCommanderTournament("Old Commander", 8, 1, {}, 77);
        for (qint64 id : p)
            tdb::enrollPlayer(cmd, id);
        cdb::startEvent(cmd);
        playRound(cmd);
        QCOMPARE(tdb::tournamentById(cmd)["status"].toString(), tdb::COMPLETED);

        const QStringList tables{"tournaments", "enrollments", "rounds", "matches", "commander_events", "commander_rounds",
                                 "commander_pods", "commander_seats", "commander_byes", "commander_audit"};
        const auto everything = [&tables] {
            QList<Rows> out;
            for (const QString &t : tables)
                out << db::query(QStringLiteral("SELECT * FROM %1 ORDER BY 1, 2").arg(t));
            out << db::query("SELECT player_id, display_name, date_joined FROM players ORDER BY player_id");
            return out;
        };
        const QList<Rows> before = everything();
        const Rows standings = swiss::viewStandings(swissTid, "POKEMON");
        const QVariantList cmdStandings = cdb::getStandings(cmd), cmdRounds = cdb::getRounds(cmd);
        const Rows opponentMatches = pdb::matchHistory(opponent, swissTid), opponentStats = pdb::lifetimeStats(opponent);
        const tdb::GameStats stats = tdb::gameStats("POKEMON");
        const QHash<qint64, QStringList> played = pdb::gamesPlayed();
        QVERIFY(!played.value(target).isEmpty());

        QVERIFY(pdb::removePlayer(target));
        QVERIFY(!pdb::removePlayer(target));                    // a second request changes nothing
        // gone from the directory, searches and the list of namesakes
        QCOMPARE(pdb::allPlayers().size(), 7);
        for (const Row &r : pdb::allPlayers())
            QVERIFY(r["player_id"].toLongLong() != target);
        QVERIFY(pdb::searchPlayers("R001").isEmpty());
        QVERIFY(pdb::playersNamed("R001").isEmpty());
        QVERIFY(pdb::isRemoved(target));
        // still their own record: same id, same name, marked with when it was removed
        const Row kept = pdb::playerById(target);
        QCOMPARE(kept["display_name"].toString(), QString("R001"));
        QVERIFY(!kept["deleted_at"].toString().isEmpty());

        // nothing else changed: no match, registration, seat, bye, point, tiebreaker or placing
        QCOMPARE(everything(), before);
        Rows now = swiss::viewStandings(swissTid, "POKEMON");
        for (Row &r : now)
            r["removed"] = 0;
        Rows then = standings;
        for (Row &r : then)
            r["removed"] = 0;
        QCOMPARE(now, then);                                    // the same table, name included
        QCOMPARE(cdb::getStandings(cmd), cmdStandings);
        QCOMPARE(cdb::getRounds(cmd), cmdRounds);               // pods and seats, names included
        QCOMPARE(pdb::matchHistory(opponent, swissTid), opponentMatches);
        QCOMPARE(pdb::lifetimeStats(opponent), opponentStats);
        QCOMPARE(tdb::gameStats("POKEMON").gamesPlayed, stats.gamesPlayed);
        QCOMPARE(tdb::gameStats("POKEMON").players, stats.players);
        QCOMPARE(pdb::gamesPlayed(), played);                   // history still counts them
        // the screens can tell that this participant no longer has a profile
        bool flagged = false;
        for (const Row &r : swiss::viewStandings(swissTid, "POKEMON"))
            if (r["player_id"].toLongLong() == target)
                flagged = r["removed"].toInt() == 1 && r["display_name"].toString() == "R001";
        QVERIFY(flagged);

        // they cannot be enrolled or renamed any more
        const qint64 fresh = tdb::createTournament("New Cup", "POKEMON", 1);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::enrollPlayer(fresh, target));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::renamePlayer(target, "Back Again"));
        QCOMPARE(count("enrollments", fresh), 0);
        // a new player with the same name is somebody else: a new id and no history
        const qint64 namesake = pdb::addPlayer("R001");
        QVERIFY(namesake > p.last());
        QVERIFY(pdb::tournamentHistory(namesake).isEmpty() && pdb::lifetimeStats(namesake).isEmpty());
        QVERIFY(!pdb::gamesPlayed().contains(namesake));
        QCOMPARE(pdb::playersNamed("R001").size(), 1);
        QVERIFY(pdb::directoryDuplicates().isEmpty());          // the removed one is not in the directory to clash with
        QVERIFY(tdb::enrollPlayer(fresh, namesake) > 0);
        QCOMPARE(tdb::enrollPlayer(fresh, namesake), qint64(0));        // and never twice in one tournament
        QCOMPARE(everything().mid(2, 8), before.mid(2, 8));     // the old tournaments' records are as they were

        restart();
        QVERIFY(pdb::isRemoved(target));
        QCOMPARE(pdb::allPlayers().size(), 8);
        QCOMPARE(cdb::getStandings(cmd), cmdStandings);
        QCOMPARE(pdb::matchHistory(opponent, swissTid), opponentMatches);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::removePlayer(987654));
    }

    void aPlayerStillInATournamentCannotBeRemoved()
    {
        const QList<qint64> p = addPlayers(10, "A");
        const auto refused = [](qint64 player) -> QString {
            try {
                pdb::removePlayer(player);
            } catch (const pdb::StillPlaying &e) {
                return e.reasons.join(" | ");
            }
            return QString();
        };
        // registered for a tournament that has not started
        const qint64 pending = tdb::createTournament("Sign-up Sunday", "ONEPIECE", 2);
        tdb::enrollPlayer(pending, p[0]);
        QVERIFY2(refused(p[0]).contains("Sign-up Sunday") && refused(p[0]).contains("remove them from its player list"),
                 qPrintable(refused(p[0])));
        QVERIFY(!pdb::isRemoved(p[0]));
        QCOMPARE(count("enrollments", pending), 1);             // asking changed nothing
        tdb::unenrollPlayer(pending, p[0]);
        QVERIFY(pdb::removePlayer(p[0]));

        // in a one-on-one tournament being played: first a result is outstanding, then the event itself
        const qint64 running = tdb::createTournament("Modern Monday", "MTG", 2, "Modern");
        tdb::enrollPlayer(running, p[1]);
        tdb::enrollPlayer(running, p[2]);
        swiss::startTournament(running);
        QVERIFY2(refused(p[1]).contains("Modern Monday") && refused(p[1]).contains("not reported yet"), qPrintable(refused(p[1])));
        const Row match = tdb::roundPairings(tdb::currentRound(running)["round_id"].toLongLong()).first();
        QCOMPARE(match["result"], QVariant());                  // no result was invented
        tdb::reportMatchResult(match["match_id"].toLongLong(), "PLAYER1");
        QVERIFY2(refused(p[1]).contains("finish that tournament, or end it early"), qPrintable(refused(p[1])));
        QVERIFY(!pdb::isRemoved(p[1]));
        QCOMPARE(count("enrollments", running), 2);
        tdb::terminateTournament(running);
        QVERIFY(pdb::removePlayer(p[1]));                       // once it is over they can go; their match stays
        QCOMPARE(count("matches", running), 1);

        // in a Commander event: the round in play, then being due for the next one, then dropped
        const qint64 cmd = cdb::createCommanderTournament("Commander Night", 8, 2, {}, 5);
        for (int i = 2; i < 10; ++i)
            tdb::enrollPlayer(cmd, p[i]);
        cdb::startEvent(cmd);
        QVERIFY2(refused(p[3]).contains("Commander Night") && refused(p[3]).contains("not finalized yet"), qPrintable(refused(p[3])));
        playRound(cmd);
        QVERIFY2(refused(p[3]).contains("drop them there first"), qPrintable(refused(p[3])));
        cdb::dropPlayer(cmd, p[3]);
        QVERIFY(pdb::removePlayer(p[3]));
        cdb::publishNextRound(cmd, 2);
        for (const QVariant &pod : currentRound(cmd)["pods"].toList())
            QVERIFY(!ids(pod.toMap()["seats"].toList()).contains(p[3]));       // not paired again
        QVERIFY(ids(cdb::getRounds(cmd).first().toMap()["pods"].toList()[0].toMap()["seats"].toList()
                    + cdb::getRounds(cmd).first().toMap()["pods"].toList()[1].toMap()["seats"].toList()).contains(p[3]));
        // several reasons are all given
        const qint64 both = p[4];
        tdb::enrollPlayer(pending, both);
        const QString reasons = refused(both);
        QVERIFY2(reasons.contains("Sign-up Sunday") && reasons.contains("Commander Night"), qPrintable(reasons));
    }

    void playersWithTheSameNameAreToldApartByIdWithinATournament()
    {
        const qint64 a = pdb::addPlayer("Alex Smith"), b = pdb::addPlayer("alex smith"), sam = pdb::addPlayer("Sam Lee");
        const qint64 outside = pdb::addPlayer("Sam Lee");       // a namesake who is not in this tournament
        const QString la = QStringLiteral("Alex Smith · ") + pdb::formatId(a), lb = QStringLiteral("alex smith · ") + pdb::formatId(b);
        const qint64 tid = tdb::createTournament("Twins Cup", "MTG", 1, "Modern");
        for (qint64 id : {a, b, sam})
            tdb::enrollPlayer(tid, id);
        QCOMPARE(pdb::tournamentDuplicates(tid), (QSet<qint64>{a, b}));
        QHash<qint64, QString> shown;
        for (const Row &r : tdb::enrolledPlayers(tid))
            shown.insert(r["player_id"].toLongLong(), r["display_name"].toString());
        QCOMPARE(shown.value(a), la);
        QCOMPARE(shown.value(b), lb);
        QCOMPARE(shown.value(sam), QString("Sam Lee"));         // his namesake is not a participant here
        swiss::startTournament(tid);
        QStringList names;
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong())) {
            names << m["player1_name"].toString();
            if (!m["player2_name"].isNull()) {
                names << m["player2_name"].toString();
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
            }
        }
        names.sort();
        QCOMPARE(names, (QStringList{la, "Sam Lee", lb}));      // on the pairings, and so on the printed sheet
        swiss::finalizeTournament(tid, "MTG");
        names.clear();
        for (const Row &r : swiss::viewStandings(tid, "MTG"))
            names << r["display_name"].toString();
        names.sort();
        QCOMPARE(names, (QStringList{la, "Sam Lee", lb}));
        for (qint64 id : {a, b, sam})
            for (const Row &m : pdb::matchHistory(id, tid))
                if (!m["opponent_name"].isNull())
                    QVERIFY2(m["opponent_name"].toString() == la || m["opponent_name"].toString() == lb
                             || m["opponent_name"].toString() == "Sam Lee", qPrintable(m["opponent_name"].toString()));
        // removing one of them from the directory does not hide the id history needs
        QVERIFY(pdb::removePlayer(b));
        QCOMPARE(pdb::tournamentDuplicates(tid), (QSet<qint64>{a, b}));
        QVERIFY(!pdb::directoryDuplicates().contains(a));       // while in the directory the remaining one is unambiguous
        names.clear();
        for (const Row &r : swiss::viewStandings(tid, "MTG"))
            names << r["display_name"].toString();
        QVERIFY(names.contains(la) && names.contains(lb));

        // a shared first name or last name alone never brings out an id
        const qint64 other = tdb::createTournament("Cousins Cup", "MTG", 1, "Modern");
        for (qint64 id : {a, pdb::addPlayer("Alex Jones"), pdb::addPlayer("Jordan Smith")})
            tdb::enrollPlayer(other, id);
        QVERIFY(pdb::tournamentDuplicates(other).isEmpty());
        for (const Row &r : tdb::enrolledPlayers(other))
            QVERIFY(!r["display_name"].toString().contains("#"));

        // Commander: seats and standings carry the id; what is saved keeps the plain name
        const qint64 c1 = pdb::addPlayer("Robin Fox"), c2 = pdb::addPlayer("ROBIN  FOX");
        const qint64 cmd = cdb::createCommanderTournament("Twin Pods", 4, 1, {}, 9);
        for (qint64 id : {c1, c2, sam, outside})
            tdb::enrollPlayer(cmd, id);
        cdb::startEvent(cmd);
        playRound(cmd);
        QStringList seats, table;
        for (const QVariant &s : cdb::getRounds(cmd).last().toMap()["pods"].toList()[0].toMap()["seats"].toList())
            seats << s.toMap()["display_name"].toString();
        for (const QVariant &r : cdb::getStandings(cmd))
            table << r.toMap()["display_name"].toString();
        for (const QStringList &list : {seats, table}) {
            QVERIFY(list.contains(QStringLiteral("Robin Fox · ") + pdb::formatId(c1)));
            QVERIFY(list.contains(QStringLiteral("ROBIN  FOX · ") + pdb::formatId(c2)));
            QVERIFY(list.contains(QStringLiteral("Sam Lee · ") + pdb::formatId(sam)));          // both Sam Lees play here
            QVERIFY(list.contains(QStringLiteral("Sam Lee · ") + pdb::formatId(outside)));
        }
        QVERIFY(!cdb::getEvent(cmd)["final_standings_json"].toString().contains("#"));
    }

    void lookingAtStandingsNeverChangesWhatIsSaved()
    {
        const QList<qint64> p = addPlayers(4, "V");
        const qint64 tid = tdb::createTournament("View Cup", "POKEMON", 1, "Standard");
        for (qint64 id : p)
            tdb::enrollPlayer(tid, id);
        swiss::startTournament(tid);
        const Rows pairings = tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong());
        const auto saved = [tid] { return db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid}); };

        // while it is played: the table follows the results, but showing it writes nothing
        tdb::reportMatchResult(pairings[0]["match_id"].toLongLong(), "PLAYER1");
        const Rows untouched = saved();
        const Rows live = swiss::viewStandings(tid, "POKEMON");
        QCOMPARE(live.first()["player_id"], pairings[0]["player1_id"]);
        QCOMPARE(live.first()["match_points"].toInt(), swiss::points("POKEMON").win);
        QCOMPARE(live.first()["standing"].toInt(), 1);
        QCOMPARE(saved(), untouched);
        swiss::viewStandings(tid, "POKEMON");
        QCOMPARE(saved(), untouched);

        // once finished: the saved figures and placings are the result, however often they are shown
        tdb::reportMatchResult(pairings[1]["match_id"].toLongLong(), "PLAYER2");
        const Rows final = swiss::finalizeTournament(tid, "POKEMON");
        const Rows finished = saved();
        const Rows shown = swiss::viewStandings(tid, "POKEMON");
        QCOMPARE(shown.size(), 4);
        for (int i = 0; i < shown.size(); ++i) {
            QCOMPARE(shown[i]["player_id"], final[i]["player_id"]);
            QCOMPARE(shown[i]["standing"].toInt(), i + 1);
            QCOMPARE(shown[i]["final_placement"].toInt(), i + 1);
            QCOMPARE(shown[i]["match_points"], final[i]["match_points"]);
        }
        QCOMPARE(saved(), finished);
        // even if a match record is missing (as the old way of deleting a player left things),
        // a finished tournament's table is not worked out again from what remains
        db::exec("DELETE FROM matches WHERE match_id = ?", {pairings[0]["match_id"]});
        const Rows after = swiss::viewStandings(tid, "POKEMON");
        for (int i = 0; i < after.size(); ++i) {
            QCOMPARE(after[i]["player_id"], shown[i]["player_id"]);
            QCOMPARE(after[i]["match_points"], shown[i]["match_points"]);
            QCOMPARE(after[i]["standing"], shown[i]["standing"]);
        }
        QCOMPARE(saved(), finished);
    }

    void tournamentDetailsAreValidatedBeforeAnythingIsSaved()
    {
        QVERIFY(!tdb::isValidRoundMinutes(0));
        QVERIFY(tdb::isValidRoundMinutes(tdb::MIN_ROUND_MINUTES));
        QVERIFY(tdb::isValidRoundMinutes(tdb::MAX_ROUND_MINUTES));
        QVERIFY(!tdb::isValidRoundMinutes(tdb::MAX_ROUND_MINUTES + 1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("T", "MTG", 3, "Modern", {}, 0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("T", "MTG", 3, "Modern", {}, 241));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("T", "MTG", 0, "Modern"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("   ", "MTG", 3, "Modern"));
        QCOMPARE(db::value("SELECT COUNT(*) FROM tournaments").toInt(), 0);
        QVERIFY(tdb::createTournament("T", "MTG", 1, "Modern", {}, 240) > 0);
    }

    void roundStatusIsTheSameAnswerForEveryScreen()
    {
        const qint64 modern = tdb::createTournament("Modern", "MTG", 3, "Modern");
        for (qint64 p : addPlayers(4, "S"))
            tdb::enrollPlayer(modern, p);
        live::RoundStatus s = live::roundStatus(modern);
        QVERIFY(!s.playing);
        QCOMPARE(s.totalRounds, 3);
        swiss::startTournament(modern);
        s = live::roundStatus(modern);
        QVERIFY(s.playing && s.kind == timerdb::Kind::OneOnOne);
        QCOMPARE(s.roundNumber, 1);
        QCOMPARE(s.roundId, tdb::currentRound(modern)["round_id"].toLongLong());

        const qint64 cmd = makeEvent(4, 2).first;
        s = live::roundStatus(cmd);
        QVERIFY(s.playing && s.kind == timerdb::Kind::Commander && !s.needsFinish);
        QCOMPARE(s.roundNumber, 1);
        QCOMPARE(s.totalRounds, 2);
        const QVariantMap pod = podAt(cdb::getRounds(cmd).last().toMap(), 0);
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));
        cdb::finalizeRound(s.roundId);
        s = live::roundStatus(cmd);
        QVERIFY2(!s.playing, "between rounds no clock applies");
        QCOMPARE(s.roundNumber, 1);
    }

    void hubStatisticsCountPlayedGamesOfEachKind()
    {
        const qint64 modern = tdb::createTournament("Modern", "MTG", 1, "Modern");
        const QList<qint64> p = addPlayers(4, "H");
        for (qint64 id : p)
            tdb::enrollPlayer(modern, id);
        swiss::startTournament(modern);
        QCOMPARE(tdb::gameStats("MTG").gamesPlayed, 0);          // paired but not reported
        reportWholeRound(modern);
        swiss::finalizeTournament(modern, "MTG");
        const qint64 cmd = cdb::createCommanderTournament("Cmd", 4, 1, {}, 3);
        for (qint64 id : p)
            tdb::enrollPlayer(cmd, id);
        cdb::startEvent(cmd);
        const QVariantMap pod = podAt(cdb::getRounds(cmd).last().toMap(), 0);
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));

        const tdb::GameStats stats = tdb::gameStats("MTG");
        QCOMPARE(stats.players, 4);                              // the same four people in both events
        QCOMPARE(stats.gamesPlayed, 3);                          // two matches and one pod
        QVERIFY(!stats.topPlayers.isEmpty());
        QCOMPARE(tdb::gameStats("POKEMON").players, 0);
    }

    void eachGameListsItsTiebreakersInTheOrderStandingsUseThem()
    {
        QCOMPARE(swiss::tiebreakColumns("MTG").size(), 3);
        QCOMPARE(swiss::tiebreakColumns("ONEPIECE").size(), 2);
        QCOMPARE(swiss::tiebreakColumns("POKEMON")[1].first, QString("ogw_pct"));
        for (const char *game : {"MTG", "ONEPIECE", "POKEMON"})
            QCOMPARE(swiss::tiebreakColumns(game)[0].first, QString("omw_pct"));
    }

    void preferencesPersistBesideTheDatabase()
    {
        QJsonObject p = prefs::load();
        QCOMPARE(p["theme"].toString(), QString("system"));
        QCOMPARE(p["text_size"].toString(), QString("standard"));
        QCOMPARE(p["alert_sound"].toBool(), false);
        QCOMPARE(p["alert_notify"].toBool(), true);
        prefs::setPref("theme", "dark");
        prefs::setPref("text_size", "large");
        prefs::setPref("alert_sound", true);
        prefs::setPref("MTG", 65);
        QVERIFY(prefs::filePath().startsWith(db::dataDir()));
        p = prefs::load();                                      // read back from the file
        QCOMPARE(p["theme"].toString(), QString("dark"));
        QCOMPARE(p["text_size"].toString(), QString("large"));
        QCOMPARE(p["alert_sound"].toBool(), true);
        QCOMPARE(prefs::defaultRoundMinutes("MTG"), 65);
        prefs::setPref("theme", "neon");                       // unknown values fall back
        QCOMPARE(prefs::load()["theme"].toString(), QString("system"));
        p = prefs::restoreDefaults();
        QCOMPARE(p["text_size"].toString(), QString("standard"));
        QCOMPARE(p["alert_sound"].toBool(), false);
        QVERIFY2(prefs::defaultRoundMinutes("MTG") == 65, "restoring interface defaults leaves round lengths alone");
        QCOMPARE(prefs::issuesUrl(), QString("https://github.com/nchiamsachang/tcg-tournament-manager/issues"));
        QCOMPARE(QString(prefs::PRODUCER), QString("Nathan Chiamsachang"));
    }

private:
    // A running one-on-one tournament: 5 players, round 1 paired, one result in, the clock started.
    qint64 runningSwiss(QList<qint64> *pids = nullptr)
    {
        const qint64 tid = tdb::createTournament("Cut Short Cup", "POKEMON", 3, "Standard", {}, 50);
        const QList<qint64> players = addPlayers(5, "S");
        for (qint64 p : players)
            tdb::enrollPlayer(tid, p);
        swiss::startTournament(tid);
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        for (const Row &m : tdb::roundPairings(rid)) {
            if (!m["player2_id"].isNull()) {
                tdb::reportMatchResult(m["match_id"].toLongLong(), "PLAYER1");
                break;
            }
        }
        timerdb::start(timerdb::Kind::OneOnOne, rid);
        if (pids)
            *pids = players;
        return tid;
    }

private slots:
    // Ending a tournament early

    void endingEarlyKeepsEveryRecordAndMarksTheTournamentTerminated()
    {
        QList<qint64> pids;
        const qint64 tid = runningSwiss(&pids);
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        const Rows matches = tdb::allMatches(tid);
        const Rows enrolled = db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid});
        QCOMPARE(tdb::pendingMatchCount(rid), 1);
        QCOMPARE(tdb::activeTournaments().size(), 1);

        QVERIFY(tdb::terminateTournament(tid));
        const Row t = tdb::tournamentById(tid);
        QCOMPARE(t["status"].toString(), tdb::TERMINATED);
        QVERIFY(!t["terminated_at"].toString().isEmpty());
        QVERIFY(tdb::isTerminated(tid));
        QVERIFY(tdb::activeTournaments().isEmpty());                    // gone from the active list
        QCOMPARE(tdb::tournamentsByGame("POKEMON").size(), 1);          // still on record
        // nothing was invented or removed: same matches, the unreported one still unreported, no placings
        QCOMPARE(tdb::allMatches(tid), matches);
        QCOMPARE(db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid}), enrolled);
        QCOMPARE(tdb::pendingMatchCount(rid), 1);
        QCOMPARE(db::value("SELECT COUNT(*) FROM enrollments WHERE tournament_id = ? AND final_placement IS NOT NULL", {tid}).toInt(), 0);
        QVERIFY(pdb::lifetimeStats(pids[0]).isEmpty());                 // not counted as a completed tournament
        QCOMPARE(pdb::tournamentHistory(pids[0]).first()["status"].toString(), tdb::TERMINATED);
        QCOMPARE(pdb::gamesPlayed().value(pids[0]), QStringList{"POKEMON"});   // they did play

        // a second request changes nothing, and so does a restart
        const QString when = t["terminated_at"].toString();
        QVERIFY(!tdb::terminateTournament(tid));
        restart();
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);
        QCOMPARE(tdb::tournamentById(tid)["terminated_at"].toString(), when);
        QCOMPARE(tdb::allMatches(tid), matches);
    }

    void aTerminatedTournamentRefusesFurtherChanges()
    {
        const qint64 tid = runningSwiss();
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        qint64 open = 0, reported = 0;
        for (const Row &m : tdb::roundPairings(rid)) {
            if (m["player2_id"].isNull())
                continue;
            (m["result"].isNull() ? open : reported) = m["match_id"].toLongLong();
        }
        QVERIFY(open && reported);
        tdb::terminateTournament(tid);
        const Rows matches = tdb::allMatches(tid);
        const qint64 late = pdb::addPlayer("Latecomer");

        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(open, "PLAYER2"));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(reported, {}));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::enrollPlayer(tid, late));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::unenrollPlayer(tid, late));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 1));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::generatePairings(tid, 2));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::finalizeTournament(tid, "POKEMON"));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::startTournament(tid));
        QCOMPARE(tdb::allMatches(tid), matches);
        QCOMPARE(tdb::rounds(tid).size(), 1);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);

        // only a running or not-yet-started tournament can be ended early
        const qint64 done = tdb::createTournament("Finished Cup", "POKEMON", 1);
        for (qint64 p : addPlayers(2, "F"))
            tdb::enrollPlayer(done, p);
        swiss::startTournament(done);
        tdb::reportMatchResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(done, "POKEMON");
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::terminateTournament(done));
        QCOMPARE(tdb::tournamentById(done)["status"].toString(), tdb::COMPLETED);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::terminateTournament(987654));
    }

    void endingEarlyStopsTheClockForGood()
    {
        for (timerdb::Kind kind : {timerdb::Kind::OneOnOne, timerdb::Kind::Commander}) {
            qint64 tid = 0, rid = 0;
            if (kind == timerdb::Kind::OneOnOne) {
                tid = runningSwiss();
                rid = tdb::currentRound(tid)["round_id"].toLongLong();
            } else {
                tid = makeEvent(8, 2).first;
                rid = currentRound(tid)["round_id"].toLongLong();
                timerdb::start(kind, rid);
            }
            QVERIFY(timerdb::get(kind, rid).state == timerdb::State::Running);
            QVERIFY(!timerdb::get(kind, rid).closed);
            QVERIFY(live::roundStatus(tid).playing);

            tdb::terminateTournament(tid);
            timerdb::Timer t = timerdb::get(kind, rid);
            QVERIFY(t.valid && t.closed);
            QVERIFY(t.state == timerdb::State::Stopped && !t.hasStart);
            const double left = timerdb::remaining(t, t.now);
            QVERIFY(left > 0 && left <= t.limit);
            QCOMPARE(timerdb::remaining(t, t.now + 3600), left);        // an hour later: not a second less
            // Start, Pause and Reset no longer do anything
            QVERIFY(timerdb::start(kind, rid).state == timerdb::State::Stopped);
            timerdb::pause(kind, rid);
            timerdb::reset(kind, rid);
            t = timerdb::get(kind, rid);
            QVERIFY(t.closed && t.state == timerdb::State::Stopped);
            QCOMPARE(timerdb::remaining(t, t.now), left);
            // nothing is "being played" any more, so no pill, row or alert follows this clock
            const live::RoundStatus status = live::roundStatus(tid);
            QVERIFY(status.terminated && !status.playing);
            QCOMPARE(status.roundId, rid);
        }
        // finishing a tournament the normal way closes its clock too
        const qint64 done = tdb::createTournament("Finished Cup", "POKEMON", 1);
        for (qint64 p : addPlayers(2, "F"))
            tdb::enrollPlayer(done, p);
        swiss::startTournament(done);
        const qint64 rid = tdb::currentRound(done)["round_id"].toLongLong();
        timerdb::start(timerdb::Kind::OneOnOne, rid);
        tdb::reportMatchResult(tdb::roundPairings(rid).first()["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(done, "POKEMON");
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, rid).closed);
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, rid).state == timerdb::State::Stopped);
    }

    void endingACommanderEventEarlyLeavesItUnfinishedAndReadOnly()
    {
        auto [tid, pids] = makeEvent(8, 3);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap pod0 = podAt(rnd, 0), pod1 = podAt(rnd, 1);
        cdb::reportPodResult(podId(pod0), "WIN", seatPlayer(pod0, 0));
        const QVariantList standings = cdb::getStandings(tid);

        QVERIFY(tdb::terminateTournament(tid));
        QVariantMap state = cdb::getState(tid);
        QVERIFY(state["terminated"].toBool());
        QCOMPARE(state["next"].toString(), QString("NONE"));
        const QVariantMap ev = state["event"].toMap();
        QCOMPARE(ev["stage"].toString(), QString("SWISS"));             // not completed
        QVERIFY(ev["champion_player_id"].isNull());                     // nobody is declared the winner
        QVERIFY(ev["final_standings_json"].isNull());
        QVERIFY(state["final_standings"].isNull());
        QCOMPARE(state["rounds"].toList().size(), 1);
        QCOMPARE(state["rounds"].toList()[0].toMap()["pending"].toInt(), 1);     // the unreported pod stays unreported
        QCOMPARE(cdb::getStandings(tid), standings);
        QVERIFY(auditActions(tid).contains("EVENT_TERMINATED"));
        QVERIFY(!cdb::progress(tid)["active"].toBool());
        QVERIFY(cdb::progress(tid)["long"].toString().startsWith("Ended early"));

        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(podId(pod1), "WIN", seatPlayer(pod1, 0)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::clearPodResult(podId(pod0)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(podId(pod0), "WIN", seatPlayer(pod0, 1)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finalizeRound(rnd["round_id"].toLongLong()));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finishTournament(tid));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid, 2));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::dropPlayer(tid, pids[0]));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::endSwissEarly(tid));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::setCheckedIn(tid, pids[0], false));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::updateEventConfig(tid, 5));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::enrollPlayer(tid, pdb::addPlayer("Latecomer")));
        restart();
        state = cdb::getState(tid);
        QVERIFY(state["terminated"].toBool());
        QCOMPARE(state["rounds"].toList().size(), 1);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("SWISS"));
        QCOMPARE(cdb::getStandings(tid), standings);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);
    }

    void repeatedAndSimultaneousRequestsEndATournamentOnce()
    {
        const qint64 tid = runningSwiss();
        const Race r = race([tid](int) { return tdb::terminateTournament(tid) ? 1 : 0; });
        QCOMPARE(r.other + r.refusals + r.conflicts, 0);
        QCOMPARE(r.results.count(1), 1);                // exactly one of them did it
        QCOMPARE(r.results.count(0), 7);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);
    }

    void upgradeAddsTheTerminatedStatusWithoutTouchingAnyRecord()
    {
        // tournaments of both kinds, with rounds, results and a clock
        const qint64 swissTid = runningSwiss();
        const qint64 cmdTid = makeEvent(8, 2).first;
        playRound(cmdTid);
        const qint64 gone = tdb::createTournament("Deleted Later", "MTG", 1);       // the newest id, then deleted
        db::exec("DELETE FROM tournaments WHERE tournament_id = ?", {gone});

        // put the tournaments table back the way the previous version made it
        db::exec("PRAGMA foreign_keys = OFF");
        db::exec(R"sql(
            CREATE TABLE tournaments_old (
                tournament_id   INTEGER PRIMARY KEY AUTOINCREMENT,
                name            TEXT    NOT NULL,
                game            TEXT    NOT NULL CHECK (game IN ('MTG', 'ONEPIECE', 'POKEMON')),
                format          TEXT,
                tournament_date TEXT    NOT NULL,
                location        TEXT,
                total_rounds    INTEGER NOT NULL,
                top_cut         INTEGER DEFAULT 0,
                round_time_mins INTEGER DEFAULT 50,
                status          TEXT    DEFAULT 'PENDING'
                                CHECK (status IN ('PENDING', 'IN_PROGRESS', 'COMPLETED', 'CANCELLED')),
                notes           TEXT
            )
        )sql");
        db::exec("INSERT INTO tournaments_old SELECT tournament_id, name, game, format, tournament_date, location, "
                 "total_rounds, top_cut, round_time_mins, status, notes FROM tournaments");
        db::exec("DROP TABLE tournaments");
        db::exec("ALTER TABLE tournaments_old RENAME TO tournaments");
        db::exec("CREATE INDEX idx_tournaments_game ON tournaments(game)");
        db::exec("CREATE INDEX idx_tournaments_date ON tournaments(tournament_date)");
        db::exec("UPDATE sqlite_sequence SET seq = ? WHERE name = 'tournaments'", {gone});
        db::exec("DELETE FROM schema_migrations WHERE version = 4");
        db::exec("PRAGMA foreign_keys = ON");
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE tournaments SET status = 'TERMINATED' WHERE tournament_id = ?", {swissTid}));

        const QStringList tables{"players", "enrollments", "rounds", "matches", "commander_events", "commander_rounds",
                                 "commander_pods", "commander_seats", "commander_byes", "commander_audit"};
        const auto everything = [&tables] {
            QList<Rows> out;
            for (const QString &t : tables)
                out << db::query(QStringLiteral("SELECT * FROM %1 ORDER BY 1, 2").arg(t));
            return out;
        };
        const QString oldColumns = "SELECT tournament_id, name, game, format, tournament_date, location, total_rounds, "
                                   "top_cut, round_time_mins, status, notes FROM tournaments ORDER BY tournament_id";
        const QList<Rows> before = everything();
        const Rows tournaments = db::query(oldColumns);
        const QString cmdState = stateJson(cmdTid);

        restart();          // the upgrade runs
        QCOMPARE(everything(), before);                                 // not one row of any other table changed
        QCOMPARE(db::query(oldColumns), tournaments);
        QCOMPARE(stateJson(cmdTid), cmdState);
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());
        QCOMPARE(db::value("PRAGMA integrity_check").toString(), QString("ok"));
        QCOMPARE(db::value("PRAGMA foreign_keys").toInt(), 1);          // switched back on afterwards
        QCOMPARE(db::value("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND tbl_name = 'tournaments' "
                           "AND name LIKE 'idx_tournaments_%'").toInt(), 2);
        QVERIFY(!db::value("SELECT 1 FROM sqlite_master WHERE name = 'tournaments_rebuild'").isValid());
        QVERIFY(db::value("SELECT 1 FROM schema_migrations WHERE version = 4").isValid());
        // a backup of the database as it was is saved first
        QCOMPARE(QDir(db::dataDir() + "/backups").entryList({"*.db"}, QDir::Files).size(), 1);

        // the new status is accepted, and ids are not reused
        QVERIFY(tdb::terminateTournament(swissTid));
        QCOMPARE(tdb::tournamentById(swissTid)["status"].toString(), tdb::TERMINATED);
        QVERIFY(tdb::createTournament("Next One", "MTG", 1) > gone);
        // other tables still point at their tournaments: deleting one removes only its own rounds
        const int otherRounds = db::value("SELECT COUNT(*) FROM commander_rounds WHERE tournament_id = ?", {cmdTid}).toInt();
        QVERIFY(otherRounds > 0);
        QVERIFY(db::value("SELECT COUNT(*) FROM rounds WHERE tournament_id = ?", {swissTid}).toInt() > 0);
        db::exec("DELETE FROM matches WHERE tournament_id = ?", {swissTid});
        db::exec("DELETE FROM tournaments WHERE tournament_id = ?", {swissTid});
        QCOMPARE(db::value("SELECT COUNT(*) FROM rounds WHERE tournament_id = ?", {swissTid}).toInt(), 0);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_rounds WHERE tournament_id = ?", {cmdTid}).toInt(), otherRounds);
        restart();          // nothing left to do the second time
        QCOMPARE(QDir(db::dataDir() + "/backups").entryList({"*.db"}, QDir::Files).size(), 1);
    }

    // Renaming a tournament

    void renamingChangesOnlyTheNameInEveryState()
    {
        // one tournament in each state: registering, running, completed, ended early, and a Commander event
        const qint64 pending = tdb::createTournament("Pending Cup", "ONEPIECE", 2);
        tdb::enrollPlayer(pending, pdb::addPlayer("Waiting"));
        const qint64 running = runningSwiss();
        const qint64 done = tdb::createTournament("Finished Cup", "POKEMON", 1);
        for (qint64 p : addPlayers(2, "F"))
            tdb::enrollPlayer(done, p);
        swiss::startTournament(done);
        tdb::reportMatchResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(done, "POKEMON");
        const qint64 ended = runningSwiss();
        tdb::terminateTournament(ended);
        const qint64 cmd = makeEvent(8, 1).first;
        playRound(cmd);                                  // one round: finalizing it completes the event
        QCOMPARE(tdb::tournamentById(cmd)["status"].toString(), tdb::COMPLETED);

        const QStringList tables{"players", "enrollments", "rounds", "matches", "commander_events", "commander_rounds",
                                 "commander_pods", "commander_seats", "commander_byes"};
        const auto everything = [&tables] {
            QList<Rows> out;
            for (const QString &t : tables)
                out << db::query(QStringLiteral("SELECT * FROM %1 ORDER BY 1, 2").arg(t));
            // every tournament column except the name
            out << db::query("SELECT tournament_id, game, format, tournament_date, location, total_rounds, top_cut, "
                             "round_time_mins, status, notes, terminated_at FROM tournaments ORDER BY tournament_id");
            return out;
        };
        const QList<Rows> before = everything();
        const int count = db::value("SELECT COUNT(*) FROM tournaments").toInt();
        const QVariantList standings = cdb::getStandings(cmd);

        int n = 0;
        for (qint64 tid : {pending, running, done, ended, cmd}) {
            const QString name = QStringLiteral("Demo Event %1").arg(++n);
            QVERIFY(tdb::renameTournament(tid, "   " + name + "  "));      // spaces around it are removed
            QCOMPARE(tdb::tournamentById(tid)["name"].toString(), name);
            QVERIFY(!tdb::renameTournament(tid, name));                     // already its name: nothing to do
            QVERIFY(!tdb::renameTournament(tid, " " + name + " "));
        }
        QCOMPARE(everything(), before);                                     // nothing else changed anywhere
        QCOMPARE(db::value("SELECT COUNT(*) FROM tournaments").toInt(), count);     // no tournament was created
        QCOMPARE(cdb::getStandings(cmd), standings);
        QCOMPARE(cdb::getEvent(cmd)["name"].toString(), QString("Demo Event 5"));
        QVERIFY(auditActions(cmd).contains("EVENT_RENAMED"));
        // a finished or ended tournament is still closed: renaming did not reopen it
        QCOMPARE(tdb::tournamentById(done)["status"].toString(), tdb::COMPLETED);
        QCOMPARE(tdb::tournamentById(ended)["status"].toString(), tdb::TERMINATED);
        const qint64 anyMatch = tdb::allMatches(ended).first()["match_id"].toLongLong();
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::reportMatchResult(anyMatch, "PLAYER2"));
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, tdb::currentRound(ended)["round_id"].toLongLong()).closed);
        QVERIFY(timerdb::get(timerdb::Kind::OneOnOne, tdb::currentRound(running)["round_id"].toLongLong()).state
                == timerdb::State::Running);                                // the running clock kept running

        // names that are refused leave the old one in place
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::renameTournament(done, ""));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::renameTournament(done, "    "));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::renameTournament(done, QString(tdb::MAX_NAME_LENGTH + 1, 'x')));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::renameTournament(987654, "Nobody"));
        QCOMPARE(tdb::tournamentById(done)["name"].toString(), QString("Demo Event 3"));
        QVERIFY(tdb::renameTournament(done, QString(tdb::MAX_NAME_LENGTH, 'x')));      // the limit itself is allowed
        QVERIFY(tdb::renameTournament(done, "Demo Event 3"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament(QString(tdb::MAX_NAME_LENGTH + 1, 'x'), "MTG", 1));

        restart();
        n = 0;
        for (qint64 tid : {pending, running, done, ended, cmd})
            QCOMPARE(tdb::tournamentById(tid)["name"].toString(), QStringLiteral("Demo Event %1").arg(++n));
        QCOMPARE(everything(), before);
    }

    void gamesAreListedInOnePlaceAndInOneOrder()
    {
        QCOMPARE(tdb::supportedGames(), (QStringList{"ONEPIECE", "POKEMON", "MTG"}));
        for (const QString &game : tdb::supportedGames())               // each is a game the schema accepts
            QVERIFY(tdb::createTournament("Any " + game, game, 1) > 0);
    }

    // Where the data lives

    void firstLaunchStartsInTheUserFolder()
    {
        QTemporaryDir user, program;
        const QString file = db::resolveDataFile(user.filePath("data"), program.path());
        QCOMPARE(file, user.filePath("data/tcg_tournament.db"));
        QVERIFY(QDir(user.filePath("data")).exists());
        QVERIFY(!QFileInfo::exists(file));                      // initialize() creates it
        QVERIFY(QDir(program.path()).isEmpty());                // nothing is written beside the program
    }

    void earlierDatabaseIsCopiedOnceAndNeverOverwritten()
    {
        // an earlier build's database (write-ahead log mode) in the folder above the program
        QTemporaryDir user, old;
        const QString earlier = old.filePath("tcg_tournament.db");
        db::closeThreadConnection();
        db::setPath(earlier);
        db::initialize();
        db::value("PRAGMA journal_mode = WAL");
        pdb::addPlayer("Kept Player");
        db::closeThreadConnection();
        QFile settings(old.filePath("settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write("{\"theme\": \"dark\"}");
        settings.close();
        QVERIFY(QDir(old.path()).mkpath("dist/App"));
        const auto bytes = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        const QByteArray before = bytes(earlier);

        const QString file = db::resolveDataFile(user.filePath("data"), old.filePath("dist/App"));
        QCOMPARE(file, user.filePath("data/tcg_tournament.db"));
        QCOMPARE(bytes(earlier), before);                       // the original is left exactly as it was
        QVERIFY(!QFileInfo::exists(file + ".importing"));
        db::setPath(file);
        QCOMPARE(pdb::allPlayers().size(), 1);
        QCOMPARE(prefs::load()["theme"].toString(), QString("dark"));
        pdb::addPlayer("Added Later");
        db::closeThreadConnection();

        // every later launch uses the user's database as it is
        QCOMPARE(db::resolveDataFile(user.filePath("data"), old.filePath("dist/App")), file);
        db::setPath(file);
        QCOMPARE(pdb::allPlayers().size(), 2);
        db::closeThreadConnection();
        QCOMPARE(bytes(earlier), before);
    }

    void existingDatabaseIsBackedUpBeforeASchemaUpdate()
    {
        const QString backups = db::dataDir() + "/backups";
        QVERIFY(!QDir(backups).exists());                       // a database created just now is not backed up
        pdb::addPlayer("Before Update");
        db::exec("DELETE FROM schema_migrations WHERE version = 3");
        db::initialize();
        const QStringList files = QDir(backups).entryList({"*.db"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        db::initialize();                                       // nothing pending: nothing more is copied
        QCOMPARE(QDir(backups).entryList({"*.db"}, QDir::Files).size(), 1);

        // the backup is the database as it was before the update
        const QString live = db::path();
        db::closeThreadConnection();
        db::setPath(backups + "/" + files.first());
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), 1);
        QVERIFY(!db::value("SELECT 1 FROM schema_migrations WHERE version = 3").isValid());
        db::closeThreadConnection();
        db::setPath(live);
        QVERIFY(db::value("SELECT 1 FROM schema_migrations WHERE version = 3").isValid());
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"
