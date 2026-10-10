# Test report

What was run, on what, and what happened. Add a new dated section for each later run;
do not edit an old one.

## How to run the tests yourself

Needs Visual Studio 2022 with the C++ workload and Qt 6 for MSVC 2022 64-bit (see
`README.md`). From the repository folder, in a normal Command Prompt:

```
run_tests.bat
```

It builds (Release, 64-bit) and runs the three test programs. Each one uses its own
temporary database; the real data folder is never opened. The last lines printed are the
totals, and the full output is in `build\core_results.txt`, `build\rules_results.txt` and
`build\ui_results.txt`. The exit code is 0 only if all three programs pass.

To run one program or one test (Qt's `bin` folder must be on `PATH`):

```
build.bat
set PATH=C:\Qt\6.10.2\msvc2022_64\bin;%PATH%
build\test_rules.exe
build\test_rules.exe anAvoidableRematchIsNotPaired
build\test_rules.exe -functions
```

For the manual cases, `qa_sandbox.bat fresh` starts the built app on an empty test
database in `%TEMP%\tcg-qa-sandbox`.

---

## Run of 2026-10-09

| | |
|---|---|
| Version | 0.2.1-preview |
| Commit | `5c4fa23` ("Bump version to 0.2.1"), branch `main` |
| Tested twice | (1) the commit as it is; (2) the commit plus the uncommitted changes listed below |
| Machine | Windows 11 Home 10.0.26200, 64-bit |
| Toolchain | Visual Studio 2022 17.8.5 (MSVC), CMake and Ninja from Visual Studio, Qt 6.10.2 `msvc2022_64`, QtTest 6.10.2 |
| Build | Release, 64-bit, `build.bat` |
| Data | Temporary databases created by the tests. No real player or tournament data was opened. |
| Run by | Claude (AI assistant), from the command line. No test was performed by a person. |

### Automated results

QtTest's totals count `initTestCase` and `cleanupTestCase` as two extra passes per program,
and count each data row of a data-driven test separately.

| Program | Test functions | (1) `5c4fa23` as committed | (2) with the changes below |
|---|---:|---|---|
| `test_core` (existing) | 71 | 73 passed, 0 failed | 73 passed, 0 failed |
| `test_ui` (existing) | 30 | 32 passed, 0 failed | 32 passed, 0 failed |
| `test_rules` (new) | 10 | 12 passed, **2 failed**, 1 expected failure | 14 passed, 0 failed, 1 expected failure |

The two failures in column (1) are the regression tests for BUG-001, run before the fix:

```
FAIL!  : RulesTests::anAvoidableRematchIsNotPaired() ... (R0-005 and R0-006 were paired again
         although a round with no rematch exists)
FAIL!  : RulesTests::aRematchIsOnlyPairedWhenNoOtherPairingExists() ... (5 players, round 2: a
         match was repeated although a pairing with no rematch exists)
```

The expected failure (`XFAIL`) in both columns is `pokemonSecondTiebreakerIsWorkedOut`, the
marker for BUG-002, which is still open.

Run times varied a lot between runs on this machine (`test_core` took 32 s in one run and
87 s in another; `test_ui` 257 s and 403 s). The two slowest core tests are Commander tests
that do not use the file that was changed. No performance claim is made either way.

### What the new tests check (`tests/test_rules.cpp`)

| Test | What it proves |
|---|---|
| `mtgStandingsMatchAHandWorkedExampleWithADrawAndByes` | Points, record, OMW%, GW%, OGW% and place for 5 players over 2 rounds with a draw and two byes equal figures worked out by hand (the arithmetic is in the comment above the test) |
| `aPointsTieIsBrokenByOpponentsMatchWinPercentage` (MTG, One Piece, Pokémon) | Four players level on points are ordered by OMW%, with each game's own win points and floor |
| `tiebreakersApplyInEachGamesConfiguredOrder` | Each game applies its tiebreakers in the order it lists them |
| `pokemonSecondTiebreakerIsWorkedOut` | Expected failure: BUG-002 |
| `everyOneOnOneRoundAssignsEachActivePlayerExactlyOnce` | Every round of 16 events (2 to 33 players, all three games): each player in exactly one match, one bye only for an odd count, tables 1..k, and the event stops at its configured rounds |
| `nobodyGetsASecondByeBeforeEveryoneHasHadOne` | 3, 5 and 7 players: each player gets exactly one bye |
| `anAvoidableRematchIsNotPaired` | BUG-001, the fixed six-player case |
| `aRematchIsOnlyPairedWhenNoOtherPairingExists` | BUG-001 in general: 140 rounds of simulated 4–10 player events; any round with a rematch is checked against every possible pairing |
| `commanderStandingsMatchAHandWorkedExampleWithADrawAndByes` | Commander points, MW%, OMW% and order for 5 players over 2 rounds equal figures worked out by hand |
| `commanderPodsFollowThePolicyAsPlayersDropRoundByRound` | 10 players dropping to 5: pods are always the sizes the policy gives, every active player is seated once, dropped players are never seated again |

### Uncommitted changes that were tested

Nothing was committed, pushed or published.

| File | Change |
|---|---|
| `src/core/swiss.cpp` | Fix for BUG-001 (`pairWithoutRematch`) |
| `tests/test_rules.cpp` | New test program |
| `CMakeLists.txt`, `run_tests.bat` | Build and run `test_rules` |
| `qa_sandbox.bat` | Starts the app on a disposable data folder |
| `docs/qa/*`, `.github/ISSUE_TEMPLATE/bug_report.md` | This documentation |
| `README.md` | Mentions the new test program, `qa_sandbox.bat` and `docs/qa` |

### Bugs

| | Count | Which |
|---|---:|---|
| Confirmed (reproduced by a failing test) | 2 | BUG-001, BUG-002 |
| Fixed and retested | 1 | BUG-001 |
| Remaining open | 1 | BUG-002 (needs a rules decision) |
| Suspected, not reproduced | 3 | SUS-001 to SUS-003 in BUG_LOG.md |

Retest of BUG-001: both regression tests fail on `5c4fa23` and pass with the fix; the
existing `modernNeverRematchesWhileItCanAvoidIt` and every other core, rules and UI test
pass with the fix. Measured on `5c4fa23` with a temporary counter in the simulation test:
28, 29 and 35 of 140 rounds (three runs) repeated a match that another pairing would have
avoided. With the fix the same test allows none; in the run recorded here 4 of 140 rounds
had a rematch, each one checked to be unavoidable.

### Manual cases

**None of the manual cases has been run.** All 28 rows of TEST_PLAN.md and all 53 cases of
REGRESSION_CASES.md are `Not Run`. In particular nothing here says how the app behaved at a
real event.

One setup step was checked by script, not by a person: `qa_sandbox.bat fresh` started the
app, a new database appeared in `%TEMP%\tcg-qa-sandbox`, and the names, sizes and
modification times of the files in `%LOCALAPPDATA%\TcgTournamentManager` were the same
before and after.

Still waiting for a person, in rough priority order:

1. Part A of TEST_PLAN.md on the build to be used at the next event.
2. RG-403 and RG-404 (BUG-001 seen through the screens) and RG-704 (BUG-002 as the organizer sees it).
3. Printing on paper (RG-1001 to RG-1003): the automated tests check what is on the sheet, not whether it can be read across a room.
4. Layout at 150 % display scaling (RG-1404), which no automated test covers.
5. Parts B and C at a real event.

### Not measured

- **Code coverage.** No coverage tool was found on this machine (`OpenCppCoverage`, `gcov`
  and `llvm-cov` are not installed), so no percentage is given.
- **Performance.** See the note on run times above.

## Addendum, 2026-10-10: BUG-002 closed

The tables above are the run of 2026-10-09 and are left as they were. Since then the club
chose Pokémon's second tiebreaker (opponents' opponents' win percentage), it was implemented,
and the expected-failure test `pokemonSecondTiebreakerIsWorkedOut` was replaced by five tests
that pass. Build `0.2.1-preview.3`, working tree on top of `5c4fa23`, not committed.

| Program | Passed | Failed | Expected failures | Skipped / blocked |
|---|---:|---:|---:|---:|
| `test_core` | 88 | 0 | 0 | 0 |
| `test_rules` | 18 | 0 | 0 | 0 |
| `test_ui` | 34 | 0 | 0 |

Bugs: confirmed 2 (BUG-001, BUG-002), fixed and retested by automated tests 2, remaining
open 0. The manual cases RG-403, RG-404 and RG-704 are still Not Run.

## Addendum 2, 2026-10-10: remaining Pokémon checks

Build `0.2.1-preview.3, build 5c4fa23c-modified-20261010-0122`, packaged exe, disposable data or a scratch copy of the data; not committed.

| Check | Result |
|---|---|
| Automated: `test_core` / `test_rules` / `test_ui` | 88 / 18 / 34 passed; 0 failed, 0 expected failures, 0 skipped in each |
| Ten Pokémon tournaments completed before the second tiebreaker existed (scratch copy) | Pass after a fix. Order, records and Opp Win% as saved; "—" in every second-tiebreaker cell, none of which had a saved value; nothing rewritten. Found and fixed: the standings screen and CSV numbered rows 1 to n instead of showing the saved placing, which differed for one tournament whose saved placings skip a number (PDF and player profile already showed the saved placing) |
| Pokémon tournament completed with this build, after a restart | Pass. Both tiebreakers saved and shown; figures match a hand calculation |
| Standings PDF: columns, long names | Pass after a fix. No column runs into another, also with 100-character names. Found and fixed: text was broken in the middle of words although it had spaces; it now breaks between words, and only a word too long for the column is broken inside |
| RG-704 as written | Blocked: the worked example needs round-2 pairings the app does not make. Recorded in REGRESSION_CASES.md |
| RG-704b (the same check with whatever pairings the app makes) | Pass |

Pokémon's two tiebreakers are stored and compared at nine-decimal precision and shown to three.
