// Tests for the database, rules and workflow layer.
//
// reference.json holds recorded events; the replay tests require the engine
// to produce exactly the same pods, seats, byes and standings from the same
// inputs and seeds.
#include "applog.h"
#include "commander.h"
#include "commander_db.h"
#include "db.h"
#include "round_status.h"
#include "store.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <chrono>
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
                                           const QVariantMap &settings = {}, bool topFourFinal = false)
    {
        const qint64 tid = cdb::createCommanderTournament("Test Commander", n, rounds, settings, seed, {}, 50, topFourFinal);
        const QList<qint64> pids = addPlayers(n);
        for (qint64 p : pids)
            tdb::enrollPlayer(tid, p);
        if (start)
            cdb::startEvent(tid);
        return {tid, pids};
    }

    // an event as an earlier version created it (commander-rules-3): second place is
    // recognition only and there is no final
    QPair<qint64, QList<qint64>> makeEarlierEvent(int n, int rounds = 0, bool start = true, qint64 seed = 12345)
    {
        const auto made = makeEvent(n, rounds, false, seed);
        QVariantMap settings = cdb::getEvent(made.first)["settings"].toMap();
        settings.remove("second_place_points");
        settings["rules_version"] = "commander-rules-3";
        db::exec("UPDATE commander_events SET rules_version = 'commander-rules-3', settings_json = ? WHERE tournament_id = ?",
                 {QString::fromUtf8(QJsonDocument::fromVariant(settings).toJson(QJsonDocument::Compact)), made.first});
        if (start)
            cdb::startEvent(made.first);
        return made;
    }

    QVariantMap currentRound(qint64 tid) { return cdb::getRounds(tid).last().toMap(); }

    // seat 1 wins every pod still to report; no clear second, so no second-place points
    QVariantMap playRound(qint64 tid, bool finalize = true)
    {
        const QVariantMap rnd = currentRound(tid);
        for (const QVariant &pv : rnd["pods"].toList()) {
            const QVariantMap pod = pv.toMap();
            if (!pod["reported"].toBool())
                cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
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
    void initTestCase() { db::requireChosenPath(); }
    void init()
    {
        tmp_ = std::make_unique<QTemporaryDir>();
        db::closeThreadConnection();
        db::setPath(tmp_->filePath("test.db"));
        db::initialize();
    }

    void cleanup()
    {
        applog::setDirectory(QString());
        applog::setLimits(2 * 1024 * 1024, 5);
        applog::setRetryInterval(30000);
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

    // the organizer sets the number that happens to be suggested; later check-ins must not change it
    void roundsTheOrganizerSetAreNotChangedByCheckIns()
    {
        auto [tid, pids] = makeEvent(8, 0, false);
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 3);          // suggested for 8
        cdb::updateEventConfig(tid, 3);
        for (qint64 p : addPlayers(12, "Late"))
            tdb::enrollPlayer(tid, p);
        cdb::syncRecommendation(tid);                                        // 20 checked in: the suggestion is 4
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 3);
        Q_UNUSED(pids);
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
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::reportPodResult(podId(pod), "WIN", seated[1], {}, v1 - 1, {}, "test correction"));
        const int v2 = cdb::reportPodResult(podId(pod), "WIN", seated[1], {}, v1, {}, "test correction");
        QCOMPARE(v2, v1 + 1);
        QHash<qint64, int> table;
        for (const QVariant &r : cdb::getStandings(tid))
            table.insert(r.toMap()["player_id"].toLongLong(), r.toMap()["points"].toInt());
        QCOMPARE(table[seated[0]], 0);
        QCOMPARE(table[seated[1]], 5);
        const int v3 = cdb::clearPodResult(podId(pod), v2, "test correction");
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
        QVERIFY(cdb::correctResult(podId(pod), "WIN", seated[2], {}, {}, "misreported", cdb::SecondPlace::noClearSecond()));
        QVERIFY2(!cdb::correctResult(podId(pod), "WIN", seated[2], {}, {}, "test correction", cdb::SecondPlace::noClearSecond()), "no change, nothing to do");
        const QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["reason"].toString(), QString("misreported"));

        cdb::publishNextRound(tid);
        const QString before = QString::fromUtf8(QJsonDocument::fromVariant(currentRound(tid)["pods"]).toJson());
        try {
            cdb::correctResult(podId(pod), "WIN", seated[1], {}, {}, "test correction", cdb::SecondPlace::noClearSecond());
            QFAIL("a resolution is required");
        } catch (const cdb::ResolutionRequired &e) {
            QCOMPARE(e.laterRounds.size(), 1);
            QCOMPARE(e.laterRounds[0].toMap()["round_number"].toInt(), 2);
        }
        cdb::correctResult(podId(pod), "WIN", seated[1], {}, "KEEP", "test correction", cdb::SecondPlace::noClearSecond());
        QCOMPARE(QString::fromUtf8(QJsonDocument::fromVariant(currentRound(tid)["pods"]).toJson()), before);

        cdb::correctResult(podId(pod), "WIN", seated[3], {}, "REBUILD", "test correction", cdb::SecondPlace::noClearSecond());
        QCOMPARE(cdb::getRounds(tid).size(), 1);
        QCOMPARE(cdb::getState(tid)["next"].toString(), QString("SWISS"));
        cdb::publishNextRound(tid);
        const QVariantMap r2 = currentRound(tid);
        cdb::reportPodResult(podId(podAt(r2, 0)), "WIN", seatPlayer(podAt(r2, 0), 0));
        try {
            cdb::correctResult(podId(pod), "WIN", seated[0], {}, "REBUILD", "test correction", cdb::SecondPlace::noClearSecond());
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
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(pod, "WIN", top4[0], {}, {}, "test correction"));
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
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(r2pod, "DRAW", 0, {}, "REBUILD", "test correction"));
        cdb::correctResult(r2pod, "DRAW", 0, {}, "KEEP", "test correction");
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
        swiss::reportResult(played[0]["match_id"].toLongLong(), "PLAYER1");
        swiss::reportResult(played[1]["match_id"].toLongLong(), "DRAW");
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
                swiss::reportResult(p["match_id"].toLongLong(), "PLAYER2");
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
        swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(tid, "POKEMON");
        const qint64 winner = m["player1_id"].toLongLong();
        QCOMPARE(pdb::matchHistory(winner, tid)[0]["result"].toString(), QString("WIN"));
        QCOMPARE(pdb::tournamentHistory(winner)[0]["final_placement"].toInt(), 1);
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
        for (const char *t : {"commander_final_players", "commander_finals", "commander_audit", "commander_advancements", "commander_byes", "commander_seats",
                              "commander_pods", "commander_rounds", "commander_events", "schema_migrations"})
            db::exec(QStringLiteral("DROP TABLE %1").arg(t));
        db::exec("ALTER TABLE enrollments DROP COLUMN checked_in");
        db::exec("INSERT INTO players (display_name) VALUES ('Legacy')");
        db::exec("INSERT INTO tournaments (name, game, format, tournament_date, total_rounds, status) "
                 "VALUES ('Old Modern', 'MTG', 'Modern', '2026-01-01', 4, 'COMPLETED')");
        db::exec("INSERT INTO enrollments (tournament_id, player_id, match_points) VALUES (1, 1, 9)");
        restart();
        const Rows versions = db::query("SELECT version, name FROM schema_migrations ORDER BY version");
        QCOMPARE(versions.size(), 9);
        QCOMPARE(versions[8]["name"].toString(), QString("commander_final_play_started"));
        QCOMPARE(versions[7]["name"].toString(), QString("commander_scored_second_and_final"));
        QCOMPARE(versions[6]["name"].toString(), QString("commander_second_place"));
        QCOMPARE(versions[5]["name"].toString(), QString("matches_round_index"));
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(played, "PLAYER3"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(played, "BYE"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(played, "player1"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(987654, "PLAYER1"));
        QCOMPARE(winner(played), QString("PENDING"));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(bye, "PLAYER2"));    // a bye stays a bye
        QCOMPARE(winner(bye), QString("BYE"));

        // a result can be entered, changed and cleared while its round is the one in play
        swiss::reportResult(played, "PLAYER1");
        swiss::reportResult(played, "DRAW");
        QCOMPARE(winner(played), QString("DRAW"));
        swiss::reportResult(played, "");
        QCOMPARE(winner(played), QString("PENDING"));

        // once the next round is paired from it, it is fixed
        reportWholeRound(tid);
        swiss::advanceToNextRound(tid, 1);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(played, "PLAYER2"));
        QCOMPARE(winner(played), QString("PLAYER1"));
        reportWholeRound(tid);
        swiss::finalizeTournament(tid, "MTG");
        qint64 last = 0;
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                last = m["match_id"].toLongLong();
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(last, "PLAYER2"));
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
        const QList<Rows> reported = savedResults(tid);
        db::exec("CREATE TRIGGER test_fail_match BEFORE INSERT ON matches WHEN NEW.table_number = 2 "
                 "BEGIN SELECT RAISE(ABORT, 'forced failure'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, swiss::advanceToNextRound(tid, 1));
        QCOMPARE(count("rounds", tid), 1);                                              // no empty round 2
        QCOMPARE(count("matches", tid), 2);
        QVERIFY2(tdb::currentRound(tid)["ended_at"].isNull(), "round 1 is still the open round");
        QCOMPARE(savedResults(tid), reported);          // records and results as they were before the attempt
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

    // An inner step fails and its error is caught before it reaches the owner of the
    // transaction.  The owner must not be able to save what is left.
    void aFailedInnerStepCannotBeCommittedByItsOwner()
    {
        const auto players = [] { return db::value("SELECT COUNT(*) FROM players").toInt(); };
        const auto rejected = [](db::Tx &tx) {
            try {
                tx.commit();
            } catch (const db::Error &e) {
                return e.message.contains("none of it was saved");
            }
            return false;
        };

        {
            db::Tx outer;
            pdb::addPlayer("outer");
            try {
                db::Tx inner;
                pdb::addPlayer("inner");
                throw std::runtime_error("failure inside the inner step");
            } catch (const std::runtime_error &) {
                // swallowed: the owner never hears of it
            }
            QVERIFY(rejected(outer));
        }
        QVERIFY(!db::inTransaction());
        QCOMPARE(players(), 0);                         // neither the outer nor the inner write was saved

        // the same through real operations: the registration fails (no such tournament)
        {
            db::Tx outer;
            const qint64 id = pdb::addPlayer("kept by mistake");
            bool failed = false;
            try {
                tdb::enrollPlayer(987654, id);
            } catch (...) {
                failed = true;
            }
            QVERIFY(failed);
            QVERIFY(rejected(outer));
        }
        QCOMPARE(players(), 0);

        // two levels down, with the level in between committing as if all were well
        {
            db::Tx outer;
            pdb::addPlayer("outer");
            {
                db::Tx middle;
                try {
                    db::Tx inner;
                    pdb::addPlayer("inner");
                    throw std::runtime_error("failure two levels down");
                } catch (const std::runtime_error &) {
                }
                middle.commit();
            }
            QVERIFY(rejected(outer));
        }
        QCOMPARE(players(), 0);

        // nothing of that is left over: an independent transaction commits
        {
            db::Tx tx;
            pdb::addPlayer("alone");
            tx.commit();
        }
        QCOMPARE(players(), 1);
        // a failed step whose owner gives up without committing leaves nothing behind either
        {
            db::Tx outer;
            try {
                db::Tx inner;
                throw std::runtime_error("failure");
            } catch (const std::runtime_error &) {
            }
        }
        {
            db::Tx outer;
            pdb::addPlayer("outer");
            {
                db::Tx inner;
                pdb::addPlayer("inner");
                inner.commit();
            }
            outer.commit();                             // ordinary nested success still commits
        }
        QCOMPARE(players(), 3);
        // a step that only reads and returns without committing does not spoil its owner
        {
            db::Tx outer;
            pdb::addPlayer("with a read");
            {
                db::Tx reading;
                db::value("SELECT COUNT(*) FROM players");
            }
            outer.commit();
        }
        QCOMPARE(players(), 4);
        restart();
        QCOMPARE(players(), 4);
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
            swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
        const Rows opponentMatches = pdb::matchHistory(opponent, swissTid), opponentEvents = pdb::tournamentHistory(opponent);
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
        QCOMPARE(pdb::tournamentHistory(opponent), opponentEvents);
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
        QVERIFY(pdb::tournamentHistory(namesake).isEmpty());
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
        swiss::reportResult(match["match_id"].toLongLong(), "PLAYER1");
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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
        swiss::reportResult(pairings[0]["match_id"].toLongLong(), "PLAYER1");
        const Rows untouched = saved();
        const Rows live = swiss::viewStandings(tid, "POKEMON");
        QCOMPARE(live.first()["player_id"], pairings[0]["player1_id"]);
        QCOMPARE(live.first()["match_points"].toInt(), swiss::points("POKEMON").win);
        QCOMPARE(live.first()["standing"].toInt(), 1);
        QCOMPARE(saved(), untouched);
        swiss::viewStandings(tid, "POKEMON");
        QCOMPARE(saved(), untouched);

        // once finished: the saved figures and placings are the result, however often they are shown
        swiss::reportResult(pairings[1]["match_id"].toLongLong(), "PLAYER2");
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
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
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
        QCOMPARE(swiss::tiebreakColumns("POKEMON")[1].second, QString("Opp Opp Win%"));
        QCOMPARE(swiss::tiebreakColumns("MTG")[2].second, QString("OGW%"));
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
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
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

        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(open, "PLAYER2"));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(reported, {}));
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
        swiss::reportResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
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
        swiss::reportResult(tdb::roundPairings(rid).first()["match_id"].toLongLong(), "PLAYER1");
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
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::clearPodResult(podId(pod0), cdb::NONE, "test correction"));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(podId(pod0), "WIN", seatPlayer(pod0, 1), {}, {}, "test correction"));
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
        swiss::reportResult(tdb::roundPairings(tdb::currentRound(done)["round_id"].toLongLong()).first()["match_id"].toLongLong(), "PLAYER1");
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
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(anyMatch, "PLAYER2"));
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

private:
    // Everything in the log folder, oldest entry first.
    QStringList logLines()
    {
        QStringList out;
        const QDir dir(tmp_->filePath("logs"));
        QStringList files = dir.entryList({"app*.log"}, QDir::Files, QDir::Name);
        std::sort(files.begin(), files.end(), [](const QString &x, const QString &y) {      // app.4.log … app.1.log, app.log
            const int a = x.section('.', 1, 1).toInt(), b = y.section('.', 1, 1).toInt();
            return a > b;
        });
        for (const QString &name : files) {
            QFile f(dir.filePath(name));
            if (f.open(QIODevice::ReadOnly))
                out << QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        }
        return out;
    }

    QStringList logged(const QString &containing)
    {
        QStringList out;
        for (const QString &l : logLines())
            if (l.contains(containing))
                out << l;
        return out;
    }

