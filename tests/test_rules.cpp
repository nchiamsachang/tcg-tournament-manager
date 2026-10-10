// Rule checks whose expected values were worked out by hand, and pairing rules checked on
// every round of many small events.
//
// test_core.cpp replays recorded events and covers the workflow; this file is the
// independent check: each expected figure below is derived in the comment beside it from
// the rules in swiss.cpp and docs/COMMANDER.md, not copied from the program's output.
// Every test runs in its own temporary database.
#include "commander.h"
#include "commander_db.h"
#include "db.h"
#include "store.h"

#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <random>

using db::Row;
using db::Rows;

namespace {

using Pair = QPair<qint64, qint64>;

Pair pairOf(qint64 a, qint64 b) { return {qMin(a, b), qMax(a, b)}; }

bool near(double a, double b) { return qAbs(a - b) < 1e-6; }

// A one-on-one match as it was played.  p2 == 0 is a bye.
struct Played {
    qint64 p1;
    qint64 p2;
    QString result;     // PLAYER1, PLAYER2 or DRAW
};

// True when `players` can all be paired without any pair in `played`.  Tries every pairing,
// so it is only for small fields.
bool rematchFreePairingExists(QList<qint64> players, const QSet<Pair> &played)
{
    if (players.isEmpty())
        return true;
    const qint64 first = players.takeFirst();
    for (int i = 0; i < players.size(); ++i) {
        if (played.contains(pairOf(first, players[i])))
            continue;
        QList<qint64> rest = players;
        rest.removeAt(i);
        if (rematchFreePairingExists(rest, played))
            return true;
    }
    return false;
}

} // namespace

class RulesTests : public QObject {
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> tmp_;

    QList<qint64> addPlayers(int n, const QString &prefix)
    {
        QList<qint64> out;
        for (int i = 0; i < n; ++i)
            out << pdb::addPlayer(QStringLiteral("%1%2").arg(prefix).arg(i + 1, 3, 10, QChar('0')));
        return out;
    }

    // A started one-on-one tournament with nobody paired yet, for rounds written by playFixedRound.
    qint64 fixedEvent(const QString &game, int rounds, const QList<qint64> &players)
    {
        const qint64 tid = tdb::createTournament("Worked example", game, rounds);
        for (qint64 p : players)
            tdb::enrollPlayer(tid, p);
        tdb::updateTournamentStatus(tid, tdb::IN_PROGRESS);
        return tid;
    }

    // Saves a round with exactly these pairings and results, instead of the random pairing.
    void playFixedRound(qint64 tid, int number, const QList<Played> &matches)
    {
        const qint64 rid = tdb::createRound(tid, number);
        int table = 0;
        for (const Played &m : matches) {
            if (!m.p2) {
                tdb::createMatch(rid, tid, m.p1);
                continue;
            }
            tdb::reportMatchResult(tdb::createMatch(rid, tid, m.p1, m.p2, ++table), m.result);
        }
    }

    static QHash<qint64, Row> byPlayer(const Rows &table)
    {
        QHash<qint64, Row> out;
        for (const Row &r : table)
            out.insert(r["player_id"].toLongLong(), r);
        return out;
    }

    // What is wrong with one standings row, or an empty string.
    static QString figureProblem(const Row &r, int points, int wins, int losses, int draws, double omw, double gw,
                                 double ogw, int standing)
    {
        QStringList wrong;
        const auto whole = [&](const char *key, int want) {
            if (r[key].toInt() != want)
                wrong << QStringLiteral("%1 is %2, expected %3").arg(key).arg(r[key].toInt()).arg(want);
        };
        const auto pct = [&](const char *key, double want) {
            if (!near(r[key].toDouble(), want))
                wrong << QStringLiteral("%1 is %2, expected %3").arg(key).arg(r[key].toDouble()).arg(want);
        };
        whole("match_points", points);
        whole("match_wins", wins);
        whole("match_losses", losses);
        whole("match_draws", draws);
        pct("omw_pct", omw);
        pct("gw_pct", gw);
        pct("ogw_pct", ogw);
        whole("standing", standing);
        return wrong.join("; ");
    }

    // What is wrong with a saved one-on-one round, or an empty string.  Every player in
    // `active` must be in exactly one match; an odd field has exactly one bye and an even
    // field none; played matches sit at tables 1..k.
    static QString roundProblem(qint64 roundId, QList<qint64> active)
    {
        QList<qint64> assigned;
        QList<int> tables;
        int byes = 0;
        for (const Row &m : tdb::roundPairings(roundId)) {
            assigned << m["player1_id"].toLongLong();
            if (m["player2_id"].isNull()) {
                ++byes;
                if (m["winner"].toString() != "BYE")
                    return "a bye is not recorded as a bye";
                continue;
            }
            if (m["player1_id"] == m["player2_id"])
                return "a player is paired against themselves";
            if (m["winner"].toString() != "PENDING")
                return "a new match already has a result";
            assigned << m["player2_id"].toLongLong();
            tables << m["table_number"].toInt();
        }
        std::sort(assigned.begin(), assigned.end());
        std::sort(active.begin(), active.end());
        if (assigned != active)
            return "not every active player is in exactly one match";
        if (byes != active.size() % 2)
            return QStringLiteral("%1 byes for %2 players").arg(byes).arg(active.size());
        std::sort(tables.begin(), tables.end());
        for (int i = 0; i < tables.size(); ++i)
            if (tables[i] != i + 1)
                return "table numbers are not 1..k";
        return {};
    }

