<!-- Local draft. Not published to GitHub. -->

# [Bug] One-on-one pairing: two players are paired again although a round with no rematch exists

## Version and environment

- App version: 0.2.1-preview
- Commit: `5c4fa23`
- Windows 11 Home 10.0.26200; Qt 6.10.2; MSVC 2022 (Visual Studio 17.8.5); Release, 64-bit
- Data used: disposable test database (automated test)

## Where it happened

- Game and format: every one-on-one format (Modern, One Piece, Pokémon); found with MTG
- Players and rounds: 6 players, pairing round 3 of 3. Also seen in simulated events from
  5 players, round 2
- Screen: none; `swiss::generatePairings` in `src/core/swiss.cpp`

## Steps to reproduce

Pairings are shuffled, so the reliable reproduction is the automated test, which saves two
fixed rounds and asks the app for the third:

1. Six players P1–P6 in an MTG tournament of 3 rounds.
2. Round 1: P1 beats P3, P2 draws P4, P5 draws P6.
3. Round 2: P1 draws P4, P2 beats P5, P3 beats P6. Points are now 4, 4, 3, 2, 1, 1.
4. End round 2 so that round 3 is paired.

To see it fail, set the fix aside, run the test, then put the fix back (the test program
needs Qt's `bin` folder on `PATH`):

```
git stash push src/core/swiss.cpp
build.bat
build\test_rules.exe anAvoidableRematchIsNotPaired
git stash pop
build.bat
```

Reproduces: always (5 of 5 attempts inside the test).

## Expected result

Round 3 has no rematch. One exists: P1–P2, P3–P5, P4–P6. The project's own test is named
`modernNeverRematchesWhileItCanAvoidIt`.

## Actual result

Round 3 is P1–P2, P3–P4, **P5–P6**, and P5 and P6 already played in round 1.

```
FAIL!  : RulesTests::anAvoidableRematchIsNotPaired() ... (R0-005 and R0-006 were paired
again although a round with no rematch exists)
FAIL!  : RulesTests::aRematchIsOnlyPairedWhenNoOtherPairingExists() ... (5 players, round 2:
a match was repeated although a pairing with no rematch exists)
```

Cause: the pairing goes down the standings giving each player the first opponent they have
not met and never takes a choice back, so the last two players are paired whether or not
they have met.

## Severity and impact

- [x] **S2 Major** — every player is still assigned exactly once and results are saved
  correctly, but the pairing breaks the no-rematch rule, and the organizer cannot change a
  pairing in the app.

Impact: small fields in their later rounds, which is a typical club event. In simulated
events of 4–10 players played for (players − 1) rounds, 28 to 35 of 140 rounds repeated a
match that another pairing would have avoided (three runs on `5c4fa23`; pairings are
shuffled, so the count varies). That is a stress measurement, not a rate for a normal event.

Workaround: none in the app.

## Evidence

Test output above. No screenshot: it was found by reading the pairing code and writing a
test, not at an event.

## For the maintainer

- Status: Fixed locally, not committed
- Regression test: `tests/test_rules.cpp` — `anAvoidableRematchIsNotPaired` (the fixed case)
  and `aRematchIsOnlyPairedWhenNoOtherPairingExists` (every round of simulated events,
  checked against every possible pairing). Manual: RG-403, RG-404
- Fix: working-tree change to `src/core/swiss.cpp` on top of `5c4fa23`: `pairWithoutRematch`
  takes a choice back when it would leave the players below with nobody new. When no
  rematch-free round exists the earlier behaviour is kept
- Retest result: see `docs/qa/TEST_REPORT.md`