private slots:
    // The diagnostic log

    void logEntriesCarryTimeSeverityVersionActionAndIds()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        applog::info("test.plain", {{"tournament", 12}, {"round", 34}, {"flag", true}, {"text", "two words"}});
        applog::warning("test.detail", {{"player", 7}}, "first line\nsecond line");
        applog::error("test.error");
        const QStringList lines = logLines();
        QCOMPARE(lines.size(), 3);
        // local time with milliseconds and its offset from UTC, then severity, version and action
        static const QRegularExpression head(QStringLiteral(
            "^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}[+-]\\d\\d:\\d\\d (INFO |WARN |ERROR) %1 test\\.")
                                                 .arg(QRegularExpression::escape(prefs::VERSION)));
        for (const QString &l : lines)
            QVERIFY2(head.match(l).hasMatch(), qPrintable(l));
        QVERIFY2(lines[0].endsWith("test.plain tournament=12 round=34 flag=yes text=\"two words\""), qPrintable(lines[0]));
        QVERIFY2(lines[1].contains(" WARN ") && lines[1].endsWith("test.detail player=7 | first line / second line"), qPrintable(lines[1]));
        QVERIFY(lines[2].contains(" ERROR "));
        QVERIFY(QFileInfo(tmp_->filePath("logs/app.log")).size() > 0);       // on disk already: nothing waits to be flushed
        // references are short, different each time, and fit on one line of a dialog
        const QString ref = applog::newReference();
        QVERIFY(QRegularExpression("^R-[2-9B-DF-HJ-NP-TV-XZ]{6}$").match(ref).hasMatch());
        QVERIFY(ref != applog::newReference());
    }

    void actionsAreLoggedAsAttemptedThenOkOrFailedByIdNotByName()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        // one-on-one
        const qint64 zelda = pdb::addPlayer("Zelda Hyrule"), link = pdb::addPlayer("Link Kokiri");
        const qint64 tid = tdb::createTournament("Secret Winter Cup", "POKEMON", 2, "Standard");
        tdb::enrollPlayer(tid, zelda);
        tdb::enrollPlayer(tid, link);
        QCOMPARE(tdb::enrollPlayer(tid, link), qint64(0));
        swiss::startTournament(tid);
        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
        const qint64 match = tdb::roundPairings(rid).first()["match_id"].toLongLong();
        timerdb::start(timerdb::Kind::OneOnOne, rid);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, 1));       // a result is missing
        swiss::reportResult(match, "PLAYER1");
        swiss::advanceToNextRound(tid, 1);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(987654, "PLAYER1"));
        tdb::renameTournament(tid, "Secret Spring Cup");
        tdb::terminateTournament(tid);
        QVERIFY(!tdb::terminateTournament(tid));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(match, "PLAYER2"));
        pdb::renamePlayer(link, "Link Ordon");
        pdb::removePlayer(link);
        // Commander
        auto [cmd, pids] = makeEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(cmd), 0);
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finalizeRound(currentRound(cmd)["round_id"].toLongLong()));
        playRound(cmd);
        cdb::dropPlayer(cmd, pids[0]);
        cdb::publishNextRound(cmd, 2);

        const auto results = [&](const QString &action, const QString &ids) {
            QStringList out;
            static const QRegularExpression result(" status=([a-z-]+)");
            for (const QString &l : logged(" " + action + " "))
                if (l.contains(ids))
                    out << result.match(l).captured(1);
            return out.join(",");
        };
        const QString t = QStringLiteral("tournament=%1").arg(tid);
        QCOMPARE(results("tournament.create", t), QString("ok"));           // the id is only known once it is saved
        QCOMPARE(results("player.enroll", t + QStringLiteral(" player=%1").arg(link)), QString("attempt,ok,attempt,already-enrolled"));
        QCOMPARE(results("tournament.start", t), QString("attempt,ok"));
        QCOMPARE(results("clock.start", QStringLiteral("round=%1").arg(rid)), QString("attempt,ok"));
        QCOMPARE(results("round.end", t), QString("attempt,failed,attempt,ok"));
        QCOMPARE(results("result.report", QStringLiteral("match=%1 ").arg(match)), QString("attempt,ok,attempt,failed"));
        QCOMPARE(results("result.report", "match=987654"), QString("attempt,failed"));
        QCOMPARE(results("tournament.rename", t), QString("attempt,ok"));
        QCOMPARE(results("tournament.end_early", t), QString("attempt,ok,attempt,no-change"));
        QCOMPARE(results("player.rename", QStringLiteral("player=%1").arg(link)), QString("attempt,ok"));
        QCOMPARE(results("player.remove", QStringLiteral("player=%1").arg(link)), QString("attempt,ok"));
        const QString c = QStringLiteral("tournament=%1").arg(cmd);
        QCOMPARE(results("tournament.start", c), QString("attempt,ok"));
        QCOMPARE(results("result.report", QStringLiteral("pod=%1 ").arg(podId(pod))), QString("attempt,ok"));
        QVERIFY(results("round.finalize", "round=").startsWith("attempt,failed,attempt,ok"));
        QCOMPARE(results("player.drop", c), QString("attempt,ok"));
        QCOMPARE(results("round.publish", c), QString("attempt,ok"));
        // a failed action is a warning; a successful one is not
        for (const QString &l : logLines()) {
            if (l.contains("status=failed"))
                QVERIFY2(l.contains(" WARN "), qPrintable(l));
            if (l.contains("status=ok") || l.contains("status=attempt"))
                QVERIFY2(l.contains(" INFO "), qPrintable(l));
        }
        // ids, not names: no player or tournament name was written by any of this
        const QString all = logLines().join('\n');
        for (const char *name : {"Zelda", "Hyrule", "Link", "Kokiri", "Ordon", "Secret", "Winter Cup", "Spring Cup", "Test Commander"})
            QVERIFY2(!all.contains(name), name);
        // the Commander audit history in the database is still written, as before
        const QStringList audit = auditActions(cmd);
        for (const char *action : {"EVENT_STARTED", "ROUND_PUBLISHED", "RESULT_REPORTED", "ROUND_FINALIZED", "PLAYER_DROPPED"})
            QVERIFY2(audit.contains(action), action);
    }

    void aSuccessIsOnlyLoggedOnceTheChangeIsSaved()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 tid = runningSwiss();
        // the "ok" line is written after the transaction has ended, so the change is already readable
        // from another connection by the time it exists
        QCOMPARE(logged("tournament.start status=ok").size(), 1);
        std::thread([&] {
            QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);
            db::closeThreadConnection();
        }).join();
        // an action that is refused part-way leaves no "ok" and no change
        const Rows before = tdb::allMatches(tid);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::finalizeTournament(tid, "POKEMON"));
        QCOMPARE(logged("tournament.finalize status=ok").size(), 0);
        QCOMPARE(logged("tournament.finalize status=failed").size(), 1);
        QCOMPARE(tdb::allMatches(tid), before);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::IN_PROGRESS);
    }

    void databaseUpdatesAndBackupsAreLogged()
    {
        pdb::addPlayer("Somebody");
        db::exec("ALTER TABLE players DROP COLUMN deleted_at");
        db::exec("DELETE FROM schema_migrations WHERE version = 5");
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        restart();          // the update runs
        QCOMPARE(logged("db.backup status=ok").size(), 1);
        QVERIFY(logged("db.backup status=ok").first().contains("file=tcg_tournament.before-update-"));
        QCOMPARE(logged("db.update status=attempt version=5 name=player_removed").size(), 1);
        QCOMPARE(logged("db.update status=ok version=5 name=player_removed").size(), 1);
        QVERIFY(logLines().indexOf(logged("db.backup").first()) < logLines().indexOf(logged("db.update status=ok").first()));
        restart();          // nothing to do: nothing more is logged about updates
        QCOMPARE(logged("db.update").size(), 2);
    }

    void logFilesRotateAndStayWithinTheirLimits()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        applog::setLimits(4096, 3);
        for (int i = 0; i < 400; ++i)
            applog::info("test.fill", {{"n", i}}, QString(40, 'x'));
        const QDir dir(tmp_->filePath("logs"));
        QCOMPARE(dir.entryList({"app*.log"}, QDir::Files, QDir::Name), (QStringList{"app.1.log", "app.2.log", "app.log"}));
        for (const QString &name : dir.entryList({"app*.log"}, QDir::Files))
            QVERIFY2(QFileInfo(dir.filePath(name)).size() <= 4096, qPrintable(name));
        // the newest entries are kept, in order, and the oldest have gone
        const QStringList lines = logLines();
        QVERIFY(lines.size() > 50 && lines.size() < 400);
        QVERIFY(lines.last().contains("n=399 "));
        for (int i = 1; i < lines.size(); ++i) {
            static const QRegularExpression n(" n=(\\d+) ");
            QCOMPARE(n.match(lines[i]).captured(1).toInt(), n.match(lines[i - 1]).captured(1).toInt() + 1);
        }
        const QStringList recent = applog::recentLines(10, 100000);
        QCOMPARE(recent.size(), 10);
        QVERIFY(recent.last().contains("n=399 ") && recent.first().contains("n=390 "));
        QVERIFY(applog::recentLines(1000, 1000).join('\n').size() <= 1000);        // bounded by size as well as by lines
    }

    void loggingNeverGetsInTheWay()
    {
        // entries from before the folder is known are kept and written first
        applog::setDirectory(QString());
        applog::info("test.early", {{"n", 1}});
        applog::setDirectory(tmp_->filePath("logs"));
        applog::info("test.later", {{"n", 2}});
        QVERIFY(logLines().size() >= 2);
        QVERIFY(logged("test.early").size() == 1 && logLines().indexOf(logged("test.early").first()) < logLines().indexOf(logged("test.later").first()));

        // a folder that cannot be used: nothing is thrown, nothing is written, and the app's own work is unaffected
        QFile blocker(tmp_->filePath("not-a-folder"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        applog::setDirectory(tmp_->filePath("not-a-folder/logs"));
        for (int i = 0; i < 50; ++i)
            applog::error("test.lost", {{"n", i}}, "nowhere to go");
        const qint64 tid = runningSwiss();
        QVERIFY(tdb::terminateTournament(tid));
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);
        QVERIFY(!QFileInfo::exists(tmp_->filePath("not-a-folder/logs")));
        QVERIFY(applog::recentLines().isEmpty());
        // it says so, once, with the reason and a count; it does not complain on every entry
        const applog::Status down = applog::status();
        QVERIFY(!down.available);
        QVERIFY2(down.reason.contains("log folder could not be created") || down.reason.contains("could not be opened"), qPrintable(down.reason));
        QVERIFY(!down.since.isEmpty());
        QVERIFY2(down.lost >= 50, qPrintable(QString::number(down.lost)));
        // the report still has the version details, says entries may be missing, and carries the newest unwritten ones
        const QString partial = applog::diagnosticReport();
        QVERIFY(partial.contains("App version:") && partial.contains("Build:"));
        QVERIFY2(partial.contains("Logging:      UNAVAILABLE since") && partial.contains("recent")
                 && partial.contains("entries may be missing from this report"), qPrintable(partial));
        QVERIFY(partial.contains("Entries that could NOT be written to the log file"));
        QVERIFY(partial.contains("tournament.end_early status=ok"));
        QVERIFY(partial.contains("(none)"));                            // nothing could be read from the file
        // when the folder becomes usable again, logging resumes by itself at the next try and says what was missed
        applog::setRetryInterval(0);
        QVERIFY(QFile::remove(tmp_->filePath("not-a-folder")));
        applog::info("test.back", {{"n", 1}});
        QVERIFY(applog::status().available);
        QCOMPARE(applog::status().lost, 0);
        QFile resumed(tmp_->filePath("not-a-folder/logs/app.log"));
        QVERIFY(resumed.open(QIODevice::ReadOnly));
        const QStringList after = QString::fromUtf8(resumed.readAll()).split(QChar(10), Qt::SkipEmptyParts);
        QCOMPARE(after.size(), 2);
        QVERIFY2(after[0].contains(" WARN ") && after[0].contains("log.resumed lost=") && after[1].contains("test.back n=1"),
                 qPrintable(after.join(" || ")));
        QVERIFY(applog::diagnosticReport().contains("Logging:      working"));
        applog::setRetryInterval(30000);

        // many threads at once: every entry arrives whole
        applog::setDirectory(tmp_->filePath("logs2"));
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([t] {
                for (int i = 0; i < 100; ++i)
                    applog::info("test.thread", {{"thread", t}, {"n", i}}, QString(60, QChar('a' + t)));
            });
        for (std::thread &th : threads)
            th.join();
        QFile f(tmp_->filePath("logs2/app.log"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 800);
        static const QRegularExpression whole("test\\.thread thread=(\\d) n=\\d+ \\| ([a-h])\\2{59}$");
        for (const QString &l : lines)
            QVERIFY2(whole.match(l).hasMatch(), qPrintable(l));
    }

    void anAttemptAndItsOutcomeShareAnOperationId()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        static const QRegularExpression opIn(" op=([0-9A-F]{8})(?: |$)"), refIn(" ref=(R-[0-9A-Z]{6})(?: |$)");
        const auto opOf = [](const QString &line) { return opIn.match(line).captured(1); };

        // one action: two entries, one id, with other entries free to come between them
        const qint64 tid = runningSwiss();
        const QStringList start = logged(" tournament.start ");
        QCOMPARE(start.size(), 2);
        QVERIFY(start[0].contains("status=attempt") && start[1].contains("status=ok"));
        QVERIFY(!opOf(start[0]).isEmpty());
        QCOMPARE(opOf(start[0]), opOf(start[1]));
        // every action has its own
        QSet<QString> attempts;
        int attemptLines = 0;
        for (const QString &l : logLines()) {
            if (!l.contains("status=attempt"))
                continue;
            ++attemptLines;
            attempts.insert(opOf(l));
        }
        QVERIFY(attemptLines > 8);
        QCOMPARE(int(attempts.size()), attemptLines);
        // and every outcome belongs to exactly one attempt
        for (const QString &l : logLines()) {
            if (l.contains("status=attempt") || !l.contains(" status="))
                continue;
            QVERIFY2(attempts.contains(opOf(l)), qPrintable(l));
        }

        // a failure carries the operation id and an error reference; the dialog that reports it uses the same two
        qint64 match = 0;
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                match = m["match_id"].toLongLong();
        QVERIFY(match);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(match, "NONSENSE"));
        const QStringList bad = logged("match_result=NONSENSE");
        QCOMPARE(bad.size(), 2);
        QVERIFY(bad[1].contains("status=failed") && bad[1].contains(" WARN "));
        // the operation's outcome and the match's are different fields, and no line names a field twice
        QVERIFY(bad[0].contains(" status=attempt ") && bad[0].contains(" match_result=NONSENSE"));
        static const QRegularExpression field(" ([a-z_]+)=");
        for (const QString &line : logLines()) {
            QStringList seen;
            auto it = field.globalMatch(line.section(" | ", 0, 0));
            while (it.hasNext()) {
                const QString key = it.next().captured(1);
                QVERIFY2(!seen.contains(key), qPrintable(line));
                seen << key;
            }
            QVERIFY2(!seen.contains("result"), qPrintable(line));
        }
        QCOMPARE(opOf(bad[0]), opOf(bad[1]));
        const QString ref = refIn.match(bad[1]).captured(1);
        QVERIFY(!ref.isEmpty() && !bad[0].contains(" ref="));
        QString op;
        QCOMPARE(applog::errorReference(&op), ref);
        QCOMPARE(op, opOf(bad[1]));
        // used once: an unrelated error afterwards gets its own reference and no operation
        op = "x";
        QVERIFY(applog::errorReference(&op) != ref);
        QVERIFY(op.isEmpty());
        // a later successful action clears an unreported failure, so it is never attached to the wrong error
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, swiss::reportResult(match, "NONSENSE"));
        swiss::reportResult(match, "PLAYER1");
        QVERIFY(applog::errorReference(&op).startsWith("R-") && op.isEmpty());

        // simultaneous attempts on the same tournament: eight ids, each with its own matched outcome
        const Race r = race([tid](int) { return tdb::terminateTournament(tid) ? 1 : 0; });
        QCOMPARE(r.results.size(), 8);
        QHash<QString, QStringList> byOp;
        for (const QString &l : logged(QStringLiteral(" tournament.end_early ")))
            byOp[opOf(l)] << QRegularExpression(" status=([a-z-]+)").match(l).captured(1);
        QCOMPARE(int(byOp.size()), 8);
        int ok = 0, unchanged = 0;
        for (const QStringList &results : byOp) {
            QCOMPARE(results.size(), 2);
            QCOMPARE(results[0], QString("attempt"));
            ok += results[1] == "ok";
            unchanged += results[1] == "no-change";
        }
        QCOMPARE(ok, 1);                    // exactly one of them saved the change
        QCOMPARE(unchanged, 7);

        // whatever is logged while an operation runs carries its id, an operation inside another
        // shares it, and it can be carried to another thread
        QString outer, inner, onThread;
        {
            applog::Action action("test.outer", {{"n", 1}});
            outer = action.operation();
            QCOMPARE(applog::currentOperation(), outer);
            applog::info("test.during");
            {
                applog::Action nested("test.inner");
                inner = nested.operation();
            }
            std::thread([&] {
                applog::OperationScope scope(outer);
                onThread = applog::currentOperation();
                applog::info("test.elsewhere");
            }).join();
        }
        QVERIFY(applog::currentOperation().isEmpty());
        QCOMPARE(inner, outer);
        QCOMPARE(onThread, outer);
        for (const char *name : {"test.outer", "test.during", "test.inner", "test.elsewhere"})
            for (const QString &l : logged(QStringLiteral(" %1").arg(name)))
                QCOMPARE(opOf(l), outer);
        applog::info("test.after");
        QVERIFY(opOf(logged(" test.after").first()).isEmpty());
        // ids are short and fixed in length, so they do not make entries grow
        for (const QString &id : attempts)
            QCOMPARE(id.size(), 8);
    }

    void theStartEntryAndTheReportNameTheExactBuild()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const QString build = prefs::BUILD;
        // a Git commit, marked when the files had been changed since, or "no-git": never empty or invented
        static const QRegularExpression shape("^([0-9a-f]{8}(-modified-\\d{8}-\\d{4})?|no-git)$");
        QVERIFY2(shape.match(build).hasMatch(), qPrintable(build));
        applog::sessionStarted();
        const QStringList start = logged(" app.start ");
        QCOMPARE(start.size(), 1);
        QVERIFY2(start.first().contains(QStringLiteral(" %1 app.start build=%2 qt=").arg(prefs::VERSION, build)), qPrintable(start.first()));
        QVERIFY(applog::diagnosticReport().contains(QStringLiteral("Build:        ") + build));
        // a session that is not closed is noted at the next start, without saying why it ended
        applog::sessionStarted();
        const QStringList note = logged(" app.previous_session ");
        QCOMPARE(note.size(), 1);
        QVERIFY2(note.first().contains("The previous session did not close normally.") && note.first().contains("The reason is not known"),
                 qPrintable(note.first()));
        QVERIFY(!note.first().contains("crash", Qt::CaseInsensitive));
        applog::sessionEnded();
        applog::sessionStarted();
        QCOMPARE(logged(" app.previous_session ").size(), 1);           // closed normally: nothing to note
        applog::sessionEnded();
    }

    void recognisedNamesAreReplacedAndTheRestIsLeft()
    {
        const qint64 zelda = pdb::addPlayer("Zelda Hyrule"), al = pdb::addPlayer("Al");
        const qint64 tid = tdb::createTournament("Secret Winter Cup", "POKEMON", 1);
        Q_UNUSED(al);
        const QString text = "Zelda Hyrule and Al are registered for Secret Winter Cup; zelda hyrule again; Ganon is not known.";
        QCOMPARE(applog::scrub(text),
                 QStringLiteral("[player %1] and Al are registered for [tournament %2]; [player %1] again; Ganon is not known.")
                     .arg(pdb::formatId(zelda)).arg(tid));
        QCOMPARE(applog::scrub(applog::scrub(text)), applog::scrub(text));      // doing it twice changes nothing more
        QCOMPARE(applog::scrub("result.report status=ok match=41"), QString("result.report status=ok match=41"));
    }

    void aDiagnosticReportLeavesOutNamesPathsAndTheDatabase()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 zelda = pdb::addPlayer("Zelda Hyrule");
        pdb::addPlayer("Al");                                           // too short to tell from ordinary text
        const qint64 tid = tdb::createTournament("Secret Winter Cup", "POKEMON", 1, "Standard");
        const QString home = QDir::toNativeSeparators(QDir::homePath());
        // an error as a dialog would word it: with the names, and a path into the user's folder
        applog::warning("dialog.error", {{"ref", "R-TEST22"}, {"title", "Player not removed"}},
                        "Zelda Hyrule is still part of a tournament.\nThey are registered for Secret Winter Cup, which has not started. "
                        "Saved to " + home + "\\Documents\\backup.db and C:\\Users\\someone.else\\Desktop\\x.db");
        const QString line = logged("R-TEST22").first();
        QVERIFY2(!line.contains(home) && line.contains("~\\Documents\\backup.db"), qPrintable(line));    // no user folder, even in the file
        QVERIFY2(!line.contains("someone.else"), qPrintable(line));

        const QString report = applog::diagnosticReport();
        QVERIFY(report.contains(QStringLiteral("App version:  ") + prefs::VERSION));
        QVERIFY(report.contains(QStringLiteral("Build:        ") + prefs::BUILD));
        QVERIFY(report.contains("Logging:      working"));
        // it says plainly what it may still contain
        QVERIFY(report.contains("PLEASE READ THIS THROUGH BEFORE SHARING IT."));
        QVERIFY(report.contains("may contain personal information") && report.contains("best") && report.contains("can remain"));
        QVERIFY(report.contains(QStringLiteral("Qt version:   ") + qVersion()));
        QVERIFY(report.contains("System:") && report.contains(QSysInfo::currentCpuArchitecture()));
        QVERIFY(report.contains("updates applied: 1, 2, 3, 4, 5"));
        QVERIFY(report.contains("1 tournaments, 2 players"));
        QVERIFY(report.contains("R-TEST22"));
        // names it can recognise are replaced by ids; the user's folder is not spelled out
        QVERIFY2(!report.contains("Zelda") && !report.contains("Hyrule") && !report.contains("Secret Winter Cup"), qPrintable(report));
        QVERIFY(report.contains(QStringLiteral("[player %1] is still part of a tournament").arg(pdb::formatId(zelda))));
        QVERIFY(report.contains(QStringLiteral("registered for [tournament %1], which has not started").arg(tid)));
        QVERIFY(!report.contains(home) && !report.contains(QDir::homePath()));
        QVERIFY(!report.contains("SQLite format"));
        // bounded: a long log gives a report of limited size
        for (int i = 0; i < 3000; ++i)
            applog::info("test.fill", {{"n", i}}, QString(80, 'y'));
        const QString big = applog::diagnosticReport();
        QVERIFY2(big.size() < 110 * 1024, qPrintable(QString::number(big.size())));
        QVERIFY(big.contains("n=2999 ") && !big.contains("n=5 "));
    }

private:
    // What is saved about a tournament's results: its matches, and the records kept with the registrations.
    QList<Rows> savedResults(qint64 tid)
    {
        return {db::query("SELECT * FROM matches WHERE tournament_id = ? ORDER BY match_id", {tid}),
                db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid}),
                db::query("SELECT status FROM tournaments WHERE tournament_id = ?", {tid})};
    }

    // The stored records are exactly what the matches work out to.
    void assertStoredFiguresMatchTheMatches(qint64 tid, const QString &game)
    {
        QHash<qint64, Row> stored;
        for (const Row &r : swiss::standings(tid, game))
            stored.insert(r["player_id"].toLongLong(), r);
        for (const Row &live : swiss::viewStandings(tid, game)) {
            const Row s = stored.value(live["player_id"].toLongLong());
            for (const char *key : {"match_points", "match_wins", "match_losses", "match_draws", "omw_pct", "gw_pct", "ogw_pct"})
                QCOMPARE(s[key].toDouble(), live[key].toDouble());
        }
    }

    // The log entries of one operation, as "action:result" in order.
    QStringList operationOf(const QString &lineContaining)
    {
        static const QRegularExpression opIn(" op=([0-9A-F]{8})"), part(" ([a-z_.]+) status=([a-z-]+)");
        const QStringList hits = logged(lineContaining);
        if (hits.isEmpty())
            return {};
        const QString op = opIn.match(hits.last()).captured(1);
        QStringList out;
        for (const QString &l : logged(" op=" + op)) {
            const auto m = part.match(l);
            out << (m.hasMatch() ? m.captured(1) + ":" + m.captured(2) : QStringLiteral("other"));
        }
        return out;
    }