    // Reports a result, chosen by `rng`, for every open match of the current round.
    static void reportRound(qint64 tid, std::mt19937 &rng, bool allowDraws = true)
    {
        static const QStringList results{"PLAYER1", "PLAYER2", "DRAW"};
        for (const Row &m : tdb::roundPairings(tdb::currentRound(tid)["round_id"].toLongLong()))
            if (!m["player2_id"].isNull())
                tdb::reportMatchResult(m["match_id"].toLongLong(), results[int(rng() % (allowDraws ? 3 : 2))]);
    }

    static QSet<Pair> pairsPlayed(qint64 tid)
    {
        QSet<Pair> out;
        for (const Row &m : tdb::allMatches(tid))
            if (!m["player2_id"].isNull())
                out.insert(pairOf(m["player1_id"].toLongLong(), m["player2_id"].toLongLong()));
        return out;
    }

    // Six players after two rounds, shared by the tie tests:
    //   round 1: A beats B, C beats D, E beats F      round 2: A beats C, B beats E, D beats F
    // A is 2-0, F is 0-2 and B, C, D, E are all 1-1.
    qint64 sixPlayerEvent(const QString &game, QList<qint64> *players)
    {
        *players = addPlayers(6, "S");
        const QList<qint64> &p = *players;
        const qint64 tid = fixedEvent(game, 2, p);
        playFixedRound(tid, 1, {{p[0], p[1], "PLAYER1"}, {p[2], p[3], "PLAYER1"}, {p[4], p[5], "PLAYER1"}});
        playFixedRound(tid, 2, {{p[0], p[2], "PLAYER1"}, {p[1], p[4], "PLAYER1"}, {p[3], p[5], "PLAYER1"}});
        return tid;
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

    // One-on-one standings

    // MTG (win 3, draw 1, floor 0.33), five players, two rounds:
    //   round 1: A beats B, C draws D, E has the bye
    //   round 2: A draws C, E beats D, B has the bye
    //
    //            record   points  match-win% for opponents (byes left out, floor 0.33)
    //   A        1-0-1    4       1/2 = 0.50
    //   B        0-1 +bye 3       0/1 -> 0.33
    //   C        0-0-2    2       0/2 -> 0.33
    //   D        0-1-1    1       0/2 -> 0.33
    //   E        1-0 +bye 6       1/1 = 1.00
    //
    //   OMW% = average of the opponents' figures:  A (B, C) 0.33   B (A) 0.50   C (D, A) 0.415
    //                                              D (C, E) 0.665  E (D) 0.33
    //   GW%  = wins / matches, a bye counting as a win:  A 1/2  B 1/2  C 0  D 0  E 2/2
    //   OGW% = average of the opponents' GW%, floor 0.33:  A (0.50, 0.33) 0.415   B (0.50) 0.50
    //          C (0.33, 0.50) 0.415   D (0.33, 1.00) 0.665   E (0.33) 0.33
    void mtgStandingsMatchAHandWorkedExampleWithADrawAndByes()
    {
        const QList<qint64> p = addPlayers(5, "H");
        const qint64 a = p[0], b = p[1], c = p[2], d = p[3], e = p[4];
        const qint64 tid = fixedEvent("MTG", 2, p);
        playFixedRound(tid, 1, {{a, b, "PLAYER1"}, {c, d, "DRAW"}, {e, 0, {}}});
        playFixedRound(tid, 2, {{a, c, "DRAW"}, {e, d, "PLAYER1"}, {b, 0, {}}});

        const QHash<qint64, Row> t = byPlayer(swiss::currentStandings(tid, "MTG"));
        QCOMPARE(t.size(), 5);
        //                                  pts W  L  D  OMW%   GW%  OGW%   place
        QCOMPARE(figureProblem(t[e], 6, 2, 0, 0, 0.33, 1.0, 0.33, 1), QString());
        QCOMPARE(figureProblem(t[a], 4, 1, 0, 1, 0.33, 0.5, 0.415, 2), QString());
        QCOMPARE(figureProblem(t[b], 3, 1, 1, 0, 0.5, 0.5, 0.5, 3), QString());
        QCOMPARE(figureProblem(t[c], 2, 0, 0, 2, 0.415, 0.0, 0.415, 4), QString());
        QCOMPARE(figureProblem(t[d], 1, 0, 1, 1, 0.665, 0.0, 0.665, 5), QString());

        // the table that is shown is the same one, and finishing saves those places
        const Rows shown = swiss::viewStandings(tid, "MTG");
        QList<qint64> order;
        for (const Row &r : shown)
            order << r["player_id"].toLongLong();
        QCOMPARE(order, (QList<qint64>{e, a, b, c, d}));
        const Rows final = swiss::finalizeTournament(tid, "MTG");
        for (int i = 0; i < final.size(); ++i) {
            QCOMPARE(final[i]["player_id"].toLongLong(), order[i]);
            QCOMPARE(final[i]["final_placement"].toInt(), i + 1);
        }
    }

    void aPointsTieIsBrokenByOpponentsMatchWinPercentage_data()
    {
        QTest::addColumn<QString>("game");
        QTest::addColumn<int>("winPoints");
        QTest::addColumn<double>("floor");
        QTest::newRow("MTG") << "MTG" << 3 << 0.33;
        QTest::newRow("One Piece") << "ONEPIECE" << 3 << 0.33;
        QTest::newRow("Pokemon") << "POKEMON" << 1 << 0.25;
    }

    // sixPlayerEvent: B, C, D and E all finish 1-1.  Match-win%: A 1.00, the four 0.50, F at
    // the floor.  B met A and E, and C met D and A: OMW% (1.00 + 0.50) / 2 = 0.75.  D met C
    // and F, and E met F and B: OMW% (0.50 + floor) / 2.  So B and C place above D and E.
    void aPointsTieIsBrokenByOpponentsMatchWinPercentage()
    {
        QFETCH(QString, game);
        QFETCH(int, winPoints);
        QFETCH(double, floor);
        QCOMPARE(swiss::points(game).win, winPoints);
        QCOMPARE(swiss::omwFloor(game), floor);
        QList<qint64> p;
        const qint64 tid = sixPlayerEvent(game, &p);
        const QHash<qint64, Row> t = byPlayer(swiss::currentStandings(tid, game));

        QCOMPARE(t[p[0]]["match_points"].toInt(), 2 * winPoints);
        QCOMPARE(t[p[5]]["match_points"].toInt(), 0);
        for (int i : {1, 2, 3, 4})
            QCOMPARE(t[p[i]]["match_points"].toInt(), winPoints);
        QVERIFY(near(t[p[0]]["omw_pct"].toDouble(), 0.5));
        QVERIFY(near(t[p[5]]["omw_pct"].toDouble(), 0.5));
        for (int i : {1, 2})
            QVERIFY(near(t[p[i]]["omw_pct"].toDouble(), 0.75));
        for (int i : {3, 4})
            QVERIFY(near(t[p[i]]["omw_pct"].toDouble(), (0.5 + floor) / 2));

        QCOMPARE(t[p[0]]["standing"].toInt(), 1);
        QCOMPARE(t[p[5]]["standing"].toInt(), 6);
        for (int i : {1, 2})
            QVERIFY2(t[p[i]]["standing"].toInt() == 2 || t[p[i]]["standing"].toInt() == 3, "B and C take places 2 and 3");
        for (int i : {3, 4})
            QVERIFY2(t[p[i]]["standing"].toInt() == 4 || t[p[i]]["standing"].toInt() == 5, "D and E take places 4 and 5");
    }

    // The order of the tiebreakers after match points, from swiss::tiebreakColumns:
    //   MTG        OMW%, GW%, OGW%
    //   One Piece  OMW%, GW%
    //   Pokemon    OMW% ("Opp Win%"), then "Opp Opp Win%" (kept in the ogw_pct column)
    // The figures are written straight into the registrations so each key is decided alone.
    void tiebreakersApplyInEachGamesConfiguredOrder()
    {
        for (const QString &game : tdb::supportedGames()) {
            const QList<qint64> p = addPlayers(5, game.left(1));
            const qint64 tid = tdb::createTournament("Order", game, 1);
            const auto set = [&](int i, int points, double omw, double gw, double ogw) {
                tdb::enrollPlayer(tid, p[i]);
                tdb::updateEnrollmentStats(tid, p[i], {{"match_points", points}, {"omw_pct", omw}, {"gw_pct", gw},
                                                       {"ogw_pct", ogw}});
            };
            set(0, 3, 0.5, 0.5, 0.4);
            set(1, 3, 0.5, 0.4, 0.9);
            set(2, 3, 0.6, 0.1, 0.1);       // best OMW% of the players on 3 points
            set(3, 6, 0.1, 0.1, 0.1);       // most points: first whatever the tiebreakers say
            set(4, 3, 0.5, 0.5, 0.7);
            QList<qint64> order;
            for (const Row &r : swiss::standings(tid, game))
                order << r["player_id"].toLongLong();
            const QByteArray tag = game.toUtf8();
            QVERIFY2(order.mid(0, 2) == (QList<qint64>{p[3], p[2]}), tag);
            if (game == "MTG") {
                QVERIFY2(order.mid(2) == (QList<qint64>{p[4], p[0], p[1]}), tag);      // GW% 0.5, 0.5, 0.4; then OGW% 0.7 over 0.4
            } else if (game == "ONEPIECE") {
                QVERIFY2(order.last() == p[1], tag);                                    // GW% 0.4 is last; OGW% is not used
            } else {
                QVERIFY2(order.mid(2) == (QList<qint64>{p[1], p[4], p[0]}), tag);      // Opp Opp Win% 0.9, 0.7, 0.4; GW% is not used
            }
        }
    }

    // BUG-002 (closed): Pokemon's second tiebreaker was listed and sorted on but never worked
    // out.  Club policy is now opponents' opponents' win percentage: the average of each
    // opponent's own Opp Win%.  Worked by hand, six players, two rounds, no byes, floor 0.25:
    //
    //   round 1: A beats B, D beats C, E beats F      round 2: C beats A, E beats B, F beats D
    //
    //        record  points  win rate   opponents  Opp Win%                 Opp Opp Win%
    //   A    1-1     1       0.50       B, C       (0.25 + 0.50) / 2 0.375  (0.75  + 0.50)  / 2 = 0.625
    //   B    0-2     0       0 -> 0.25  A, E       (0.50 + 1.00) / 2 0.75   (0.375 + 0.375) / 2 = 0.375
    //   C    1-1     1       0.50       D, A       (0.50 + 0.50) / 2 0.50   (0.50  + 0.375) / 2 = 0.4375
    //   D    1-1     1       0.50       C, F       (0.50 + 0.50) / 2 0.50   (0.50  + 0.75)  / 2 = 0.625
    //   E    2-0     2       1.00       F, B       (0.50 + 0.25) / 2 0.375  (0.75  + 0.75)  / 2 = 0.75
    //   F    1-1     1       0.50       E, D       (1.00 + 0.50) / 2 0.75   (0.375 + 0.50)  / 2 = 0.4375
    //
    // C and D are level on points (1) and on Opp Win% (0.50); D is ahead on Opp Opp Win%.
    // Final order: E, F, D, C, A, B.  The figure Magic keeps in the same column (the average
    // of the opponents' own win rates) is 0.50 for both C and D, so it could not separate them.
    qint64 oppOppEvent(const QString &game, QList<qint64> *players)
    {
        *players = addPlayers(6, "O");
        const QList<qint64> &p = *players;
        const qint64 tid = fixedEvent(game, 2, p);
        playFixedRound(tid, 1, {{p[0], p[1], "PLAYER1"}, {p[3], p[2], "PLAYER1"}, {p[4], p[5], "PLAYER1"}});
        playFixedRound(tid, 2, {{p[2], p[0], "PLAYER1"}, {p[4], p[1], "PLAYER1"}, {p[5], p[3], "PLAYER1"}});
        return tid;
    }

    void pokemonTiesOnOppWinAreBrokenByOppOppWin()
    {
        QCOMPARE(swiss::tiebreakColumns("POKEMON")[1].first, QString("ogw_pct"));
        QCOMPARE(swiss::tiebreakColumns("POKEMON")[1].second, QString("Opp Opp Win%"));
        QCOMPARE(swiss::points("POKEMON").win, 1);          // club scoring is unchanged
        QCOMPARE(swiss::points("POKEMON").draw, 0);
        QList<qint64> p;
        const qint64 tid = oppOppEvent("POKEMON", &p);
        const Rows table = swiss::currentStandings(tid, "POKEMON");
        const QHash<qint64, Row> t = byPlayer(table);

        const int points[] = {1, 0, 1, 1, 2, 1};
        const double oppWin[] = {0.375, 0.75, 0.5, 0.5, 0.375, 0.75};
        const double oppOpp[] = {0.625, 0.375, 0.4375, 0.625, 0.75, 0.4375};
        for (int i = 0; i < 6; ++i) {
            const QByteArray who = QByteArray(1, char('A' + i));
            QVERIFY2(t[p[i]]["match_points"].toInt() == points[i], who);
            QVERIFY2(near(t[p[i]]["omw_pct"].toDouble(), oppWin[i]), who);
            QVERIFY2(near(t[p[i]]["ogw_pct"].toDouble(), oppOpp[i]), who);
        }
        // C and D: the same points and Opp Win%, and D places above C
        QCOMPARE(t[p[2]]["match_points"], t[p[3]]["match_points"]);
        QCOMPARE(t[p[2]]["omw_pct"].toDouble(), t[p[3]]["omw_pct"].toDouble());
        QList<qint64> order;
        for (const Row &row : table)
            order << row["player_id"].toLongLong();
        QCOMPARE(order, (QList<qint64>{p[4], p[5], p[3], p[2], p[0], p[1]}));
        // 0.4375 is kept as it is, not as the 0.438 that is shown
        QCOMPARE(db::value("SELECT ogw_pct FROM enrollments WHERE tournament_id = ? AND player_id = ?", {tid, p[2]}).toDouble(), 0.4375);
        QCOMPARE(swiss::tiebreakText("POKEMON", "ogw_pct", 0.4375), QString("0.438"));

        // the same matches under Magic's rules: its figure in that column is level for C and D
        QList<qint64> m;
        const qint64 magic = oppOppEvent("MTG", &m);
        const QHash<qint64, Row> mt = byPlayer(swiss::currentStandings(magic, "MTG"));
        QVERIFY(near(mt[m[2]]["ogw_pct"].toDouble(), 0.5));
        QVERIFY(near(mt[m[3]]["ogw_pct"].toDouble(), 0.5));
        QVERIFY(near(mt[m[2]]["omw_pct"].toDouble(), mt[m[3]]["omw_pct"].toDouble()));
        QVERIFY(near(mt[m[2]]["gw_pct"].toDouble(), mt[m[3]]["gw_pct"].toDouble()));
    }

    // Byes and a repeated opponent.  Three players, floor 0.25:
    //   round 1: A beats B (C has the bye)   round 2: B beats A (C bye)   round 3: C beats A (B bye)
    // Win rate, byes left out: A 1/3, B 1/2, C 1/1.  A bye is not an opponent; B met A twice.
    //   Opp Win%      A (B, B, C) (0.5 + 0.5 + 1) / 3 = 0.6667   B (A, A) 0.3333   C (A) 0.3333
    //   Opp Opp Win%  A (0.3333 + 0.3333 + 0.3333) / 3 = 0.3333   B (A, A) 0.6667   C (A) 0.6667
    // Were B counted once for A, A's Opp Win% would be 0.75 and B's and C's second figure too.
    void pokemonOppOppWinLeavesOutByesAndCountsARematchTwice()
    {
        const QList<qint64> p = addPlayers(3, "R");
        const qint64 a = p[0], b = p[1], c = p[2];
        const qint64 tid = fixedEvent("POKEMON", 3, p);
        playFixedRound(tid, 1, {{a, b, "PLAYER1"}, {c, 0, {}}});
        playFixedRound(tid, 2, {{b, a, "PLAYER1"}, {c, 0, {}}});
        playFixedRound(tid, 3, {{c, a, "PLAYER1"}, {b, 0, {}}});
        const QHash<qint64, Row> t = byPlayer(swiss::currentStandings(tid, "POKEMON"));
        QVERIFY(near(t[a]["omw_pct"].toDouble(), 2.0 / 3));
        QVERIFY(near(t[b]["omw_pct"].toDouble(), 1.0 / 3));
        QVERIFY(near(t[c]["omw_pct"].toDouble(), 1.0 / 3));
        QVERIFY(near(t[a]["ogw_pct"].toDouble(), 1.0 / 3));
        QVERIFY(near(t[b]["ogw_pct"].toDouble(), 2.0 / 3));
        QVERIFY(near(t[c]["ogw_pct"].toDouble(), 2.0 / 3));
    }

    // Nobody to compare with: a player whose only round was a bye, and an event with no
    // matches yet, get the floor for both figures.  One round: A beats B, C has the bye.
    //   A: Opp Win% = B's 0 -> 0.25; Opp Opp Win% = B's Opp Win% = A's win rate 1.00
    //   B: Opp Win% = 1.00;          Opp Opp Win% = A's Opp Win% = 0.25
    void pokemonPlayerWithNoOpponentsGetsTheFloor()
    {
        const QList<qint64> p = addPlayers(3, "N");
        const qint64 tid = fixedEvent("POKEMON", 1, p);
        for (const Row &row : swiss::viewStandings(tid, "POKEMON")) {       // nothing played yet
            QCOMPARE(row["omw_pct"].toDouble(), 0.25);
            QCOMPARE(row["ogw_pct"].toDouble(), 0.25);
        }
        playFixedRound(tid, 1, {{p[0], p[1], "PLAYER1"}, {p[2], 0, {}}});
        const QHash<qint64, Row> t = byPlayer(swiss::currentStandings(tid, "POKEMON"));
        QCOMPARE(t[p[2]]["omw_pct"].toDouble(), 0.25);
        QCOMPARE(t[p[2]]["ogw_pct"].toDouble(), 0.25);
        QVERIFY(near(t[p[0]]["omw_pct"].toDouble(), 0.25) && near(t[p[0]]["ogw_pct"].toDouble(), 1.0));
        QVERIFY(near(t[p[1]]["omw_pct"].toDouble(), 1.0) && near(t[p[1]]["ogw_pct"].toDouble(), 0.25));
    }

    // Values are compared as they are kept (nine-decimal precision), not as they are shown
    // (three): 0.43751 and 0.43752 both read 0.438 on screen and the larger still places first.
    void pokemonTiebreakersAreNotRoundedForDisplayBeforeComparing()
    {
        const QList<qint64> p = addPlayers(2, "D");
        const qint64 tid = tdb::createTournament("Close", "POKEMON", 1);
        tdb::enrollPlayer(tid, p[0]);
        tdb::enrollPlayer(tid, p[1]);
        tdb::updateEnrollmentStats(tid, p[0], {{"match_points", 1}, {"omw_pct", 0.5}, {"ogw_pct", 0.43751}});
        tdb::updateEnrollmentStats(tid, p[1], {{"match_points", 1}, {"omw_pct", 0.5}, {"ogw_pct", 0.43752}});
        QCOMPARE(swiss::tiebreakText("POKEMON", "ogw_pct", 0.43751), swiss::tiebreakText("POKEMON", "ogw_pct", 0.43752));
        QCOMPARE(swiss::standings(tid, "POKEMON").first()["player_id"].toLongLong(), p[1]);
    }

    // Compatibility.  A Pokemon tournament finished before the figure existed has 0 saved for
    // it and placings decided without it: both stay exactly as saved.  One still being played
    // shows the figure at once, and it is saved with the next result.
    void pokemonOppOppWinLeavesFinishedTournamentsAloneAndReachesLiveOnes()
    {
        const auto saved = [](qint64 tid) {
            return db::query("SELECT player_id, match_points, omw_pct, gw_pct, ogw_pct, final_placement FROM enrollments "
                             "WHERE tournament_id = ? ORDER BY player_id", {tid});
        };
        // finished the old way: C placed above D, nothing in the column
        QList<qint64> p;
        const qint64 old = oppOppEvent("POKEMON", &p);
        swiss::calculateTiebreakers(old, "POKEMON");
        const QList<qint64> oldOrder{p[4], p[5], p[2], p[3], p[0], p[1]};
        for (int i = 0; i < oldOrder.size(); ++i)
            db::exec("UPDATE enrollments SET ogw_pct = 0, final_placement = ? WHERE tournament_id = ? AND player_id = ?",
                     {i + 1, old, oldOrder[i]});
        tdb::updateTournamentStatus(old, tdb::COMPLETED);
        const Rows before = saved(old);
        const Rows shown = swiss::viewStandings(old, "POKEMON");
        for (int i = 0; i < shown.size(); ++i) {
            QCOMPARE(shown[i]["player_id"].toLongLong(), oldOrder[i]);
            QCOMPARE(shown[i]["standing"].toInt(), i + 1);
            QCOMPARE(shown[i]["ogw_pct"].toDouble(), 0.0);
            QCOMPARE(swiss::tiebreakText("POKEMON", "ogw_pct", shown[i]["ogw_pct"].toDouble()), QString::fromUtf8("\xe2\x80\x94"));
        }
        QCOMPARE(saved(old), before);
        // Magic's column of the same name can really be shown as a number
        QCOMPARE(swiss::tiebreakText("MTG", "ogw_pct", 0.0), QString("0.000"));

        // still being played: nothing saved for it yet (the results were written the old way)
        QList<qint64> q;
        const qint64 live = oppOppEvent("POKEMON", &q);
        for (const Row &row : saved(live))
            QCOMPARE(row["ogw_pct"].toDouble(), 0.0);
        const Rows liveBefore = saved(live);
        QList<qint64> order;
        for (const Row &row : swiss::viewStandings(live, "POKEMON"))
            order << row["player_id"].toLongLong();
        QCOMPARE(order, (QList<qint64>{q[4], q[5], q[3], q[2], q[0], q[1]}));    // shown with the figure
        QCOMPARE(saved(live), liveBefore);                                        // and looking wrote nothing
        // the next result that is saved (here the same one again) saves the figures with it
        const Row match = tdb::roundPairings(tdb::currentRound(live)["round_id"].toLongLong()).first();
        swiss::reportResult(match["match_id"].toLongLong(), match["result"].toString());
        const QHash<qint64, Row> now = byPlayer(saved(live));
        QVERIFY(near(now[q[3]]["ogw_pct"].toDouble(), 0.625));
        QVERIFY(near(now[q[2]]["ogw_pct"].toDouble(), 0.4375));
    }

    // One-on-one pairing

    void everyOneOnOneRoundAssignsEachActivePlayerExactlyOnce()
    {
        std::mt19937 results(20261009);
        int event = 0;
        for (int n : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 32, 33}) {
            const QString game = tdb::supportedGames()[event++ % tdb::supportedGames().size()];
            const int rounds = swiss::suggestedRounds(n);
            const QByteArray tag = QStringLiteral("%1 players, %2").arg(n).arg(game).toUtf8();
            const qint64 tid = tdb::createTournament(QStringLiteral("Field of %1").arg(n), game, rounds);
            const QList<qint64> pids = addPlayers(n, QStringLiteral("F%1-").arg(n));
            for (qint64 p : pids)
                tdb::enrollPlayer(tid, p);
            swiss::startTournament(tid);
            for (int r = 1; r <= rounds; ++r) {
                const Row rnd = tdb::currentRound(tid);
                QVERIFY2(rnd["round_number"].toInt() == r, tag);
                const QString problem = roundProblem(rnd["round_id"].toLongLong(), pids);
                QVERIFY2(problem.isEmpty(), tag + " round " + QByteArray::number(r) + ": " + problem.toUtf8());
                reportRound(tid, results);
                if (r < rounds)
                    swiss::advanceToNextRound(tid, r);
            }
            // the configured rounds are the whole event, and everyone gets one place
            QVERIFY_THROWS_EXCEPTION(swiss::RuleError, swiss::advanceToNextRound(tid, rounds));
            const Rows final = swiss::finalizeTournament(tid, game);
            QVERIFY2(tdb::rounds(tid).size() == rounds, tag);
            QVERIFY2(final.size() == n, tag);
            for (int i = 0; i < final.size(); ++i)
                QVERIFY2(final[i]["final_placement"].toInt() == i + 1, tag);
        }
    }

