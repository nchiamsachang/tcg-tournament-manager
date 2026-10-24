# Demo checklist: packaged app

A short manual run of the packaged app before a demo: one four-player, two-round
one-on-one tournament from creation to final standings (about ten minutes), then the
Commander checks for 0.2.4: second place, the top-four final and the usability pass
(about fifty minutes).
The fuller plan is [TEST_PLAN.md](TEST_PLAN.md) and the detailed cases are in
[REGRESSION_CASES.md](REGRESSION_CASES.md).

Every status below starts as `Not Run`. Fill a row in only after doing the step and
seeing the result yourself.

## Run details

| | |
|---|---|
| App version (home screen) | |
| Build ID (Settings, bottom line) | |
| Package file | |
| Tester | |
| Date | |

## Before you start: use a disposable data folder

The app keeps its data in `%LOCALAPPDATA%\TcgTournamentManager` unless the
`TCG_DATA_DIR` environment variable names another folder. Run this checklist on a
throwaway folder so your own players and tournaments are never opened.

1. Extract the whole ZIP, for example to `C:\Temp\TcgTournamentManager-0.2.4`.
2. Open a Command Prompt and run these three lines:

   ```
   set TCG_DATA_DIR=%TEMP%\tcg-demo-check
   cd /d C:\Temp\TcgTournamentManager-0.2.4
   start "" TcgTournamentManager.exe
   ```

   The setting only applies to programs started from that Command Prompt window.
   Start the app from the same window again for the restart steps (D-13, C-12, C-16).

   A local test launcher prepared for a manual test does the same for one program: it sets
   the data folder only for the program it starts, keeps its data between launches, and
   refuses to start when the folder could be the real one. Either way, D-01 checks it.
3. To repeat the checklist from an empty database, close the app and delete the
   `%TEMP%\tcg-demo-check` folder. Do not delete anything under
   `%LOCALAPPDATA%\TcgTournamentManager`.

## How to fill it in

- **Status** is `Pass`, `Fail`, `Not Run` or `Blocked`.
- **Actual** is what you saw, in a few words, also for a pass.
- A `Fail` gets an entry in [BUG_LOG.md](BUG_LOG.md). Stop and note the step if the app
  closes by itself.

## Checks

