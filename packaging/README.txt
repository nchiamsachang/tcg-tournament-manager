TCG Tournament Manager @VERSION@
================================

Runs trading card game tournaments for a club, on one Windows PC, without internet:

  - One Piece, Pokemon and Magic: The Gathering Modern: one-on-one Swiss rounds
  - Magic: The Gathering Commander: pods of 3 or 4 players

It pairs each round, keeps a round clock, records results, works out standings and
prints pairings. The scoring is this club's own setup (see "Known limitations"); it
is not certified by Bandai, The Pokemon Company or Wizards of the Coast.

Made for 64-bit Windows 10 and 11. It has been tested on Windows 11 only.


Getting started
---------------
1. Download TcgTournamentManager-@VERSION@-win64.zip from the project's release page:
   https://github.com/nchiamsachang/tcg-tournament-manager/releases
2. Right-click the ZIP and choose Extract All. Extract everything; do not start the
   program from inside the ZIP window, because it needs the other files with it.
3. Open the extracted folder and double-click TcgTournamentManager.exe.

Nothing has to be installed. Keep every file and folder from the ZIP together.

The program is not code-signed, so Windows may say "Windows protected your PC" the
first time. Before running it, check that the ZIP came from the release page above and
that its SHA-256 checksum matches the one on that page. In PowerShell:

    Get-FileHash .\TcgTournamentManager-@VERSION@-win64.zip

If they match, choose "More info" and then "Run anyway" for this program. Do not turn
off antivirus or other Windows protection.


Running a tournament
--------------------
One-on-one (One Piece, Pokemon, Magic Modern):

1. On the home page, choose Open Hub on the game, then New Tournament.
2. Enter a name, the number of rounds and the round length in minutes, then
   Create & register players. The number of rounds you set is the whole tournament.
3. Add players: type a new name and choose Add & enroll, or find an existing player
   with the search box and choose Enroll. Remove takes someone off the list.
4. Choose Start tournament. Round 1 is paired: each table shows two players.
5. Report a result by clicking the winner's name. To fix a mistake choose Undo,
   confirm, and click the right player.
6. When every table has a result, choose End round to pair the next round.
7. In the last round, choose Finalize tournament instead. Final Standings appears.
   You can open it again later from History on the game's hub.

Commander:

1. Choose Open Hub on Magic, then New Tournament, and set Format to Commander.
2. Add players as above. Everyone starts checked in: click "Checked in" next to anyone
   who has not arrived (it changes to "Check in") and click it again when they do.
   Only checked-in players are seated.