    // The bye goes to the lowest-placed player who has not had one, so in an odd field
    // playing as many rounds as it has players everybody sits out exactly once.
    void nobodyGetsASecondByeBeforeEveryoneHasHadOne()
    {
        std::mt19937 results(7);
        for (int n : {3, 5, 7}) {
            const qint64 tid = tdb::createTournament(QStringLiteral("Byes %1").arg(n), "MTG", n);
            const QList<qint64> pids = addPlayers(n, QStringLiteral("B%1-").arg(n));
            for (qint64 p : pids)
                tdb::enrollPlayer(tid, p);
            swiss::startTournament(tid);
            for (int r = 1; r <= n; ++r) {
                reportRound(tid, results);
                if (r < n)
                    swiss::advanceToNextRound(tid, r);
            }
            QHash<qint64, int> byes;
            for (const Row &m : tdb::allMatches(tid))
                if (m["winner"].toString() == "BYE")
                    byes[m["player1_id"].toLongLong()] += 1;
            for (qint64 p : pids)
                QVERIFY2(byes.value(p) == 1, qPrintable(QStringLiteral("%1 players: a player had %2 byes").arg(n).arg(byes.value(p))));
        }
    }

    // Regression test for docs/qa/BUG_LOG.md BUG-001.  Six players (MTG) after two rounds:
    //   round 1: P1 beats P3, P2 draws P4, P5 draws P6
    //   round 2: P1 draws P4, P2 beats P5, P3 beats P6
    // Points: P1 4, P2 4, P3 3, P4 2, P5 1, P6 1.  Pairing down the standings takes P1-P2 and
    // P3-P4, both new, and leaves P5-P6, who met in round 1.  A round with no rematch exists
    // (P1-P2, P3-P5, P4-P6), so round 3 must not repeat a match.  The players tied on points
    // are next to each other either way round, so the shuffle between them cannot hide it.
    void anAvoidableRematchIsNotPaired()
    {
        for (int attempt = 0; attempt < 5; ++attempt) {
            const QList<qint64> p = addPlayers(6, QStringLiteral("R%1-").arg(attempt));
            const qint64 tid = fixedEvent("MTG", 3, p);
            playFixedRound(tid, 1, {{p[0], p[2], "PLAYER1"}, {p[1], p[3], "DRAW"}, {p[4], p[5], "DRAW"}});
            playFixedRound(tid, 2, {{p[0], p[3], "DRAW"}, {p[1], p[4], "PLAYER1"}, {p[2], p[5], "PLAYER1"}});
            const QSet<Pair> before = pairsPlayed(tid);
            QVERIFY(rematchFreePairingExists(p, before));

            const qint64 rid = swiss::advanceToNextRound(tid, 2);
            QCOMPARE(roundProblem(rid, p), QString());
            for (const Row &m : tdb::roundPairings(rid))
                QVERIFY2(!before.contains(pairOf(m["player1_id"].toLongLong(), m["player2_id"].toLongLong())),
                         qPrintable(QStringLiteral("%1 and %2 were paired again although a round with no rematch exists")
                                        .arg(m["player1_name"].toString(), m["player2_name"].toString())));
        }
    }

