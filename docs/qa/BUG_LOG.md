# Bug log

One row per problem. Only things that were actually seen go in the first table; anything
that is a worry from reading the code, or was seen once and could not be made to happen
again, goes under **Suspected**. Full write-ups are in [issue-drafts/](issue-drafts/), in
the same shape as the GitHub template (`.github/ISSUE_TEMPLATE/bug_report.md`). Drafts are
local: nothing here has been published as a GitHub issue.

**Severity** — S1 Critical: wrong pairings, results or standings, lost or changed data, or
the app cannot be used. S2 Major: a feature fails or a rule is broken, saved data is
correct. S3 Minor: works but is confusing or awkward. S4 Cosmetic.

**Status** — New → Reproduced → Fixed (retested) / Open / Won't fix / Cannot reproduce.

## Reproduced

| ID | Title | Version / commit | Environment | Severity and impact | How it was found | Status | Regression check | Fix | Retest |
|---|---|---|---|---|---|---|---|---|---|
| BUG-001 | One-on-one pairing repeats a match although a round with no rematch exists | 0.2.1-preview / `5c4fa23` | Windows 11 Home 10.0.26200, Qt 6.10.2, MSVC 2022, Release x64, test database | S2. Small fields in later rounds; the organizer cannot change a pairing. No saved data is wrong. | Reading `swiss::generatePairings`, then a test that fails on `5c4fa23` | **Fixed locally, not committed** | `rules` `anAvoidableRematchIsNotPaired`, `aRematchIsOnlyPairedWhenNoOtherPairingExists`; manual RG-403, RG-404 | Working-tree change to `src/core/swiss.cpp` (`pairWithoutRematch`) | 2026-10-09: both tests fail before the fix and pass after it; full suite rerun, see TEST_REPORT.md. Manual RG-403/404 not run. |
| BUG-002 | Pokémon standings: the second tiebreaker (OGW%) is 0 for every player | 0.2.1-preview / `5c4fa23` | Same | S2. Pokémon players tied on points and Opp Win% are not separated by the tiebreaker the screen names. | Reading `figuresFromMatches`, then a test | **Fixed locally, not committed.** Rules decision made 2026-10-10 (club policy): opponents' opponents' win percentage, shown as "Opp Opp Win%" | `rules` `pokemonTiesOnOppWinAreBrokenByOppOppWin` (hand-worked), `pokemonOppOppWinLeavesOutByesAndCountsARematchTwice`, `pokemonPlayerWithNoOpponentsGetsTheFloor`, `pokemonTiebreakersAreNotRoundedForDisplayBeforeComparing`, `pokemonOppOppWinLeavesFinishedTournamentsAloneAndReachesLiveOnes`; manual RG-704 | Working-tree change to `src/core/swiss.cpp` (`figuresFromMatches`, `tiebreakColumns`, `tiebreakText`) | 2026-10-10: the expected-failure test was replaced by the five tests named here; all pass (rules suite 18 passed, 0 failed, 0 expected failures). Seen in the packaged app on disposable data, see the issue draft. Manual, 2026-10-10: RG-704 is Blocked as written (the app chooses the round-2 pairings, so the worked example cannot be replayed); its executable form RG-704b passed in the packaged app. Stored and compared at nine-decimal precision. |

Details: [BUG-001](issue-drafts/BUG-001-avoidable-rematch.md),
[BUG-002](issue-drafts/BUG-002-pokemon-ogw-always-zero.md).

## Suspected — not reproduced

These come from reading the code on 2026-10-09. None has been seen to go wrong and none has
a failing test. Move one to the table above only after reproducing it.

| ID | What might be wrong | Why it is only suspected | Next step |
|---|---|---|---|
| SUS-001 | When every possible round contains a rematch, the pairing does not look for the round with the fewest rematches, and the bye is chosen before pairing, so a different bye might have avoided one. | Not tested. The fix for BUG-001 deliberately left this case as it was. | Decide whether it matters at club size; if so, write a test with the smallest example first. |
| SUS-002 | `swiss::finalizeTournament` does not itself check that the last configured round has been reached. | The round screen only offers "Finalize tournament" on the last round, so no way to reach it through the app was found. Not tried at the function level either. | Try it in a test; if it completes an event early, decide whether the function should refuse. |
| SUS-003 | One-on-one standings label a column "GW%" (game-win), but it is match wins ÷ matches with a bye counted as a win; the figure used for opponents leaves byes out. | This is how the code is written and the worked example in `test_rules` confirms the arithmetic. Whether it is a bug or just a label depends on the intended rule. | Confirm the intended rule; rename the column or change the figure. |

## Observations that are not bugs

| What | Note |
|---|---|
| One-on-one formats have no check-in and no drop. | A player can be removed from the list only before the event starts. Someone who leaves mid-event is still paired each round. Feature gap; the test plan marks check-in and drop as Commander-only. |
| Pokémon scores a win as 1 point and a draw as 0; One Piece scores a draw as 0. | These are the configured rules (`swiss::points`), the same as the earlier Python version, and the tests expect them. Confirm they are what the club wants. |

## Template for a new row

Copy a row, give it the next id, and fill in: a title that says what went wrong and where;
the version from the home screen and the commit if built from source; Windows version,
display scaling, theme and text size when it is a layout problem; numbered steps; expected
against actual; severity with who was affected and any workaround; a screenshot, photo or
log file name; then status, fix reference and retest result as they happen. For a visual
problem that cannot be automated, add a case to REGRESSION_CASES.md section 14 or 15 and
name it under "Regression check".
