#include "commander.h"

#include "db.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <tuple>

namespace cmdr {

const char *const RULES_VERSION = "commander-rules-3";
const char *const PAIRING_ALGORITHM = "cmdr-swiss-2";
const char *const LEGACY_THREE_POD_PLACEMENT = "BEST_MATCH";

QVariantMap defaultSettings()
{
    return {
        {"rules_version", RULES_VERSION},
        // scoring (TopDeck addendum: 5 / 0 / 1, bye counts as a win)
        {"win_points", 5},
        {"loss_points", 0},
        {"draw_points", 1},
        {"bye_points", 5},
        // ELIMINATED_LOSE: players already eliminated when the game is drawn take a loss.
        // ALL_DRAW: everyone seated in the pod receives the draw.
        {"draw_policy", "ELIMINATED_LOSE"},
        // tiebreakers (addendum: 0.20 floor; a bye adds 3 opponents at the floor)
        {"tiebreak_floor", 0.20},
        {"bye_phantom_opponents", 3},
        {"tiebreakers", QVariantList{"OMW", "MW", "RANDOM"}},
        // where three-player pods sit in the standings (custom):
        //   LOW  — among the lowest match points (default)   HIGH — among the highest
        //   BEST_MATCH — wherever the points line up best
        {"three_pod_placement", "LOW"},
        // pairing cost weights (custom)
        {"pairing", QVariantMap{{"score", 4}, {"rematch", 30}, {"three_pod", 12}, {"first_seat", 3}, {"placement", 200}}},
    };
}

bool isDrawPolicy(const QString &s)
{
    return s == "ELIMINATED_LOSE" || s == "ALL_DRAW";
}

bool isThreePodPlacement(const QString &s)
{
    return s == "LOW" || s == "HIGH" || s == "BEST_MATCH";
}

int recommendedRounds(int n)
{
    // (max players, rounds) — TopDeck addendum "Recommended Number of Rounds"
    static const int table[][2] = {{16, 3}, {34, 4}, {64, 5}, {128, 5}, {208, 6}, {304, 7}, {540, 8}, {960, 9}};
    for (const auto &row : table)
        if (n <= row[0])
            return row[1];
    return 10;
}

PodSizes podSizes(int n)
{
    if (n < 3)
        throw PairingError(QStringLiteral("Commander needs at least 3 active players to form a pod; only %1 %2 active.")
                               .arg(n).arg(n == 1 ? "is" : "are").toStdString());
    PodSizes out;
    if (n == 5) {
        out.sizes = {4};
        out.byes = 1;
        return out;
    }
    for (int a = n / 4; a >= 0; --a) {
        const int rest = n - 4 * a;
        if (rest % 3 == 0) {
            for (int i = 0; i < a; ++i) out.sizes << 4;
            for (int i = 0; i < rest / 3; ++i) out.sizes << 3;
            return out;
        }
    }
    throw PairingError(QStringLiteral("No valid pod layout for %1 players.").arg(n).toStdString());   // unreachable for n >= 6
}

static qint64 hash48(const QString &text)
{
    const QByteArray digest = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex();
    return digest.left(12).toLongLong(nullptr, 16);
}

qint64 deriveSeed(qint64 baseSeed, int roundNumber)
{
    return hash48(QStringLiteral("%1:%2").arg(baseSeed).arg(roundNumber));
}

static qint64 randomKey(qint64 baseSeed, qint64 playerId)
{
    return hash48(QStringLiteral("%1:tiebreak:%2").arg(baseSeed).arg(playerId));
}

QMap<qint64, SeatResult> seatResults(const QList<qint64> &playerIds, const QString &outcome, qint64 winnerId,
                                     const QList<qint64> &eliminatedIds, const QVariantMap &settings)
{
    const QSet<qint64> eliminated(eliminatedIds.begin(), eliminatedIds.end());
    for (qint64 e : eliminated)
        if (!playerIds.contains(e))
            throw std::invalid_argument("Eliminated players must be seated in this pod.");

    QMap<qint64, SeatResult> results;
    if (outcome == "WIN") {
        if (!winnerId || !playerIds.contains(winnerId))
            throw std::invalid_argument("The winner must be a player seated in this pod.");
        for (qint64 p : playerIds)
            results[p] = p == winnerId ? SeatResult{"WIN", 0} : SeatResult{"LOSS", 1};
        return results;
    }
    if (outcome == "DRAW") {
        if (winnerId)
            throw std::invalid_argument("A drawn pod cannot also have a winner.");
        int alive = 0;
        for (qint64 p : playerIds)
            if (!eliminated.contains(p))
                ++alive;
        if (alive < 2)
            throw std::invalid_argument("A draw needs at least two players still in the game; "
                                        "with one player left, report that player as the winner.");
        const bool eliminatedLose = settings.value("draw_policy").toString() == "ELIMINATED_LOSE";
        for (qint64 p : playerIds) {
            const bool out = eliminated.contains(p);
            results[p] = (out && eliminatedLose) ? SeatResult{"LOSS", 1} : SeatResult{"DRAW", out ? 1 : 0};
        }
        return results;
    }
    throw std::invalid_argument(QStringLiteral("Unknown outcome: '%1'").arg(outcome).toStdString());
}

namespace {
struct Record {
    qint64 id = 0;
    int points = 0, wins = 0, losses = 0, draws = 0, byes = 0, played = 0;
    QList<qint64> opponents;
    double mw = 0, omw = 0;
    qint64 key = 0;
};
}

QVariantList computeStandings(const QList<qint64> &playerIds, const QVariantList &rounds, const QVariantMap &settings,
                              qint64 baseSeed)
{
    const double floor = settings.value("tiebreak_floor").toDouble();
    const int winPts = settings.value("win_points").toInt();
    const int lossPts = settings.value("loss_points").toInt();
    const int drawPts = settings.value("draw_points").toInt();
    const int byePts = settings.value("bye_points").toInt();
    const int phantom = settings.value("bye_phantom_opponents").toInt();

    QList<Record> recs;
    QHash<qint64, int> at;
    for (qint64 p : playerIds) {
        at.insert(p, recs.size());
        Record r;
        r.id = p;
        recs.append(r);
    }

    for (const QVariant &rv : rounds) {
        const QVariantMap rnd = rv.toMap();
        for (const QVariant &pv : rnd.value("pods").toList()) {
            const QVariantMap pod = pv.toMap();
            if (!pod.value("reported").toBool())
                continue;
            const QVariantList seats = pod.value("seats").toList();
            QList<qint64> ids;
            for (const QVariant &s : seats)
                ids << s.toMap().value("player_id").toLongLong();
            for (const QVariant &sv : seats) {
                const QVariantMap s = sv.toMap();
                const qint64 pid = s.value("player_id").toLongLong();
                auto it = at.constFind(pid);
                if (it == at.constEnd())
                    continue;
                Record &r = recs[*it];
                const QString res = s.value("result").toString();
                if (res == "WIN") { r.points += winPts; r.wins++; }
                else if (res == "LOSS") { r.points += lossPts; r.losses++; }
                else { r.points += drawPts; r.draws++; }
                r.played++;
                for (qint64 o : ids)
                    if (o != pid)
                        r.opponents << o;
            }
        }
        for (const QVariant &b : rnd.value("byes").toList()) {
            auto it = at.constFind(b.toLongLong());
            if (it != at.constEnd()) {
                Record &r = recs[*it];
                r.points += byePts;
                r.byes++;
                r.played++;
            }
        }
    }

    auto mw = [&](qint64 p) {
        auto it = at.constFind(p);
        if (it == at.constEnd() || !recs[*it].played)
            return floor;
        const Record &r = recs[*it];
        return std::max(floor, std::min(1.0, double(r.points) / (winPts * r.played)));
    };

    for (Record &r : recs) {
        double sum = 0;
        int count = 0;
        for (qint64 o : r.opponents) { sum += mw(o); ++count; }
        for (int i = 0; i < phantom * r.byes; ++i) { sum += floor; ++count; }
        r.mw = db::roundTo(mw(r.id), 6);
        r.omw = count ? db::roundTo(sum / count, 6) : floor;
        r.key = randomKey(baseSeed, r.id);
    }

    std::stable_sort(recs.begin(), recs.end(), [](const Record &a, const Record &b) {
        return std::make_tuple(-a.points, -a.omw, -a.mw, a.key) < std::make_tuple(-b.points, -b.omw, -b.mw, b.key);
    });

    QVariantList out;
    for (int i = 0; i < recs.size(); ++i) {
        const Record &r = recs[i];
        out << QVariantMap{{"player_id", r.id}, {"points", r.points}, {"wins", r.wins}, {"losses", r.losses},
                           {"draws", r.draws}, {"byes", r.byes}, {"played", r.played}, {"mw_pct", r.mw},
                           {"omw_pct", r.omw}, {"random_key", r.key}, {"standing", i + 1}};
    }
    return out;
}

QVariantMap pairingInputs(const QVariantList &activeStandings, const QVariantList &rounds, int roundNumber,
                          const QVariantMap &settings)
{
    // pairing history used as optimisation input: meetings, 3-pod counts, seats, byes
    QMap<QPair<qint64, qint64>, int> meetings;
    QHash<qint64, int> threes, byes;
    QHash<qint64, QList<int>> seats;
    for (const QVariant &rv : rounds) {
        const QVariantMap rnd = rv.toMap();
        for (const QVariant &pv : rnd.value("pods").toList()) {
            const QVariantList podSeats = pv.toMap().value("seats").toList();
            QList<qint64> ids;
            for (const QVariant &s : podSeats)
                ids << s.toMap().value("player_id").toLongLong();
            QList<qint64> sorted = ids;
            std::sort(sorted.begin(), sorted.end());
            for (int i = 0; i < sorted.size(); ++i)
                for (int j = i + 1; j < sorted.size(); ++j)
                    meetings[{sorted[i], sorted[j]}] += 1;
            for (const QVariant &sv : podSeats) {
                const QVariantMap s = sv.toMap();
                const qint64 p = s.value("player_id").toLongLong();
                if (ids.size() == 3)
                    threes[p] += 1;
                if (!seats.contains(p))
                    seats[p] = {0, 0, 0, 0};
                seats[p][s.value("seat").toInt() - 1] += 1;
            }
        }
        for (const QVariant &b : rnd.value("byes").toList())
            byes[b.toLongLong()] += 1;
    }

    QSet<qint64> active;
    for (const QVariant &r : activeStandings)
        active.insert(r.toMap().value("player_id").toLongLong());

    QVariantList players;
    for (int i = 0; i < activeStandings.size(); ++i) {
        const QVariantMap r = activeStandings[i].toMap();
        const qint64 id = r.value("player_id").toLongLong();
        QVariantList seatCounts;
        for (int c : seats.value(id, {0, 0, 0, 0}))
            seatCounts << c;
        players << QVariantMap{{"player_id", id}, {"points", r.value("points").toInt()}, {"rank", i + 1},
                               {"byes", byes.value(id)}, {"three_pods", threes.value(id)}, {"seats", seatCounts}};
    }
    QVariantList met;
    for (auto it = meetings.constBegin(); it != meetings.constEnd(); ++it)       // sorted by (a, b)
        if (active.contains(it.key().first) && active.contains(it.key().second))
            met << QVariant(QVariantList{it.key().first, it.key().second, it.value()});

    return {
        {"round_number", roundNumber},
        {"weights", settings.value("pairing").toMap()},
        {"three_pod_placement", settings.value("three_pod_placement", LEGACY_THREE_POD_PLACEMENT).toString()},
        {"players", players},
        {"meetings", met},
    };
}

// Seeded random numbers (MT19937).

SeededRandom::SeededRandom(quint64 seed)
{
    // init_genrand(19650218)
    mt_[0] = 19650218U;
    for (int i = 1; i < 624; ++i)
        mt_[i] = 1812433253U * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + quint32(i);
    // init_by_array(key): the seed's 32-bit words, least significant first
    quint32 key[2] = {quint32(seed & 0xffffffffU), quint32(seed >> 32)};
    const int keyLength = key[1] ? 2 : 1;
    int i = 1, j = 0;
    for (int k = std::max(624, keyLength); k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] + quint32(j);
        if (++i >= 624) { mt_[0] = mt_[623]; i = 1; }
        if (++j >= keyLength) j = 0;
    }
    for (int k = 623; k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) - quint32(i);
        if (++i >= 624) { mt_[0] = mt_[623]; i = 1; }
    }
    mt_[0] = 0x80000000U;
    index_ = 624;
}