| ID | Step | Expected result | Actual | Status |
|---|---|---|---|---|
| D-01 | Open Settings (gear, top right) and read the DATA path and the bottom line. | The path ends in `tcg-demo-check\tcg_tournament.db`, not `AppData\Local\TcgTournamentManager`. The bottom line shows the version and a build ID. Write both into Run details. If the path is wrong, stop: close the app and redo "Before you start". | | Not Run |
| D-02 | Go to Home. | The badge above the title shows the version only (`v0.2.4`), with no build ID beside it. | | Not Run |
| D-03 | Open the Pokemon hub, choose New Tournament. Name it `Demo Cup`, set Rounds to 2 and Round length to 1, then Create & register players. | The registration page opens, titled with `Demo Cup`, showing 0 players enrolled. Start tournament is disabled. | | Not Run |
| D-04 | Add four players with New player name and Add & enroll: `Ada`, `Ben`, `Cal`, `Dee`. | The count reads 4 players enrolled and all four names are listed. Start tournament is enabled. | | Not Run |
| D-05 | Choose Start tournament and answer Yes. | The question names the tournament and says 4 players. Round 1 of 2 opens with 2 tables, 2 pending. Each of the four players appears exactly once and nobody has a bye. | | Not Run |
| D-06 | In ROUND CLOCK choose Start, wait a few seconds, choose Pause, wait five seconds, choose Resume. | The clock counts down from 01:00. While paused the time does not change and the tournament's pill in the top bar says Paused at that time. After Resume it continues from the same time. | | Not Run |
| D-07 | Click the first-listed player on table 1 and on table 2. | Each table shows that player wins, and the counter reads 2 / 2 reported. | | Not Run |
| D-08 | On table 1 choose Undo and answer Yes, then click the other player. | The question reads "Clear this match result?". After Yes the table shows Awaiting result and 1 / 2 reported. After the click it shows the other player wins and 2 / 2 reported. | | Not Run |
| D-09 | Choose View standings. Work out the expected table on paper first: 1 point per win, table 1's corrected result counted. | Two players have 1 point and 1 win, two have 0 points and 1 loss, and the table 1 winner is the corrected one. Go back to the round. | | Not Run |
| D-10 | Wait until the clock has run out (one minute of running time in total). | The clock reads Time expired and a notice says results are not finalized automatically. Both results are unchanged and still 2 / 2 reported. | | Not Run |
| D-11 | Choose End round and answer Yes. Report both tables of round 2. | Round 2 of 2 opens with a fresh 01:00 clock and new pairings in which nobody meets their round 1 opponent. The page offers Finalize tournament and no End round. | | Not Run |
| D-12 | Choose Finalize tournament and answer Yes. Then choose Export PDF and save it to the Desktop. | Final Standings shows four rows placed 1st to 4th, "2 rounds", and each player's wins plus losses equals 2. The PDF opens and has three pages: round 1, round 2 and the final standings, matching the screen. | | Not Run |
| D-13 | Close the app. Start it again from the same Command Prompt window. Open the Pokemon hub, open `Demo Cup` under History, then View final standings. | The hub shows no active tournaments and `Demo Cup` as Completed with 4 players and 2 rounds. The final standings are the same as in D-12. The round pages are read-only. | | Not Run |
| D-14 | Drag the window as narrow as it goes, then open Home, the hub and the final standings. | Nothing is cut off or overlapping. The top bar wraps or shortens, and the standings table scrolls sideways inside its panel instead of spilling out. | | Not Run |
| D-15 | Widen the window. In Settings choose Text size: Large, then Done. Open the same three pages. | All text is larger, nothing is cut off, and buttons still show their whole label. | | Not Run |
| D-16 | Click the sun or moon button in the top bar, look at the same three pages, then click it again. | The whole app switches between dark and light, text stays readable in both, and every button icon (home, players, gear, back arrow, clock) is visible in both. | | Not Run |
| D-17 | Close the app with the window's close button. | It closes at once with no error message. | | Not Run |

## Commander (0.2.4): second place, the top-four final and the usability pass

Three separate Commander events, each from Magic's hub, New Tournament, Format Commander,
Round length 50. Write the points down where a step says so; later steps compare with them.
Player names: event A `Ana`, `Ben`, `Cal`, `Dee`, `Eli`, `Fay`, `Gus`, `Hal`; events B and C
any four new names.

The club's rules being checked: win 5, clear second place 2.5 (still a loss), other losses
0, draw 1, bye 5. In the final the first player out is 4th, the next 3rd, the last opponent
out 2nd and the winner 1st; players out at the same moment are ordered by qualifying seed
among themselves only; with no winner, the players still in are placed by seed.

Routine results are entered on the pod cards: no window opens for a winner, a second place
or a draw. A window opens only for a correction, and every correction asks for a reason.

### Event A: `Club Final`, 8 players, 2 qualifying rounds, final on

