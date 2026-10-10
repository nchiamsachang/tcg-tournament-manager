# Regression cases

The detailed cases behind [TEST_PLAN.md](TEST_PLAN.md). Run them on **test data only**
(`qa_sandbox.bat fresh`), never on the real database and never during a live event: several
of them end tournaments, remove players and restart the app on purpose.

- **Status**: `Pass`, `Fail`, `Not Run` or `Blocked`. Every manual status below is `Not Run`
  until somebody performs the steps; record runs in [TEST_REPORT.md](TEST_REPORT.md).
- **Automated by** names the test that checks the same behaviour without a person. A manual
  case is still worth running when it says so: the automated test proves the rule, the
  manual case proves a person can see and use it. Test programs: `core` = `tests/test_core.cpp`,
  `rules` = `tests/test_rules.cpp`, `ui` = `tests/test_ui.cpp`.
- Sample players are listed in TEST_PLAN.md.

The rules these cases expect are the ones the project documents and implements
(`README.md`, `docs/COMMANDER.md`, `src/core/swiss.cpp`):

| | One-on-one (Modern, One Piece, Pokémon) | Commander |
|---|---|---|
| Points | MTG 3 / 1 / 0, One Piece 3 / 0 / 0, Pokémon 1 / 0 / 0 (win / draw / loss); a bye is a win | win 5, draw 1, loss 0, bye 5 |
| Groups | Pairs; one bye when the count is odd, to the lowest-placed player without one | Pods of 3 or 4 with the most fours; 5 players = one pod of 4 and a bye; fewer than 3 cannot be paired |
| Tiebreakers | MTG: OMW%, GW%, OGW%. One Piece: OMW%, GW%. Pokémon: Opp Win%, Opp Opp Win% (club policy). Floor 33 % (Pokémon 25 %) | OMW%, MW%, then a seeded random order. Floor 20 % |
| Rounds | The configured number is the whole event | The configured number is the whole event; no playoff or final |
| Check-in / drops | Not available: a player can only be removed from the list before the start | Check-in before the start; drops between rounds |

---

## 1. Create and rename a tournament

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-101 | Empty test database | Create a tournament for each game (and MTG Commander) with a name, 2 rounds, default round length. | Each appears in its game's list as not started, with 0 players. | | Not Run | | core `commanderAndModernCoexist` |
| RG-102 | — | Try to create one with a blank name, then with 0 rounds, then a round length of 0 and of 241 minutes. | Each is refused with a message; nothing is added to the list. | | Not Run | | core `tournamentDetailsAreValidatedBeforeAnythingIsSaved` |
| RG-103 | A tournament in each state: registering, running, completed, ended early | Rename each from its page. Try a blank name and a 101-character name. | The new name shows in the heading, lists and History; blank and over-long names are refused; rounds, players, results and status are unchanged. | | Not Run | | core `renamingChangesOnlyTheNameInEveryState`; ui `tournamentsCanBeRenamedFromTheirPagesInEveryState` |

## 2. Register, enroll, check in and drop players

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-201 | Modern tournament, not started | Enroll `Ana`…`Ed` with "Add & enroll" and from the search list. Enroll `Ana` again. Remove `Ed`, then add him back. | Five players listed once each; "Start tournament" is enabled from 2 players. | | Not Run | | core `removingAPlayerHidesThemAndKeepsEveryRecord` (no double enrollment) |
| RG-202 | Commander event, 4 players registered | Un-tick "Checked in" for two of them. Try to start. Check one back in and start. | With 2 checked in, starting is refused ("at least 3"). With 3 it starts and only those 3 are seated. | | Not Run | | core `startRequiresThreeCheckedInPlayers`, `playersNotCheckedInAreLeftOut` |
| RG-203 | Commander event, 8 players, round 1 finalized | Drop one player in "Players & drops". Start round 2. | 7 players seated; the dropped player is in round 1's pods and in the standings, marked dropped after R1. | | Not Run | | core `dropsChangeFuturePodsButKeepHistoryAndRoundCount` |
| RG-204 | RG-203 before starting round 2 | Drop a player, then reinstate them, then start round 2. | All 8 are seated. | | Not Run | | core `dropCanBeUndoneBeforeTheNextRound` |
| RG-205 | Commander event, 4 players, round 1 finalized | Drop two players. | Round 2 cannot be started; the screen explains why and offers "Finish tournament on current standings". Finishing adds no round. | | Not Run | | core `tooManyDropsBlockPairingWithAnExplanation` |
| RG-206 | Commander event, 10 players, 6 rounds | Drop one player after each round. | Pods each round: 4-3-3, 3-3-3, 4-4, 4-3, 3-3, then one pod of 4 and a bye. | | Not Run | | rules `commanderPodsFollowThePolicyAsPlayersDropRoundByRound` |