quint32 SeededRandom::next32()
{
    if (index_ >= 624) {
        static const quint32 mag[2] = {0U, 0x9908b0dfU};
        int kk = 0;
        for (; kk < 624 - 397; ++kk) {
            const quint32 y = (mt_[kk] & 0x80000000U) | (mt_[kk + 1] & 0x7fffffffU);
            mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ mag[y & 1U];
        }
        for (; kk < 623; ++kk) {
            const quint32 y = (mt_[kk] & 0x80000000U) | (mt_[kk + 1] & 0x7fffffffU);
            mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1) ^ mag[y & 1U];
        }
        const quint32 y = (mt_[623] & 0x80000000U) | (mt_[0] & 0x7fffffffU);
        mt_[623] = mt_[396] ^ (y >> 1) ^ mag[y & 1U];
        index_ = 0;
    }
    quint32 y = mt_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= y >> 18;
    return y;
}

quint32 SeededRandom::below(quint32 n)
{
    if (!n)
        return 0;
    int bits = 0;
    for (quint32 v = n; v; v >>= 1)
        ++bits;
    quint32 r = next32() >> (32 - bits);
    while (r >= n)
        r = next32() >> (32 - bits);
    return r;
}

std::pair<int, int> SeededRandom::sampleTwo(int n)
{
    if (n <= 21) {                       // small population: draw from a shrinking pool
        std::vector<int> pool(n);
        std::iota(pool.begin(), pool.end(), 0);
        int result[2];
        for (int i = 0; i < 2; ++i) {
            const int j = int(below(quint32(n - i)));
            result[i] = pool[j];
            pool[j] = pool[n - i - 1];
        }
        return {result[0], result[1]};
    }
    const int first = int(below(quint32(n)));
    int second = int(below(quint32(n)));
    while (second == first)
        second = int(below(quint32(n)));
    return {first, second};
}