private slots:
    // Errors that used to be hidden, and operations that are saved whole or not at all

    void onlyARealDuplicateIsReportedAsAlreadyEnrolled()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 tid = tdb::createTournament("Sign-up", "POKEMON", 2);
        const qint64 a = pdb::addPlayer("Ana"), b = pdb::addPlayer("Bo");
        QVERIFY(tdb::enrollPlayer(tid, a) > 0);
        QCOMPARE(tdb::enrollPlayer(tid, a), qint64(0));         // the one case that is not an error
        QCOMPARE(count("enrollments", tid), 1);

        // something that does not exist is said to not exist
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::enrollPlayer(987654, a));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::enrollPlayer(tid, 987654));

        // the database refusing the write for any other reason is an error, with its own message
        db::exec("CREATE TRIGGER refuse_enrollment BEFORE INSERT ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'enrollments are closed'); END");
        bool thrown = false;
        try {
            tdb::enrollPlayer(tid, b);
        } catch (const db::Error &e) {
            thrown = true;
            QVERIFY(e.constraint && !e.unique() && !e.foreignKey() && !e.busy());
            QVERIFY2(e.message.contains("enrollments are closed"), qPrintable(e.message));
        }
        QVERIFY2(thrown, "a refused write is not passed off as 'already enrolled'");
        QCOMPARE(count("enrollments", tid), 1);
        QCOMPARE(operationOf(QStringLiteral("player.enroll status=attempt tournament=%1 player=%2").arg(tid).arg(b)),
                 (QStringList{"player.enroll:attempt", "player.enroll:failed"}));
        QCOMPARE(tdb::enrollPlayer(tid, a), qint64(0));         // an existing registration is still just reported
        db::exec("DROP TRIGGER refuse_enrollment");

        // the error codes tell the kinds of refusal apart (no reading of message text)
        try {
            db::exec("INSERT INTO enrollments (tournament_id, player_id) VALUES (?, ?)", {tid, a});
            QFAIL("a second row for the same player and tournament was accepted");
        } catch (const db::Error &e) {
            QVERIFY(e.unique() && e.constraint && !e.foreignKey());
        }
        try {
            db::exec("INSERT INTO enrollments (tournament_id, player_id) VALUES (?, ?)", {tid, 987654});
            QFAIL("a registration for a player who does not exist was accepted");
        } catch (const db::Error &e) {
            QVERIFY(e.foreignKey() && e.constraint && !e.unique());
        }

        // a database held by another connection: reported as busy after the wait, never as a duplicate
        std::atomic<bool> holding{false}, release{false};
        std::thread holder([&] {
            {
                db::Tx tx;
                holding = true;
                while (!release)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            db::closeThreadConnection();
        });
        while (!holding)
            std::this_thread::yield();
        thrown = false;
        try {
            tdb::enrollPlayer(tid, b);
        } catch (const db::Error &e) {
            thrown = true;
            QVERIFY2(e.busy() && !e.unique() && !e.constraint, qPrintable(e.message));
        }
        release = true;
        holder.join();
        QVERIFY(thrown);
        QCOMPARE(count("enrollments", tid), 1);

        // registering the same player from eight places at once: one registration, the rest "already", no error
        const Race r = race([tid, b](int) { return tdb::enrollPlayer(tid, b) > 0 ? 1 : 0; });
        QCOMPARE(r.other + r.refusals + r.conflicts, 0);
        QCOMPARE(r.results.count(1), 1);
        QCOMPARE(r.results.count(0), 7);
        QCOMPARE(count("enrollments", tid), 2);
        restart();
        QCOMPARE(count("enrollments", tid), 2);
    }

    void addAndEnrollCreatesBothRecordsOrNeither()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 tid = tdb::createTournament("Sign-up", "ONEPIECE", 2);
        const auto everyone = [] { return db::query("SELECT * FROM players ORDER BY player_id"); };

        // it works: one new player, registered, and only the owning action says "ok"
        const qint64 id = tdb::addAndEnrollPlayer(tid, "  Nia Walker ");
        QCOMPARE(pdb::playerById(id)["display_name"].toString(), QString("Nia Walker"));
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        QCOMPARE(operationOf(" player.add_and_enroll status=ok"),
                 (QStringList{"player.add_and_enroll:attempt", "player.add:attempt", "player.add:pending", "player.enroll:attempt",
                              "player.enroll:pending", "player.add_and_enroll:ok"}));

        // the registration is refused after the player was created: neither is left behind
        const Rows before = everyone();
        db::exec("CREATE TRIGGER refuse_enrollment BEFORE INSERT ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'enrollments are closed'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, tdb::addAndEnrollPlayer(tid, "Ghost Player"));
        QCOMPARE(everyone(), before);                           // no "Ghost Player", removed or otherwise
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        const QStringList failed = operationOf(" player.add_and_enroll status=failed");
        QCOMPARE(failed, (QStringList{"player.add_and_enroll:attempt", "player.add:attempt", "player.add:pending",
                                      "player.enroll:attempt", "player.enroll:failed", "player.add_and_enroll:failed"}));
        QVERIFY(!failed.join(' ').contains(":ok"));             // nothing about it was ever logged as a success

        // an existing player whose registration is refused is left exactly as they were
        const qint64 existing = pdb::addPlayer("Existing Player");
        const Row asWas = pdb::playerById(existing);
        QVERIFY_THROWS_EXCEPTION(db::Error, tdb::enrollPlayer(tid, existing));
        QCOMPARE(pdb::playerById(existing), asWas);
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        db::exec("DROP TRIGGER refuse_enrollment");

        // other refusals also leave nobody behind
        const Rows mid = everyone();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::addAndEnrollPlayer(tid, "   "));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::addAndEnrollPlayer(987654, "Nobody Home"));
        tdb::terminateTournament(tid);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::addAndEnrollPlayer(tid, "Too Late"));
        QCOMPARE(everyone(), mid);
        // the id that was tried is not handed out twice afterwards, and adding a player on its own still works
        const qint64 next = pdb::addPlayer("Standalone");
        QVERIFY(next > existing);
        restart();
        QCOMPARE(everyone().size(), mid.size() + 1);
        QCOMPARE(tdb::enrolledPlayers(tid).size(), 1);
        QVERIFY(pdb::playersNamed("Ghost Player").isEmpty() && pdb::playersNamed("Too Late").isEmpty());
    }

    void aResultAndTheStandingsThatFollowAreSavedTogether()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 tid = tdb::createTournament("Modern", "MTG", 1, "Modern");
        const QList<qint64> p = addPlayers(4, "M");
        for (qint64 id : p)
            tdb::enrollPlayer(tid, id);
        swiss::startTournament(tid);
        const Rows pairings = tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong());
        const qint64 m1 = pairings[0]["match_id"].toLongLong(), m2 = pairings[1]["match_id"].toLongLong();
        const qint64 p1 = pairings[0]["player1_id"].toLongLong(), p2 = pairings[0]["player2_id"].toLongLong();
        const auto record = [&](qint64 player) {
            const Row r = db::one("SELECT match_points, match_wins, match_losses, match_draws FROM enrollments "
                                  "WHERE tournament_id = ? AND player_id = ?", {tid, player});
            return QStringLiteral("%1pts %2-%3-%4").arg(r["match_points"].toInt()).arg(r["match_wins"].toInt())
                .arg(r["match_losses"].toInt()).arg(r["match_draws"].toInt());
        };

        // report, correct, draw, clear: the stored records follow every time, without anything else being called
        swiss::reportResult(m1, "PLAYER1");
        QCOMPARE(record(p1), QString("3pts 1-0-0"));
        QCOMPARE(record(p2), QString("0pts 0-1-0"));
        assertStoredFiguresMatchTheMatches(tid, "MTG");
        QCOMPARE(operationOf(QStringLiteral(" result.report status=ok match=%1 ").arg(m1)),
                 (QStringList{"result.report:attempt", "result.report:ok"}));
        swiss::reportResult(m1, "PLAYER2");
        QCOMPARE(record(p1), QString("0pts 0-1-0"));
        QCOMPARE(record(p2), QString("3pts 1-0-0"));
        swiss::reportResult(m1, "DRAW");
        QCOMPARE(record(p1), QString("1pts 0-0-1"));
        QCOMPARE(record(p2), QString("1pts 0-0-1"));
        assertStoredFiguresMatchTheMatches(tid, "MTG");
        swiss::reportResult(m1, QString());
        QCOMPARE(record(p1), QString("0pts 0-0-0"));
        QCOMPARE(tdb::pendingMatchCount(tdb::currentRound(tid)["round_id"].toLongLong()), 2);
        assertStoredFiguresMatchTheMatches(tid, "MTG");

        // the standings cannot be saved (the step after the result is written): the result is not saved either
        db::exec("CREATE TRIGGER break_standings BEFORE UPDATE ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'standings cannot be saved'); END");
        QList<Rows> before = savedResults(tid);
        bool thrown = false;
        try {
            swiss::reportResult(m1, "PLAYER1");
        } catch (const db::Error &e) {
            thrown = e.message.contains("standings cannot be saved");
        }
        QVERIFY(thrown);
        QCOMPARE(savedResults(tid), before);                    // the match is still unreported
        const QStringList failed = operationOf(QStringLiteral(" result.report status=failed match=%1 ").arg(m1));
        QCOMPARE(failed, (QStringList{"result.report:attempt", "result.report:failed"}));
        QVERIFY(logged(QStringLiteral(" result.report status=failed match=%1 ").arg(m1)).last().contains(" ref=R-"));
        db::exec("DROP TRIGGER break_standings");
        // the same when it fails part-way through the players' records, after some were already written
        db::exec(QStringLiteral("CREATE TRIGGER break_one BEFORE UPDATE ON enrollments WHEN NEW.player_id = %1 "
                                "BEGIN SELECT RAISE(ABORT, 'one record cannot be saved'); END").arg(p.last()));
        QVERIFY_THROWS_EXCEPTION(db::Error, swiss::reportResult(m1, "PLAYER1"));
        QCOMPARE(savedResults(tid), before);
        db::exec("DROP TRIGGER break_one");

        // finishing: placings and the completed status are saved with the last figures, or not at all
        swiss::reportResult(m1, "PLAYER1");
        swiss::reportResult(m2, "PLAYER2");
        db::exec("CREATE TRIGGER refuse_completion BEFORE UPDATE OF status ON tournaments "
                 "BEGIN SELECT RAISE(ABORT, 'the tournament cannot be closed'); END");
        before = savedResults(tid);
        QVERIFY_THROWS_EXCEPTION(db::Error, swiss::finalizeTournament(tid, "MTG"));
        QCOMPARE(savedResults(tid), before);                    // no placings were left behind
        QCOMPARE(db::value("SELECT COUNT(*) FROM enrollments WHERE tournament_id = ? AND final_placement IS NOT NULL", {tid}).toInt(), 0);
        QCOMPARE(logged(" tournament.finalize status=ok").size(), 0);
        db::exec("DROP TRIGGER refuse_completion");
        swiss::finalizeTournament(tid, "MTG");
        QCOMPARE(logged(" tournament.finalize status=ok").size(), 1);

        // a finished tournament: an edit is refused and nothing at all changes
        const QList<Rows> final = savedResults(tid);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(m1, "PLAYER2"));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(m2, QString()));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, swiss::reportResult(m1, "DRAW"));
        QCOMPARE(savedResults(tid), final);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);
        swiss::viewStandings(tid, "MTG");                       // and looking at the standings writes nothing
        QCOMPARE(savedResults(tid), final);
        restart();
        QCOMPARE(savedResults(tid), final);
    }

    void aCommanderResultAndItsStandingsAreAlreadySavedTogether()
    {
        auto [tid, pids] = makeEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const auto saved = [&] {
            return QList<Rows>{db::query("SELECT * FROM commander_pods WHERE tournament_id = ? ORDER BY pod_id", {tid}),
                               db::query("SELECT * FROM commander_seats WHERE tournament_id = ? ORDER BY pod_id, seat_number", {tid}),
                               db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid}),
                               db::query("SELECT * FROM commander_audit WHERE tournament_id = ? ORDER BY audit_id", {tid})};
        };
        const QList<Rows> before = saved();
        db::exec("CREATE TRIGGER break_standings BEFORE UPDATE OF match_points ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'standings cannot be saved'); END");
        // reporting, and later correcting, a pod: when the standings cannot be saved, neither is the result
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0)));
        QCOMPARE(saved(), before);
        db::exec("DROP TRIGGER break_standings");
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));
        const QList<Rows> reported = saved();
        QVERIFY(reported != before);
        db::exec("CREATE TRIGGER break_standings BEFORE UPDATE OF match_points ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'standings cannot be saved'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::clearPodResult(podId(pod), cdb::NONE, "test correction"));
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::correctResult(podId(pod), "WIN", seatPlayer(pod, 1), {}, {}, "test correction"));
        QCOMPARE(saved(), reported);
        db::exec("DROP TRIGGER break_standings");
        Q_UNUSED(pids);
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

    // audit: rules the core must enforce whatever the screens allow

    void aPlayerCannotLeaveOrJoinOnceResultsDependOnThem()
    {
        const qint64 tid = tdb::createTournament("Locked", "POKEMON", 1);
        const QList<qint64> p = addPlayers(5, "U");
        for (int i = 0; i < 4; ++i)
            tdb::enrollPlayer(tid, p[i]);
        tdb::unenrollPlayer(tid, p[3]);     // allowed while it is being set up
        QCOMPARE(tdb::enrolledPlayers(tid, true).size(), 3);
        tdb::unenrollPlayer(tid, p[3]);     // a second click changes nothing
        tdb::enrollPlayer(tid, p[3]);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::unenrollPlayer(987654, p[0]));

        swiss::startTournament(tid);
        QList<Rows> before = savedResults(tid);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::unenrollPlayer(tid, p[0]));
        QCOMPARE(savedResults(tid), before);

        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
        swiss::finalizeTournament(tid, "POKEMON");
        before = savedResults(tid);
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::unenrollPlayer(tid, p[0]));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::enrollPlayer(tid, p[4]));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::addAndEnrollPlayer(tid, "Too Late"));
        QCOMPARE(savedResults(tid), before);
        QVERIFY(pdb::playersNamed("Too Late").isEmpty());
        restart();
        QCOMPARE(savedResults(tid), before);
    }

    void aTournamentIsFinalizedOnlyAfterItsLastRound()
    {
        const qint64 tid = tdb::createTournament("Three rounds", "MTG", 3, "Modern");
        const QList<qint64> p = addPlayers(4, "F");
        for (qint64 id : p)
            tdb::enrollPlayer(tid, id);
        const auto status = [&] { return tdb::tournamentById(tid)["status"].toString(); };
        const auto placed = [&] {
            return db::value("SELECT COUNT(*) FROM enrollments WHERE tournament_id = ? AND final_placement IS NOT NULL", {tid}).toInt();
        };
        const auto reportAll = [&] {
            for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
                if (!m["player2_id"].isNull())
                    swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
        };

        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::finalizeTournament(tid, "MTG"));      // never started
        QCOMPARE(status(), tdb::PENDING);
        swiss::startTournament(tid);
        reportAll();
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::finalizeTournament(tid, "MTG"));      // round 1 of 3
        QCOMPARE(status(), tdb::IN_PROGRESS);
        QCOMPARE(placed(), 0);
        swiss::advanceToNextRound(tid, 1);
        reportAll();
        swiss::advanceToNextRound(tid, 2);
        reportAll();
        // the tournament's own game decides the scoring, whatever the caller passes
        swiss::finalizeTournament(tid, "POKEMON");
        QCOMPARE(status(), tdb::COMPLETED);
        QCOMPARE(placed(), 4);
        QCOMPARE(db::value("SELECT MAX(match_points) FROM enrollments WHERE tournament_id = ?", {tid}).toInt() % 3, 0);
        QVERIFY(db::value("SELECT MAX(match_points) FROM enrollments WHERE tournament_id = ?", {tid}).toInt() >= 6);
    }

    void aTournamentNeedsTwoPlayersToStart()
    {
        const qint64 tid = tdb::createTournament("Lonely", "ONEPIECE", 2);
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::startTournament(tid));
        tdb::enrollPlayer(tid, pdb::addPlayer("Only One"));
        QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::startTournament(tid));
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::PENDING);
        QCOMPARE(tdb::rounds(tid).size(), 0);
        tdb::enrollPlayer(tid, pdb::addPlayer("Second"));
        swiss::startTournament(tid);
        QCOMPARE(tdb::rounds(tid).size(), 1);
    }

    void playerNamesAreCheckedInTheCore()
    {
        const auto count = [] { return db::value("SELECT COUNT(*) FROM players").toInt(); };
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::addPlayer(QString()));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::addPlayer("   "));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::addPlayer(QString(tdb::MAX_NAME_LENGTH + 1, 'x')));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::addPlayer(QStringLiteral("Line\nBreak")));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::addPlayer(QStringLiteral("Tab\tbed")));
        QCOMPARE(count(), 0);

        const qint64 id = pdb::addPlayer("  Ada Lovelace  ");
        QCOMPARE(pdb::playerById(id)["display_name"].toString(), QString("Ada Lovelace"));
        pdb::addPlayer(QString(tdb::MAX_NAME_LENGTH, 'x'));
        pdb::addPlayer(QStringLiteral("Zoë O'Brien-Smith 2"));
        QCOMPARE(count(), 3);

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::renamePlayer(987654, "Nobody"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::renamePlayer(id, QString(tdb::MAX_NAME_LENGTH + 1, 'y')));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pdb::renamePlayer(id, QStringLiteral("Bad\rName")));
        QCOMPARE(pdb::playerById(id)["display_name"].toString(), QString("Ada Lovelace"));
        pdb::renamePlayer(id, "  Ada King ");
        QCOMPARE(pdb::playerById(id)["display_name"].toString(), QString("Ada King"));
        // a name saved by an earlier version is still read as it is
        db::exec("INSERT INTO players (display_name) VALUES (?)", {QString(300, 'z')});
        QCOMPARE(pdb::allPlayers().size(), 4);
    }

    void tournamentDetailsAreCheckedInTheCore()
    {
        const auto count = [] { return db::value("SELECT COUNT(*) FROM tournaments").toInt(); };
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("Chess night", "CHESS", 3));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("Endless", "MTG", tdb::MAX_ROUNDS + 1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament("Someday", "MTG", 3, {}, "next tuesday"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, tdb::createTournament(QStringLiteral("Two\nlines"), "MTG", 3));
        QVERIFY_THROWS_EXCEPTION(std::exception, cdb::createCommanderTournament("   ", 8, 3, {}, 5));
        QVERIFY_THROWS_EXCEPTION(std::exception, cdb::createCommanderTournament(QString(tdb::MAX_NAME_LENGTH + 1, 'c'), 8, 3, {}, 5));
        QVERIFY_THROWS_EXCEPTION(std::exception, cdb::createCommanderTournament("Pods", 8, 3, {}, 5, {}, 0));
        QCOMPARE(count(), 0);

        const qint64 tid = tdb::createTournament("  Friday Standard  ", "POKEMON", tdb::MAX_ROUNDS, {}, "2026-10-16");
        QCOMPARE(tdb::tournamentById(tid)["name"].toString(), QString("Friday Standard"));
        QCOMPARE(tdb::tournamentById(tid)["tournament_date"].toString(), QString("2026-10-16"));
        QVERIFY(cdb::createCommanderTournament("Pods", 8, 3, {}, 5) > 0);
    }

    void searchingTreatsPercentAndUnderscoreAsOrdinaryCharacters()
    {
        pdb::addPlayer("Ann");
        pdb::addPlayer("50% Off");
        pdb::addPlayer("a_b");
        pdb::addPlayer("Back\\slash");
        QCOMPARE(pdb::searchPlayers("%").size(), 1);
        QCOMPARE(pdb::searchPlayers("_").size(), 1);
        QCOMPARE(pdb::searchPlayers("\\").size(), 1);
        QCOMPARE(pdb::searchPlayers("AN").size(), 1);      // still not case sensitive
        QCOMPARE(pdb::searchPlayers("").size(), 4);
    }

    // backups saved from Settings.  the first three failed on the copy-over-the-file code
    // this replaced; the fourth is extra coverage that passed on it too

    static QByteArray contentOf(const QString &file)
    {
        QFile f(file);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("(unreadable)");
    }

    // players in a database file opened on its own, or -1 if it fails its integrity check
    int playersInFile(const QString &file)
    {
        const QString live = db::path();
        db::closeThreadConnection();
        db::setPath(file);
        const int n = db::value("SELECT COUNT(*) FROM players").toInt();
        const QString check = db::value("PRAGMA integrity_check").toString();
        db::closeThreadConnection();
        db::setPath(live);
        return check == "ok" ? n : -1;
    }

    void aFailedBackupLeavesTheFileThatWasThereUnchanged()
    {
        pdb::addPlayer("Kept");
        const QString live = db::path(), dest = tmp_->filePath("backup.db");
        db::exportCopy(dest);
        const QByteArray earlier = contentOf(dest);
        QVERIFY(earlier.size() > 1000);

        // the database cannot be read (its drive is gone): nothing may be lost at the destination
        db::closeThreadConnection();
        db::setPath(tmp_->filePath("unplugged/tcg_tournament.db"));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(dest));
        QCOMPARE(contentOf(dest), earlier);
        // the same when what is there is not a database at all
        QFile junk(tmp_->filePath("not-a-database.db"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(8192, 'x'));
        junk.close();
        db::closeThreadConnection();
        db::setPath(junk.fileName());
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(dest));
        QCOMPARE(contentOf(dest), earlier);

        db::closeThreadConnection();
        db::setPath(live);
        QVERIFY(QDir(tmp_->path()).entryList({"*.partial", "*.previous"}).isEmpty());
        QCOMPARE(playersInFile(dest), 1);
    }

    void aBackupOntoTheLiveDatabaseIsRefusedWithoutTouchingIt()
    {
        pdb::addPlayer("Kept");
        const QString live = db::path();
        // with the file closed nothing else protects it: the refusal has to be the app's own
        db::closeThreadConnection();
        const QByteArray before = contentOf(live);
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(live));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(tmp_->path() + "/./TEST.DB"));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(tmp_->path() + "/sub/../test.db"));
        QVERIFY(QFileInfo::exists(live));
        QCOMPARE(contentOf(live), before);
        // and while it is open, as it is in the app
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), 1);
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(live));
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), 1);
        QCOMPARE(db::value("PRAGMA integrity_check").toString(), QString("ok"));
        QVERIFY(QDir(tmp_->path()).entryList({"*.partial", "*.previous"}).isEmpty());
    }

    void aBackupHoldsEverythingCommittedAlsoInWalMode()
    {
        // in WAL mode new rows sit in a second file until a checkpoint; a copy of the main
        // file alone would not have them
        QCOMPARE(db::value("PRAGMA journal_mode = WAL").toString().toLower(), QString("wal"));
        db::exec("PRAGMA wal_autocheckpoint = 0");
        const QList<qint64> p = addPlayers(40, "W");
        const qint64 tid = tdb::createTournament("Wal Cup", "POKEMON", 1);
        tdb::enrollPlayer(tid, p[0]);
        tdb::enrollPlayer(tid, p[1]);
        swiss::startTournament(tid);
        QVERIFY(QFileInfo(db::path() + "-wal").size() > 0);

        const QString dest = tmp_->filePath("wal-backup.db");
        {
            db::Tx open;        // a change still in progress is not part of the backup
            pdb::addPlayer("Not saved yet");
            db::exportCopy(dest);
        }
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), 40);
        QCOMPARE(playersInFile(dest), 40);
        // the copy is one file that stands on its own
        QVERIFY(!QFileInfo::exists(dest + "-wal"));
        const QString live = db::path();
        db::closeThreadConnection();
        db::setPath(dest);
        QCOMPARE(db::value("SELECT COUNT(*) FROM matches WHERE tournament_id = ?", {tid}).toInt(), 1);
        QCOMPARE(db::value("SELECT name FROM tournaments WHERE tournament_id = ?", {tid}).toString(), QString("Wal Cup"));
        QCOMPARE(db::query("PRAGMA foreign_key_check").size(), 0);
        QCOMPARE(db::value("SELECT COUNT(*) FROM schema_migrations").toInt(), int(db::migrations().size()));
        db::closeThreadConnection();
        db::setPath(live);
    }

    void aBackupReplacesAnEarlierOneOnlyWhenItSucceeds()
    {
        pdb::addPlayer("Kept");
        const QString dest = tmp_->filePath("backup.db");
        db::exportCopy(dest);
        QCOMPARE(playersInFile(dest), 1);
        pdb::addPlayer("Second");
        db::exportCopy(dest);
        QCOMPARE(playersInFile(dest), 2);

        // places a copy cannot go
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(tmp_->filePath("no/such/folder/backup.db")));
        QVERIFY(QDir().mkpath(tmp_->filePath("taken.db")));         // a folder where the file should go
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(tmp_->filePath("taken.db")));
        QVERIFY(QFileInfo(tmp_->filePath("taken.db")).isDir());
