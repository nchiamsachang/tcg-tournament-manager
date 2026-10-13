# Demo checklist: packaged app

A short manual run of the packaged app before a demo: one four-player, two-round
one-on-one tournament from creation to final standings. It takes about ten minutes.
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

1. Extract the whole ZIP, for example to `C:\Temp\TcgTournamentManager-0.2.3`.
2. Open a Command Prompt and run these three lines:

   ```
   set TCG_DATA_DIR=%TEMP%\tcg-demo-check
   cd /d C:\Temp\TcgTournamentManager-0.2.3
   start "" TcgTournamentManager.exe
   ```

   The setting only applies to programs started from that Command Prompt window.
   Start the app from the same window again for the restart step (D-13).
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
| D-02 | Go to Home. | The badge above the title shows the version only (`v0.2.3`), with no build ID beside it. | | Not Run |
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

## Result

| | |
|---|---|
| Checks passed | |
| Checks failed (IDs) | |
| Checks not run or blocked (IDs) | |
| Bugs logged | |
| Ready for the demo (yes / no) | |
