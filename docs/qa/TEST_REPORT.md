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

## Addendum 3, 2026-10-12: release 0.2.3

Version 0.2.3, commit `dc39504`, build identifier `dc395042`, built against Qt 6.11.3 and packaged
from committed source as `TcgTournamentManager-0.2.3-win64.zip`.

**Status: not fully verified.** The UI suite has not had a clean run on the packaged binaries.

### Automated suites

| Run | `test_core` | `test_rules` | `test_ui` |
|---|---|---|---|
| Final source, development build folder | 101 passed, 0 failed | 18 passed, 0 failed | 42 passed, 0 failed |
| Package binaries, attempt 1 | 101 passed, 0 failed | 18 passed, 0 failed | 41 passed, **1 failed** |
| Package binaries, UI attempt 2 | not rerun | not rerun | 41 passed, **1 failed** |

No expected failures and no skipped tests in any run.

The two UI failures were different tests:

- Attempt 1: `tournamentsCanBeRenamedFromTheirPagesInEveryState`. The rename dialog's field held
  "fo" where the tournament name was expected. No test contains that text. Run alone three
  times afterwards: passed, failed (a later check of the same prefilled field), passed.
- Attempt 2: `ordinaryActionsLeaveThePageWhereItIs`. Keyboard focus was not on the button the
  test had just used.

Both tests depend on keyboard focus, and the suite opens real dialogs that take it. While they
ran the PC was in use: the last real keyboard or mouse input was under a second old when
checked. That is the likely cause, but it has not been confirmed by a clean run, so the
failures stay open here.

**Still to do:** with nobody using the PC, run `set BUILD_DIR=build-release` and then
`run_tests.bat`, and record the result in this file.

### Packaged app, disposable data

Done by hand through UI Automation on the extracted ZIP. All passed.

| Check | Result |
|---|---|
| Start on an older-schema database | Pass. Backed up first, updates 5 and 6 applied, all 8 players kept |
| One-on-one event, 2 rounds as configured | Pass. Enrollment (a name with markup shown as typed), pairing, clock start, pause, resume and expiry with results left alone, a result undone and corrected, final standings equal to a hand calculation |
| Commander event, 2 rounds as configured | Pass. Check-in undone for one player, pod created, a result cleared and corrected, a draw, final standings |
| PDF export | Pass |
| Final standings after a restart | Pass, both events |
| Large text, narrow window, both themes, icons | Pass |
| Backup export, opened from a separate folder | Pass, both events present |
| Package contents | 30 files; no databases, logs, settings, test data or assistant files; 14 Qt files, all 6.11.3 |

Duplicate names, removing a player while keeping history, and ending a tournament early were
not repeated by hand; the automated tests for them passed on the package binaries.

### Found during this check

BUG-012 (BUG_LOG.md): the first 0.2.3 package crashed when a button was pressed behind an open
question dialog. Fixed in `dc39504`; the package named above is the rebuilt one and survives
the same steps.

### Not covered

- No code signing, installer or clean-machine test.
- Enrolling a player after a tournament has started is still allowed by the core and is
  recorded only as today's behaviour (`lateEnrollmentAsItWorksToday`); the policy is undecided.
- Buttons pressed behind an open dialog by an accessibility or automation tool still act;
  they no longer crash the app.

## Addendum 4, 2026-10-15: packaged UI suite rerun for 0.2.3

This closes the open item in Addendum 3. **Status: the release checks for 0.2.3 are done.**
That covers the three automated suites and the manual packaged-app run in Addendum 3; it
is not a claim that the app has no remaining defects.

### Which binaries were tested

The test programs were run straight from the folder that produced the package
(`build-release`), without rebuilding, so the build identifier stayed `dc395042`
(commit `dc39504`). The program in that folder, the one in
`dist\TcgTournamentManager-0.2.3` and the one inside
`TcgTournamentManager-0.2.3-win64.zip` have the same SHA-256 (`23ba3879...42b0a3`), and
the ZIP's SHA-256 is unchanged (`d26359e0...a1c795`). No source, test or build file has
changed since `dc39504`; the later commits are documentation only. Qt 6.11.3, from the
kit the package was built with. `TCG_DATA_DIR` pointed at an empty disposable folder,
and each test uses its own temporary database.