// Limits on the local search.  They bound the time a round takes to pair; they are part of
// the algorithm version, because changing them can change which pairing is found.
const int SEARCH_BUDGET = 250000;       // pod-swap evaluations per round
const int SMALL_FIELD = 32;             // up to this many players, every pair of players may swap
const int SWAP_WINDOW = 16;             // larger fields: only players this close in the standings
const int RESTART_FIELD = 40;           // up to this many players, try many restarts
const int RESTARTS_SMALL = 30;
const int RESTARTS_LARGE = 4;
const int DEFAULT_PLACEMENT_WEIGHT = 200;

namespace {
struct Info {
    qint64 id = 0;
    int points = 0, rank = 0, byes = 0, threePods = 0;
    int seats[4] = {0, 0, 0, 0};
};
using Pod = std::vector<int>;       // indexes into the player table
}

QVariantMap pairRound(const QVariantMap &inputs, qint64 seed)
{
    SeededRandom rng{quint64(seed)};
    const QVariantMap w = inputs.value("weights").toMap();
    const double wScore = w.value("score").toDouble();
    const double wRematch = w.value("rematch").toDouble();
    const double wThree = w.value("three_pod").toDouble();
    const double wFirst = w.value("first_seat").toDouble();
    const double wPlace = w.value("placement", DEFAULT_PLACEMENT_WEIGHT).toDouble();

    // players, by rank; everything below refers to them by position in this table
    std::vector<Info> info;
    for (const QVariant &pv : inputs.value("players").toList()) {
        const QVariantMap p = pv.toMap();
        Info i;
        i.id = p.value("player_id").toLongLong();
        i.points = p.value("points").toInt();
        i.rank = p.value("rank").toInt();
        i.byes = p.value("byes").toInt();
        i.threePods = p.value("three_pods").toInt();
        const QVariantList seats = p.value("seats").toList();
        for (int s = 0; s < 4 && s < seats.size(); ++s)
            i.seats[s] = seats[s].toInt();
        info.push_back(i);
    }
    std::stable_sort(info.begin(), info.end(), [](const Info &a, const Info &b) { return a.rank < b.rank; });
    const int count = int(info.size());
    QHash<qint64, int> indexOf;
    for (int i = 0; i < count; ++i)
        indexOf.insert(info[i].id, i);

    const QVariantList meetings = inputs.value("meetings").toList();
    std::vector<int> met(size_t(count) * size_t(count), 0);
    for (const QVariant &mv : meetings) {
        const QVariantList m = mv.toList();
        auto a = indexOf.constFind(m.value(0).toLongLong());
        auto b = indexOf.constFind(m.value(1).toLongLong());
        if (a == indexOf.constEnd() || b == indexOf.constEnd())
            continue;
        met[size_t(*a) * count + *b] = met[size_t(*b) * count + *a] = m.value(2).toInt();
    }

    const PodSizes layout = podSizes(count);
    std::vector<int> sizes(layout.sizes.begin(), layout.sizes.end());

    // byes: fewest previous byes, then lowest standing
    std::vector<int> byes;
    if (layout.byes) {
        std::vector<int> byNeed(count);
        std::iota(byNeed.begin(), byNeed.end(), 0);
        std::stable_sort(byNeed.begin(), byNeed.end(), [&](int a, int b) {
            return std::make_pair(info[a].byes, -info[a].rank) < std::make_pair(info[b].byes, -info[b].rank);
        });
        byes.assign(byNeed.begin(), byNeed.begin() + layout.byes);
    }
    std::vector<int> order;
    for (int i = 0; i < count; ++i)
        if (std::find(byes.begin(), byes.end(), i) == byes.end())
            order.push_back(i);

    const bool firstRound = inputs.value("round_number").toInt() == 1 && meetings.isEmpty();
    if (firstRound)
        rng.shuffle(order);

    // Three-player pod placement.  The short-pod seats belong to the lowest (or
    // highest) match-point groups: `cutoff` is the points of the last player who
    // must sit in one, and anyone on the wrong side of it is penalised per point.
    // Players tied on points stay interchangeable, so rematch avoidance and
    // sharing short pods fairly still work inside the group.
    const QString placement = inputs.value("three_pod_placement", LEGACY_THREE_POD_PLACEMENT).toString();
    const int shortSeats = 3 * int(std::count(sizes.begin(), sizes.end(), 3));
    std::vector<int> misplaced;
    if ((placement == "LOW" || placement == "HIGH") && shortSeats > 0 && shortSeats < int(order.size())) {
        std::vector<int> byPoints;
        for (int p : order)
            byPoints.push_back(info[p].points);
        if (placement == "HIGH")
            std::sort(byPoints.begin(), byPoints.end(), std::greater<int>());
        else
            std::sort(byPoints.begin(), byPoints.end());
        const int cutoff = byPoints[shortSeats - 1];
        misplaced.assign(count, 0);
        for (int p : order) {
            const int gap = info[p].points - cutoff;
            misplaced[p] = std::max(0, placement == "LOW" ? gap : -gap);
        }
    }

    auto podCost = [&](const Pod &members) {
        double c = 0.0;
        for (size_t i = 0; i < members.size(); ++i) {
            for (size_t j = i + 1; j < members.size(); ++j) {
                const int a = members[i], b = members[j];
                const int d = info[a].points - info[b].points;
                const int m = met[size_t(a) * count + b];
                c += wScore * d * d + wRematch * m * m;
            }
        }
        if (members.size() == 3) {
            int threes = 0;
            for (int p : members) threes += info[p].threePods;
            c += wThree * threes;
            if (!misplaced.empty()) {
                int wrong = 0;
                for (int p : members) wrong += misplaced[p];
                c += wPlace * wrong;
            }
        }
        return c;
    };

    auto chunk = [&](const std::vector<int> &seq, const std::vector<int> &podLayout) {
        std::vector<Pod> pods;
        size_t i = 0;
        for (int size : podLayout) {
            pods.emplace_back(seq.begin() + i, seq.begin() + i + size);
            i += size;
        }
        return pods;
    };

    const int n = int(order.size());
    const int window = n <= SMALL_FIELD ? n : SWAP_WINDOW;
    long long budget = SEARCH_BUDGET;

    auto climb = [&](std::vector<Pod> &pods) {
        std::vector<std::pair<int, int>> where(count, {-1, -1});
        std::vector<int> seq;
        for (int pi = 0; pi < int(pods.size()); ++pi) {
            for (int si = 0; si < int(pods[pi].size()); ++si) {
                where[pods[pi][si]] = {pi, si};
                seq.push_back(pods[pi][si]);
            }
        }
        std::stable_sort(seq.begin(), seq.end(), [&](int a, int b) { return info[a].rank < info[b].rank; });
        std::vector<double> costs;
        for (const Pod &p : pods)
            costs.push_back(podCost(p));
        bool improved = true;
        while (improved && budget > 0) {
            improved = false;
            const int len = int(seq.size());
            for (int i = 0; i < len; ++i) {
                for (int j = i + 1; j < std::min(len, i + window); ++j) {
                    const int a = seq[i], b = seq[j];
                    const auto [pa, sa] = where[a];
                    const auto [pb, sb] = where[b];
                    if (pa == pb)
                        continue;
                    --budget;
                    pods[pa][sa] = b;
                    pods[pb][sb] = a;
                    const double ca = podCost(pods[pa]), cb = podCost(pods[pb]);
                    if (ca + cb < costs[pa] + costs[pb] - 1e-9) {
                        costs[pa] = ca;
                        costs[pb] = cb;
                        where[a] = {pb, sb};
                        where[b] = {pa, sa};
                        improved = true;
                    } else {
                        pods[pa][sa] = a;
                        pods[pb][sb] = b;
                    }
                }
            }
        }
        double total = 0;
        for (double c : costs) total += c;
        return total;
    };

    std::vector<Pod> best;
    double bestCost = 0;
    if (firstRound) {
        best = chunk(order, sizes);
        for (const Pod &p : best)
            bestCost += podCost(p);
    } else {
        std::vector<int> reversed(sizes.rbegin(), sizes.rend());
        std::vector<std::vector<int>> layouts =
            placement == "HIGH" ? std::vector<std::vector<int>>{reversed, sizes}
                                : std::vector<std::vector<int>>{sizes, reversed};
        const int threes = int(std::count(sizes.begin(), sizes.end(), 3));
        const int fours = int(std::count(sizes.begin(), sizes.end(), 4));
        if (threes && fours) {          // three-player pods spread through the standings
            std::vector<int> spread;
            int t = 0;
            const int total = int(sizes.size());
            for (int i = 0; i < total; ++i) {
                if (t < threes && (i + 1) * threes / total > t) {
                    spread.push_back(3);
                    ++t;
                } else {
                    spread.push_back(4);
                }
            }
            std::vector<int> a = spread, b = sizes;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a == b)
                layouts.push_back(spread);
        }
        bool have = false;
        for (const auto &podLayout : layouts) {
            std::vector<Pod> pods = chunk(order, podLayout);
            const double cost = climb(pods);
            if (!have || cost < bestCost - 1e-9) {
                best = pods;
                bestCost = cost;
                have = true;
            }
        }
        const int restarts = n <= RESTART_FIELD ? RESTARTS_SMALL : RESTARTS_LARGE;
        for (int r = 0; r < restarts; ++r) {
            if (bestCost <= 1e-9 || budget <= 0)
                break;
            std::vector<Pod> trial = best;
            const int podCount = int(trial.size());
            for (int k = 0; k < std::max(2, podCount); ++k) {      // perturb: a few random cross-pod swaps
                int pa = 0, pb = 0;
                if (podCount > 1)
                    std::tie(pa, pb) = rng.sampleTwo(podCount);
                if (pa == pb)
                    continue;
                const int sa = int(rng.below(quint32(trial[pa].size())));
                const int sb = int(rng.below(quint32(trial[pb].size())));
                std::swap(trial[pa][sa], trial[pb][sb]);
            }
            const double cost = climb(trial);
            if (cost < bestCost - 1e-9) {
                best = trial;
                bestCost = cost;
            }
        }
    }

    // seats: balance each player's seat history, first seat weighted heaviest
    double seatCost = 0.0;
    std::vector<Pod> seated;
    for (const Pod &pod : best) {
        std::vector<Pod> options;
        double low = 0;
        bool have = false;
        std::vector<int> idx(pod.size());
        std::iota(idx.begin(), idx.end(), 0);
        do {                                                        // same order as itertools.permutations
            double c = 0;
            for (size_t s = 0; s < idx.size(); ++s) {
                const int times = info[pod[idx[s]]].seats[s];
                c += (s == 0 ? wFirst : 1.0) * times * times;
            }
            if (!have || c < low - 1e-9) {
                options.clear();
                low = c;
                have = true;
            }
            if (std::fabs(c - low) <= 1e-9) {
                Pod perm;
                for (int k : idx) perm.push_back(pod[k]);
                options.push_back(perm);
            }
        } while (std::next_permutation(idx.begin(), idx.end()));
        seated.push_back(options[rng.below(quint32(options.size()))]);
        seatCost += low;
    }

    auto podRank = [&](const Pod &pod) {
        int sum = 0, top = info[pod[0]].points, rank = info[pod[0]].rank;
        for (int p : pod) {
            sum += info[p].points;
            top = std::max(top, info[p].points);
            rank = std::min(rank, info[p].rank);
        }
        return std::make_tuple(-double(sum) / double(pod.size()), -top, rank);
    };
    std::stable_sort(seated.begin(), seated.end(), [&](const Pod &a, const Pod &b) { return podRank(a) < podRank(b); });

    int rematchPairs = 0, threePods = 0;
    double scoreCost = 0;
    QVariantList podsOut;
    for (const Pod &pod : seated) {
        QVariantList ids;
        for (int p : pod)
            ids << info[p].id;
        podsOut << QVariant(ids);
        if (pod.size() == 3)
            ++threePods;
        for (size_t i = 0; i < pod.size(); ++i) {
            for (size_t j = i + 1; j < pod.size(); ++j) {
                if (met[size_t(pod[i]) * count + pod[j]])
                    ++rematchPairs;
                const int d = info[pod[i]].points - info[pod[j]].points;
                scoreCost += wScore * d * d;
            }
        }
    }
    QVariantList byesOut;
    for (int b : byes)
        byesOut << info[b].id;

    return {
        {"algorithm", PAIRING_ALGORITHM},
        {"seed", seed},
        {"pods", podsOut},
        {"byes", byesOut},
        {"cost", QVariantMap{{"total", db::roundTo(bestCost, 3)}, {"score", scoreCost}, {"seat", seatCost},
                             {"rematch_pairs", rematchPairs}, {"three_player_pods", threePods}}},
    };
}

} // namespace cmdr
