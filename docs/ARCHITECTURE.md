# Architecture

How the code is organised and how it keeps the data consistent. For running and building
the app see the [README](../README.md).

## Layout

| Folder | What is in it |
|---|---|
| `src/core/` | No user interface. `db` (connection, schema, migrations, transactions), `tournaments` (tournament, registration, round and match records), `players` (directory and history), `swiss` (one-on-one scoring, pairing and event flow), `commander` (multiplayer rules and pairing, no database), `commander_db` (Commander event flow), `round_clock` (countdowns saved as timestamps), `round_status` (which round is in play, for any format), `prefs` (settings file), `applog` (the diagnostic log). `store.h` includes the non-Commander headers in one line. |
| `src/ui/` | `theme` (colours and shared widgets), `main_window`, `screens_*` (every screen), `dialogs` (round clock, settings), `printing`. |
| `assets/` | Game logos and `icons/`: the [Lucide](https://lucide.dev) icons the app uses, as SVG files built into the program (`assets.qrc`). `T::button`, `T::setButtonIcon` and `T::iconLabel` in `theme` draw them in the theme's colours. To add one, copy its `.svg` into `assets/icons` and list it in `assets.qrc`. |
| `tests/` | `test_core.cpp`, `test_rules.cpp` (hand-worked rule examples and pairing checks), `test_ui.cpp`, `fixtures/reference.json`, and `bench_core.cpp` (timings on generated data, run by hand). |
| `docs/qa/` | Manual test plan for event days, detailed regression cases, the bug log and the test report. |

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
completed status. The top-four final follows the same rule: `cdb::createFinal`
(the final, its players and seeds, the audit entry), `saveFinalEliminations` (the
groups and the audit entry) and `confirmFinal` (the places, the outcome, the audit
entry, the placings and the completed status) each own one transaction. A correction
that discards a final that has not begun does so inside `correctResult`'s
transaction. Screens ask their questions before calling these, never while a change
is open.

Things worth knowing:

- If a step fails and its error is caught before it reaches the owner, the
  owner still cannot save the rest: its `commit()` rolls everything back and
  throws. There are no savepoints, so a step is never undone on its own.
- The rules are enforced in the core, whatever a screen allows: names are
  trimmed, 1 to 100 characters and free of line breaks; a tournament needs a
  known game, 1 to 15 rounds and two players to start; a player can be taken
  off the list only before the start and nobody joins a finished tournament;
  a tournament is finalized only in its last configured round.
- Dialogs are never open during a transaction. A question (same name? end the
  round?) is asked first; the write starts after it is answered.
- `tdb::enrollPlayer` returns 0 only when that player is already registered
  for that tournament, which it checks for, and which the table's
  `UNIQUE (tournament_id, player_id)` rule enforces if two requests race. Every
  other failure is thrown. `db::Error` carries SQLite's result code, so a
  duplicate (`unique()`), a missing row (`foreignKey()`) and a locked database
  (`busy()`) are told apart without reading message text.
- `tdb::writeMatchResult` writes only the match row and is meant to be called
  inside an owner's transaction; results are saved through `swiss::reportResult`.
- The figures in `enrollments` are a stored copy of what the matches work out
  to. They are rewritten by the operations above, never by looking at
  standings (`swiss::viewStandings` does not write).
- In the log, an action that runs as a step inside an owner's transaction ends
  as `pending`, not `ok`; only the owner writes `ok`, after the commit.

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

## Schema updates and older databases

Schema changes are numbered updates (migrations) in `src/core/db.cpp`, applied once each
and recorded in the `schema_migrations` table. Before an update runs on a database that
already holds data, the database is copied into `backups\` in the data folder; if that
copy cannot be made, nothing is changed. A shipped update is never edited; a new change
gets a new number.

Builds before 0.2.0-preview kept the database beside the program or in a
parent folder of it (the project root, while running from `build` or
`dist`). On first launch, when the user folder has no database yet, such a
database and its settings are copied there; the originals are left as they
were, and nothing in the user folder is ever overwritten.

Test and maintenance programs call `db::requireChosenPath()` first: with no data folder
set and no `TCG_DATA_DIR`, every database call is refused instead of falling back to the
organizer's own folder. The app itself never calls it.

## Reproducible pairings

Commander pairings are reproducible from a saved seed: the same inputs and
seed always give the same pods. `tests/fixtures/reference.json` holds 19
recorded events — 75 rounds, 3 to 97 players, all three short-pod
settings, drops and draws — with every round's inputs, seed, pods, seats,
byes and standings. `test_core` replays them and requires identical output,
so the seeded random number generator (`cmdr::SeededRandom`) must not change.