## 3. Players with the same name

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-301 | Player `Alex Smith` exists | Add `Alex Smith` again. Choose "Create a different player with this name". | A second player with a new id; the first is unchanged. Nothing is merged. | | Not Run | | ui `addingANameThatExistsAsksWhichPlayerIsMeant` |
| RG-302 | Both `Alex Smith` and `Ana` enrolled in one tournament | Start it; look at registration, pairings, standings and the printed sheet. | Both Alexes are shown as `Alex Smith · #000N` with different ids everywhere; `Ana` has no id. | | Not Run | | core `playersWithTheSameNameAreToldApartByIdWithinATournament`; ui `playersHaveIdsThatOnlyShowWhenNamesClash` |
| RG-303 | RG-302 | Rename one Alex to `Alexander Smith`. | Ids disappear from both names; each keeps its own id on its profile and its own results. | | Not Run | | core `playerIdsArePermanentAndNamesMayRepeat` |

## 4. One-on-one pairing

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-401 | Modern, 6 players | Start. | 3 matches at tables 1–3, no bye, each player once. | | Not Run | | rules `everyOneOnOneRoundAssignsEachActivePlayerExactlyOnce` (2–33 players, all three games) |
| RG-402 | Modern, 5 players, 5 rounds | Play all 5 rounds; note who has the bye each round. | One bye per round, a different player each time. | | Not Run | | rules `nobodyGetsASecondByeBeforeEveryoneHasHadOne` |
| RG-403 | Modern, 6 players, 3 rounds | Play 3 rounds, reporting any results. Compare each round's pairings with the earlier rounds. | Nobody plays the same opponent twice. (BUG-001 regression check.) | | Not Run | BUG-001 | rules `anAvoidableRematchIsNotPaired`, `aRematchIsOnlyPairedWhenNoOtherPairingExists`; core `modernNeverRematchesWhileItCanAvoidIt` |
| RG-404 | Modern, 4 players, 3 rounds | Play all 3 rounds. | Every player meets each of the other three exactly once. | | Not Run | | rules `aRematchIsOnlyPairedWhenNoOtherPairingExists` |

## 5. Commander pods

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-501 | Commander events with 3, 4, 5, 6, 7, 9 and 11 checked-in players | Start each. | Pods: 3; 4; 4 + bye; 3-3; 4-3; 3-3-3; 4-4-3. No pod of 2 or 5; every player once. | | Not Run | | core `podSizes`, `everyRoundIsValidForManyFieldSizes` |
| RG-502 | Commander, 7 players, 3 rounds, default "Lowest standings" | Play round 1, start round 2. | The two round-1 winners are in the four-player pod; the three-player pod has only players on 0 points. | | Not Run | | core `threePlayerPodsGoToTheLowestStandingsByDefault` |
| RG-503 | Commander, 5 players, 4 rounds | Play all rounds; note the bye each round. | Four different players have the bye. | | Not Run | | core `fivePlayersShareTheBye` |

