# Manual test plan — club tournament checklist

A short checklist to run around a real club event. The detailed cases, with full steps
and the automated test that backs each one, are in
[REGRESSION_CASES.md](REGRESSION_CASES.md). Problems go in [BUG_LOG.md](BUG_LOG.md).
What has actually been run so far is in [TEST_REPORT.md](TEST_REPORT.md).

## How to use it

- **Status** is one of `Pass`, `Fail`, `Not Run`, `Blocked`. Leave `Not Run` until you
  have done the step yourself and seen the result. A `Fail` needs a bug-log entry.
- **Actual** is what you saw, in a few words. Write it even for a pass ("3 matches, bye: Ed").
- **Bug / evidence** is a bug id (`BUG-003`), or a photo, screenshot or export file name.
- Copy the tables into a dated file per event (`docs/qa/runs/2026-10-17-commander.md`)
  so each event keeps its own record.

### Two kinds of data — do not mix them

| | Started with | Used for |
|---|---|---|
| **Test data** | `qa_sandbox.bat` (add `fresh` for an empty database). Data folder: `%TEMP%\tcg-qa-sandbox`. | Part A, and every case in REGRESSION_CASES.md |
| **Real data** | The app's normal shortcut or `.exe`. Data folder: `%LOCALAPPDATA%\TcgTournamentManager`. | Parts B and C only |

Settings in the app shows the data folder in use: check it before Part A.

**During a live tournament (Part B) only observe.** Do not end a tournament early, remove
players, undo reported results, reset a clock or close the app "to see what happens".
Those are Part A, on test data.

### Sample players for test data

Add these in the Players screen (or while registering). They cover same names, a long name
and a name with no spaces.

`Ana`, `Bo`, `Cy`, `Di`, `Ed`, `Flo`, `Gus`, `Alex Smith`, `Alex Smith` (a second, different
person), `Alexandra Montgomery-Wellington of the Northern Isles`,
`Supercalifragilisticexpialidocious_Player_With_No_Spaces_At_All`

---

## Part A — Before the event (test data, about 20 minutes)

Run on the build you will use at the event. Record the version shown on the home screen:
`__________`  Date: `__________`  Tester: `__________`

| ID | Feature | Preconditions / data | Steps | Expected result | Actual | Status | Bug / evidence |
|---|---|---|---|---|---|---|---|
| A-01 | Test data is in use | `qa_sandbox.bat fresh` | Open Settings and read the data folder. | Path ends in `tcg-qa-sandbox`; no real players listed. | | Not Run | |
| A-02 | Create and rename | — | Create an MTG Modern tournament, 2 rounds. Rename it from its page. | New name shows in the heading and the tournament list; rounds and players unchanged. | | Not Run | |
| A-03 | Same-name players | Sample players | Add both `Alex Smith`; answer "Create a different player with this name" for the second. Enroll both. | Two players; each shown as `Alex Smith · #000N` with different ids wherever both appear. | | Not Run | |
| A-04 | One-on-one pairing and bye | 5 players enrolled | Start tournament. | 2 matches at tables 1–2 and exactly 1 bye; every player appears once. | | Not Run | |
| A-05 | Results | A-04 | Report one win and undo it; then report every match. | Undo returns the match to pending; "End round" enables only when all are reported. | | Not Run | |
| A-06 | Round limit | A-05 | End round 1, report round 2. | Round 2 of 2 offers "Finalize tournament", not another round. After finalizing there is no round 3. | | Not Run | |
| A-07 | Commander pods | New Commander event, 7 sample players checked in | Start the event. | One pod of 4 and one pod of 3; every checked-in player seated once. | | Not Run | |
| A-08 | Commander 5 players | Commander event, 5 checked in | Start the event. | One pod of 4 and one bye. | | Not Run | |
| A-09 | Clock | A-07 round screen | Start the clock, pause, start again. Close the app, reopen with `qa_sandbox.bat`. | Paused time stands still; after reopening the clock shows the time it should (it kept running if it was running). | | Not Run | |
| A-10 | Printing | A-07 | Print → pairings, then Print → pod signs. Use the preview or print one page. | Every player and pod readable; long names not cut off; sign numbers match the pods. | | Not Run | |
| A-11 | Narrow window, large text, themes | Any round screen | Shrink the window to its minimum; switch Text size to Large; switch Light/Dark. | Nothing cut off or overlapping; all buttons reachable; both themes readable. | | Not Run | |
| A-12 | Check-in does not jump | Commander registration, 11 players, window short enough to scroll | Scroll to the bottom; toggle "Checked in" on the last few players. | The list stays where it was after each click. | | Not Run | |
| A-13 | End early keeps history | Any running test tournament | "End tournament early" and confirm. | It moves to History marked Terminated; rounds and reported results are still shown, read-only. | | Not Run | |

