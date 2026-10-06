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
CMake and Ninja) and Qt 6 for MSVC 2022 64-bit. The scripts expect Qt in
`C:\Qt\6.10.2\msvc2022_64`; set `QT_DIR` to use another copy.

```
build.bat        build into build
run_tests.bat    build, then run both test programs
deploy.bat       build, then gather the app and its Qt files into dist\TcgTournamentManager
```

`dist\TcgTournamentManager\TcgTournamentManager.exe` is the program to
run. The folder can be copied to another PC; that PC may need the Microsoft
Visual C++ 2015–2022 x64 runtime installed.

## Where the data lives

The database is `tcg_tournament.db` beside the program. When a
`tcg_tournament.db` exists in a parent folder (the project root, while the
program runs from `build` or `dist`) that one is used instead, so the
app opens the tournaments you already have. Set `TCG_DATA_DIR` to force a
folder.

## Reproducible pairings

Commander pairings are reproducible from a saved seed: the same inputs and
seed always give the same pods. `tests/fixtures/reference.json` holds 19
recorded events — 75 rounds, 3 to 97 players, all three short-pod
settings, drops and draws — with every round's inputs, seed, pods, seats,
byes and standings. `test_core` replays them and requires identical output,
so the seeded random number generator (`cmdr::SeededRandom`) must not change.
