# TCG Tournament Manager

A Windows desktop app for running trading card game tournaments at a school club:
One Piece, Pokémon and Magic: The Gathering (Modern and Commander). It is written in
C++17 with Qt 6 Widgets and keeps everything in a local SQLite database, so it works
without an internet connection or a server.

Current version: **0.2.3**. See [Project status](#project-status) for what has and has
not been verified.

## Features

- **Games:** One Piece, Pokémon and Magic Modern as one-on-one Swiss tournaments; Magic
  Commander as multiplayer pods.
- **Pairings:** Swiss pairing by points that avoids rematches whenever a round without
  one exists. With an odd number of players the bye goes to the lowest-placed player
  who has not had one.
- **Commander pods:** pods of three or four, check-in before the start, drops and
  reinstatements between rounds, byes, result corrections and an audit history.
  Pairings are reproducible from a saved seed.
- **Fixed length:** the configured number of rounds is the whole tournament. A
  tournament can also be ended early and kept as a read-only record.
- **Round clocks:** start, pause, resume and reset. The clock is saved, so it survives
  moving between pages and restarting the app. At zero the app shows a notice (and can
  play a sound); results are never finalized automatically.
- **Standings:** points and tiebreakers per game, following the club's scoring
  settings. Final placings are saved when a tournament is finalized.
- **Players:** a directory with permanent player IDs, search and a filter by game.
  Players with the same name are told apart by ID. Profiles show tournament history.
  Removing a player hides them from the directory but keeps every past result.
- **Printing and export:** pairing sheets and table or pod signs with a print preview,
  printed or saved as PDF. Standings can be exported as PDF or CSV.
- **Diagnostics:** a local log, a diagnostic report you can read before saving, and a
  database backup export, all from Settings.
- **Display:** light and dark themes, a larger text size, and layouts that fit narrow
  windows.

## Running the app (no build needed)

The app is distributed as a ZIP: `TcgTournamentManager-<version>-win64.zip`.

1. Extract the **whole** ZIP (right-click > Extract All). Do not start the program from
   inside the ZIP window: it needs the other files in the folder.
2. Open the extracted folder and double-click `TcgTournamentManager.exe`.

Nothing has to be installed. The program is not code-signed, so Windows may show a
"Windows protected your PC" notice the first time. It has been tested on 64-bit
Windows 11 only.

Where to get the ZIP:

- **GitHub releases** currently hold only older preview builds (0.2.0-preview and
  0.2.1-preview). Version 0.2.3 has not been published there.
- **A local build:** `package.bat` makes the ZIP in `dist\` from the source (see
  [Packaging](#packaging)).

The ZIP includes a short README for the people you send it to
([packaging/README.txt](packaging/README.txt)).

## Where the data is stored

Tournaments, players, results and settings live in your Windows user folder, not
beside the program:

```
%LOCALAPPDATA%\TcgTournamentManager
    tcg_tournament.db    the database
    settings.json        appearance and round-length settings
    backups\             copies made automatically before a database update
    logs\                the diagnostic log
```

Settings in the app shows the exact path. Replacing or deleting the program folder
never touches this data.

**Backups.** Before a new version changes the database format, it saves a copy in
`backups\`. You can also save a copy at any time with Settings > Data > Export database
backup. To restore one, close the app and copy the backup over `tcg_tournament.db`.

**Disposable data for demos and testing.** Set the `TCG_DATA_DIR` environment variable
to another folder and the app uses that folder instead, leaving your own data alone:

- Packaged app: in a Command Prompt run `set TCG_DATA_DIR=%TEMP%\tcg-demo`, then start
  `TcgTournamentManager.exe` from that same window. Check the path in Settings.
- From source: `qa_sandbox.bat` starts the built app on `%TEMP%\tcg-qa-sandbox`
  (`qa_sandbox.bat fresh` starts with an empty database).

The test programs refuse to run on the default folder at all: they only use a folder
they are given.

## Club scoring rules

These are this club's settings. They are not a claim to follow any publisher's full
official tournament rules.

| Game | Points (win / draw / loss) | Tiebreakers, in order |
|---|---|---|
| Magic Modern | 3 / 1 / 0 | OMW%, GW%, OGW% |
| One Piece | 3 / 0 / 0 | OMW%, GW% |
| Pokémon | 1 / 0 / 0 | Opp Win%, Opp Opp Win% |
| Magic Commander | 5 / 1 / 0 per pod | see the Commander rules |

Details, edge cases and compatibility notes:
[docs/STANDINGS.md](docs/STANDINGS.md) (one-on-one) and
[docs/COMMANDER.md](docs/COMMANDER.md) (Commander pods, which also says which rules come
from published guidelines and which are the club's own).

## Building from source

Needs:

- Windows with Visual Studio 2022 and its "Desktop development with C++" workload, which
  supplies the compiler, CMake and Ninja.
- CMake 3.24 or newer. `build.bat` uses `cmake --fresh` to set up a build folder again
  when the Qt kit changes, and that option needs 3.24. The CMake that comes with a
  current Visual Studio 2022 is newer than that (3.27 on the PC this was written on).
- Qt 6 for MSVC 2022 64-bit, with the Core, Gui, Widgets, Sql, PrintSupport, Svg and
  Test modules. The scripts look for `C:\Qt\6.11.3\msvc2022_64`. Qt 6.10.x is affected
  by a Qt SVG security advisory (CVE-2026-6210); 6.11.3 includes the fix.
- Git, optionally. It gives each build its identifier; without it the build is
  labelled `no-git`.

From the repository folder, in a Command Prompt:

```
build.bat        build (Release, 64-bit)
run_tests.bat    build, then run the core, rules and UI test programs
qa_sandbox.bat   start the built app on a disposable data folder
package.bat      build, then make the folder and ZIP to share
```

Two optional settings, made with `set` before running a script:

- `QT_DIR`: another Qt kit, for example `set QT_DIR=C:\Qt\6.10.2\msvc2022_64`.
- `BUILD_DIR`: build in a folder other than `build`, for example `set BUILD_DIR=build-release`.

If a build folder was set up for a different Qt kit, `build.bat` configures it again
from scratch, so one build never mixes two kits.

### Tests

`run_tests.bat` prints the totals and writes `core_results.txt`, `rules_results.txt` and
`ui_results.txt` in the build folder. The UI tests open real windows and dialogs that
take keyboard focus, so leave the PC alone while they run (about five minutes): typing
or clicking elsewhere can make focus-dependent tests fail.

`bench_core.exe` in the build folder prints timings on generated data. It is run by
hand and is not part of the suites.

### Packaging

`package.bat` makes `dist\TcgTournamentManager-<version>\` and
`dist\TcgTournamentManager-<version>-win64.zip`, holding the program, the Qt files it
needs, the Microsoft C++ runtime, the tester README and the licence notices. It
refuses to package:

- source that differs from its Git commit, so a package's build identifier always names
  the code inside it (`set ALLOW_MODIFIED=1` allows a throwaway local package);
- Qt files that are not all from the kit the program was built with;
- any database, settings, log, debug-symbol or assistant file in the output.

The version is set in one place, `project(VERSION ...)` at the top of `CMakeLists.txt`.
The home screen, Settings, the log, diagnostic reports, the program's Windows
properties and the package name all take it from there. Releases count up the last
number (0.2.3, 0.2.4, 0.2.5) and carry no suffix; rebuilding the same release does not
change it. Versions before 0.2.3 were labelled "preview". The exact build is shown
separately, as the Git commit in Settings and in diagnostic reports.

## Project status

Version 0.2.3 is packaged from committed source, but it is **not fully verified**. On
the packaged build the core suite (101 passed) and rules suite (18 passed) are clean.
The UI suite passed in full (42 passed) on the same source in another build folder, but
on the packaged build two runs each failed one keyboard-focus test while the PC was in
use, and a clean run there has not yet been recorded.
The packaged app also passed a manual run of a one-on-one and a Commander event. The
details and the remaining step are in
[docs/qa/TEST_REPORT.md](docs/qa/TEST_REPORT.md).

Known limitations:

- Windows only; tested on Windows 11. No installer, code signing or automatic updates.
- English only. Data stays on one PC for one Windows user; nothing is synced.
- The core still lets a player be enrolled after a tournament has started; no screen
  offers it. How that should work is an open decision, described in
  [docs/STANDINGS.md](docs/STANDINGS.md#late-enrollment).

## Documentation

- [docs/STANDINGS.md](docs/STANDINGS.md): one-on-one scoring, tiebreakers and edge cases.
- [docs/COMMANDER.md](docs/COMMANDER.md): how Commander events, pods and standings work.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): code layout, how a change is saved,
  player IDs, ending early, schema updates and reproducible pairings.
- [docs/LOGGING.md](docs/LOGGING.md): the diagnostic log and reports.
- [docs/qa/](docs/qa/): the [demo checklist](docs/qa/DEMO_CHECKLIST.md), the
  [test plan](docs/qa/TEST_PLAN.md), [regression cases](docs/qa/REGRESSION_CASES.md),
  the [bug log](docs/qa/BUG_LOG.md) and the [test report](docs/qa/TEST_REPORT.md).

## Reporting a problem

Open an issue at
[github.com/nchiamsachang/tcg-tournament-manager/issues](https://github.com/nchiamsachang/tcg-tournament-manager/issues);
"Report a bug" in the app links there, and the issue form asks for the details needed.
An error message in the app ends with a reference such as `R-7K3QF2`; include it, and
the version and build shown in Settings.

A diagnostic report (Settings > Diagnostics) helps too. It never contains the database
and nothing is sent automatically, but error messages in it can contain names or other
typed text: the app replaces the names it recognises with IDs on a best-effort basis,
so read the report before sharing it.

## Licences

The packaged app includes Qt 6 (LGPL 3), SQLite, the Microsoft Visual C++ runtime and
Lucide icons; their notices are in [packaging/licenses/](packaging/licenses/). This
repository does not yet contain a licence for the project's own code.