## 6. Recording results

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-601 | Modern, round in play | Report a win for player 1, change nothing else; "Undo"; report the other player. | The result shown follows each action; pending count is right; the bye row has no buttons to change it. | | Not Run | | core `invalidOneOnOneResultsAreRejectedAndChangeNothing` |
| RG-602 | Modern, round 2 in play | Look for any way to change a round-1 result (round screen, standings, player profile). | There is none: only the current round's results can be changed. | | Not Run | | core `invalidOneOnOneResultsAreRejectedAndChangeNothing` |
| RG-603 | Commander pod of 4 | Report a win. On another pod report a draw with one player marked eliminated. | Win: winner 5, others 0. Draw: the three still playing get 1, the eliminated player 0 and a loss. | | Not Run | | core `scoring`, `drawResultsAndStandings` |
| RG-604 | Commander, round 1 finalized, round 2 started | "Correct result…" on a round-1 pod. Choose Keep; repeat and choose Rebuild. | Keep: standings change, round 2 stays. Rebuild: round 2 is discarded and can be started again. Rebuild is refused once round 2 has a result. | | Not Run | | core `correctionBehindAPublishedRoundNeedsAResolution` |

## 7. Standings and tiebreakers (independently worked examples)

Work the expected figures out on paper from the rules table at the top **before** looking
at the standings screen. The automated tests use the same worked examples; the full
arithmetic is in the comments of `tests/test_rules.cpp`.

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-701 | Modern, 4 players, 2 rounds, no draws | Play both rounds. Call the round-1 winners W1, W2 and the losers L1, L2; W1 beats W2 in round 2. | Points 6 / 3 / 3 / 0. W1 first. If L1 beats L2: W2 and L1 both have OMW 66.5 %. If L2 beats L1: W2 (OMW 75 %) places above L2 (OMW 41.5 %). The 0-2 player is last. | | Not Run | | rules `aPointsTieIsBrokenByOpponentsMatchWinPercentage` |
| RG-702 | Modern, 5 players, 2 rounds | Play both rounds with at least one draw. For each player compute points = 3 × wins + draws, the bye counting as a win. | Every player's points and record match the paper figures. | | Not Run | | rules `mtgStandingsMatchAHandWorkedExampleWithADrawAndByes` |
| RG-703 | Commander, 5 players, 2 rounds | Round 1: a win. Round 2: a draw with nobody eliminated. Compute points (5 / 1 / 0, bye 5) and MW% = points ÷ (5 × rounds played), minimum 20 %. | Matches paper. The worked example with these results gives 6, 6, 5, 1, 1 points; of the two players on 6, the one who played both rounds (OMW 31.7 %) is above the one who had the bye (OMW 26.7 %). | | Not Run | | rules `commanderStandingsMatchAHandWorkedExampleWithADrawAndByes` |
| RG-704 | Pokémon tournament, 6 players, 2 rounds | Play the results in the comment above `pokemonTiesOnOppWinAreBrokenByOppOppWin`, then read the "Opp Opp Win%" column. | The column is headed "Opp Opp Win%". Figures 0.625, 0.375, 0.438, 0.625, 0.750, 0.438 for A to F; D places above C (level on points and Opp Win%). | 2026-10-10, 0.2.1-preview.3, build 5c4fa23c-modified-20261010-0122, packaged exe, disposable data. Round 1 was played as written (A beats B, D beats C, E beats F). Round 2 could not be: the case needs C–A, E–B, F–D and the app paired E–D, A–F, B–C. Pairings are made by the app and cannot be set by hand, so the worked example cannot be reproduced through the screens. The heading "Opp Opp Win%" was present. | Blocked | BUG-002 (fixed). Blocked by the case, not by a defect: see RG-704b | rules `pokemonTiesOnOppWinAreBrokenByOppOppWin` |
| RG-704b | Pokémon tournament, 6 players, 2 rounds | Play both rounds with any results and write down who met whom and who won. On paper, for each player: win rate (floor 25 %), Opp Win% = average of the opponents' win rates, Opp Opp Win% = average of the opponents' Opp Win%. Then open the standings. | The column is headed "Opp Opp Win%" and does not run into "Opp Win%". Every figure matches the paper figure to three decimals. Players level on points and Opp Win% are ordered by Opp Opp Win%, highest first. | 2026-10-10, 0.2.1-preview.3, build 5c4fa23c-modified-20261010-0122, packaged exe, disposable data. Round 1: P2 beat P4, P3 beat P5, P6 beat P1. Round 2: P3 beat P6, P1 beat P2, P4 beat P5. Paper and screen agree for all six: P3 2 pts 0.375 / 0.750; P6 1 pt 0.750 / 0.438; P1 1 pt 0.500 / 0.625; P2 1 pt 0.500 / 0.438; P4 1 pt 0.375 / 0.625; P5 0 pts 0.750 / 0.375. P1 and P2 are level on points and Opp Win% and P1 is placed above P2 on Opp Opp Win%. Heading correct, no overlap. | Pass | | rules `pokemonTiesOnOppWinAreBrokenByOppOppWin` |
| RG-705 | Any running tournament | Open standings several times; close and reopen the app. | Same table each time. | | Not Run | | core `lookingAtStandingsNeverChangesWhatIsSaved` |