Stop and fix (or use the previous build) if A-04 to A-08 fail: those are the event itself.

---

## Part B — During the event (real data, observe only)

Tournament: `__________`  Format: `__________`  Players: `____`  Rounds configured: `____`

| ID | Feature | When | Check | Expected result | Actual | Status | Bug / evidence |
|---|---|---|---|---|---|---|---|
| B-01 | Registration count | Before starting | Count of enrolled (Commander: checked-in) players against the sign-up sheet. | Same number; same-name players show ids. | | Not Run | |
| B-02 | Rounds configured | Before starting | Round count on the setup screen. | The number you intend to play (Commander recommends 3 for 3–16 players). | | Not Run | |
| B-03 | Everyone is assigned | Each round, as pairings appear | Count seats + byes against active players. Ask "is anyone not on the sheet?" | Every active player once. One-on-one: one bye only if the count is odd. Commander: pods of 3 or 4 only; a bye only with 5 players. | | Not Run | |
| B-04 | Pairing sheet | Each round | Printed sheet against the screen. | Same tables/pods and names. | | Not Run | |
| B-05 | Clock | Each round | Start the clock when play starts. | Counts down from the configured length; expiry is announced and changes nothing else. | | Not Run | |
| B-06 | Results | As they come in | After entering each result, read it back from the screen. | The winner shown is the one reported; pending count goes down by one. | | Not Run | |
| B-07 | Drops (Commander) | Between rounds | Drop from "Players & drops" after finalizing the round. | Player is not in the next round and still appears, marked, in the standings. | | Not Run | |
| B-08 | Standings spot check | After round 1 | Pick two players; work out their points by hand (see below). | Matches the standings screen. | | Not Run | |
| B-09 | No extra round | After the last configured round | Look at the action offered. | "Finalize tournament" / "Finish tournament"; nothing offers another round or a final. | | Not Run | |
| B-10 | Anything odd | Any time | Note the time, the screen and what you did; take a photo. Carry on with the event. | — | | Not Run | |

**Points, for B-08.** One-on-one: MTG win 3, draw 1, loss 0; One Piece win 3; Pokémon win 1;
a bye counts as a win. Commander: win 5, draw 1, loss 0, bye 5.

---

## Part C — After the event (real data)

| ID | Feature | Steps | Expected result | Actual | Status | Bug / evidence |
|---|---|---|---|---|---|---|
| C-01 | Final standings | Open the finished tournament's standings. Compare first place and two other players with your paper notes. | Same placings and points as when it was finalized. | | Not Run | |
| C-02 | Export | Export CSV (and PDF for one-on-one). Open the file. | Every player once, same order as the screen. | | Not Run | |
| C-03 | Restart | Close the app completely and reopen it. | The tournament is under History as completed with the same rounds, results and standings. | | Not Run | |
| C-04 | Player history | Open one player's profile. | Today's tournament is listed with their results. | | Not Run | |
| C-05 | Write-up | Move every "odd" note from B-10 into BUG_LOG.md: under *Reproduced* only if you can make it happen again on test data, otherwise under *Suspected*. | — | | Not Run | |

Anything destructive you want to try because of what you saw today (removing a player,
ending early, correcting an old result): do it on test data, not on the finished event.