3. Set the number of qualifying rounds. Tick "Add a top-four final" if the four best
   players should play one final pod after them (for example "2 qualifying rounds +
   1 final"). This cannot be changed once the tournament starts.
4. Choose Start tournament. Round 1 shows the pods and seats.
5. Report each pod: "Wins" beside the winner, or "Draw..." and then tick anyone who
   was already eliminated when the game was drawn. Change and Clear fix a result.
6. Second place: after "Wins", the pod says "2nd place not recorded yet" and the
   points are provisional. Choose "Record 2nd place..." and pick the player who was
   eliminated last, or "No clear second" when the last players were eliminated at the
   same moment. A round cannot be finalized until every won pod has this decision.
7. Choose Finalize Round, then Start Round for the next one. Players & drops lets
   someone drop between rounds.
8. Without a final: after the last round choose Finish tournament.
   With a final: finalize the last round, check the four players and their seeds, and
   choose Create final. Record each elimination as it happens ("Record elimination...",
   ticking everyone eliminated at the same moment), then "Record result..." to set the
   winner, or no winner, and confirm the places.

Commander scoring (this club's rules for new tournaments):
- Win 5 points, clear second place 2.5, other losses 0, draw 1, bye 5.
- Second place is another player of a won pod. It earns 2.5 points but still counts as
  a loss. There is no second place in a drawn pod, for a bye, or when the last players
  were eliminated at the same moment.
- Ranking: points, then opponents' match-win %, then own match-win %, then a random
  order fixed when the tournament is created.

Top-four final (when chosen):
- The four best players in the qualifying standings who are checked in and have not
  dropped play it, seated in seed order: seed 1 takes the first turn. It adds no points.
- The first player eliminated is 4th, the next 3rd, the last opponent eliminated 2nd
  and the winner 1st. Players eliminated at the same moment are ordered by their
  qualifying seed, but only against each other: nobody moves above a player who
  survived longer.
- If the final is drawn, or ends without a winner under the club's time procedure,
  players already eliminated keep the lower places and the players still in are placed
  by qualifying seed. This is shown as "by seed": nobody is credited with winning.
- Everyone who did not play the final follows in qualifying order.

These are the club's own rules, not an official Wizards of the Coast or TopDeck
procedure. Announce them, and whether there is a final, before the tournament starts.
Tournaments created with an earlier version keep their own rules: second place earns
nothing there and there is no final.

Ending early: "End tournament early" stops a tournament and keeps everything played so
far as a read-only record. Nothing is invented for unplayed matches.


The round clock
---------------
Start, Pause, Resume and Reset are beside each round. The clock is saved, so it keeps
going if you move between pages or restart the program.

When time runs out the clock says "Time expired" and the program shows a notice. It
does not decide any result and does not end the round: you report results and choose
End round (or Finalize Round) yourself. The top-four final has its own clock, which
also only announces the time: you record how the final ended. In Settings, under Round-end alerts, you can
also turn on a sound.


Printing and exporting
----------------------
On a round, choose Print. Under "What to print" pick:

  - Pairings, by table (or by pod)
  - Pairings, by player name
  - Table-number signs (or pod-number signs)

choose Letter or A4, then "Print..." for a printer or "Save as PDF...".

Standings: one-on-one standings have Export PDF and Export CSV. Commander standings
have Export CSV.


Where your data is saved
------------------------
Tournaments, players, results and settings are kept in your Windows user folder, not
in the program folder:

    %LOCALAPPDATA%\TcgTournamentManager

Paste that line into the File Explorer address bar to open it. Settings (the gear,
top right) shows the exact file under Data.

To make a backup, choose Settings > Export database backup... and save the copy on a
USB stick or another safe place. The program also saves a copy by itself in the
backups folder before an update changes the database format.


Updating to a newer version
---------------------------
1. Close the program.
2. Download and extract the new ZIP into a new folder.
3. Start TcgTournamentManager.exe from the new folder.

Your tournaments and players stay in the user folder above and the new version picks
them up. You can delete the old program folder afterwards; it holds no data. Do not
delete the user folder unless you want to remove everything.


Reporting a problem
-------------------
An error message ends with a reference such as R-7K3QF2. Then:

1. In Settings, under Diagnostics, choose "Export diagnostic report...". The report is
   shown before you save it.
2. Read it before sharing. It never contains the database and nothing is sent
   automatically. Player and tournament names the program recognises are replaced by
   numbers, but short or unusual names and other typed text can remain.
3. Report the problem at https://github.com/nchiamsachang/tcg-tournament-manager/issues
   (or "Report a bug" at the bottom of the program), with the reference, the report,
   and the version and build shown at the bottom of Settings.

The log helps trace problems but may miss the last moments if the program or the PC
stops unexpectedly.


Known limitations
-----------------
- Club scoring, not official rules:
    Magic Modern  3 points a win, 1 a draw; tiebreakers OMW%, GW%, OGW%
    One Piece     3 points a win;           tiebreakers OMW%, GW%
    Pokemon       1 point a win;            tiebreakers Opp Win%, Opp Opp Win%
    Commander     5 points a win, 1 a draw (per pod); see Rules in a Commander event
- A one-on-one match can only be reported as a win for one player: there is no way to
  enter a draw on the round screen in this version.
- Players cannot be added once a tournament has started; register everyone first.
- One PC and one Windows user: nothing is shared between computers.
- English only. No installer or automatic updates; you replace the folder by hand.
- Not code-signed (see "Getting started"). The program has no icon of its own yet.


Licences
--------
This program uses Qt 6 under the LGPL version 3, SQLite, the Microsoft Visual C++
runtime and Lucide icons. See the licenses folder.
