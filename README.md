# TCG Tournament Manager

A desktop app for running trading card game tournaments (One Piece, Pokémon,
Magic: The Gathering — Modern and Commander), written in C++17 on Qt 6
(Widgets, Sql, PrintSupport). The Commander rules are described in
`docs/COMMANDER.md`.

## Layout

| Folder | What is in it |
|---|---|
| `src/core/` | No user interface. `db` (connection, schema, migrations, transactions), `tournaments` (tournament, registration, round and match records), `players` (directory and history), `swiss` (one-on-one scoring, pairing and event flow), `commander` (multiplayer rules and pairing, no database), `commander_db` (Commander event flow), `round_clock` (countdowns saved as timestamps), `round_status` (which round is in play, for any format), `prefs` (settings file). `store.h` includes the non-Commander headers in one line. |
| `src/ui/` | `theme` (colours and shared widgets), `main_window`, `screens_*` (every screen), `dialogs` (round clock, settings), `printing`. |
| `tests/` | `test_core.cpp`, `test_ui.cpp`, and `fixtures/reference.json`. |

## Building

Needs Visual Studio 2022 with the C++ workload (it supplies the compiler,
CMake and Ninja) and Qt 6 for MSVC 2022 64-bit. Visual Studio is found
through its installer. Qt is looked for in `C:\Qt\6.10.2\msvc2022_64`; set
`QT_DIR` to use another copy.

```
build.bat        build (Release, 64-bit) into build
run_tests.bat    build, then run both test programs
package.bat      build, then make the folder and zip to share (see below)
```

## Packaging a release

```
package.bat
```

makes `dist\TcgTournamentManager-<version>\` and, from it,
`dist\TcgTournamentManager-<version>-win64.zip`. The folder holds
`TcgTournamentManager.exe`, the Qt files it needs, the Microsoft C++ runtime,
a README for the people you send it to (`packaging/README.txt`) and the
licence notices (`packaging/licenses/`). It runs by double-clicking the .exe
on a PC with neither Qt nor Visual Studio. The program is not code-signed.

The version is set at the top of `CMakeLists.txt`: the number in `project()`
and, below it, `TCG_VERSION_LABEL` (`preview` now; empty for a final release).
It is shown on the home screen and in Settings, and names the folder and zip.

## Where the data lives

The database (`tcg_tournament.db`), `settings.json` and a `backups` folder are
in the user's own application-data folder,
`%LOCALAPPDATA%\TcgTournamentManager`, so replacing the program never touches
them. Settings shows the exact path. Set `TCG_DATA_DIR` to use another folder,
which is how to try things out without touching your own data.

Builds before 0.2.0-preview kept the database beside the program or in a
parent folder of it (the project root, while running from `build` or
`dist`). On first launch, when the user folder has no database yet, such a
database and its settings are copied there; the originals are left as they
were, and nothing in the user folder is ever overwritten.

Before a new version changes the database format, it saves a copy of the
database in `backups`.

## Reproducible pairings

Commander pairings are reproducible from a saved seed: the same inputs and
seed always give the same pods. `tests/fixtures/reference.json` holds 19
recorded events — 75 rounds, 3 to 97 players, all three short-pod
settings, drops and draws — with every round's inputs, seed, pods, seats,
byes and standings. `test_core` replays them and requires identical output,
so the seeded random number generator (`cmdr::SeededRandom`) must not change.