## 8. Finishing after the configured rounds

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-801 | Modern, 4 players, 2 rounds | Report round 1; click "End round" twice quickly. Report round 2. | One round 2 only. Round 2 shows "Finalize tournament" and no "End round". After finalizing: completed, 2 rounds, no way to add a third. | | Not Run | | core `oneOnOneEventStopsAtItsConfiguredRoundsHoweverOftenActionsAreRepeated`; ui `modernEventRunsThroughTheScreens` |
| RG-802 | Commander, 8 players, 3 rounds | Play 3 rounds. | After round 3 the action is "Finish tournament". After finishing there are exactly 3 rounds, all normal rounds; no final, semifinal or round 4 is offered. | | Not Run | | core `threeRoundEventIsExactlyThreeRounds`, `roundFourIsRejectedEveryWay`; ui `commanderThreeRoundEventRunsThroughTheScreens` |
| RG-803 | RG-802 with one pod of round 3 unreported | Try to finish. | Refused until every pod has a result. | | Not Run | | core `finishNeedsEveryResultOfTheLastRound` |

## 9. Round clock

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-901 | Tournament with 1-minute rounds, round in play | Start the clock; watch it reach 00:00. | Counts down from 01:00; at zero the expiry is announced (per Settings) and the round, results and status are unchanged. | | Not Run | | core `clockIsSavedWithTheRoundAndSurvivesARestart`; ui `clockWidgetShowsTheSavedTimeAndOnlyAnnouncesExpiry` |
| RG-902 | Round in play, 50-minute rounds | Start; after ~20 s Pause; wait 20 s; Start; Reset. | Paused time does not move; Start resumes from it; Reset returns to the full length, stopped. | | Not Run | | core `clockIsSavedWithTheRoundAndSurvivesARestart` |
| RG-903 | Clock running | Note the time, close the app, wait 30 s, reopen with `qa_sandbox.bat`. | The clock shows about 30 s less: it is worked out from saved timestamps, not restarted. | | Not Run | | core `clockIsSavedWithTheRoundAndSurvivesARestart` |
| RG-904 | Clock running | End the tournament early. | The clock stops where it was and cannot be started again. | | Not Run | | core `endingEarlyStopsTheClockForGood` |

## 10. Printing

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1001 | Modern round, 5 players including both long names | Print → pairings. | Each table once with both names in full; the bye is listed; same-name players carry ids. | | Not Run | | ui `pairingSheetsAndSignsComeFromTheSavedRound`, `longSheetsRepeatOverSeveralPagesAndModernListsTheBye` |
| RG-1002 | Same round | Print → table signs. Print on paper once. | One sign per table, number large enough to read across a room; numbers match the pairing sheet. | | Not Run | | ui `pairingSheetsAndSignsComeFromTheSavedRound` (content only; legibility on paper is manual) |
| RG-1003 | Commander round, 11 players | Print → pairings and pod signs. | Every pod with its seats in order; pod numbers match. | | Not Run | | ui `pairingSheetsAndSignsComeFromTheSavedRound` |

## 11. Restart and saved data

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1101 | A Modern and a Commander event, each mid-round with some results reported | Close the app; reopen with `qa_sandbox.bat`. | Same round, same pairings, same reported and pending results, same standings. | | Not Run | | core `stateSurvivesRestart`, `completionSurvivesARestart` |
| RG-1102 | Settings changed (theme, text size) | Restart. | Settings kept. | | Not Run | | core `preferencesPersistBesideTheDatabase` |