    // The same rule on every round of small events played until pairings run out: when a
    // round repeats a match, trying every possible pairing of those players must show that
    // none avoids it.  (For an odd field the players are the ones left after the bye.)
    void aRematchIsOnlyPairedWhenNoOtherPairingExists()
    {
        std::mt19937 results(42);
        int roundsChecked = 0, forcedRematchRounds = 0;
        for (int repeat = 0; repeat < 4; ++repeat) {
            for (int n : {4, 5, 6, 7, 8, 9, 10}) {
                const int rounds = n - 1;
                const qint64 tid = tdb::createTournament("Rematches", "MTG", rounds);
                for (qint64 p : addPlayers(n, QStringLiteral("X%1-%2-").arg(repeat).arg(n)))
                    tdb::enrollPlayer(tid, p);
                swiss::startTournament(tid);
                for (int r = 1; r <= rounds; ++r) {
                    if (r > 1) {
                        const qint64 rid = tdb::currentRound(tid)["round_id"].toLongLong();
                        QSet<Pair> before;
                        for (const Row &m : tdb::allMatches(tid))
                            if (!m["player2_id"].isNull() && m["round_id"].toLongLong() != rid)
                                before.insert(pairOf(m["player1_id"].toLongLong(), m["player2_id"].toLongLong()));
                        QList<qint64> paired;
                        bool rematch = false;
                        for (const Row &m : tdb::roundPairings(rid)) {
                            if (m["player2_id"].isNull())
                                continue;
                            const qint64 a = m["player1_id"].toLongLong(), b = m["player2_id"].toLongLong();
                            paired << a << b;
                            rematch = rematch || before.contains(pairOf(a, b));
                        }
                        ++roundsChecked;
                        if (rematch) {
                            ++forcedRematchRounds;
                            QVERIFY2(!rematchFreePairingExists(paired, before),
                                     qPrintable(QStringLiteral("%1 players, round %2: a match was repeated although a "
                                                               "pairing with no rematch exists").arg(n).arg(r)));
                        }
                    }
                    reportRound(tid, results);
                    if (r < rounds)
                        swiss::advanceToNextRound(tid, r);
                }
            }
        }
        QVERIFY(roundsChecked >= 100);
        qInfo("%d rounds checked; %d had a rematch that no pairing could avoid", roundsChecked, forcedRematchRounds);
    }

