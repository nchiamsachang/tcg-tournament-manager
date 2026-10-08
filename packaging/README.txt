TCG Tournament Manager @VERSION@
================================

This is a PREVIEW release for testing. It is unfinished: expect rough edges, and
please report problems at
https://github.com/nchiamsachang/tcg-tournament-manager/issues


How to run it
-------------
1. Extract the WHOLE zip (right-click the zip > Extract All). Do not run the program
   from inside the zip window: it needs the other files in this folder.
2. Open the extracted folder and double-click TcgTournamentManager.exe.

Keep every file and folder that came in the zip together with the .exe. Nothing has to
be installed. Needs 64-bit Windows 10 or 11.

The program is not code-signed, so Windows may show a "Windows protected your PC"
notice the first time you start it.


Where your data is stored
-------------------------
Tournaments, players, results and settings are kept in your own Windows user folder,
not in this folder:

    %LOCALAPPDATA%\TcgTournamentManager

    tcg_tournament.db    tournaments, players and results
    settings.json        appearance and round-length settings
    backups\             copies made automatically before a database update

Paste the line above into the File Explorer address bar to open it. The exact location
is also shown in the program under Settings > Data, where "Export database backup"
saves a copy wherever you choose.

The first launch creates an empty database. If you used an earlier build that kept
tcg_tournament.db beside the program, put this folder where the old one was (or inside
it): on first launch that database and its settings are copied into the folder above.
The old files are left as they were, and an existing database is never overwritten.


Updating to a newer preview
---------------------------
1. Close the program.
2. Delete this folder, or leave it; it holds no data.
3. Extract the newer zip and run the .exe from the new folder.

Your tournaments, players, results and settings stay in the user folder above and are
picked up by the new version. If a new version has to change the database format, it
first saves a copy in the backups folder.

To remove everything, delete this folder and the user folder above.


Known limitations of this preview
---------------------------------
- Not code-signed (see above).
- No installer, Start-menu shortcut or automatic updates: you replace the folder by hand.
- The program has no icon of its own yet; Windows shows its default program icon.
- English only.
- Data is kept on this PC for this Windows user only. Nothing is synced or shared
  between computers.
- Tested on Windows 11 only.


Licences
--------
This program uses Qt 6 under the LGPL version 3, SQLite and the Microsoft Visual C++
runtime. See the licenses folder.