### Result

| Program | Passed | Failed | Expected failures | Skipped |
|---|---:|---:|---:|---:|
| `test_core` | 101 | 0 | 0 | 0 |
| `test_rules` | 18 | 0 | 0 | 0 |
| `test_ui` | 42 | 0 | 0 | 0 |

One run, 22:47 to 22:54. Both tests that failed in Addendum 3
(`tournamentsCanBeRenamedFromTheirPagesInEveryState` and
`ordinaryActionsLeaveThePageWhereItIs`) passed.

### Conditions

A log of keyboard and mouse activity was kept during the run. It recorded real input
for most of it, with other windows (a browser, VS Code, the Windows task switcher and
Run box) in the foreground at times, so the PC was not idle. The suite passed anyway.
The earlier failures are therefore not explained by this run alone: they depend on
keyboard focus and may still appear occasionally when the PC is in use. If one does,
log it with the activity at the time rather than rerunning until it passes.

## Addendum 5, 2026-10-18: 0.2.3 release ZIP with the organizer README

The ZIP for the GitHub release is the verified 0.2.3 package with only `README.txt`
replaced by the organizer guide from `packaging/README.txt`. It was made by extracting
the verified ZIP and zipping it again; nothing was rebuilt.

| | |
|---|---|
| Release ZIP | `dist\release-0.2.3\TcgTournamentManager-0.2.3-win64.zip` |
| Release ZIP SHA-256 | `D61CDB50EE72B3B3756603207FD54B56A5654F0756E2154F1406B5FF1351767B` |
| Verified ZIP, kept unchanged | `dist\TcgTournamentManager-0.2.3-win64.zip`, SHA-256 `D26359E0...A1C795` |
| `TcgTournamentManager.exe` SHA-256, in both | `23BA3879BD0F18EDB2808419C64481104D65D9AE7778168FDCC6766E1F42B0A3` |
| Build identifier | `dc395042`, source commit `dc39504` |

Checks on the release ZIP:

| Check | Result |
|---|---|
| Compared file by file with the verified ZIP | 30 files in each; only `README.txt` differs |
| Contents | Program, 14 Qt 6.11.3 files, Microsoft C++ runtime, organizer README, licence notices; no databases, logs, settings, test or assistant files |
| Extracted and started with only Windows folders on PATH, on a disposable data folder | Pass. Shows v0.2.3 and build `dc395042` |
| Create a tournament, enroll four players, report round 1, finalize, restart | Pass. Final standings shown; after the restart the tournament is listed as completed |

The test suites were not run again: no code or runtime file changed.

## Addendum 6, 2026-10-19: optional second place in Commander pods (not a release)

A build with optional second-place recording for Commander pods. It still shows version
0.2.3 because that was requested, but it is **not** the published 0.2.3: that release is
build `dc395042` (Addendum 5) and stays as it was. Tell the two apart by the build
identifier in Settings.

| | |
|---|---|
| Build identifier | `229ef86a-modified-20261019-2252` (commit `229ef86` plus the uncommitted feature changes) |
| Package | `dist\second-place-229ef86a-modified-20261019-2252\TcgTournamentManager-0.2.3-229ef86a-modified-20261019-2252-win64.zip` |
| Package SHA-256 | `875E5CDE44D136DF19CE10B5D8169A23B0736B2777CD649F886548E2C417E962` |
| `TcgTournamentManager.exe` SHA-256 | `A74C0B670CF383D648AFF456440CFACDA64D288B7A83DA0508A6C021EB9453CE` |
| Published 0.2.3 ZIP and exe | Unchanged: `D61CDB50...1767B` and `23BA3879...B0A3`, checked before and after packaging |

The package holds the binaries the suites below ran on (same exe checksum); it was not
rebuilt in between. Compared with the published 0.2.3 package: the same 30 files, and only
`TcgTournamentManager.exe` and `README.txt` differ.

### Test suites

| Program | Passed | Failed | Skipped |
|---|---|---|---|
| `test_core` | 110 | 0 | 0 |
| `test_rules` | 18 | 0 | 0 |
| `test_ui` | 43 | 0 | 0 |