| ID | Step | Expected result | Actual | Status |
|---|---|---|---|---|
| C-01 | Create `Club Final` with Rounds 2. Add the eight players. In COMMANDER SETUP tick Add a top-four final; check Rounds still says 2. | The FINAL line reads "2 qualifying rounds + 1 final". The summary says "This tournament will be 2 qualifying rounds, then the top-four final." The scoring line reads "win 5, clear second place 2.5, other losses 0, draw 1, bye 5". DRAWS shows "Knocked-out players lose" with "Players still in draw; knocked-out players lose." | | Not Run |
| C-02 | Choose Start tournament and read the question; answer it. | The question says "Rounds: 2 qualifying rounds, then the top-four final". Round 1 shows two pods of four, each saying "No result yet". The ROUNDS strip shows 1, a dashed 2 and a dashed Final. | | Not Run |
| C-03 | Scroll the page a little so pod 1 is not at the very top. In pod 1 click Winner +5 beside the seat 1 player only. | No window opens and the page does not jump. Pod 1 says "… won · 2nd place still to choose, points provisional" and "2nd place not recorded yet. Choose 2nd above, or No clear second …". The other three players show 2nd +2.5 and the winner has none. The keyboard focus is on the first 2nd +2.5 button. Under the pods: "Points are provisional. Second place is not recorded yet in pod 1." Finalize Round 1 is disabled; resting the pointer on it gives the same reason. The winner shows 5 pts, everyone else in the pod 0 pts. | | Not Run |
| C-04 | In pod 1 click 2nd +2.5 beside the seat 2 player. Open History. | No window and no reason asked. That player shows "2nd +2.5" under the name and 2.5 pts, and still LOSS. Pod 1 says "saved, provisional until the round is finalized". History has SECOND_PLACE_RECORDED, without a reason. | | Not Run |
| C-05 | In pod 2 click Winner +5 for the seat 1 player, then No clear second. | Pod 2 says "No clear second place"; nobody in pod 2 has 2.5 pts. Finalize Round 1 is now enabled. | | Not Run |
| C-06 | In pod 1 choose Correct…. Try Save correction with no reason, then with only spaces. Then move second place to the seat 3 player, type a reason and save. Open History. | Save correction stays disabled until a reason with letters in it is typed; the reason box is beside the buttons. After saving: seat 2 is back to 0 pts, seat 3 has 2.5 pts and the badge; the winner still has 5; pod 1's total is 7.5, not more. History has RESULT_CORRECTED with second_place_before, second_place and the reason. | | Not Run |
| C-07 | Choose Finalize Round 1, answer Yes, then Start Round 2. In each round 2 pod click Winner +5 for one player, then try Finalize Round 2 before recording any second place. | Finalize Round 2 stays disabled with the second-place reason. The text above it says Round 2 is the last qualifying round and the top-four final follows. | | Not Run |
| C-08 | In each round 2 pod click 2nd +2.5 for one player. Open Standings and write down every player's points and order. Back on the round, choose Finalize Round 2 and answer Yes. | Standings show the points by hand: 5 per win, 2.5 per clear second, 0 otherwise, shown as e.g. 7.5 (never 7.50 or 8). After finalizing, the tournament is not finished: a TOP-FOUR FINAL panel lists Seed 1 to 4, which are the first four in Standings (nobody has dropped), with the same points. | | Not Run |
| C-09 | Choose Create final… and read the question; answer Yes. | The question lists Seed 1-4 with seat and points and says seeds are fixed. The page becomes "Top-four final — Club Final" with one sentence saying what to do and two sections side by side. RECORD KNOCKOUT: a pill per player ("name · seed n"), Confirm knockout (disabled), Undo last knockout… (disabled) and "No knockouts yet.". DETERMINE WINNER: a pill per player, a separate No winner / Draw pill with one line explaining it, "Choose the winner, or No winner / Draw, to see the places." and Confirm final results (disabled). | | Not Run |
| C-10 | Reseeding check. In the final's ROUND CLOCK choose Start, wait ten seconds, Pause. Select seed 4 and choose Confirm knockout. Then Undo last knockout…, give a reason, and Reset the clock. Now open Standings, note the player in 5th place, go to Round 1 (ROUNDS strip) and use Correct result… on that player's pod to make them the winner, with No clear second and a reason. If asked about later rounds, choose Keep later rounds. | The correction window stays open and says "Not saved: …" because the final has already begun; choose Cancel. The final still has the same four players and seeds. The app never offers to discard the final. History has FINAL_ELIMINATIONS_CORRECTED with the reason given for the undo. | | Not Run |
| C-11 | Back on the final, select all four pills under RECORD KNOCKOUT. Then deselect all but seed 4 and choose Confirm knockout; press it again at once. | With all four selected, Confirm knockout is disabled and the line beside it says to leave at least one player in and to choose No winner / Draw if everyone still in went out at the same moment. After confirming: seed 4 is no longer offered on either side; "Knockout 1: …" appears once, not twice; no pill is left selected. | | Not Run |
| C-12 | Close the program with the window's close button and start it again (the launcher, or the same Command Prompt). Open `Club Final`. | The final is shown with "Knockout 1: …" (seed 4) still listed, seed 4 not offered, and the clock as it was left. Nothing else changed. | | Not Run |
| C-18 | Page steadiness and a knockout of two together. Make the window about 700 × 500 so the final page scrolls, and scroll so Confirm knockout is in the middle of the window. Select seeds 2 and 3 and choose Confirm knockout. | The page does not jump and the window keeps its position and size. The keyboard focus is on a control you can see (not at the bottom of the page). "Knockout 2: …, … (together)" is listed and only seed 1 is left. | | Not Run |
| C-19 | One player left. Try to select seed 1 under RECORD KNOCKOUT and read the line below it; look at DETERMINE WINNER. Then choose Undo last knockout… and give a reason. | Seed 1's knockout pill and Confirm knockout are disabled and the line says "One player remains. Confirm the winner to finish." Under DETERMINE WINNER only seed 1 is offered, not chosen for you, and No winner / Draw is disabled. Nothing was saved and the event is not finished. The undo asks for a reason; afterwards seeds 2 and 3 are offered again, the page stays put, and History has FINAL_ELIMINATIONS_CORRECTED with the reason. | | Not Run |
| C-13 | Choose How placements work, read it, choose it again. Then choose seed 2 under DETERMINE WINNER, then seed 1. Read PLACES. Then choose Confirm final results and answer the question. | The rules open and close. Only seed 1 stays chosen (a tick and a purple outline). Before confirming: 1st seed 1 "won the final", 2nd seed 2, 3rd seed 3 (both knocked out: they lost together, so seed order decides between them only), 4th seed 4; the page says the ending is saved only when confirmed. After confirming, the final standings open; the final page says "Final confirmed: … won". | | Not Run |
| C-14 | Open the final standings. Compare with the points written down in C-08. | Places 1-4 are the four finalists in the final's order with Status "Final: 1st, won", "Final: 2nd", "Final: 3rd", "Final: 4th". Places 5-8 are the other players in their C-08 order. Every player's points, W, L and 2nd are exactly as in C-08: the final added nothing. | | Not Run |
| C-15 | Choose Save PDF and Export CSV; open both files. | Both list the same eight places as the screen. The CSV has Seconds and Final columns, half points written as 2.5 / 7.5, and the Final column filled for places 1-4 only. | | Not Run |
| C-16 | Close and start the program again; open `Club Final` from History. | Still complete with the same final standings; the round and final pages are read-only (no Winner, 2nd, Confirm knockout or Create buttons; Correct final… is still offered). | | Not Run |
| C-17 | With the final page and the final standings open: Settings, Text size Large; then the sun/moon button; then drag the window as narrow as it goes. Repeat on the round 1 page. | In large text, both themes and a narrow window nothing is cut off or overlapping: the "2nd +2.5" badge, the long final lines and the Status column stay readable (the table may scroll sideways inside its panel). In a narrow window the Winner and 2nd buttons sit under the player's name. Set text size back to Standard afterwards. | | Not Run |

