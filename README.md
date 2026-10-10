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
| `assets/` | Game logos and `icons/`: the [Lucide](https://lucide.dev) icons the app uses, as SVG files built into the program (`assets.qrc`). `T::button`, `T::setButtonIcon` and `T::iconLabel` in `theme` draw them in the theme's colours. To add one, copy its `.svg` into `assets/icons` and list it in `assets.qrc`. |
| `tests/` | `test_core.cpp`, `test_rules.cpp` (hand-worked rule examples and pairing checks), `test_ui.cpp`, and `fixtures/reference.json`. |
| `docs/qa/` | Manual test plan for event days, detailed regression cases, the bug log and the test report. |

## Building

Needs Visual Studio 2022 with the C++ workload (it supplies the compiler,
CMake and Ninja) and Qt 6 for MSVC 2022 64-bit. Visual Studio is found
through its installer. Qt is looked for in `C:\Qt\6.10.2\msvc2022_64`; set
`QT_DIR` to use another copy.

```
build.bat        build (Release, 64-bit) into build
run_tests.bat    build, then run the three test programs
qa_sandbox.bat   start the built app on a disposable data folder, for manual testing
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

## How a change is saved

Screens call core functions directly, on the same thread, and every change to
the database happens inside a transaction (`db::Tx`: `BEGIN IMMEDIATE` …
`COMMIT`, rolled back if `commit()` is never reached). A `Tx` opened while
another is open on the thread joins it, so only the outermost one begins and
commits. Each operation has one owner, the function a screen calls; the steps
it calls join its transaction. An error anywhere is thrown as an exception,
unwinds to the owner and rolls everything back. The screen then shows the
error and does not refresh, so it keeps showing what is really saved.

| Operation (one-on-one) | Owner of the transaction | Saved together |
|---|---|---|
| Register a player | `tdb::enrollPlayer` | the registration |
| Add & enroll | `tdb::addAndEnrollPlayer` | the new player and the registration |
| Report, change or clear a result | `swiss::reportResult` | the match row and every player's stored record and tiebreakers |
| Start a tournament | `swiss::startTournament` | the status, round 1 and its matches |
| End a round | `swiss::advanceToNextRound` | the round's end, the stored records, the next round and its matches |
| Finalize a tournament | `swiss::finalizeTournament` | the stored records, final placings and the completed status |

Commander events work the same way and already did: `cdb::reportPodResult`,
`clearPodResult`, `correctResult`, `finalizeRound` and `finishTournament` each
own one transaction that covers the seats, the pod, the audit entry, the
standings kept with the registrations and, on the last round, the placings and
completed status.

Things worth knowing:

- If a step fails and its error is caught before it reaches the owner, the
  owner still cannot save the rest: its `commit()` rolls everything back and
  throws. There are no savepoints, so a step is never undone on its own.
- Dialogs are never open during a transaction. A question (same name? end the
  round?) is asked first; the write starts after it is answered.
- `tdb::enrollPlayer` returns 0 only when that player is already registered
  for that tournament, which it checks for, and which the table's
  `UNIQUE (tournament_id, player_id)` rule enforces if two requests race. Every
  other failure is thrown. `db::Error` carries SQLite's result code, so a
  duplicate (`unique()`), a missing row (`foreignKey()`) and a locked database
  (`busy()`) are told apart without reading message text.
- `tdb::writeMatchResult` writes only the match row and is meant to be called
  inside an owner's transaction. `tdb::reportMatchResult` is that step on its
  own; the app does not use it to save a result.
- The figures in `enrollments` are a stored copy of what the matches work out
  to. They are rewritten by the operations above, never by looking at
  standings (`swiss::viewStandings` does not write).
- In the log, an action that runs as a step inside an owner's transaction ends
  as `pending`, not `ok`; only the owner writes `ok`, after the commit.

## Standings and tiebreakers

Players are ranked by match points, then by the game's tiebreakers in order.
These are this club's settings, not a claim to follow any publisher's full
tournament rules.

| Game | Points (win / draw / loss) | Tiebreakers, in order | Floor |
|---|---|---|---|
| Magic | 3 / 1 / 0 | OMW%, GW%, OGW% | 33% |
| One Piece | 3 / 0 / 0 | OMW%, GW% | 33% |
| Pokémon | 1 / 0 / 0 | Opp Win%, Opp Opp Win% | 25% |

Pokémon's two figures:

- **Opp Win%** is the average of a player's opponents' match-win rates.
- **Opp Opp Win%** is the average of those same opponents' Opp Win% values. It
  separates players who are level on points and on Opp Win%.

For both: a bye is not an opponent and does not count in anyone's win rate; an
opponent met twice counts twice; every opponent's win rate counts as at least
the floor; a player with no real opponent (only byes, or nothing played yet)
gets the floor for both figures. A player who stops playing is treated like
any other, their record stands as it is. Pokémon's two figures are stored and
compared at nine-decimal precision (rounded to nine decimal places, which only
removes floating-point noise) and shown to three; Magic and One Piece figures
are stored and compared at four, as before. Players level on every
figure stay in name order.

Pokémon tournaments finished before 0.2.1-preview.3 keep the placings they
were saved with; the second figure did not exist then, so it shows as "—" for
them and nothing is recalculated. A Pokémon tournament still being played
shows the figure straight away, and it is saved with the next result, round
end or finish. The figure is stored in the `ogw_pct` column, which holds
Magic's different OGW% for Magic events.

## Logs and diagnostic reports

The app writes a diagnostic log in `logs\app.log` inside the data folder
(`%LOCALAPPDATA%\TcgTournamentManager\logs`). A file is closed at 2 MB and
five are kept. Each line has the local time with its UTC offset, a severity,
the app version, an action and the ids involved:

```
2026-10-09T14:03:22.123-05:00 INFO  0.2.1-preview.3 result.report status=attempt match=41 match_result=PLAYER1 op=4F2A91C7
2026-10-09T14:03:22.140-05:00 INFO  0.2.1-preview.3 result.report status=ok match=41 match_result=PLAYER1 tournament=7 op=4F2A91C7
```

`status` is how the operation went: `attempt` when it starts, then `ok`,
`failed`, `pending` (a step inside a larger operation that has not been saved
yet) or a named outcome such as `no-change`. What was being saved has its own
field, `match_result` (`PLAYER1`, `PLAYER2`, `DRAW`, `CLEARED`) or `pod_result`
for Commander, so no line has two fields with the same name. Up to
0.2.1-preview.2 both were called `result`; older lines in an existing log keep
that form. Nothing in the app reads the log back by field name.

**What is recorded.** The start of a session (with the build identifier),
database updates and backups, every error shown in a dialog, pages that fail
to open, errors caught by the safety net in `main.cpp`, and the key actions
of both one-on-one and Commander events. Timer ticks and repaints are never
logged. The Commander audit history in the database is unchanged; the log is
in addition to it.

**Attempts and outcomes.** An action is written as `attempt` when it starts
and as `ok` or `failed` when it ends; `ok` is written only after the change
has been committed. Both entries carry the same operation id (`op=`), unique
to that attempt, so they can be matched when other entries lie between them
or when the same tournament is acted on from two places at once. A failed
action also gets an error reference (`ref=R-7K3QF2`); the dialog that reports
the error shows that reference, and its own log entry carries both ids.

**The exact build.** The start entry, Settings and every report name the
build as well as the version: the Git commit, with `-modified-<date>-<time>`
when files had been changed since that commit, or `no-git` when the app was
built without Git. The version itself (`project(VERSION)` and
`TCG_VERSION_LABEL` in `CMakeLists.txt`) names the package.

**Privacy.** Actions name players, tournaments and rounds by id, never by
name. Error messages are different: they are worded for the organizer and may
contain personal information. Before such a message is written, and again
when a report is made, the player and tournament names the app recognises
are replaced by ids and user-folder paths are shortened. That is best effort.
Names of one or two characters, names the app does not recognise, and
anything else that was typed in can remain. Read a report before sharing it.

**Reports.** Settings > Diagnostics has "Open log folder" and "Export
diagnostic report". The report is shown before it is saved, exactly as it
will be saved: version, build and system details, whether logging was
working, and a recent log excerpt. It never contains the database, and
nothing is uploaded.

**When the log cannot be written.** The app carries on normally. There is no
dialog and no error per entry; Settings > Diagnostics shows "Logging
unavailable" with the reason, a report says that recent entries may be
missing (and includes the newest unwritten ones, which are kept in memory),
and the file is tried again every 30 seconds. When it works again, a
`log.resumed` entry says how many entries were missed.

**What a log cannot tell you.** Each entry is handed to the operating system
as soon as it is written, which makes it likely, not certain, that it
survives if the app stops unexpectedly; a crash or a power failure can still
lose the most recent entries. Nothing is written by a crash itself and there
are no stack traces. If a session did not close normally, the next start
notes that, without knowing why: the app may have been ended from outside,
the computer may have shut down, or the app may have stopped unexpectedly.

The code is `applog` in `src/core`. A function that changes data declares an
`applog::Action` as its first statement.

## Players: ids, same names and removal

A player is the row's `player_id`, a number the database assigns once and
never reuses, not the name. Registrations, matches, pod seats and byes all
refer to that id, so two people can have the same name and a rename changes
nothing else. Ids are shown as `#0042` (at least four digits).

The id is always on a player's profile. Elsewhere it appears only when it is
needed: a name is shown as `Alex Smith · #0042` when another player being
shown has the same name. Names are stored in one field, so "the same name"
means the whole name matches exactly once case and extra spaces are ignored
(`pdb::normalizedName`); a shared first or last name alone is not a match.
The comparison uses the full set (the whole directory, or everyone who took
part in a tournament, removed or not) before any search or filter.

Adding a name that a player in the directory already has asks which player
is meant, or whether this is a different person; nothing is merged.

"Remove player" sets `deleted_at` (migration 5) and nothing else. The player
leaves the directory, searches and new registrations, and their profile can
no longer be opened; every tournament record keeps their id and name. A
player who is registered for a tournament that has not started, or is still
in one being played, has to be resolved there first.

Standings shown or printed never write to the database
(`swiss::viewStandings`): a finished tournament shows the figures and
placings saved when it was finalized.

## Ending a tournament early

"End tournament early" on a round screen saves the tournament as `TERMINATED`
(`tdb::terminateTournament`): its status and `terminated_at` are set and its
round clocks are stopped in one transaction. Nothing is deleted or invented:
players, pairings and reported results stay, unreported matches stay
unreported, and no placings or winner are saved. It leaves the active list and
is shown under History, marked "Terminated", as a read-only record; the
functions that enrol, pair and save results refuse to change it.

Migration 4 widens the status rule on the `tournaments` table to allow this.
SQLite cannot alter such a rule in place, so the table is rebuilt with every
row copied across unchanged, after the usual backup.

## Reproducible pairings

Commander pairings are reproducible from a saved seed: the same inputs and
seed always give the same pods. `tests/fixtures/reference.json` holds 19
recorded events — 75 rounds, 3 to 97 players, all three short-pod
settings, drops and draws — with every round's inputs, seed, pods, seats,
byes and standings. `test_core` replays them and requires identical output,
so the seeded random number generator (`cmdr::SeededRandom`) must not change.