#ifdef Q_OS_WIN
        // another program has the earlier backup open, so it cannot be replaced: it stays as it is
        const QByteArray earlier = contentOf(dest);
        QFile held(dest);
        QVERIFY(held.open(QIODevice::ReadOnly));
        pdb::addPlayer("Third");
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exportCopy(dest));
        held.close();
        QCOMPARE(contentOf(dest), earlier);
#endif
        QVERIFY(QDir(tmp_->path()).entryList({"*.partial", "*.previous"}).isEmpty());
        QCOMPARE(playersInFile(dest), 2);
    }

    void theDatabaseEnforcesItsReferencesAndFindsARoundsMatchesByIndex()
    {
        QCOMPARE(db::value("PRAGMA foreign_keys").toInt(), 1);
        const qint64 tid = tdb::createTournament("Refs", "MTG", 2);
        const qint64 a = pdb::addPlayer("A"), b = pdb::addPlayer("B");
        tdb::enrollPlayer(tid, a);
        tdb::enrollPlayer(tid, b);
        swiss::startTournament(tid);
        bool refused = false;
        try {
            db::exec("DELETE FROM players WHERE player_id = ?", {a});
        } catch (const db::Error &e) {
            refused = e.foreignKey();
        }
        QVERIFY(refused);
        // the queries a round screen runs on every refresh do not read the whole table
        for (const char *sql : {"SELECT COUNT(*) FROM matches WHERE round_id = 1 AND winner = 'PENDING'",
                                "SELECT * FROM matches m WHERE m.round_id = 1 ORDER BY m.table_number"}) {
            QString plan;
            for (const Row &r : db::query(QStringLiteral("EXPLAIN QUERY PLAN ") + sql))
                plan += r["detail"].toString() + "; ";
            QVERIFY2(plan.contains("idx_matches_round"), qPrintable(plan));
        }
        // and the update that adds the index reaches a database made before it
        db::exec("DROP INDEX idx_matches_round");
        db::exec("DELETE FROM schema_migrations WHERE version = 6");
        restart();
        QVERIFY(db::value("SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'idx_matches_round'").isValid());
        QCOMPARE(db::value("SELECT COUNT(*) FROM matches WHERE tournament_id = ?", {tid}).toInt(), 1);
        QCOMPARE(QDir(tmp_->filePath("backups")).entryList({"*.db"}).size(), 1);       // backed up first
    }

    // characterization, not a rule that was decided: what joining after the start does today.
    // no screen offers it; it is recorded here so a change of policy is a deliberate one
    void lateEnrollmentAsItWorksToday()
    {
        const qint64 tid = tdb::createTournament("Late", "MTG", 3, "Modern");
        const QList<qint64> p = addPlayers(5, "L");
        for (int i = 0; i < 4; ++i)
            tdb::enrollPlayer(tid, p[i]);
        swiss::startTournament(tid);
        const qint64 late = p[4];
        const auto inRound = [&](int number) {
            return db::value("SELECT COUNT(*) FROM matches m JOIN rounds r ON r.round_id = m.round_id WHERE m.tournament_id = ? "
                             "AND r.round_number = ? AND (m.player1_id = ? OR m.player2_id = ?)", {tid, number, late, late}).toInt();
        };
        const auto reportAll = [&] {
            for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
                if (!m["player2_id"].isNull())
                    swiss::reportResult(m["match_id"].toLongLong(), "PLAYER1");
        };

        // joins the list at once, with nothing for the round already paired
        QVERIFY(tdb::enrollPlayer(tid, late) > 0);
        QCOMPARE(tdb::enrollPlayer(tid, late), 0);              // still only once
        QCOMPARE(inRound(1), 0);
        Rows table = swiss::viewStandings(tid, "MTG");
        QCOMPARE(table.size(), 5);
        const Row joined = *std::find_if(table.begin(), table.end(), [&](const Row &r) { return r["player_id"].toLongLong() == late; });
        QCOMPARE(joined["match_points"].toInt() + joined["match_wins"].toInt() + joined["match_losses"].toInt(), 0);
        // and is paired from the next round on; the missed round is neither a loss nor a bye
        reportAll();
        swiss::advanceToNextRound(tid, 1);
        QCOMPARE(inRound(1), 0);
        QCOMPARE(inRound(2), 1);
        QCOMPARE(tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()).size(), 3);    // five players: two matches and a bye
        reportAll();
        swiss::advanceToNextRound(tid, 2);
        reportAll();
        table = swiss::finalizeTournament(tid, "MTG");
        QCOMPARE(table.size(), 5);
        for (const Row &r : table)
            QVERIFY(r["final_placement"].toInt() >= 1 && r["final_placement"].toInt() <= 5);
        QCOMPARE(pdb::matchHistory(late, tid).size(), 2);
        QCOMPARE(pdb::tournamentHistory(late).size(), 1);

        // a Commander event that has started: the newcomer is added as checked in and active,
        // although check-in itself and removal are both closed by then
        auto [cid, seated] = makeEvent(8, 2);
        const qint64 extra = pdb::addPlayer("Late Commander");
        QVERIFY(tdb::enrollPlayer(cid, extra) > 0);
        QCOMPARE(db::value("SELECT checked_in FROM enrollments WHERE tournament_id = ? AND player_id = ?", {cid, extra}).toInt(), 1);
        QCOMPARE(cdb::getStandings(cid).size(), seated.size() + 1);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::setCheckedIn(cid, extra, false));
        QVERIFY_THROWS_EXCEPTION(std::logic_error, tdb::unenrollPlayer(cid, extra));
    }

    // the suites run with db::requireChosenPath(): losing the test folder must never mean
    // falling back to the organizer's own data
    void aTestProgramNeverFallsBackToTheDefaultDataFolder()
    {
        const QByteArray chosen = qgetenv("TCG_DATA_DIR");
        const QString test = db::path();
        db::closeThreadConnection();
        db::setPath(QString());
        qunsetenv("TCG_DATA_DIR");
        QVERIFY_THROWS_EXCEPTION(db::Error, db::path());
        QVERIFY_THROWS_EXCEPTION(db::Error, db::dataDir());
        QVERIFY_THROWS_EXCEPTION(db::Error, db::value("SELECT 1"));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::initialize());
        QVERIFY_THROWS_EXCEPTION(db::Error, pdb::addPlayer("Nowhere"));
        // a folder named through TCG_DATA_DIR is a choice, and is the one used
        const QString named = tmp_->filePath("named by the environment");
        qputenv("TCG_DATA_DIR", named.toLocal8Bit());
        QCOMPARE(QDir::cleanPath(db::path()), QDir::cleanPath(named + "/tcg_tournament.db"));
        db::initialize();
        pdb::addPlayer("Somewhere");
        QVERIFY(QFileInfo::exists(named + "/tcg_tournament.db"));
        db::closeThreadConnection();
        if (chosen.isEmpty())
            qunsetenv("TCG_DATA_DIR");
        else
            qputenv("TCG_DATA_DIR", chosen);
        db::setPath(test);
        QCOMPARE(db::value("SELECT COUNT(*) FROM players").toInt(), 0);     // the test's own database again
    }
private:
    static qint64 savedSecond(qint64 pod)
    {
        return db::value("SELECT second_place_player_id FROM commander_pods WHERE pod_id = ?", {pod}).toLongLong();
    }

    QList<Rows> savedCommander(qint64 tid)
    {
        return {db::query("SELECT * FROM commander_pods WHERE tournament_id = ? ORDER BY pod_id", {tid}),
                db::query("SELECT * FROM commander_seats WHERE tournament_id = ? ORDER BY pod_id, seat_number", {tid}),
                db::query("SELECT * FROM enrollments WHERE tournament_id = ? ORDER BY enrollment_id", {tid}),
                db::query("SELECT * FROM commander_audit WHERE tournament_id = ? ORDER BY audit_id", {tid})};
    }

    // everything scoring and pairing depend on, with ids that are the same in a fresh database
    QStringList commanderOutcome(qint64 tid)
    {
        const auto json = [](const QVariant &v) { return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson()); };
        QStringList out{json(cdb::getStandings(tid))};
        for (const QVariant &rv : cdb::getRounds(tid)) {
            QVariantList pods;
            for (const QVariant &pv : rv.toMap()["pods"].toList())
                pods << pv.toMap()["seats"];
            out << json(pods) << json(rv.toMap()["byes"]);
        }
        for (const Row &r : db::query("SELECT pairing_inputs_json FROM commander_rounds WHERE tournament_id = ? "
                                      "ORDER BY round_number", {tid}))
            out << r["pairing_inputs_json"].toString();
        out << db::value("SELECT final_standings_json FROM commander_events WHERE tournament_id = ?", {tid}).toString();
        for (const Row &r : db::query("SELECT player_id, match_points, match_wins, match_losses, match_draws, final_placement "
                                      "FROM enrollments WHERE tournament_id = ? ORDER BY player_id", {tid}))
            out << json(r);
        return out;
    }