### Event B: `Draw Final`, 4 players, 1 qualifying round, final on

| ID | Step | Expected result | Actual | Status |
|---|---|---|---|---|
| C-20 | Create `Draw Final` (Rounds 1, Add a top-four final ticked), add four players and start. Choose Draw… in the pod, tick three players as knocked out before the draw and choose Save draw. | The card says "Not saved: A draw needs at least two players still in the game …", the three ticks are still there, and the pod still says No result yet. | | Not Run |
| C-21 | Untick one player and choose Save draw. Then Correct…, make one player the winner with a clear second, give a reason and save. Finalize Round 1; Create final… and Yes. | The draw saves: two players DRAW +1, two OUT. The correction saves, and all four players are in the final, seeded by the round 1 standings. | | Not Run |
| C-24 | Before C-22, on the live final page: Settings, Text size Large; the sun/moon button; drag the window as narrow as it goes and back to normal. Then, with the keyboard only, Tab to a knockout pill and press Space, Tab to a winner pill and press Space. | In a normal window the two sections sit side by side; in a narrow one they are stacked, RECORD KNOCKOUT first. Nothing is cut off or overlapping in either theme or text size: pills, names, Confirm knockout, Confirm final results. Tab goes through the knockout pills, Confirm knockout and Undo, then the winner pills, No winner / Draw and Confirm final results; the focused control has a visible outline and a selected pill shows a tick. Deselect both, and set text size back to Standard. | | Not Run |
| C-22 | On the final page choose No winner / Draw without recording any knockout; read PLACES; Confirm final results and answer Yes. | Places 1-4 follow the seeds, each "still in at the end, placed by seed n". The final page says no winner, placed by seed. Standings show "Final: 1st, by seed" and so on; nobody gained a win or points. | | Not Run |
| C-23 | Correct final…: Save correction is disabled. Select seed 1, Add knockout, keep No winner / Draw, type a reason beside Save, Save correction. Then open History. | The window shows the saved places at the top and the places after the correction below. Seed 1 is 4th; seeds 2, 3 and 4 are 1st-3rd by seed. History has FINAL_CORRECTED with the earlier places and the reason. | | Not Run |