    // Commander

    // Default scoring (win 5, draw 1, loss 0, bye 5; floor 0.20; a bye adds three opponents
    // at the floor), five players, two rounds:
    //   round 1: pod P1 P2 P3 P4, P1 wins; P5 has the bye
    //   round 2: pod P1 P2 P3 P5, drawn;   P4 has the bye
    //
    //        points  MW% = points / (5 x rounds played), floor 0.20
    //   P1   5 + 1   6/10 = 0.60
    //   P2   0 + 1   1/10 -> 0.20
    //   P3   0 + 1   1/10 -> 0.20
    //   P4   0 + 5   5/10 = 0.50
    //   P5   5 + 1   6/10 = 0.60
    //
    //   OMW% = average MW% of every opponent, once per shared pod:
    //   P1   P2 P3 P4, P2 P3 P5:  (0.2 + 0.2 + 0.5 + 0.2 + 0.2 + 0.6) / 6 = 1.9 / 6 = 0.316667
    //   P2   P1 P3 P4, P1 P3 P5:  (0.6 + 0.2 + 0.5 + 0.6 + 0.2 + 0.6) / 6 = 2.7 / 6 = 0.45
    //   P3   P1 P2 P4, P1 P2 P5:  the same, 0.45
    //   P4   P1 P2 P3, bye:       (0.6 + 0.2 + 0.2 + 3 x 0.2) / 6 = 1.6 / 6 = 0.266667
    //   P5   bye, P1 P2 P3:       the same, 0.266667
    //
    // Order: P1 and P5 have 6 points and P1 has the higher OMW%; then P4 on 5; P2 and P3 are
    // level on everything and are separated by the seeded random key.
    void commanderStandingsMatchAHandWorkedExampleWithADrawAndByes()
    {
        const auto seat = [](qint64 player, int number, const char *result) {
            return QVariantMap{{"player_id", player}, {"seat", number}, {"result", result}};
        };
        const QVariantList rounds{
            QVariantMap{{"pods", QVariantList{QVariantMap{{"reported", true},
                                                          {"seats", QVariantList{seat(1, 1, "WIN"), seat(2, 2, "LOSS"),
                                                                                 seat(3, 3, "LOSS"), seat(4, 4, "LOSS")}}}}},
                        {"byes", QVariantList{5}}},
            QVariantMap{{"pods", QVariantList{QVariantMap{{"reported", true},
                                                          {"seats", QVariantList{seat(1, 1, "DRAW"), seat(2, 2, "DRAW"),
                                                                                 seat(3, 3, "DRAW"), seat(5, 4, "DRAW")}}}}},
                        {"byes", QVariantList{4}}},
        };
        const QVariantList table = cmdr::computeStandings({1, 2, 3, 4, 5}, rounds, cmdr::defaultSettings(), 99);
        QCOMPARE(table.size(), 5);
        QHash<qint64, QVariantMap> t;
        QList<qint64> order;
        for (const QVariant &r : table) {
            t.insert(r.toMap()["player_id"].toLongLong(), r.toMap());
            order << r.toMap()["player_id"].toLongLong();
        }
        const auto check = [&](qint64 player, int points, int wins, int losses, int draws, int byes, double mw, double omw) {
            const QVariantMap r = t[player];
            return r["points"].toInt() == points && r["wins"].toInt() == wins && r["losses"].toInt() == losses
                   && r["draws"].toInt() == draws && r["byes"].toInt() == byes && r["played"].toInt() == 2
                   && near(r["mw_pct"].toDouble(), mw) && near(r["omw_pct"].toDouble(), omw);
        };
        QVERIFY(check(1, 6, 1, 0, 1, 0, 0.6, 0.316667));
        QVERIFY(check(2, 1, 0, 1, 1, 0, 0.2, 0.45));
        QVERIFY(check(3, 1, 0, 1, 1, 0, 0.2, 0.45));
        QVERIFY(check(4, 5, 0, 1, 0, 1, 0.5, 0.266667));
        QVERIFY(check(5, 6, 0, 0, 1, 1, 0.6, 0.266667));
        QCOMPARE(order.mid(0, 3), (QList<qint64>{1, 5, 4}));
        QCOMPARE(QSet<qint64>(order.begin() + 3, order.end()), (QSet<qint64>{2, 3}));
        // the last tiebreaker is fixed by the event's seed: the same seed, the same order
        QCOMPARE(cmdr::computeStandings({5, 4, 3, 2, 1}, rounds, cmdr::defaultSettings(), 99), table);
    }