private slots:
    // events created before commander-rules-4: second place is recognition only, never points

    void secondPlaceIsSavedWithTheResultAndEarnsNothing()
    {
        auto [tid, pids] = makeEarlierEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        const int v = cdb::reportPodResult(podId(pod), "WIN", seated[0], {}, cdb::NONE, seated[2]);
        QCOMPARE(savedSecond(podId(pod)), seated[2]);
        const QVariantMap shown = podAt(currentRound(tid), 0);
        QCOMPARE(shown["second_place_player_id"].toLongLong(), seated[2]);
        QCOMPARE(shown["result_version"].toInt(), v);
        QHash<qint64, QVariantMap> table;
        for (const QVariant &r : cdb::getStandings(tid))
            table.insert(r.toMap()["player_id"].toLongLong(), r.toMap());
        QCOMPARE(table[seated[2]]["points"].toInt(), 0);              // second place is still a loss
        QCOMPARE(table[seated[2]]["losses"].toInt(), 1);
        QCOMPARE(table[seated[2]]["points"], table[seated[1]]["points"]);
        QCOMPARE(table[seated[2]]["losses"], table[seated[1]]["losses"]);
        QCOMPARE(table[seated[0]]["points"].toInt(), 5);
        const QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_REPORTED"));
        QCOMPARE(entry["detail"].toMap()["second_place"].toLongLong(), seated[2]);
        QVERIFY(entry["detail"].toMap()["second_place_before"].isNull());
        QCOMPARE(cdb::reportPodResult(podId(pod), "WIN", seated[0], {}, cdb::NONE, seated[2]), v);    // a repeat is a no-op

        // not recorded is the default, and a pod without one finalizes like any other
        const QVariantMap other = podAt(currentRound(tid), 1);
        cdb::reportPodResult(podId(other), "WIN", seatPlayer(other, 1));
        QVERIFY(db::value("SELECT second_place_player_id FROM commander_pods WHERE pod_id = ?", {podId(other)}).isNull());
        QVERIFY(cdb::finalizeRound(currentRound(tid)["round_id"].toLongLong()));

        // kept by permanent id across a restart, and shown in the player's pod history
        restart();
        QCOMPARE(podAt(cdb::getRounds(tid)[0].toMap(), 0)["second_place_player_id"].toLongLong(), seated[2]);
        const Rows history = pdb::podHistory(seated[2], tid);
        QCOMPARE(history.size(), 1);
        QCOMPARE(history[0]["result"].toString(), QString("LOSS"));
        QCOMPARE(history[0]["second_place"].toInt(), 1);
        QCOMPARE(pdb::podHistory(seated[1], tid)[0]["second_place"].toInt(), 0);
        QCOMPARE(pdb::podHistory(seated[0], tid)[0]["second_place"].toInt(), 0);
        Q_UNUSED(pids);
    }

    void anInvalidSecondPlaceIsRefusedAndChangesNothing()
    {
        auto [tid, pids] = makeEarlierEvent(5, 2);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap pod = podAt(rnd, 0);
        const QList<qint64> seated = ids(pod["seats"].toList());
        QCOMPARE(rnd["byes"].toList().size(), 1);
        const qint64 bye = rnd["byes"].toList()[0].toMap()["player_id"].toLongLong();
        const qint64 outsider = pdb::addPlayer("Not Enrolled");
        const QList<Rows> before = savedCommander(tid);
        const auto refused = [&](const QString &outcome, qint64 winner, qint64 second, const char *why) {
            try {
                cdb::reportPodResult(podId(pod), outcome, winner, {}, cdb::NONE, second);
                QFAIL(why);
            } catch (const cdb::CommanderError &e) {
                QVERIFY2(!e.message.isEmpty(), why);
            }
            QCOMPARE(savedCommander(tid), before);
        };
        refused("WIN", seated[0], seated[0], "the winner as second");
        refused("WIN", seated[0], bye, "a player on a bye");
        refused("WIN", seated[0], outsider, "a player from outside the tournament");
        refused("WIN", seated[0], 987654, "an unknown id");
        refused("DRAW", 0, seated[1], "a drawn pod");
        refused("WIN", 0, seated[1], "no winner");

        // a player from another pod
        const qint64 big = makeEarlierEvent(8, 1).first;
        const QVariantMap a = podAt(currentRound(big), 0), b = podAt(currentRound(big), 1);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, cdb::NONE, seatPlayer(b, 1)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::correctResult(podId(a), "WIN", seatPlayer(a, 0), {}, {}, "test correction", seatPlayer(b, 1)));
        QCOMPARE(podAt(currentRound(big), 0)["status"].toString(), QString("PENDING"));
        QVERIFY(auditActions(big).filter("RESULT").isEmpty());
        Q_UNUSED(pids);
    }

    void secondPlaceIsCorrectedAndClearedWithTheResult()
    {
        auto [tid, pids] = makeEarlierEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const qint64 id = podId(pod);
        const QList<qint64> seated = ids(pod["seats"].toList());
        const int v1 = cdb::reportPodResult(id, "WIN", seated[0], {}, cdb::NONE, seated[1]);
        const QVariantList standings = cdb::getStandings(tid);

        // changing only second place is a correction like any other, with no effect on the standings
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::reportPodResult(id, "WIN", seated[0], {}, cdb::NONE, seated[2]));
        const int v2 = cdb::reportPodResult(id, "WIN", seated[0], {}, v1, seated[2], "test correction");
        QCOMPARE(v2, v1 + 1);
        QCOMPARE(savedSecond(id), seated[2]);
        QCOMPARE(cdb::getStandings(tid), standings);
        QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["second_place_before"].toLongLong(), seated[1]);
        QCOMPARE(entry["detail"].toMap()["second_place"].toLongLong(), seated[2]);

        // and back to not recorded
        const int v3 = cdb::reportPodResult(id, "WIN", seated[0], {}, v2, 0, "test correction");
        QCOMPARE(savedSecond(id), qint64(0));
        QCOMPARE(cdb::getStandings(tid), standings);

        // a new winner never keeps a second place that was given for the old one
        const int v4 = cdb::reportPodResult(id, "WIN", seated[0], {}, v3, seated[3], "test correction");
        const int v5 = cdb::reportPodResult(id, "WIN", seated[3], {}, v4, {}, "test correction");
        QCOMPARE(savedSecond(id), qint64(0));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(id, "WIN", seated[3], {}, v5, seated[3], "test correction"));

        // a draw has no second place
        const int v6 = cdb::reportPodResult(id, "WIN", seated[3], {}, v5, seated[0], "test correction");
        QCOMPARE(savedSecond(id), seated[0]);
        const int v7 = cdb::reportPodResult(id, "DRAW", 0, {seated[1]}, v6, {}, "test correction");
        QCOMPARE(savedSecond(id), qint64(0));

        // clearing a pod's result, its undo, removes it along with the result
        const int v8 = cdb::reportPodResult(id, "WIN", seated[1], {}, v7, seated[2], "test correction");
        QCOMPARE(savedSecond(id), seated[2]);
        cdb::clearPodResult(id, v8, "test correction");
        QCOMPARE(savedSecond(id), qint64(0));
        QCOMPARE(podAt(currentRound(tid), 0)["status"].toString(), QString("PENDING"));
        entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CLEARED"));
        QCOMPARE(entry["detail"].toMap()["second_place_before"].toLongLong(), seated[2]);
        Q_UNUSED(pids);
    }

    void secondPlaceInAFinalizedRoundIsCorrectedWithoutTouchingLaterRounds()
    {
        auto [tid, pids] = makeEarlierEvent(8, 3);
        const QVariantMap r1 = playRound(tid);
        const qint64 id = podId(podAt(r1, 0));
        const QList<qint64> seated = ids(podAt(r1, 0)["seats"].toList());
        cdb::publishNextRound(tid);
        const QStringList before = commanderOutcome(tid);
        const int rounds = cdb::getRounds(tid).size();

        // second place alone changes no points, so there is no keep-or-rebuild question
        QVERIFY(cdb::correctResult(id, "WIN", seated[0], {}, {}, "second recorded late", seated[1]));
        QCOMPARE(savedSecond(id), seated[1]);
        QCOMPARE(commanderOutcome(tid), before);
        QCOMPARE(cdb::getRounds(tid).size(), rounds);
        QVERIFY2(!cdb::correctResult(id, "WIN", seated[0], {}, {}, "test correction", seated[1]), "no change, nothing to do");

        // a correction to a draw changes points, so the question is asked as before, and it clears second place
        QVERIFY_THROWS_EXCEPTION(cdb::ResolutionRequired, cdb::correctResult(id, "DRAW", 0, {}, {}, "test correction"));
        QCOMPARE(savedSecond(id), seated[1]);
        QVERIFY(cdb::correctResult(id, "DRAW", 0, {}, "KEEP", "test correction"));
        QCOMPARE(savedSecond(id), qint64(0));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(id, "DRAW", 0, {}, "KEEP", "test correction", seated[1]));
        Q_UNUSED(pids);
    }

    void secondPlaceInACompletedEventLeavesTheFinalPlacingsAlone()
    {
        auto [tid, pids] = makeEarlierEvent(8, 1);
        const QVariantMap r1 = playRound(tid);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);
        const qint64 id = podId(podAt(r1, 0));
        const QList<qint64> seated = ids(podAt(r1, 0)["seats"].toList());
        const QStringList before = commanderOutcome(tid);
        const int entries = auditActions(tid).size();
        QVERIFY(cdb::correctResult(id, "WIN", seated[0], {}, {}, "test correction", seated[3]));
        QCOMPARE(savedSecond(id), seated[3]);
        QCOMPARE(commanderOutcome(tid), before);
        QCOMPARE(auditActions(tid).size(), entries + 1);               // the correction only, no new final standings
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);

        // an event ended early stays read-only for second place too
        const qint64 ended = makeEarlierEvent(8, 2).first;
        const QVariantMap pod = podAt(currentRound(ended), 0);
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));
        QVERIFY(tdb::terminateTournament(ended));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::correctResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, {}, "test correction", seatPlayer(pod, 1)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, seatPlayer(pod, 1)));
        QCOMPARE(savedSecond(podId(pod)), qint64(0));
        Q_UNUSED(pids);
    }

    void standingsAndPairingsAreIdenticalWithAndWithoutSecondPlace()
    {
        // the same event twice from an empty database: no second places, then one in every pod
        const auto play = [this](const QString &file, bool withSecond) {
            db::closeThreadConnection();
            db::setPath(tmp_->filePath(file));
            db::initialize();
            const qint64 tid = makeEarlierEvent(11, 3, true, 4242).first;
            for (int number = 1; number <= 3; ++number) {
                const QVariantMap rnd = currentRound(tid);
                for (const QVariant &pv : rnd["pods"].toList()) {
                    const QVariantMap pod = pv.toMap();
                    cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, number % 3), {}, cdb::NONE,
                                         withSecond ? seatPlayer(pod, (number + 1) % 3) : 0);
                }
                cdb::finalizeRound(rnd["round_id"].toLongLong());
                if (number < 3)
                    cdb::publishNextRound(tid, number + 1);
            }
            const int seconds = db::value("SELECT COUNT(second_place_player_id) FROM commander_pods").toInt();
            return qMakePair(commanderOutcome(tid), seconds);
        };
        const auto without = play("plain.db", false);
        const auto with = play("second.db", true);
        QCOMPARE(without.second, 0);
        QCOMPARE(with.second, 9);                                       // three pods in each of three rounds
        QCOMPARE(with.first.size(), without.first.size());
        for (int i = 0; i < with.first.size(); ++i)
            QCOMPARE(with.first[i], without.first[i]);
        QVERIFY(!with.first.filter("final_placement").isEmpty());
    }

    void aFailedSaveKeepsNeitherTheResultNorItsSecondPlace()
    {
        auto [tid, pids] = makeEarlierEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const qint64 id = podId(pod);
        const QList<Rows> before = savedCommander(tid);
        for (const char *when : {"BEFORE UPDATE OF match_points ON enrollments", "BEFORE INSERT ON commander_audit"}) {
            db::exec(QStringLiteral("CREATE TRIGGER break_save %1 BEGIN SELECT RAISE(ABORT, 'cannot be saved'); END").arg(when));
            QVERIFY_THROWS_EXCEPTION(db::Error,
                                     cdb::reportPodResult(id, "WIN", seatPlayer(pod, 0), {}, cdb::NONE, seatPlayer(pod, 1)));
            QCOMPARE(savedCommander(tid), before);
            db::exec("DROP TRIGGER break_save");
        }
        const int v = cdb::reportPodResult(id, "WIN", seatPlayer(pod, 0), {}, cdb::NONE, seatPlayer(pod, 1));
        const QList<Rows> reported = savedCommander(tid);
        db::exec("CREATE TRIGGER break_save BEFORE INSERT ON commander_audit BEGIN SELECT RAISE(ABORT, 'cannot be saved'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::reportPodResult(id, "WIN", seatPlayer(pod, 0), {}, v, seatPlayer(pod, 2), "test correction"));
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::clearPodResult(id, v, "test correction"));
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::correctResult(id, "DRAW", 0, {}, {}, "test correction"));
        QCOMPARE(savedCommander(tid), reported);
        QCOMPARE(savedSecond(id), seatPlayer(pod, 1));
        db::exec("DROP TRIGGER break_save");
        Q_UNUSED(pids);
    }

    void aRemovedPlayerKeepsTheirSecondPlaceAndName()
    {
        auto [tid, pids] = makeEarlierEvent(8, 1);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const qint64 second = seatPlayer(pod, 2);
        const QString name = db::value("SELECT display_name FROM players WHERE player_id = ?", {second}).toString();
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, second);
        playRound(tid);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);
        QVERIFY(pdb::removePlayer(second));
        restart();
        QCOMPARE(savedSecond(podId(pod)), second);
        const QVariantMap shown = podAt(cdb::getRounds(tid)[0].toMap(), 0);
        QCOMPARE(shown["second_place_player_id"].toLongLong(), second);
        QString shownName;
        for (const QVariant &s : shown["seats"].toList())
            if (s.toMap()["player_id"].toLongLong() == second)
                shownName = s.toMap()["display_name"].toString();
        QCOMPARE(shownName, name);
        QCOMPARE(pdb::podHistory(second, tid)[0]["second_place"].toInt(), 1);
        Q_UNUSED(pids);
    }

    void olderResultsGainNoSecondPlaceWhenTheDatabaseIsUpdated()
    {
        auto [tid, pids] = makeEarlierEvent(8, 2);
        playRound(tid);
        cdb::publishNextRound(tid);
        playRound(tid, false);

        // rebuild the pods table as an older version left it, before updates 7 and 8
        const QString current = db::value("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'commander_pods'").toString();
        QString older = current;
        older.replace(QRegularExpression(",\\s*second_place_player_id INTEGER REFERENCES players\\(player_id\\)"), "");
        older.replace(QRegularExpression(",\\s*second_place_status TEXT CHECK \\(second_place_status IN \\([^)]*\\)\\)"), "");
        older.replace("CREATE TABLE commander_pods", "CREATE TABLE commander_pods_older");
        QVERIFY2(older != current && !older.contains("second_place"), qPrintable(older));
        const QString kept = "pod_id, round_id, tournament_id, pod_number, status, outcome, advancing_player_id, "
                             "result_version, reported_at, confirmed_at";
        const Rows podsBefore = db::query("SELECT " + kept + " FROM commander_pods ORDER BY pod_id");
        const auto results = [] {
            return QList<Rows>{db::query("SELECT * FROM commander_seats ORDER BY pod_id, seat_number"),
                               db::query("SELECT * FROM enrollments ORDER BY enrollment_id"),
                               db::query("SELECT * FROM commander_audit ORDER BY audit_id")};
        };
        const QList<Rows> resultsBefore = results();
        const QVariantList standings = cdb::getStandings(tid);
        db::exec("PRAGMA foreign_keys = OFF");
        // what update 8 added goes too: its triggers, final tables and final option
        for (const char *trigger : {"commander_round_finalized_complete", "commander_second_place_valid",
                                    "commander_final_enabled_frozen"})
            db::exec(QStringLiteral("DROP TRIGGER %1").arg(trigger));
        db::exec("DROP TABLE commander_final_players");
        db::exec("DROP TABLE commander_finals");
        db::exec("ALTER TABLE commander_events DROP COLUMN final_enabled");
        db::exec(older);
        db::exec("INSERT INTO commander_pods_older SELECT " + kept + " FROM commander_pods");
        db::exec("DROP TABLE commander_pods");
        db::exec("ALTER TABLE commander_pods_older RENAME TO commander_pods");
        db::exec("CREATE INDEX idx_cmd_pods_round ON commander_pods(round_id)");
        db::exec("PRAGMA foreign_keys = ON");
        db::exec("DELETE FROM schema_migrations WHERE version IN (7, 8, 9)");
        QCOMPARE(db::query("SELECT * FROM commander_pods ORDER BY pod_id"), podsBefore);

        restart();
        QCOMPARE(db::value("SELECT name FROM schema_migrations WHERE version = 7").toString(), QString("commander_second_place"));
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_pods WHERE second_place_status IS NOT NULL").toInt(), 0);
        QCOMPARE(db::value("SELECT final_enabled FROM commander_events WHERE tournament_id = ?", {tid}).toInt(), 0);
        QVERIFY(!cdb::getEvent(tid)["scores_second"].toBool());       // the earlier rules stay
        QCOMPARE(db::query("SELECT " + kept + " FROM commander_pods ORDER BY pod_id"), podsBefore);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_pods WHERE second_place_player_id IS NOT NULL").toInt(), 0);
        QCOMPARE(results(), resultsBefore);
        QCOMPARE(cdb::getStandings(tid), standings);
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());

        // a copy was saved before the update, and it is the older database
        const QStringList backups = QDir(db::dataDir() + "/backups").entryList({"*.db"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        const QString live = db::path();
        db::closeThreadConnection();
        db::setPath(db::dataDir() + "/backups/" + backups.first());
        QVERIFY(!db::value("SELECT 1 FROM schema_migrations WHERE version = 7").isValid());
        QVERIFY(!db::value("SELECT sql FROM sqlite_master WHERE name = 'commander_pods'").toString().contains("second_place"));
        db::closeThreadConnection();
        db::setPath(live);

        // an older finalized result can be given one by correction, still without points
        const QVariantMap pod = podAt(cdb::getRounds(tid)[0].toMap(), 0);
        QVERIFY(cdb::correctResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, {}, "test correction", seatPlayer(pod, 1)));
        QCOMPARE(savedSecond(podId(pod)), seatPlayer(pod, 1));
        QCOMPARE(cdb::getStandings(tid), standings);
        Q_UNUSED(pids);
    }
private:
    using Groups = QList<QList<qint64>>;

    QHash<qint64, QVariantMap> rowsOf(qint64 tid)
    {
        QHash<qint64, QVariantMap> out;
        for (const QVariant &r : cdb::getStandings(tid))
            out.insert(r.toMap()["player_id"].toLongLong(), r.toMap());
        return out;
    }

    double pts(qint64 tid, qint64 player) { return rowsOf(tid)[player]["points"].toDouble(); }

    // the qualifying figures only, without the saved placings
    QList<QVariantList> qualifyingFigures(qint64 tid)
    {
        QList<QVariantList> out;
        for (const QVariant &v : cdb::getStandings(tid)) {
            const QVariantMap r = v.toMap();
            out << QVariantList{r["player_id"], r["points"], r["wins"], r["losses"], r["draws"], r["byes"], r["seconds"],
                                r["omw_pct"], r["mw_pct"], r["standing"]};
        }
        return out;
    }

    // every pod still to report: seat 1 wins and seat 2 is a clear second
    void playWithSeconds(qint64 tid, bool finalize = true)
    {
        const QVariantMap rnd = currentRound(tid);
        for (const QVariant &pv : rnd["pods"].toList()) {
            const QVariantMap pod = pv.toMap();
            if (!pod["reported"].toBool())
                cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, seatPlayer(pod, 1));
        }
        if (finalize)
            cdb::finalizeRound(rnd["round_id"].toLongLong());
    }

    // an event with the top-four final whose qualifying rounds are all played and finalized
    QPair<qint64, QList<qint64>> qualified(int n, int rounds, qint64 seed = 12345)
    {
        const auto made = makeEvent(n, rounds, true, seed, {}, true);
        for (int r = 1; r <= rounds; ++r) {
            playWithSeconds(made.first);
            if (r < rounds)
                cdb::publishNextRound(made.first, r + 1);
        }
        return made;
    }

    QList<qint64> candidateSeeds(qint64 tid)
    {
        QList<qint64> out;
        for (const QVariant &p : cdb::finalCandidates(tid)["players"].toList())
            out << p.toMap()["player_id"].toLongLong();
        return out;
    }

    QList<qint64> finalSeedsOf(qint64 tid)
    {
        QList<qint64> out;
        for (const QVariant &p : cdb::getFinal(tid)["players"].toList())
            out << p.toMap()["player_id"].toLongLong();
        return out;
    }

    // player ids in the order the final placed them, and how each place was decided
    QList<qint64> finalOrder(qint64 tid, QStringList *bases = nullptr)
    {
        QMap<int, QPair<qint64, QString>> byPlace;
        for (const QVariant &p : cdb::getFinal(tid)["players"].toList())
            byPlace.insert(p.toMap()["placement"].toInt(),
                           {p.toMap()["player_id"].toLongLong(), p.toMap()["placement_basis"].toString()});
        QList<qint64> out;
        for (auto it = byPlace.begin(); it != byPlace.end(); ++it) {
            out << it.value().first;
            if (bases)
                *bases << it.value().second;
        }
        return out;
    }

    QList<qint64> finalStandingIds(qint64 tid)
    {
        return ids(cdb::getState(tid)["final_standings"].toList());
    }

    int finalVersion(qint64 tid) { return cdb::getFinal(tid)["result_version"].toInt(); }

    // everything a final or a result writes, for comparing before and after a refusal
    QList<Rows> savedEvent(qint64 tid)
    {
        QList<Rows> out = savedCommander(tid);
        out << db::query("SELECT * FROM commander_finals WHERE tournament_id = ?", {tid})
            << db::query("SELECT * FROM commander_final_players WHERE tournament_id = ? ORDER BY seed", {tid})
            << db::query("SELECT * FROM commander_events WHERE tournament_id = ?", {tid})
            << db::query("SELECT status FROM tournaments WHERE tournament_id = ?", {tid})
            << db::query("SELECT * FROM commander_rounds WHERE tournament_id = ? ORDER BY round_id", {tid});
        return out;
    }

    void breakAuditInserts(bool broken)
    {
        if (broken)
            db::exec("CREATE TRIGGER break_save BEFORE INSERT ON commander_audit BEGIN SELECT RAISE(ABORT, 'cannot be saved'); END");
        else
            db::exec("DROP TRIGGER break_save");
    }

    // takes away what update 8 added, as a database of the previous version has it
    void rewindUpdate8()
    {
        for (const char *trigger : {"commander_round_finalized_complete", "commander_second_place_valid",
                                    "commander_final_enabled_frozen"})
            db::exec(QStringLiteral("DROP TRIGGER %1").arg(trigger));
        db::exec("DROP TABLE commander_final_players");
        db::exec("DROP TABLE commander_finals");
        db::exec("ALTER TABLE commander_events DROP COLUMN final_enabled");
        const QString current = db::value("SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'commander_pods'").toString();
        QString older = current;
        older.replace(QRegularExpression(",\\s*second_place_status TEXT CHECK \\(second_place_status IN \\([^)]*\\)\\)"), "");
        older.replace("CREATE TABLE commander_pods", "CREATE TABLE commander_pods_older");
        QVERIFY2(older != current && !older.contains("second_place_status"), qPrintable(older));
        const QString kept = "pod_id, round_id, tournament_id, pod_number, status, outcome, advancing_player_id, "
                             "result_version, reported_at, confirmed_at, second_place_player_id";
        db::exec("PRAGMA foreign_keys = OFF");
        db::exec(older);
        db::exec("INSERT INTO commander_pods_older SELECT " + kept + " FROM commander_pods");
        db::exec("DROP TABLE commander_pods");
        db::exec("ALTER TABLE commander_pods_older RENAME TO commander_pods");
        db::exec("CREATE INDEX idx_cmd_pods_round ON commander_pods(round_id)");
        db::exec("PRAGMA foreign_keys = ON");
        db::exec("DELETE FROM schema_migrations WHERE version IN (8, 9)");
    }