### Event C: `No Final`, 4 players, 1 round, final off

| ID | Step | Expected result | Actual | Status |
|---|---|---|---|---|
| C-30 | Create `No Final` (Rounds 1, Add a top-four final not ticked), add four players and start. | The FINAL line reads "Off: places come from the standings after Round 1"; the ROUNDS strip has no Final chip. | | Not Run |
| C-31 | Report the pod with Winner +5 and 2nd +2.5, then Clear… on the pod: the window asks for a reason. Give one. Report it again and choose Finish tournament and Yes. | Clearing needs a reason and History has RESULT_CLEARED with it. Final standings follow points (winner 5, second 2.5, the others 0); no final is offered at any point. | | Not Run |

### Setup and scrolling

| ID | Step | Expected result | Actual | Status |
|---|---|---|---|---|
| C-40 | New Tournament, Format Commander, Expected players 20. Note the rounds; type 2 in Rounds; change Expected players to 40. | The suggestion reads "Suggested for 40 expected players: n rounds." and Rounds stays 2. | | Not Run |
| C-41 | Create a Commander event with nobody checked in yet (leave Rounds as suggested). On its setup page read beside ROUNDS; then check in three players. | First "A suggestion appears once three players check in." and Use suggested rounds is disabled. With three: "Suggested for 3 checked in: n rounds." and "Follows check-ins until you change it." | | Not Run |
| C-42 | Change DRAWS, then check in one more player. Then set ROUNDS by hand to the number suggested (change it away and back), and check in one more player. | Changing DRAWS does not fix the rounds: they still follow. After setting the rounds by hand they stay as set and the line says "Set by you: check-ins leave it alone." | | Not Run |
| C-43 | Choose Use suggested rounds. | The rounds become the suggestion for the players checked in now, and the line says they follow check-ins again. DRAWS and the other settings are unchanged. | | Not Run |
| C-44 | Make the window short so the setup page scrolls. Rest the pointer on DRAWS, 3-PLAYER PODS and ROUNDS in turn and roll the mouse wheel; repeat after clicking into each field, and with a laptop trackpad if there is one. | The page scrolls each time and none of the values changes. Click DRAWS: the open list still scrolls with the wheel. The arrow keys and the ROUNDS arrow buttons still change the values. The player list and the tag strips scroll as before. | | Not Run |
| C-45 | With the keyboard only (Tab, Space, Enter, arrows): report a winner and a second place in a pod, then select a knockout pill and confirm the knockout, and choose a winner in a final. | Every button and choice can be reached, the focused one has a visible outline, and after each save the focus is on the next sensible control, not at the top of the page. | | Not Run |

## Result

| | |
|---|---|
| Checks passed | |
| Checks failed (IDs) | |
| Checks not run or blocked (IDs) | |
| Bugs logged | |
| Ready for the demo (yes / no) | |