## 12. Removing a player keeps history

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1201 | A completed tournament in which `Bo` played | Note Bo's opponent's record and the final standings. Remove `Bo` from his profile. | Bo is gone from Players and from search. The finished tournament's pairings, results and standings still show Bo with the same figures; his opponent's history is unchanged. | | Not Run | | core `removingAPlayerHidesThemAndKeepsEveryRecord`; ui `removingAPlayerHidesTheirProfileAndLeavesHistoryAlone` |
| RG-1202 | `Cy` enrolled in a tournament that has not started; `Di` in one being played | Try to remove each. | Refused, with the tournament named and what to do there first. Nothing changes. | | Not Run | | core `aPlayerStillInATournamentCannotBeRemoved` |
| RG-1203 | RG-1201 | Add a new player called `Bo`. | A new id with no history; the old Bo's records are not attached to them. | | Not Run | | core `removingAPlayerHidesThemAndKeepsEveryRecord` |

## 13. Ending a tournament early

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1301 | Modern round with one result reported and one pending | "End tournament early"; first choose "Keep tournament", then do it again and confirm. | Keep: nothing changes. Confirm: it leaves the active list and appears under History marked Terminated; the reported result is still there, the pending match still pending, no winner or placings. | | Not Run | | core `endingEarlyKeepsEveryRecordAndMarksTheTournamentTerminated`; ui `endingATournamentEarlyAsksFirstAndKeepsItAsATerminatedRecord` |
| RG-1302 | RG-1301 | Open the terminated tournament; try to report, enroll or continue. | Read-only: no action changes it. | | Not Run | | core `aTerminatedTournamentRefusesFurtherChanges` |
| RG-1303 | Commander event mid-round | End it early. | Same as RG-1301; History shows the ended-early entry. | | Not Run | | core `endingACommanderEventEarlyLeavesItUnfinishedAndReadOnly`; ui `endingACommanderEventEarlyLeavesAReadOnlyRecord` |

## 14. Layout: narrow windows, larger text, long names, themes

These need eyes. The automated sweep checks that nothing is clipped or off-screen; it
cannot judge whether a screen is pleasant or a colour readable.

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1401 | A round in play with both long sample names | For each of: home, game hub, registration, round, standings, players, profile, settings — drag the window to its smallest size, then maximise. | No text cut off, no overlapping controls, no horizontal page scroll (standings tables may scroll sideways); every button can be reached. | | Not Run | | ui `everyScreenFitsEveryWindowSizeThemeAndTextSize`, `longNamesWrapAndActionsFitInANarrowWindow` |
| RG-1402 | RG-1401 | Settings → Text size → Large; repeat RG-1401 on the round and standings screens. | Same. | | Not Run | | ui `everyScreenFitsEveryWindowSizeThemeAndTextSize` |
| RG-1403 | RG-1401 | Switch Light / Dark from the top bar and from Settings; open a dropdown and a dialog in each. | Both controls agree; text, icons, dropdowns and dialogs are readable in both themes; clocks are green while running and red at zero. | | Not Run | | ui `themeToggleAndSettingsStayInSync`, `openedDropdownsAreOpaqueAndThemed`, `clocksAreGreenWhileCountingDownRedAtZeroAndNeutralOtherwise` |
| RG-1404 | Windows display scaling at 100 % and at 150 % | Repeat RG-1401 on the round screen. | Same. | | Not Run | | — (not automated) |

## 15. Routine actions do not move the page

| ID | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence | Automated by |
|---|---|---|---|---|---|---|---|
| RG-1501 | Commander registration with 11 players in a window short enough to scroll | Scroll to the bottom. Toggle "Checked in" on three players near the bottom. | After each click the same rows are under the mouse; the page has not jumped to the top or to another row. | | Not Run | | ui `ordinaryActionsLeaveThePageWhereItIs` |
| RG-1502 | Modern round with 5+ tables, scrolled to the last table | Report the last table's result, then undo it. | The page stays at the last table. | | Not Run | | ui `ordinaryActionsLeaveThePageWhereItIs` |
| RG-1503 | Either screen above | Repeat using only the keyboard (Tab, Space/Enter). | Focus stays on the control used; the page does not jump. | | Not Run | | ui `everyControlIsKeyboardReachable` (reachability only) |