private slots:
    // commander-rules-4 qualifying scoring: win 5, clear second 2.5, other losses 0, draw 1, bye 5

    void winSecondLossDrawAndByeAddUpExactlyOverRounds()
    {
        auto [tid, pids] = makeEvent(5, 3);         // a pod of four and a bye every round
        QHash<qint64, double> expected;             // added up here from the club's values
        QHash<qint64, int> seconds, losses;
        for (int r = 1; r <= 3; ++r) {
            const QVariantMap rnd = currentRound(tid);
            const QVariantMap pod = podAt(rnd, 0);
            const QList<qint64> s = ids(pod["seats"].toList());
            expected[rnd["byes"].toList()[0].toMap()["player_id"].toLongLong()] += 5;
            if (r == 1) {
                cdb::reportPodResult(podId(pod), "WIN", s[0], {}, cdb::NONE, s[1]);
                expected[s[0]] += 5;
                expected[s[1]] += 2.5;
                ++seconds[s[1]];
                for (int i : {1, 2, 3})
                    ++losses[s[i]];
            } else if (r == 2) {
                cdb::reportPodResult(podId(pod), "DRAW", 0, {s[3]});         // s[3] was out: a loss
                for (int i : {0, 1, 2})
                    expected[s[i]] += 1;
                ++losses[s[3]];
            } else {
                cdb::reportPodResult(podId(pod), "WIN", s[2], {}, cdb::NONE, cdb::SecondPlace::noClearSecond());
                expected[s[2]] += 5;
                for (int i : {0, 1, 3})
                    ++losses[s[i]];
            }
            cdb::finalizeRound(rnd["round_id"].toLongLong());
            if (r < 3)
                cdb::publishNextRound(tid, r + 1);
        }
        double total = 0;
        for (qint64 p : pids) {
            const QVariantMap row = rowsOf(tid)[p];
            QCOMPARE(row["points"].toDouble(), expected.value(p));
            QCOMPARE(row["seconds"].toInt(), seconds.value(p));
            QCOMPARE(row["losses"].toInt(), losses.value(p));          // a clear second is still a loss
            QCOMPARE(db::value("SELECT match_points FROM enrollments WHERE tournament_id = ? AND player_id = ?",
                               {tid, p}).toDouble(), expected.value(p));
            total += row["points"].toDouble();
        }
        QCOMPARE(total, 3 * 5 + 5 + 2.5 + 3 * 1 + 5.0);
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));   // no final chosen: done after round 3
    }

    void secondPlaceIsPendingAPlayerOrConfirmedNone()
    {
        auto [tid, pids] = makeEvent(12, 2);
        const QVariantMap rnd = currentRound(tid);
        const qint64 rid = rnd["round_id"].toLongLong();
        const QVariantMap a = podAt(rnd, 0), b = podAt(rnd, 1), c = podAt(rnd, 2);
        const auto state = [](const QVariantMap &pod) {
            return db::value("SELECT second_place_status FROM commander_pods WHERE pod_id = ?", {podId(pod)});
        };
        QVERIFY(state(a).isNull());                                 // not reported: not applicable
        cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0));     // the winner only, as the quick Wins button does
        cdb::reportPodResult(podId(b), "WIN", seatPlayer(b, 0), {}, cdb::NONE, seatPlayer(b, 2));
        cdb::reportPodResult(podId(c), "DRAW", 0, {});
        QCOMPARE(state(a).toString(), QString("PENDING"));
        QCOMPARE(state(b).toString(), QString("PLAYER"));
        QVERIFY(state(c).isNull());                                 // a draw has no second place
        for (int i : {1, 2, 3})
            QCOMPARE(pts(tid, seatPlayer(a, i)), 0.0);              // nothing is awarded while it is pending
        QCOMPARE(pts(tid, seatPlayer(b, 2)), 2.5);
        const QVariantList pending = cdb::getState(tid)["second_pending"].toList();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending[0].toMap()["pod_id"].toLongLong(), podId(a));

        // the round cannot be finalized until it is decided, and asking changes nothing
        const QList<Rows> before = savedCommander(tid);
        try {
            cdb::finalizeRound(rid);
            QFAIL("a pending second place blocks the round");
        } catch (const cdb::CommanderError &e) {
            QVERIFY2(e.message.contains("Second place is not recorded yet"), qPrintable(e.message));
        }
        QCOMPARE(savedCommander(tid), before);
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_rounds SET status = 'FINALIZED' WHERE round_id = ?", {rid}));

        // confirmed: no clear second
        const int v = podAt(currentRound(tid), 0)["result_version"].toInt();
        cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, v, cdb::SecondPlace::noClearSecond(), "test correction");
        QCOMPARE(state(a).toString(), QString("NONE"));
        QVERIFY(db::value("SELECT second_place_player_id FROM commander_pods WHERE pod_id = ?", {podId(a)}).isNull());
        for (int i : {1, 2, 3})
            QCOMPARE(pts(tid, seatPlayer(a, i)), 0.0);
        const QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("SECOND_PLACE_RECORDED"));
        QCOMPARE(entry["detail"].toMap()["second_place_before_state"].toString(), QString("PENDING"));
        QCOMPARE(entry["detail"].toMap()["second_place_state"].toString(), QString("NONE"));
        QVERIFY(cdb::getState(tid)["second_pending"].toList().isEmpty());
        QVERIFY(cdb::finalizeRound(rid));
        restart();
        QCOMPARE(state(a).toString(), QString("NONE"));
        QCOMPARE(state(b).toString(), QString("PLAYER"));
        Q_UNUSED(pids);
    }

    void secondPlaceMustBeAnotherPlayerOfAWonPod()
    {
        auto [tid, pids] = makeEvent(5, 2);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap pod = podAt(rnd, 0);
        const QList<qint64> s = ids(pod["seats"].toList());
        const qint64 bye = rnd["byes"].toList()[0].toMap()["player_id"].toLongLong();
        const qint64 outsider = pdb::addPlayer("Not Here");
        const QList<Rows> before = savedCommander(tid);
        const auto refused = [&](const QString &outcome, qint64 winner, const cdb::SecondPlace &second, const char *why) {
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(podId(pod), outcome, winner, {}, cdb::NONE, second));
            QVERIFY2(savedCommander(tid) == before, why);
        };
        refused("WIN", s[0], s[0], "the winner as second");
        refused("WIN", s[0], bye, "the player with the bye");
        refused("WIN", s[0], outsider, "a player outside the event");
        refused("WIN", s[0], 987654, "an unknown id");
        refused("DRAW", 0, s[1], "a drawn pod");
        refused("DRAW", 0, cdb::SecondPlace::noClearSecond(), "a drawn pod, no clear second");

        const qint64 big = makeEvent(8, 1).first;
        const QVariantMap a = podAt(currentRound(big), 0), b = podAt(currentRound(big), 1);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, cdb::NONE, seatPlayer(b, 0)));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::correctResult(podId(a), "WIN", seatPlayer(a, 0), {}, {}, "test correction", seatPlayer(b, 1)));
        QCOMPARE(podAt(currentRound(big), 0)["status"].toString(), QString("PENDING"));
        // the database refuses a second place who did not lose in that pod
        cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, cdb::NONE, seatPlayer(a, 1));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_pods SET second_place_player_id = ? WHERE pod_id = ?",
                                                     {seatPlayer(b, 0), podId(a)}));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_pods SET second_place_status = 'NONE' WHERE pod_id = ?",
                                                     {podId(a)}));
        Q_UNUSED(pids);
    }

    void correctingAResultMovesTheAwardWithoutAddingUp()
    {
        auto [tid, pids] = makeEvent(4, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const qint64 id = podId(pod);
        const QList<qint64> s = ids(pod["seats"].toList());
        const auto points = [&] {
            QList<double> out;
            for (qint64 p : s)
                out << pts(tid, p);
            return out;
        };
        int v = cdb::reportPodResult(id, "WIN", s[0], {}, cdb::NONE, s[1]);
        QCOMPARE(points(), (QList<double>{5, 2.5, 0, 0}));
        QCOMPARE(cdb::reportPodResult(id, "WIN", s[0], {}, cdb::NONE, s[1]), v);       // a repeated save: once only
        QCOMPARE(cdb::reportPodResult(id, "WIN", s[0], {}, v, s[1], "test correction"), v);
        QCOMPARE(points(), (QList<double>{5, 2.5, 0, 0}));
        v = cdb::reportPodResult(id, "WIN", s[0], {}, v, s[2], "test correction");
        QCOMPARE(points(), (QList<double>{5, 0, 2.5, 0}));
        v = cdb::reportPodResult(id, "WIN", s[1], {}, v, s[0], "test correction");
        QCOMPARE(points(), (QList<double>{2.5, 5, 0, 0}));
        v = cdb::reportPodResult(id, "WIN", s[1], {}, v, cdb::SecondPlace::noClearSecond(), "test correction");
        QCOMPARE(points(), (QList<double>{0, 5, 0, 0}));
        v = cdb::reportPodResult(id, "DRAW", 0, {}, v, {}, "test correction");
        QCOMPARE(points(), (QList<double>{1, 1, 1, 1}));
        QVERIFY(db::value("SELECT second_place_status FROM commander_pods WHERE pod_id = ?", {id}).isNull());
        v = cdb::reportPodResult(id, "WIN", s[3], {}, v, s[2], "test correction");
        QCOMPARE(points(), (QList<double>{0, 0, 2.5, 5}));
        cdb::clearPodResult(id, v, "test correction");
        QCOMPARE(points(), (QList<double>{0, 0, 0, 0}));
        QVERIFY(db::value("SELECT second_place_player_id FROM commander_pods WHERE pod_id = ?", {id}).isNull());
        QCOMPARE(auditActions(tid).count("RESULT_REPORTED"), 1);
        QCOMPARE(auditActions(tid).count("RESULT_CORRECTED"), 5);
        QCOMPARE(auditActions(tid).count("RESULT_CLEARED"), 1);
        Q_UNUSED(pids);
    }

    void aSecondPlaceCorrectionInAFinalizedRoundIsAScoringChange()
    {
        auto [tid, pids] = makeEvent(8, 3);
        playWithSeconds(tid);
        cdb::publishNextRound(tid, 2);
        const QVariantMap pod = podAt(cdb::getRounds(tid)[0].toMap(), 0);
        const QList<qint64> s = ids(pod["seats"].toList());
        const QString round2 = db::value("SELECT pairing_inputs_json FROM commander_rounds WHERE tournament_id = ? "
                                         "AND round_number = 2", {tid}).toString();
        // once the round is final its second places are decided
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(podId(pod), "WIN", s[0], {}, "KEEP", "test correction", 0));
        // moving the award changes points, so round 2 needs a decision like any other score change
        QVERIFY_THROWS_EXCEPTION(cdb::ResolutionRequired, cdb::correctResult(podId(pod), "WIN", s[0], {}, {}, "test correction", s[2]));
        QCOMPARE(pts(tid, s[1]), 2.5);
        QVERIFY(cdb::correctResult(podId(pod), "WIN", s[0], {}, "KEEP", "slip misread", s[2]));
        QCOMPARE(pts(tid, s[1]), 0.0);
        QCOMPARE(pts(tid, s[2]), 2.5);
        QCOMPARE(db::value("SELECT pairing_inputs_json FROM commander_rounds WHERE tournament_id = ? AND round_number = 2",
                           {tid}).toString(), round2);          // kept as published
        const QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["second_place_before"].toLongLong(), s[1]);
        QCOMPARE(entry["detail"].toMap()["second_place"].toLongLong(), s[2]);
        QCOMPARE(entry["detail"].toMap()["resolution"].toString(), QString("KEEP"));
        Q_UNUSED(pids);
    }

    void theNextRoundIsPairedFromHalfPointStandings()
    {
        auto [tid, pids] = makeEvent(8, 2);
        playWithSeconds(tid);
        const QHash<qint64, QVariantMap> table = rowsOf(tid);
        cdb::publishNextRound(tid, 2);
        const Row row = db::one("SELECT pairing_inputs_json, pairing_seed FROM commander_rounds WHERE tournament_id = ? "
                                "AND round_number = 2", {tid});
        const QVariantMap inputs = QJsonDocument::fromJson(row["pairing_inputs_json"].toString().toUtf8()).object().toVariantMap();
        int halves = 0;
        for (const QVariant &p : inputs["players"].toList()) {
            const double points = p.toMap()["points"].toDouble();
            QCOMPARE(points, table[p.toMap()["player_id"].toLongLong()]["points"].toDouble());
            if (points == 2.5)
                ++halves;
        }
        QCOMPARE(halves, 2);                    // the two clear seconds go into pairing as 2.5, not 2
        const QVariantMap again = cmdr::pairRound(inputs, row["pairing_seed"].toLongLong());
        const QVariantList pods = currentRound(tid)["pods"].toList();
        for (int i = 0; i < pods.size(); ++i) {
            QList<qint64> rerun;
            for (const QVariant &p : again["pods"].toList()[i].toList())
                rerun << p.toLongLong();
            QCOMPARE(rerun, ids(pods[i].toMap()["seats"].toList()));
        }
        Q_UNUSED(pids);
    }

    void earlierEventsKeepSecondPlaceAsRecognitionOnly()
    {
        auto [tid, pids] = makeEarlierEvent(4, 2);
        QVERIFY(!cdb::getEvent(tid)["scores_second"].toBool());
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const QList<qint64> s = ids(pod["seats"].toList());
        int v = cdb::reportPodResult(podId(pod), "WIN", s[0], {}, cdb::NONE, s[1]);
        QCOMPARE(pts(tid, s[1]), 0.0);
        QVERIFY(db::value("SELECT second_place_status FROM commander_pods WHERE pod_id = ?", {podId(pod)}).isNull());
        // no clear second is not a separate state there: it is saved as nothing, and nothing blocks the round
        v = cdb::reportPodResult(podId(pod), "WIN", s[0], {}, v, cdb::SecondPlace::noClearSecond(), "test correction");
        QVERIFY(db::value("SELECT second_place_player_id FROM commander_pods WHERE pod_id = ?", {podId(pod)}).isNull());
        QVERIFY(db::value("SELECT second_place_status FROM commander_pods WHERE pod_id = ?", {podId(pod)}).isNull());
        QVERIFY(cdb::getState(tid)["second_pending"].toList().isEmpty());
        QVERIFY(cdb::finalizeRound(currentRound(tid)["round_id"].toLongLong()));
        // and earlier rules have no final
        const qint64 later = makeEarlierEvent(4, 2, false).first;
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::updateEventConfig(later, 0, {}, {}, 1));
        QVERIFY(!cdb::getEvent(later)["final_enabled"].toBool());
        // the scoring of a new event is the club's, not a setting
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError,
                                 cdb::createCommanderTournament("Custom", 8, 2, {{"second_place_points", 0}}));
        Q_UNUSED(pids);
    }

    void aRemovedPlayerKeepsTheirAwardsAndFinalPlace()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> seeds = candidateSeeds(tid);
        cdb::createFinal(tid, seeds);
        cdb::confirmFinal(tid, {{seeds[3]}, {seeds[2]}, {seeds[1]}}, "WIN", seeds[0], finalVersion(tid));
        const qint64 gone = seeds[2];
        const double points = pts(tid, gone);
        const int seconds = rowsOf(tid)[gone]["seconds"].toInt();
        const QString name = db::value("SELECT display_name FROM players WHERE player_id = ?", {gone}).toString();
        QVERIFY(pdb::removePlayer(gone));
        restart();
        QCOMPARE(pts(tid, gone), points);
        QCOMPARE(rowsOf(tid)[gone]["seconds"].toInt(), seconds);
        QCOMPARE(finalOrder(tid)[2], gone);
        QCOMPARE(db::value("SELECT final_placement FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tid, gone}).toInt(), 3);
        for (const QVariant &p : cdb::getFinal(tid)["players"].toList())
            if (p.toMap()["player_id"].toLongLong() == gone)
                QCOMPARE(p.toMap()["display_name"].toString(), name);
        Q_UNUSED(pids);
    }

    // the optional top-four final

    void withoutTheFinalTheEventEndsAfterItsQualifyingRounds()
    {
        auto [tid, pids] = makeEvent(8, 2);
        playWithSeconds(tid);
        cdb::publishNextRound(tid, 2);
        playWithSeconds(tid);
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(cdb::getRounds(tid).size(), 2);
        QVERIFY(cdb::getFinal(tid).isEmpty());
        QVERIFY(!cdb::getEvent(tid)["final_enabled"].toBool());
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::createFinal(tid, ids(cdb::getStandings(tid)).mid(0, 4)));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("INSERT INTO commander_finals (tournament_id, qualifying_json) "
                                                     "VALUES (?, '[]')", {tid}));
        Q_UNUSED(pids);
    }

    void withTheFinalTwoQualifyingRoundsAreFollowedByOneFinalPod()
    {
        auto [tid, pids] = qualified(8, 2);
        QVariantMap state = cdb::getState(tid);
        QCOMPARE(state["rounds"].toList().size(), 2);
        QCOMPARE(state["event"].toMap()["stage"].toString(), QString("SWISS"));       // not completed yet
        QCOMPARE(state["next"].toString(), QString("FINAL"));
        QVERIFY(state["final_standings"].isNull());
        QVERIFY(state["event"].toMap()["champion_player_id"].isNull());
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), QString("IN_PROGRESS"));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::publishNextRound(tid, 3));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finishTournament(tid));

        // the four best in the qualifying standings, best seed first
        const QVariantMap candidates = cdb::finalCandidates(tid);
        QVERIFY(candidates["ready"].toBool());
        const QList<qint64> seeds = candidateSeeds(tid);
        QCOMPARE(seeds, ids(cdb::getStandings(tid)).mid(0, 4));
        const qint64 finalId = cdb::createFinal(tid, seeds);
        QCOMPARE(cdb::createFinal(tid, seeds), finalId);                    // a repeated click
        QList<qint64> other = seeds;
        std::reverse(other.begin(), other.end());
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::createFinal(tid, other));

        const QVariantMap fin = cdb::getFinal(tid);
        QCOMPARE(fin["status"].toString(), QString("PLAYING"));
        QCOMPARE(finalSeedsOf(tid), seeds);
        const QVariantList players = fin["players"].toList();
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(players[i].toMap()["seed"].toInt(), i + 1);
            QCOMPARE(players[i].toMap()["seat"].toInt(), i + 1);            // seed 1 takes the first turn
            QCOMPARE(players[i].toMap()["standing"].toInt(), i + 1);
        }
        QCOMPARE(fin["qualifying"].toList().size(), 8);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_finals WHERE tournament_id = ?", {tid}).toInt(), 1);
        QCOMPARE(cdb::getRounds(tid).size(), 2);                             // the final is not another round
        state = cdb::getState(tid);
        QCOMPARE(state["next"].toString(), QString("FINAL_PLAYING"));
        const live::RoundStatus live = live::roundStatus(tid);
        QVERIFY(live.playing && live.finalRound && live.kind == timerdb::Kind::CommanderFinal);
        QCOMPARE(live.roundId, finalId);
        QVERIFY(timerdb::get(timerdb::Kind::CommanderFinal, finalId).valid);

        // no second final, and the option cannot change now
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("INSERT INTO commander_finals (tournament_id, qualifying_json) "
                                                     "VALUES (?, '[]')", {tid}));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::updateEventConfig(tid, 0, {}, {}, 0));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_events SET final_enabled = 0 WHERE tournament_id = ?", {tid}));
        const QVariantMap created = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(created["action"].toString(), QString("FINAL_CREATED"));
        QCOMPARE(created["detail"].toMap()["seeds"].toList().size(), 4);
        Q_UNUSED(pids);
    }

    void theFinalTakesTheBestFourStillInAndNeedsFour()
    {
        try {
            cdb::startEvent(makeEvent(3, 1, false, 1, {}, true).first);
            QFAIL("three players cannot play a top-four final");
        } catch (const cdb::CommanderError &e) {
            QVERIFY2(e.message.contains("four"), qPrintable(e.message));
        }
        auto [tid, pids] = makeEvent(8, 1, true, 12345, {}, true);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::createFinal(tid, {}));       // round 1 is not finalized
        QVERIFY(!cdb::finalCandidates(tid)["ready"].toBool());
        playWithSeconds(tid);
        const QList<qint64> table = ids(cdb::getStandings(tid));
        // the leader leaves after the last round: the next four are the finalists
        cdb::dropPlayer(tid, table[0]);
        QCOMPARE(candidateSeeds(tid), table.mid(1, 4));
        // four more leave: three players are still in
        for (int i : {1, 2, 3, 4})
            cdb::dropPlayer(tid, table[i]);
        const QVariantMap candidates = cdb::finalCandidates(tid);
        QVERIFY(!candidates["ready"].toBool());
        QCOMPARE(candidates["eligible"].toInt(), 3);
        QVERIFY(candidates["reason"].toString().contains("needs four"));
        QVERIFY(candidates["reason"].toString().contains("end the tournament early"));
        try {
            cdb::createFinal(tid, candidateSeeds(tid));
            QFAIL("three players cannot play the final");
        } catch (const cdb::CommanderError &e) {
            QCOMPARE(e.message, candidates["reason"].toString());
        }
        QVERIFY(cdb::getFinal(tid).isEmpty());
        QVERIFY(cdb::getEvent(tid)["champion_player_id"].isNull());          // no champion is made up
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("SWISS"));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::endSwissEarly(tid));
        // undoing two of the drops brings back enough players
        cdb::reinstatePlayer(tid, table[0]);
        cdb::reinstatePlayer(tid, table[1]);
        const QList<qint64> seeds = candidateSeeds(tid);
        QCOMPARE(seeds, (QList<qint64>{table[0], table[1], table[5], table[6]}));
        cdb::createFinal(tid, seeds);
        // with the final created a drop cannot be undone, and a finalist cannot be dropped
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reinstatePlayer(tid, table[2]));
        const QList<Rows> before = savedEvent(tid);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::dropPlayer(tid, table[0]));
        QCOMPARE(savedEvent(tid), before);
        // a player who did not reach the final can still leave; the final is not touched
        cdb::dropPlayer(tid, table[7]);
        QCOMPARE(finalSeedsOf(tid), seeds);
        Q_UNUSED(pids);
    }

    void fullyTiedQualifiersAreSeededByTheEventsFixedRandomOrder()
    {
        const qint64 seed = 4242;
        auto [tid, pids] = makeEvent(8, 1, true, seed, {}, true);
        for (const QVariant &pv : currentRound(tid)["pods"].toList())
            cdb::reportPodResult(podId(pv.toMap()), "DRAW", 0, {});      // everyone 1 point, 20% and 20%
        cdb::finalizeRound(currentRound(tid)["round_id"].toLongLong());
        // worked out here: the order is the SHA-256 of "<seed>:tiebreak:<player id>", smallest first
        const auto key = [seed](qint64 p) {
            return QCryptographicHash::hash(QStringLiteral("%1:tiebreak:%2").arg(seed).arg(p).toUtf8(), QCryptographicHash::Sha256)
                .toHex().left(12).toLongLong(nullptr, 16);
        };
        QList<qint64> byKey = pids;
        std::sort(byKey.begin(), byKey.end(), [&](qint64 a, qint64 b) { return key(a) < key(b); });
        for (const QVariant &r : cdb::getStandings(tid))
            QCOMPARE(r.toMap()["points"].toDouble(), 1.0);
        QCOMPARE(candidateSeeds(tid), byKey.mid(0, 4));
        cdb::createFinal(tid, byKey.mid(0, 4));
        QCOMPARE(finalSeedsOf(tid), byKey.mid(0, 4));
    }

    void simultaneousFinalCreationMakesOneFinal()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> seeds = candidateSeeds(tid);
        const qint64 t = tid;
        const Race r = race([t, seeds](int) { return cdb::createFinal(t, seeds); });
        QCOMPARE(r.results.size(), 8);
        QCOMPARE(QSet<qint64>(r.results.begin(), r.results.end()).size(), 1);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_finals WHERE tournament_id = ?", {tid}).toInt(), 1);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_final_players WHERE tournament_id = ?", {tid}).toInt(), 4);
        QCOMPARE(auditActions(tid).count("FINAL_CREATED"), 1);
        Q_UNUSED(pids);
    }

    // the example agreed with the club: Dan is out first (4th); Ben and Chris lose at the same
    // moment, Ben with the better qualifying seed is 2nd and Chris 3rd; Alex wins (1st).
    void theFinalPlacesByEliminationOrderAndTheRestFollowQualifying()
    {
        auto [tid, pids] = qualified(8, 2);
        const QList<QVariantList> qualifying = qualifyingFigures(tid);
        const QList<qint64> table = ids(cdb::getStandings(tid));
        const QList<qint64> seeds = candidateSeeds(tid);
        const qint64 alex = seeds[0], ben = seeds[1], chris = seeds[2], dan = seeds[3];
        cdb::createFinal(tid, seeds);
        const QVariantList snapshot = cdb::getFinal(tid)["qualifying"].toList();
        int v = cdb::saveFinalEliminations(tid, {{dan}}, finalVersion(tid));
        restart();                                              // what was recorded survives a restart
        QCOMPARE(cdb::getFinal(tid)["groups"].toList(), (QVariantList{QVariant(QVariantList{dan})}));
        QVERIFY(cdb::getFinal(tid)["begun"].toBool());
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::confirmFinal(tid, {{dan}}, "WIN", alex, v));   // Ben and Chris unaccounted for

        const QVariantList preview = cdb::previewFinal(tid, {{dan}, {ben, chris}}, "WIN", alex);
        QList<qint64> previewOrder;
        for (const QVariant &p : preview)
            previewOrder << p.toMap()["player_id"].toLongLong();
        QCOMPARE(previewOrder, (QList<qint64>{alex, ben, chris, dan}));
        QCOMPARE(cdb::getFinal(tid)["status"].toString(), QString("PLAYING"));      // a preview saves nothing

        v = cdb::confirmFinal(tid, {{dan}, {chris, ben}}, "WIN", alex, v);
        QStringList bases;
        QCOMPARE(finalOrder(tid, &bases), (QList<qint64>{alex, ben, chris, dan}));
        QCOMPARE(bases, (QStringList{"WINNER", "ELIMINATION", "ELIMINATION", "ELIMINATION"}));
        // places 1-4 from the final, then everyone else in qualifying order
        QList<qint64> expected{alex, ben, chris, dan};
        for (qint64 p : table)
            if (!expected.contains(p))
                expected << p;
        QCOMPARE(finalStandingIds(tid), expected);
        for (int i = 0; i < expected.size(); ++i)
            QCOMPARE(db::value("SELECT final_placement FROM enrollments WHERE tournament_id = ? AND player_id = ?",
                               {tid, expected[i]}).toInt(), i + 1);
        const QVariantMap ev = cdb::getEvent(tid);
        QCOMPARE(ev["stage"].toString(), QString("COMPLETE"));
        QCOMPARE(ev["champion_player_id"].toLongLong(), alex);
        QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::COMPLETED);
        // the final adds no points, wins or seconds, and its seeding snapshot stays as it was
        QCOMPARE(qualifyingFigures(tid), qualifying);
        QCOMPARE(cdb::getFinal(tid)["qualifying"].toList(), snapshot);
        QCOMPARE(cdb::getFinal(tid)["winner_player_id"].toLongLong(), alex);
        const QString before = stateJson(tid);
        restart();
        QCOMPARE(stateJson(tid), before);
        const QStringList actions = auditActions(tid);
        for (const char *a : {"FINAL_CREATED", "FINAL_ELIMINATIONS_RECORDED", "FINAL_CONFIRMED", "EVENT_COMPLETED"})
            QVERIFY2(actions.count(a) == 1, a);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::saveFinalEliminations(tid, {{dan}}));
        Q_UNUSED(pids);
    }

    void aDrawnFinalPlacesThoseStillInBySeedAndCorrectionsReplaceThePlaces()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> s = candidateSeeds(tid);
        cdb::createFinal(tid, s);
        const QHash<qint64, QVariantMap> before = rowsOf(tid);
        // drawn with all four still in: the seeds give every place, and nobody won the final
        int v = cdb::confirmFinal(tid, {}, "NO_WINNER", 0, finalVersion(tid));
        QStringList bases;
        QCOMPARE(finalOrder(tid, &bases), s);
        QCOMPARE(bases, (QStringList{"QUALIFYING_RANK", "QUALIFYING_RANK", "QUALIFYING_RANK", "QUALIFYING_RANK"}));
        QVERIFY(cdb::getFinal(tid)["winner_player_id"].isNull());
        QCOMPARE(cdb::getFinal(tid)["outcome"].toString(), QString("NO_WINNER"));
        QCOMPARE(cdb::getEvent(tid)["champion_player_id"].toLongLong(), s[0]);
        QCOMPARE(rowsOf(tid)[s[0]]["wins"], before[s[0]]["wins"]);           // first by seed is not a game won
        QCOMPARE(rowsOf(tid)[s[0]]["points"], before[s[0]]["points"]);
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["first_place_basis"].toString(), QString("QUALIFYING_RANK"));

        // corrected: the best seed was out first, then the other three drew
        v = cdb::confirmFinal(tid, {{s[0]}}, "NO_WINNER", 0, v, "seed 1 was out early");
        bases.clear();
        QCOMPARE(finalOrder(tid, &bases), (QList<qint64>{s[1], s[2], s[3], s[0]}));
        QCOMPARE(bases, (QStringList{"QUALIFYING_RANK", "QUALIFYING_RANK", "QUALIFYING_RANK", "ELIMINATION"}));
        const QVariantList audit = cdb::getAudit(tid);
        const auto entry = [&audit](const QString &action) {
            for (const QVariant &a : audit)
                if (a.toMap()["action"].toString() == action)
                    return a.toMap()["detail"].toMap();
            return QVariantMap();
        };
        QCOMPARE(entry("FINAL_CORRECTED")["reason"].toString(), QString("seed 1 was out early"));
        QCOMPARE(entry("FINAL_CORRECTED")["places_before"].toList(), (QVariantList{s[0], s[1], s[2], s[3]}));
        QCOMPARE(entry("FINAL_CORRECTED")["places"].toList(), (QVariantList{s[1], s[2], s[3], s[0]}));
        QCOMPARE(cdb::getEvent(tid)["champion_player_id"].toLongLong(), s[1]);
        QCOMPARE(finalStandingIds(tid).mid(0, 4), (QList<qint64>{s[1], s[2], s[3], s[0]}));

        // two still in at the end
        v = cdb::confirmFinal(tid, {{s[1]}, {s[0]}}, "NO_WINNER", 0, v, "two were still in");
        QCOMPARE(finalOrder(tid), (QList<qint64>{s[2], s[3], s[0], s[1]}));
        // the fourth seed won after the other three were knocked out together: the group comes before any seed
        v = cdb::confirmFinal(tid, {{s[0], s[1], s[2]}}, "WIN", s[3], v, "seed 4 won after all");
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::confirmFinal(tid, {}, "NO_WINNER", 0, v));      // a correction needs a reason
        bases.clear();
        QCOMPARE(finalOrder(tid, &bases), (QList<qint64>{s[3], s[0], s[1], s[2]}));
        QCOMPARE(bases.first(), QString("WINNER"));
        QCOMPARE(cdb::confirmFinal(tid, {{s[1], s[0], s[2]}}, "WIN", s[3], v), v);       // the same result again
        QVERIFY_THROWS_EXCEPTION(cdb::ConflictError, cdb::confirmFinal(tid, {}, "NO_WINNER", 0, v - 1));
        QCOMPARE(qualifyingFigures(tid).size(), 8);
        Q_UNUSED(pids);
    }

    // a knockout never removes every player still in: the last one left won, or the last ones went
    // out together and nobody did (the no-winner ending); the refusal says so and changes nothing,
    // the marker that play started included

    void aKnockoutOfEveryPlayerStillInIsRefusedAndPointsToNoWinner()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> s = candidateSeeds(tid);
        cdb::createFinal(tid, s);
        int v = cdb::saveFinalEliminations(tid, {{s[3]}}, finalVersion(tid));
        v = cdb::saveFinalEliminations(tid, {{s[3]}, {s[2], s[1]}}, v);              // two together; one player left
        const QVariant started = db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {tid});
        QVERIFY(!started.isNull());
        const QList<Rows> before = savedEvent(tid);
        const auto refusal = [&](const auto &fn) {
            try {
                fn();
            } catch (const cdb::CommanderError &e) {
                return e.message;
            }
            return QStringLiteral("not refused");
        };
        const auto pointsToNoWinner = [](const QString &why) {
            return why.contains("cannot remove every player still in") && why.contains("choose them as the winner")
                   && why.contains("No winner / Draw");
        };
        // the last player alone, as reporting
        QString why = refusal([&] { cdb::saveFinalEliminations(tid, {{s[3]}, {s[2], s[1]}, {s[0]}}, v); });
        QVERIFY2(pointsToNoWinner(why), qPrintable(why));
        // the last three together, even as a correction with a reason
        why = refusal([&] { cdb::saveFinalEliminations(tid, {{s[3]}, {s[2], s[1], s[0]}}, v, "they all went out"); });
        QVERIFY2(pointsToNoWinner(why), qPrintable(why));
        // and as the final's result
        why = refusal([&] { cdb::confirmFinal(tid, {{s[3]}, {s[2], s[1]}, {s[0]}}, "NO_WINNER", 0, v); });
        QVERIFY2(pointsToNoWinner(why), qPrintable(why));
        QVERIFY(savedEvent(tid) == before);
        db::closeThreadConnection();                                                   // read back from the file
        QVERIFY(savedEvent(tid) == before);
        QCOMPARE(db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {tid}), started);

        // the ways out: the player left is the winner, or the last two went out together and nobody won
        QVariantList places = cdb::previewFinal(tid, {{s[3]}, {s[2], s[1]}}, "WIN", s[0]);
        QCOMPARE(places[0].toMap()["player_id"].toLongLong(), s[0]);
        QCOMPARE(places[0].toMap()["basis"].toString(), QString("WINNER"));
        QCOMPARE(places[1].toMap()["player_id"].toLongLong(), s[1]);                  // out together: seed 2 above seed 3
        QCOMPARE(places[2].toMap()["player_id"].toLongLong(), s[2]);
        v = cdb::saveFinalEliminations(tid, {{s[3]}}, v, "the last three were recorded wrongly");
        places = cdb::previewFinal(tid, {{s[3]}}, "NO_WINNER");
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(places[i].toMap()["player_id"].toLongLong(), s[i]);
            QCOMPARE(places[i].toMap()["basis"].toString(), QString("QUALIFYING_RANK"));
        }
        QCOMPARE(places[3].toMap()["basis"].toString(), QString("ELIMINATION"));
        QCOMPARE(db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {tid}), started);
        Q_UNUSED(pids);
    }

    void contradictoryOrIncompleteFinalResultsAreRefused()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> s = candidateSeeds(tid);
        const qint64 finalId = cdb::createFinal(tid, s);
        qint64 outsider = 0;
        for (qint64 p : pids)
            if (!s.contains(p))
                outsider = p;
        const int v = finalVersion(tid);
        const QList<Rows> before = savedEvent(tid);
        const auto refused = [&](const char *why, auto fn) {
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, fn());
            QVERIFY2(savedEvent(tid) == before, why);
        };
        refused("a player outside the final", [&] { return cdb::saveFinalEliminations(tid, {{outsider}}, v); });
        refused("a player out twice", [&] { return cdb::saveFinalEliminations(tid, {{s[0]}, {s[0]}}, v); });
        refused("an empty group", [&] { return cdb::saveFinalEliminations(tid, {{}}, v); });
        refused("everyone out", [&] { return cdb::saveFinalEliminations(tid, {{s[0], s[1]}, {s[2], s[3]}}, v); });
        refused("the winner eliminated", [&] { return cdb::confirmFinal(tid, {{s[0]}, {s[1], s[2]}}, "WIN", s[0], v); });
        refused("players unaccounted for", [&] { return cdb::confirmFinal(tid, {{s[0]}}, "WIN", s[1], v); });
        refused("a draw with one player left", [&] { return cdb::confirmFinal(tid, {{s[0]}, {s[1], s[2]}}, "NO_WINNER", 0, v); });
        refused("no winner, yet a winner", [&] { return cdb::confirmFinal(tid, {}, "NO_WINNER", s[1], v); });
        refused("a winner outside the final", [&] { return cdb::confirmFinal(tid, {{s[0]}, {s[1]}, {s[2], s[3]}}, "WIN", outsider, v); });
        refused("an unknown ending", [&] { return cdb::confirmFinal(tid, {}, "TIE", 0, v); });
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::previewFinal(tid, {{s[0]}}, "WIN", s[1]));

        // the database refuses what the core never writes
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_finals SET status = 'CONFIRMED', outcome = 'NO_WINNER' "
                                                     "WHERE final_id = ?", {finalId}));          // nobody placed
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_finals SET outcome = 'WIN' WHERE final_id = ?", {finalId}));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_final_players SET placement = 1, placement_basis = 'WINNER' "
                                                     "WHERE final_id = ?", {finalId}));           // four first places
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("UPDATE commander_final_players SET seed = 4 WHERE final_id = ? "
                                                     "AND player_id = ?", {finalId, s[0]}));
        QVERIFY_THROWS_EXCEPTION(db::Error, db::exec("INSERT INTO commander_final_players (final_id, tournament_id, player_id, "
                                                     "seed, seat_number, qualifying_rank) VALUES (?, ?, ?, 4, 4, 9)",
                                                     {finalId, tid, outsider}));
        QCOMPARE(savedEvent(tid), before);
        // the final is no pod: the round functions have nothing of it to report or clear
        QCOMPARE(cdb::getRounds(tid).size(), 1);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_pods WHERE tournament_id = ?", {tid}).toInt(), 2);
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());
        QCOMPARE(db::value("PRAGMA integrity_check").toString(), QString("ok"));
    }

    // twelve players, one qualifying round: pods A and B each have a winner and a clear second,
    // pod C is drawn.  the four finalists come from A and B, so pod C alone can be corrected
    // without touching them, unless a pod C player wins.
    void qualifyingCorrectionsNeverQuietlyReseedTheFinal()
    {
        auto [tid, pids] = makeEvent(12, 1, true, 12345, {}, true);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap a = podAt(rnd, 0), b = podAt(rnd, 1), c = podAt(rnd, 2);
        const QList<qint64> cs = ids(c["seats"].toList());
        cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, cdb::NONE, seatPlayer(a, 1));
        cdb::reportPodResult(podId(b), "WIN", seatPlayer(b, 0), {}, cdb::NONE, seatPlayer(b, 1));
        cdb::reportPodResult(podId(c), "DRAW", 0, {cs[2], cs[3]});
        cdb::finalizeRound(rnd["round_id"].toLongLong());
        const QList<qint64> seeds = candidateSeeds(tid);
        QCOMPARE(QSet<qint64>(seeds.begin(), seeds.end()),
                 (QSet<qint64>{seatPlayer(a, 0), seatPlayer(a, 1), seatPlayer(b, 0), seatPlayer(b, 1)}));
        const qint64 finalId = cdb::createFinal(tid, seeds);

        // a correction that leaves the qualifiers and their seeds is saved; the final stays
        QVERIFY(cdb::correctResult(podId(c), "DRAW", 0, {cs[0], cs[1]}, {}, "who was out"));
        QCOMPARE(finalSeedsOf(tid), seeds);
        QCOMPARE(cdb::getFinal(tid)["final_id"].toLongLong(), finalId);

        // one that changes them offers to discard the final, and saves nothing until asked
        const double before = pts(tid, cs[0]);
        try {
            cdb::correctResult(podId(c), "WIN", cs[0], {}, {}, "test correction", cdb::SecondPlace::noClearSecond());
            QFAIL("the qualifiers would change");
        } catch (const cdb::FinalAffected &e) {
            QCOMPARE(e.seedsBefore, (QVariantList{seeds[0], seeds[1], seeds[2], seeds[3]}));
            QVERIFY(e.seedsAfter.contains(cs[0]));
        }
        QCOMPARE(pts(tid, cs[0]), before);
        QCOMPARE(finalSeedsOf(tid), seeds);

        // before play has started the organizer can discard the final explicitly
        QVERIFY(cdb::correctResult(podId(c), "WIN", cs[0], {}, "REBUILD_FINAL", "cs0 won pod C",
                                   cdb::SecondPlace::noClearSecond()));
        QVERIFY(cdb::getFinal(tid).isEmpty());
        QCOMPARE(cdb::getState(tid)["next"].toString(), QString("FINAL"));
        QVERIFY(candidateSeeds(tid).contains(cs[0]));
        const QVariantMap discarded = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(discarded["action"].toString(), QString("FINAL_DISCARDED"));
        QCOMPARE(discarded["detail"].toMap()["final_id"].toLongLong(), finalId);
        QCOMPARE(discarded["detail"].toMap()["reason"].toString(), QString("cs0 won pod C"));
        QVERIFY(discarded["detail"].toMap()["seeds_after"].toList().contains(cs[0]));
        const QList<qint64> newSeeds = candidateSeeds(tid);
        const qint64 newFinal = cdb::createFinal(tid, newSeeds);
        QVERIFY(finalSeedsOf(tid).contains(cs[0]));

        // once play has started the seeds are protected, even after the clock is reset and the
        // knockout is taken back: a correction that would reseed is refused outright
        timerdb::start(timerdb::Kind::CommanderFinal, newFinal);
        int v = cdb::saveFinalEliminations(tid, {{newSeeds[3]}}, finalVersion(tid));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::saveFinalEliminations(tid, {}, v));     // taking it back needs a reason
        v = cdb::saveFinalEliminations(tid, {}, v, "recorded for the wrong player");
        timerdb::reset(timerdb::Kind::CommanderFinal, newFinal);
        QVERIFY(cdb::getFinal(tid)["groups"].toList().isEmpty());
        QVERIFY(cdb::getFinal(tid)["begun"].toBool());
        const double cs1 = pts(tid, cs[1]);
        try {
            cdb::correctResult(podId(c), "WIN", cs[1], {}, "REBUILD_FINAL", "cs1 won instead", cdb::SecondPlace::noClearSecond());
            QFAIL("the final has begun");
        } catch (const cdb::FinalAffected &) {
            QFAIL("a final that has begun is never offered for discarding");
        } catch (const cdb::CommanderError &e) {
            QVERIFY2(e.message.contains("already begun"), qPrintable(e.message));
        }
        QCOMPARE(pts(tid, cs[1]), cs1);
        QCOMPARE(finalSeedsOf(tid), newSeeds);
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("FINAL_ELIMINATIONS_CORRECTED"));
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["reason"].toString(), QString("recorded for the wrong player"));
        Q_UNUSED(pids);
    }

    void afterTheFinalOnlyTheNonFinalistsCanMove()
    {
        auto [tid, pids] = makeEvent(12, 1, true, 12345, {}, true);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap a = podAt(rnd, 0), b = podAt(rnd, 1), c = podAt(rnd, 2);
        const QList<qint64> cs = ids(c["seats"].toList());
        cdb::reportPodResult(podId(a), "WIN", seatPlayer(a, 0), {}, cdb::NONE, seatPlayer(a, 1));
        cdb::reportPodResult(podId(b), "WIN", seatPlayer(b, 0), {}, cdb::NONE, seatPlayer(b, 1));
        cdb::reportPodResult(podId(c), "DRAW", 0, {cs[2], cs[3]});          // cs0 and cs1 draw: 1 point each
        cdb::finalizeRound(rnd["round_id"].toLongLong());
        const QList<qint64> s = candidateSeeds(tid);
        cdb::createFinal(tid, s);
        cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], finalVersion(tid));
        const auto place = [tid](qint64 p) {
            return db::value("SELECT final_placement FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tid, p}).toInt();
        };
        QCOMPARE(QSet<int>({place(cs[0]), place(cs[1])}), (QSet<int>{5, 6}));
        // corrected: cs2 and cs3 were the ones still in; they now follow the finalists
        QVERIFY(cdb::correctResult(podId(c), "DRAW", 0, {cs[0], cs[1]}, {}, "who was out"));
        QCOMPARE(finalOrder(tid), s);
        QCOMPARE(finalStandingIds(tid).mid(0, 4), s);
        QCOMPARE(QSet<int>({place(cs[2]), place(cs[3])}), (QSet<int>{5, 6}));
        QVERIFY(place(cs[0]) > 6 && place(cs[1]) > 6);
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["action"].toString(), QString("FINAL_STANDINGS_UPDATED"));
        // a correction that would change who played the final is refused
        const QList<Rows> before = savedEvent(tid);
        try {
            cdb::correctResult(podId(c), "WIN", cs[2], {}, "REBUILD_FINAL", "test correction", cdb::SecondPlace::noClearSecond());
            QFAIL("the final has been played");
        } catch (const cdb::CommanderError &e) {
            QVERIFY2(e.message.contains("been played"), qPrintable(e.message));
        }
        QCOMPARE(savedEvent(tid), before);
        Q_UNUSED(pids);
    }

    void endingEarlyDuringTheFinalLeavesItUnplacedAndReadOnly()
    {
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> s = candidateSeeds(tid);
        const qint64 finalId = cdb::createFinal(tid, s);
        timerdb::start(timerdb::Kind::CommanderFinal, finalId);
        const int v = cdb::saveFinalEliminations(tid, {{s[3]}}, finalVersion(tid));
        QVERIFY(tdb::terminateTournament(tid));
        const auto check = [&] {
            const QVariantMap fin = cdb::getFinal(tid);
            QCOMPARE(fin["status"].toString(), QString("PLAYING"));
            QVERIFY(fin["outcome"].isNull());
            for (const QVariant &p : fin["players"].toList())
                QVERIFY(p.toMap()["placement"].isNull());
            QCOMPARE(fin["groups"].toList(), (QVariantList{QVariant(QVariantList{s[3]})}));
            const QVariantMap ev = cdb::getEvent(tid);
            QCOMPARE(ev["stage"].toString(), QString("SWISS"));
            QVERIFY(ev["champion_player_id"].isNull());
            QVERIFY(ev["final_standings_json"].isNull());
            QCOMPARE(db::value("SELECT COUNT(*) FROM enrollments WHERE tournament_id = ? AND final_placement IS NOT NULL",
                               {tid}).toInt(), 0);
            QCOMPARE(tdb::tournamentById(tid)["status"].toString(), tdb::TERMINATED);
            QVERIFY(timerdb::get(timerdb::Kind::CommanderFinal, finalId).closed);
            QVERIFY(!live::roundStatus(tid).playing);
            QCOMPARE(cdb::progress(tid)["long"].toString(), QString("Ended early during the top-four final"));
        };
        check();
        QCOMPARE(db::value("SELECT timer_state FROM commander_finals WHERE final_id = ?", {finalId}).toString(), QString("ENDED"));
        const QVariantMap ended = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(ended["action"].toString(), QString("EVENT_TERMINATED"));
        QCOMPARE(ended["detail"].toMap()["final"].toString(), QString("PLAYING"));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::saveFinalEliminations(tid, {{s[3]}, {s[2]}}, v));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], v));
        restart();
        check();
        Q_UNUSED(pids);
    }

    void aFailedSaveLeavesNoScoringFinalOrPlacesBehind()
    {
        auto [tid, pids] = makeEvent(8, 1, true, 12345, {}, true);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        QList<Rows> before = savedEvent(tid);
        breakAuditInserts(true);
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE,
                                                                 seatPlayer(pod, 1)));
        QCOMPARE(savedEvent(tid), before);
        breakAuditInserts(false);
        playWithSeconds(tid);
        const QList<qint64> s = candidateSeeds(tid);

        before = savedEvent(tid);
        breakAuditInserts(true);
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::createFinal(tid, s));
        QCOMPARE(savedEvent(tid), before);
        breakAuditInserts(false);
        cdb::createFinal(tid, s);

        before = savedEvent(tid);
        const int v = finalVersion(tid);
        breakAuditInserts(true);
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::saveFinalEliminations(tid, {{s[3]}}, v));
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], v));
        QCOMPARE(savedEvent(tid), before);
        breakAuditInserts(false);
        // completing fails when the standings cannot be saved: the final stays unconfirmed
        db::exec("CREATE TRIGGER break_standings BEFORE UPDATE OF match_points ON enrollments "
                 "BEGIN SELECT RAISE(ABORT, 'standings cannot be saved'); END");
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], v));
        QCOMPARE(savedEvent(tid), before);
        db::exec("DROP TRIGGER break_standings");

        restart();                                      // what is on disk is what was there before the failures
        QCOMPARE(savedEvent(tid), before);
        QCOMPARE(cdb::getFinal(tid)["status"].toString(), QString("PLAYING"));
        cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], v);
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());
        QCOMPARE(db::value("PRAGMA integrity_check").toString(), QString("ok"));
        Q_UNUSED(pids);
    }

    void earlierEventsKeepTheirRulesAndRecordsThroughTheUpdate()
    {
        // a finished event, one in progress with a recognised second place, and a legacy playoff
        const qint64 done = makeEarlierEvent(8, 1).first;
        playRound(done);
        auto [open, b] = makeEarlierEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(open), 0);
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, cdb::NONE, seatPlayer(pod, 1));
        QList<qint64> top4;
        qint64 legacyPod = 0;
        const qint64 legacy = legacyEvent(true, &top4, &legacyPod);
        const auto figures = [this](qint64 t) { return qualifyingFigures(t); };
        const auto pods = [](qint64 t) {
            return db::query("SELECT pod_id, status, outcome, second_place_player_id FROM commander_pods "
                             "WHERE tournament_id = ? ORDER BY pod_id", {t});
        };
        const QList<QList<QVariantList>> standings{figures(done), figures(open), figures(legacy)};
        const QList<Rows> podRows{pods(done), pods(open), pods(legacy)};
        const QString doneSnapshot = cdb::getEvent(done)["final_standings_json"].toString();

        const auto backups = [] { return QDir(db::dataDir() + "/backups").entryList({"*.db"}, QDir::Files).size(); };
        const qsizetype backedUp = backups();
        rewindUpdate8();
        restart();
        // the update was backed up first, recorded, and left every record as it was
        QCOMPARE(backups(), backedUp + 1);
        QCOMPARE(db::value("SELECT name FROM schema_migrations WHERE version = 8").toString(),
                 QString("commander_scored_second_and_final"));
        QCOMPARE((QList<QList<QVariantList>>{figures(done), figures(open), figures(legacy)}), standings);
        QCOMPARE((QList<Rows>{pods(done), pods(open), pods(legacy)}), podRows);
        QCOMPARE(cdb::getEvent(done)["final_standings_json"].toString(), doneSnapshot);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_pods WHERE second_place_status IS NOT NULL").toInt(), 0);
        QCOMPARE(db::value("SELECT COUNT(*) FROM commander_events WHERE final_enabled <> 0").toInt(), 0);
        QVERIFY(db::query("PRAGMA foreign_key_check").isEmpty());

        // the unfinished earlier event goes on under its own rules: no points for second, nothing to decide
        QVERIFY(!cdb::getEvent(open)["scores_second"].toBool());
        QCOMPARE(pts(open, seatPlayer(pod, 1)), 0.0);
        playRound(open);
        QCOMPARE(currentRound(open)["status"].toString(), QString("FINALIZED"));
        // a legacy playoff is still its own read-only record, never a new final
        QVERIFY(cdb::getFinal(legacy).isEmpty());
        QVERIFY(!cdb::getEvent(legacy)["final_enabled"].toBool());
        QVERIFY(cdb::getEvent(legacy)["legacy_playoff"].toBool());
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::createFinal(legacy, top4));
        QVERIFY(cdb::getRounds(legacy).last().toMap()["legacy"].toBool());
        Q_UNUSED(b);
    }

    void finalAndSecondPlaceActionsAreLoggedByIdOnlyOnceSaved()
    {
        applog::setDirectory(QString());
        applog::setDirectory(tmp_->filePath("logs"));
        const qint64 tid = cdb::createCommanderTournament("Secret Final Cup", 4, 1, {}, 77, {}, 50, true);
        for (qint64 p : addPlayers(4, "Zed Hidden"))
            tdb::enrollPlayer(tid, p);
        cdb::startEvent(tid);
        const QVariantMap rnd = currentRound(tid);
        const QVariantMap pod = podAt(rnd, 0);
        const qint64 rid = rnd["round_id"].toLongLong();
        const int v = cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0));
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::finalizeRound(rid));
        cdb::reportPodResult(podId(pod), "WIN", seatPlayer(pod, 0), {}, v, seatPlayer(pod, 1), "test correction");
        cdb::finalizeRound(rid);
        const QList<qint64> s = candidateSeeds(tid);
        cdb::createFinal(tid, s);
        cdb::createFinal(tid, s);
        const int fv = cdb::saveFinalEliminations(tid, {{s[3]}}, finalVersion(tid));
        breakAuditInserts(true);
        QVERIFY_THROWS_EXCEPTION(db::Error, cdb::confirmFinal(tid, {{s[3]}, {s[1], s[2]}}, "WIN", s[0], fv));
        breakAuditInserts(false);
        cdb::confirmFinal(tid, {{s[3]}, {s[1], s[2]}}, "WIN", s[0], fv);

        const auto results = [&](const QString &action, const QString &ids) {
            QStringList out;
            static const QRegularExpression result(" status=([a-z-]+)");
            for (const QString &l : logged(" " + action + " "))
                if (l.contains(ids))
                    out << result.match(l).captured(1);
            return out.join(",");
        };
        const QString t = QStringLiteral("tournament=%1").arg(tid);
        QCOMPARE(results("tournament.create", t), QString("ok"));
        QVERIFY(logged(" tournament.create ").last().contains("rules=commander-rules-4"));
        QVERIFY(logged(" tournament.create ").last().contains("top_four_final=yes"));
        QCOMPARE(results("result.report", QStringLiteral("pod=%1 ").arg(podId(pod))), QString("attempt,ok,attempt,ok"));
        QVERIFY(logged(" result.report ").last().contains("second_state=player"));
        QCOMPARE(results("round.finalize", QStringLiteral("round=%1 ").arg(rid)), QString("attempt,failed,attempt,ok"));
        const QStringList blocked = logged(" round.blocked ");
        QCOMPARE(blocked.size(), 1);
        QVERIFY(blocked[0].contains("reason=second_place_pending") && blocked[0].contains(QStringLiteral("pods=%1").arg(podId(pod))));
        static const QRegularExpression op(" op=([0-9A-F]{8})");
        const QString failedFinalize = logged(" round.finalize ")[1];
        QCOMPARE(op.match(blocked[0]).captured(1), op.match(failedFinalize).captured(1));     // the same operation
        QCOMPARE(results("final.create", t), QString("attempt,ok,attempt,no-change"));
        QCOMPARE(results("final.eliminations", t), QString("attempt,ok"));
        QCOMPARE(results("final.confirm", t), QString("attempt,failed,attempt,ok"));
        QVERIFY(logged(" final.confirm ")[1].contains(" ref=R-"));
        // no entry names a field twice, and none of it names a player or the tournament
        static const QRegularExpression key(" ([a-z_]+)=");
        for (const QString &l : logLines()) {
            QStringList keys;
            auto it = key.globalMatch(l.section(" | ", 0, 0));
            while (it.hasNext())
                keys << it.next().captured(1);
            QVERIFY2(QSet<QString>(keys.begin(), keys.end()).size() == keys.size(), qPrintable(l));
        }
        const QString all = logLines().join('\n');
        for (const char *name : {"Secret", "Final Cup", "Zed", "Hidden"})
            QVERIFY2(!all.contains(name), name);
        // the audit history has each saved step once, and nothing from the failed attempt
        const QStringList actions = auditActions(tid);
        QCOMPARE(actions.count("FINAL_CONFIRMED"), 1);
        QCOMPARE(actions.count("FINAL_CREATED"), 1);
        QCOMPARE(actions.count("FINAL_ELIMINATIONS_RECORDED"), 1);
    }

    void theFinalIsSavedWhenTheLogFileCannotBeWritten()
    {
        QFile blocker(tmp_->filePath("not-a-folder"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        applog::setDirectory(tmp_->filePath("not-a-folder/logs"));
        auto [tid, pids] = qualified(8, 1);
        const QList<qint64> s = candidateSeeds(tid);
        cdb::createFinal(tid, s);
        cdb::confirmFinal(tid, {{s[3]}, {s[2]}, {s[1]}}, "WIN", s[0], finalVersion(tid));
        QCOMPARE(cdb::getEvent(tid)["stage"].toString(), QString("COMPLETE"));
        QVERIFY(!applog::status().available);
        Q_UNUSED(pids);
    }
    // usability pass: settings, corrections with reasons, and a final whose play has started

    void newEventsDefaultToEliminatedPlayersLosingAndKeepTheirChoice()
    {
        const qint64 fresh = makeEvent(4, 1, false).first;
        QCOMPARE(cdb::getEvent(fresh)["settings"].toMap()["draw_policy"].toString(), QString("ELIMINATED_LOSE"));
        const qint64 chosen = makeEvent(4, 1, false).first;
        cdb::updateEventConfig(chosen, 0, "ALL_DRAW");
        restart();
        QCOMPARE(cdb::getEvent(chosen)["settings"].toMap()["draw_policy"].toString(), QString("ALL_DRAW"));
        QCOMPARE(cdb::getEvent(fresh)["settings"].toMap()["draw_policy"].toString(), QString("ELIMINATED_LOSE"));
        // what the default awards in a drawn pod: a draw for each player still in, a loss for anyone already out
        cdb::startEvent(fresh);
        const QVariantMap pod = podAt(currentRound(fresh), 0);
        const QList<qint64> s = ids(pod["seats"].toList());
        cdb::reportPodResult(podId(pod), "DRAW", 0, {s[3]});
        QCOMPARE(pts(fresh, s[0]), 1.0);
        QCOMPARE(pts(fresh, s[2]), 1.0);
        QCOMPARE(pts(fresh, s[3]), 0.0);
    }

    void suggestedRoundsFollowCheckInsOnlyUntilTheOrganizerSetsThem()
    {
        auto [tid, pids] = makeEvent(8, 0, false);
        cdb::updateEventConfig(tid, 0, "ALL_DRAW", "HIGH", 1);       // other settings, not the rounds
        for (qint64 p : addPlayers(12, "More"))
            tdb::enrollPlayer(tid, p);
        cdb::syncRecommendation(tid);
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 4);     // not chosen: follows the 20 checked in
        QVERIFY(!cdb::getEvent(tid)["rounds_chosen"].toBool());
        cdb::updateEventConfig(tid, 2);                               // chosen
        QVERIFY(cdb::getEvent(tid)["rounds_chosen"].toBool());
        for (qint64 p : addPlayers(20, "Even more"))
            tdb::enrollPlayer(tid, p);
        cdb::syncRecommendation(tid);
        QCOMPARE(cdb::getEvent(tid)["swiss_rounds"].toInt(), 2);
        // "use suggested rounds" sets the suggestion for the 40 checked in, and changes nothing else
        QCOMPARE(cdb::useSuggestedRounds(tid), 5);
        const QVariantMap ev = cdb::getEvent(tid);
        QCOMPARE(ev["swiss_rounds"].toInt(), 5);
        QVERIFY(!ev["rounds_chosen"].toBool());
        QCOMPARE(ev["settings"].toMap()["draw_policy"].toString(), QString("ALL_DRAW"));
        QCOMPARE(ev["settings"].toMap()["three_pod_placement"].toString(), QString("HIGH"));
        QVERIFY(ev["final_enabled"].toBool());
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["rounds_before"].toInt(), 2);
        // without three checked in there is no suggestion, and nothing follows it
        const qint64 small = makeEvent(2, 3, false).first;
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::useSuggestedRounds(small));
        cdb::syncRecommendation(small);
        QCOMPARE(cdb::getEvent(small)["swiss_rounds"].toInt(), 3);
        cdb::startEvent(tid);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::useSuggestedRounds(tid));
        Q_UNUSED(pids);
    }

    void changingASavedPodResultNeedsAReasonAndFinishingASecondPlaceDoesNot()
    {
        auto [tid, pids] = makeEvent(8, 2);
        const QVariantMap pod = podAt(currentRound(tid), 0);
        const qint64 id = podId(pod);
        const QList<qint64> s = ids(pod["seats"].toList());
        int v = cdb::reportPodResult(id, "WIN", s[0]);                    // the quick Winner action
        v = cdb::reportPodResult(id, "WIN", s[0], {}, v, s[1]);           // finishing the pending second: reporting
        QVariantMap entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("SECOND_PLACE_RECORDED"));
        QVERIFY(!entry["detail"].toMap().contains("reason"));
        QCOMPARE(pts(tid, s[1]), 2.5);
        // anything else that changes it is a correction
        QList<Rows> before = savedCommander(tid);
        for (const QString &blank : {QString(), QString("   ")}) {
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(id, "WIN", s[0], {}, v, s[2], blank));
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::reportPodResult(id, "WIN", s[3], {}, v, s[1], blank));
            QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::clearPodResult(id, v, blank));
        }
        QCOMPARE(savedCommander(tid), before);
        v = cdb::reportPodResult(id, "WIN", s[0], {}, v, s[2], "  second was misheard ");
        entry = cdb::getAudit(tid)[0].toMap();
        QCOMPARE(entry["action"].toString(), QString("RESULT_CORRECTED"));
        QCOMPARE(entry["detail"].toMap()["reason"].toString(), QString("second was misheard"));
        QCOMPARE(entry["detail"].toMap()["second_place_before"].toLongLong(), s[1]);
        QCOMPARE(entry["detail"].toMap()["second_place"].toLongLong(), s[2]);
        QCOMPARE(pts(tid, s[1]), 0.0);
        QCOMPARE(pts(tid, s[2]), 2.5);
        cdb::clearPodResult(id, v, "reported in the wrong pod");
        QCOMPARE(cdb::getAudit(tid)[0].toMap()["detail"].toMap()["reason"].toString(), QString("reported in the wrong pod"));
        // a finalized round and a confirmed final need one as well
        playWithSeconds(tid);
        QVERIFY_THROWS_EXCEPTION(cdb::CommanderError, cdb::correctResult(id, "WIN", s[1], {}, {}, {}, s[0]));
        QVERIFY(cdb::correctResult(id, "WIN", s[1], {}, {}, "slip", s[0]));
        Q_UNUSED(pids);
    }

    void finalsThatAlreadyStartedAreMarkedByTheUpdate()
    {
        auto [played, a] = qualified(8, 1);
        const QList<qint64> seeds = candidateSeeds(played);
        cdb::createFinal(played, seeds);
        cdb::saveFinalEliminations(played, {{seeds[3]}}, finalVersion(played));
        auto [unplayed, b] = qualified(8, 1, 999);
        cdb::createFinal(unplayed, candidateSeeds(unplayed));
        // as the previous version left them: no mark yet
        db::exec("ALTER TABLE commander_finals DROP COLUMN play_started_at");
        db::exec("DELETE FROM schema_migrations WHERE version = 9");
        restart();
        QVERIFY(!db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {played}).isNull());
        QVERIFY(db::value("SELECT play_started_at FROM commander_finals WHERE tournament_id = ?", {unplayed}).isNull());
        QVERIFY(cdb::getFinal(played)["begun"].toBool());
        QVERIFY(!cdb::getFinal(unplayed)["begun"].toBool());
        // taking the knockout back with a reason leaves the final begun
        cdb::saveFinalEliminations(played, {}, finalVersion(played), "wrong player");
        QVERIFY(cdb::getFinal(played)["begun"].toBool());
        Q_UNUSED(a);
        Q_UNUSED(b);
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"
