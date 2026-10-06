// Commander (multiplayer) tournament rules and pairing engine.
//
// Pure logic only — no database access — so every function here is
// deterministic for a given input and can be re-run from the inputs saved
// with each round.  See docs/COMMANDER.md for which rules come from the
// TopDeck.gg multiplayer addendum and which are this app's own policy.
// This is NOT an official Wizards of the Coast pairing algorithm.
#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cmdr {

extern const char *const RULES_VERSION;
extern const char *const PAIRING_ALGORITHM;
extern const char *const LEGACY_THREE_POD_PLACEMENT;   // events started before the setting existed

QVariantMap defaultSettings();
bool isDrawPolicy(const QString &s);                   // ELIMINATED_LOSE | ALL_DRAW
bool isThreePodPlacement(const QString &s);            // LOW | HIGH | BEST_MATCH

struct PairingError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

int recommendedRounds(int playerCount);

struct PodSizes {
    QList<int> sizes;
    int byes = 0;
};
// Solves 4a + 3b = n maximising a; pods are only ever 3 or 4 players.  Five
// players is the one count with no solution: one four-player pod and one bye.
PodSizes podSizes(int activePlayers);

qint64 deriveSeed(qint64 baseSeed, int roundNumber);

struct SeatResult {
    QString result;     // WIN | LOSS | DRAW
    int eliminated = 0;
    bool operator==(const SeatResult &o) const { return result == o.result && eliminated == o.eliminated; }
};
// Turns a pod-level report into one result per player.  winnerId 0 means "no
// winner".  Throws std::invalid_argument for a report that makes no sense.
QMap<qint64, SeatResult> seatResults(const QList<qint64> &playerIds, const QString &outcome, qint64 winnerId,
                                     const QList<qint64> &eliminatedIds, const QVariantMap &settings);

// Recomputes standings from saved results only.
//   rounds: [{"pods": [{"reported": bool, "seats": [{"player_id", "seat", "result"}]}], "byes": [player_id]}]
// Order: match points, OMW%, MW%, then a seeded random key (custom final tiebreak).
//   MW%  = points / (win_points x rounds played), floored at tiebreak_floor.
//   OMW% = average MW% of every opponent faced (once per shared pod); each bye
//          adds `bye_phantom_opponents` opponents at the floor.
QVariantList computeStandings(const QList<qint64> &playerIds, const QVariantList &rounds, const QVariantMap &settings,
                              qint64 baseSeed);

// Everything pairRound needs, in a JSON-serialisable form that is saved with the round.
QVariantMap pairingInputs(const QVariantList &activeStandings, const QVariantList &rounds, int roundNumber,
                          const QVariantMap &settings);

// Multiplayer Swiss pairing.
//
// Hard constraints: pods of exactly 3 or 4, every active player assigned
// exactly once (to one pod or to the single five-player bye).
// Optimised (weighted cost, lower is better):
//   placement — three-player pods are filled from the lowest (LOW) or highest
//               (HIGH) match-point groups; weighted so it outranks the rest
//   score     — squared match-point difference for every pair sharing a pod
//   rematch   — squared number of previous meetings for every pair sharing a pod
//   three_pod — previous three-player pods of everyone placed in a three-player pod
// then seats are chosen per pod to balance each player's seat history,
// weighting the first seat (starting player) more heavily.
//
// Round one has no history, so the seeded shuffle alone decides it.  Later
// rounds start from standings order and improve by bounded local search
// (pairwise swaps inside a standings window, with seeded restarts).
// Deterministic for a given (inputs, seed).
//
// Returns {"algorithm", "seed", "pods": [[player_id]], "byes": [player_id], "cost": {...}}.
QVariantMap pairRound(const QVariantMap &inputs, qint64 seed);

// The pairing random number generator (Mersenne Twister, MT19937).  Its
// seeding and sampling must never change: saved seeds have to keep producing
// the pairings they always produced.
class SeededRandom {
public:
    explicit SeededRandom(quint64 seed);
    quint32 next32();
    quint32 below(quint32 n);                      // uniform in [0, n)
    template <typename T> void shuffle(T &seq)
    {
        for (int i = int(seq.size()) - 1; i >= 1; --i)
            std::swap(seq[i], seq[below(quint32(i + 1))]);
    }
    std::pair<int, int> sampleTwo(int n);          // two different indexes in [0, n)

private:
    quint32 mt_[624];
    int index_ = 625;
};

} // namespace cmdr