`run_tests.bat` built the program and ran `test_ui`, but its shell could not start
`test_core.exe` and `test_rules.exe` by bare name and printed the old result files from
2026-10-12. Those two were then run directly on the same build. The script does not notice
when a test program fails to start; that is not fixed here.

New tests: core `secondPlaceIsSavedWithTheResultAndEarnsNothing`,
`anInvalidSecondPlaceIsRefusedAndChangesNothing`, `secondPlaceIsCorrectedAndClearedWithTheResult`,
`secondPlaceInAFinalizedRoundIsCorrectedWithoutTouchingLaterRounds`,
`secondPlaceInACompletedEventLeavesTheFinalPlacingsAlone`,
`standingsAndPairingsAreIdenticalWithAndWithoutSecondPlace`,
`aFailedSaveKeepsNeitherTheResultNorItsSecondPlace`, `aRemovedPlayerKeepsTheirSecondPlaceAndName`,
`olderResultsGainNoSecondPlaceWhenTheDatabaseIsUpdated`; ui
`commanderPodsRecordAnOptionalSecondPlace`. `existingDatabaseIsUpgradedInPlace` now expects
seven updates.

### The package on older data

On a disposable copy of a database made by the published 0.2.3 build (one finished Commander
event of two rounds, five players), with only Windows folders on PATH:

| Check | Result |
|---|---|
| First start | Pass. Update 7 applied; a copy was saved in `backups` first, without the new column |
| Existing results | Pass. Seats, points, final placings, final standings and status identical to the backup; no second place filled in |
| "Correct result..." on round 1, Second place = a losing player, with a reason | Pass. "2nd" shown beside that player, points unchanged, no Keep or Rebuild question although round 2 exists, event still complete, one audit entry |
| Restart | Pass. The badge is still there; no second backup; Settings shows `v0.2.3 · build 229ef86a-modified-20261019-2252` |

## Addendum 7, 2026-10-23: club scoring with second place, and the optional top-four final (not a release)

Development build of the agreed Commander rules (`commander-rules-4`): a clear second place
earns 2.5 points and stays a loss, second place is recorded as not yet recorded, a player,
or no clear second, and an event may add one top-four final pod after its qualifying rounds.
The version shown is still 0.2.3, as asked; this is **not** the published 0.2.3 (build
`dc395042`), and that release's verification does not cover it.

| | |
|---|---|
| Starting point | commit `53d63cc`, clean working tree, Qt 6.11.3, MSVC 2022, Release, build folder `build-qt6113` |
| Baseline before any change | build `53d63cc1-modified-20261023-1222` (only `run_tests.bat` changed): core 110, rules 18, UI 43 passed, 0 failed |
| Final build tested | `53d63cc1-modified-20261023-1410` (commit `53d63cc` plus the uncommitted changes) |
| `TcgTournamentManager.exe` SHA-256 (final build) | `527E0181704D4DDAD4957309112864171B86EA4608A6A4B0C1330FDF2EAF5A24` |
| Package | none: see below |

### Test runner first

`run_tests.bat` had BUG-013: in a shell where Windows does not search the current folder it
could not start `test_core.exe` and `test_rules.exe`, and printed the result files of
2026-10-12 as if they were new (seen in Addendum 6). It was fixed first and checked with a
scratch harness that runs the script's own `:run` block against a missing program, one that
writes no results, a failing one and a passing one, each with a stale result file present:
the first three are reported as failed, the last as passed, and the stale totals are never
shown. Every result below comes from the fixed script, which prints the build id and the
SHA-256 of each test program it runs.

### Test suites

| Program | Passed | Failed | Skipped |
|---|---|---|---|
| `test_core` | 133 | 0 | 0 |
| `test_rules` | 31 | 0 | 0 |
| `test_ui` | 45 | 0 | 0 |

One run of `run_tests.bat`, 14:10 to 14:17, exit code 0, on the final build. The run before
it (build `...-1350`, 13:50 to 13:57) gave the same totals; between the two only the
registration summary's wording changed.