    // Ten players, one drop after every round: 10, 9, 8, 7, 6 and 5 active players.  Every
    // round must seat each active player once in pods of the sizes the policy gives
    // (4a + 3b with the most fours; five players are one pod of four and a bye), and a
    // dropped player is never seated again.
    void commanderPodsFollowThePolicyAsPlayersDropRoundByRound()
    {
        const int rounds = 6;
        const qint64 tid = cdb::createCommanderTournament("Drops", 10, rounds, {}, 2026);
        QList<qint64> active = addPlayers(10, "D");
        for (qint64 p : active)
            tdb::enrollPlayer(tid, p);
        cdb::startEvent(tid);
        for (int r = 1; r <= rounds; ++r) {
            const QVariantMap rnd = cdb::getRounds(tid).last().toMap();
            QCOMPARE(rnd["round_number"].toInt(), r);
            QList<qint64> assigned;
            QList<int> sizes;
            for (const QVariant &pv : rnd["pods"].toList()) {
                const QVariantList seats = pv.toMap()["seats"].toList();
                sizes << int(seats.size());
                for (const QVariant &s : seats)
                    assigned << s.toMap()["player_id"].toLongLong();
            }
            const cmdr::PodSizes policy = cmdr::podSizes(int(active.size()));
            QList<int> expected = policy.sizes;
            std::sort(sizes.begin(), sizes.end());
            std::sort(expected.begin(), expected.end());
            QCOMPARE(sizes, expected);
            QCOMPARE(int(rnd["byes"].toList().size()), policy.byes);
            for (const QVariant &b : rnd["byes"].toList())
                assigned << b.toMap()["player_id"].toLongLong();
            std::sort(assigned.begin(), assigned.end());
            QList<qint64> want = active;
            std::sort(want.begin(), want.end());
            QCOMPARE(assigned, want);

            for (const QVariant &pv : rnd["pods"].toList()) {
                const QVariantMap pod = pv.toMap();
                cdb::reportPodResult(pod["pod_id"].toLongLong(), "WIN",
                                     pod["seats"].toList()[0].toMap()["player_id"].toLongLong());
            }
            cdb::finalizeRound(rnd["round_id"].toLongLong());
            if (r < rounds) {
                cdb::dropPlayer(tid, active.takeFirst());
                cdb::publishNextRound(tid);
            }
        }
        QCOMPARE(cdb::getRounds(tid).size(), rounds);
        QCOMPARE(cdb::getStandings(tid).size(), 10);        // dropped players stay in the standings
    }
};

QTEST_GUILESS_MAIN(RulesTests)
#include "test_rules.moc"