New tests: core 21 (scoring over rounds, the three second-place states, invalid second
places, corrections without double awards, a finalized-round correction, pairing from half
points, earlier events, a removed player, and the final: with and without it, eligibility,
fully tied seeds, simultaneous creation, places, drawn finals, invalid data, corrections and
rebuilds, ending early, failed saves, the update of older data, logging and an unusable log
folder); rules 4 functions with 9 data rows (a hand-worked scoring example, points shown as 5
and 2.5, pairing with half points, the final's places); UI 2 (second place on the round
screen; the final from registration to correction, in both themes, both text sizes and a
narrow window). Existing tests that played rounds now give each won pod a second-place
decision; the previous feature's second-place tests now use an event with the earlier rules,
which is what they describe.

Two of the new tests were checked against deliberate faults: reading points as whole numbers
in pairing, and ordering a group of finalists by the worse seed. Both were caught (the first
only after the test was changed to compare the pairing's score cost, worked out by hand).

### Smoke test of the program

Run on the development build `53d63cc1-modified-20261023-1350` (`TcgTournamentManager.exe`
SHA-256 `D2CEB772C3E800E12EE01FBB27E90A4BD39C7C0A9E2079C57C8C9E70369710B4`), started from
the build folder with the Qt 6.11.3 kit and Windows folders only on PATH, on disposable data
folders (`TCG_DATA_DIR`), driven through UI Automation. Not a packaged program.

| Check | Result |
|---|---|
| New Commander event, 8 players including `=Gus Formula` and a long name, 2 rounds, "Add a top-four final" | Pass. Registration showed "2 qualifying rounds + 1 final" and the scoring; the summary line still said "2 rounds in total": fixed afterwards, see below |
| Round 1: Wins, then "Record 2nd place…" with a player; second pod with "No clear second" | Pass. Provisional marking and disabled Finalize until both were decided; "2nd +2.5" badge; 2.5 points |
| Correct round 1 after round 2 was published: second place moved | Pass. Keep or Rebuild asked; Keep moved the 2.5 points; no "not recorded yet" offered for a finalized round |
| Round 2, finalize the last qualifying round | Pass. Event not finished; four qualifiers with seeds shown |
| Create final; print the final pod | Pass. Seeds confirmed in a question; one pod seated by seed; print preview showed the four seats |
| Record an elimination, close and reopen the program | Pass. Still recorded after the restart |
| Record result: winner while two players were still in | Pass. Preview and saved places Eve 1st (won), Hal 2nd, Chris 3rd (out together, by seed), Dan 4th |
| Standings: Export CSV and Save PDF | Pass for CSV (places, notes, 7.5, formula-safe name). PDF written, 20,563 bytes; not rendered here (no PDF viewer in this environment) |
| Correct the final to a drawn final, three still in | Pass. Placed by seed, marked "by seed", banner says nobody won; audit `FINAL_CORRECTED` with the earlier places and the reason |
| Saved records, read afterwards from the scratch file | Pass. Second-place states, final rows, places 1-8, audit entries, ids-only log entries; foreign keys and integrity check clean |
| Copy of an older database (update 7, a finished event with a recognised second place) | Pass. Backup written first; update 8 applied; the event kept `commander-rules-3`, no points for its second place, no final, no 2nd column |

Not run by hand: the drawn final with four and with two players still in, the fewer-than-four
case, ending early during the final, and a failed save. These are covered by core tests only.

The registration summary was changed after the smoke test; the final build was run through
all three suites, and the UI test checks the new wording. The smoke test itself was not
repeated on the final build.

### Package

None was made. `package.bat` packages only committed source (or a changed tree with
`ALLOW_MODIFIED=1`) and always writes `dist\TcgTournamentManager-<version>` and its ZIP, so
with the version still 0.2.3 it would replace the verified 0.2.3 package. A package needs the
changes committed and, to keep the published 0.2.3 intact, a new version number.

## Addendum 8, 2026-10-23: version set to 0.2.4

On the branch `release/0.2.4` the version in `CMakeLists.txt` was changed from 0.2.3 to
0.2.4 for the changes tested in Addendum 7. Nothing else changed. No build carrying 0.2.4 has
been made or tested yet: the results in Addendum 7 are for builds labelled 0.2.3 from the
same code, and the release checks for 0.2.4 still have to be run on its packaged build.

