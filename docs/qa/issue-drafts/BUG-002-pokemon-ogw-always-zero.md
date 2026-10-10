<!-- Local draft. Not published to GitHub. -->

# [Bug] Pokémon standings: the second tiebreaker (OGW%) is 0 for every player

## Version and environment

- App version: 0.2.1-preview
- Commit: `5c4fa23`
- Windows 11 Home 10.0.26200; Qt 6.10.2; MSVC 2022 (Visual Studio 17.8.5); Release, 64-bit
- Data used: disposable test database (automated test)

## Where it happened

- Game and format: Pokémon
- Players and rounds: 6 players, 2 rounds
- Screen: standings (the figure comes from `figuresFromMatches` in `src/core/swiss.cpp`)

## Steps to reproduce

1. Pokémon tournament, 6 players A–F, 2 rounds.
2. Round 1: A beats B, C beats D, E beats F. Round 2: A beats C, B beats E, D beats F.
3. Work out the standings and read `ogw_pct` for every player.

With Qt's `bin` folder on `PATH`:

```
build.bat
build\test_rules.exe pokemonSecondTiebreakerIsWorkedOut
```

Reproduces: always.

## Expected result

Pokémon lists two tiebreakers, "Opp Win%" then "OGW%" (`swiss::tiebreakColumns`), and the
standings are sorted on both. The second should be a real figure for players whose opponents
won matches.

## Actual result

`ogw_pct` is 0.0 for all six players. The figure is only calculated when the game is MTG;
for every other game it is set to 0. Pokémon standings therefore show an OGW% column of
zeros, and players level on points and Opp Win% are not separated by it.

```
XFAIL  : RulesTests::pokemonSecondTiebreakerIsWorkedOut() BUG-002: the Pokemon OGW%
tiebreaker is never calculated
```

The earlier Python version does the same (`app/logic/swiss.py`: `ogw[pid] = 0.0` for games
other than MTG), so this was carried over, not introduced by the C++ port.

## Severity and impact

- [x] **S2 Major** — points and the first tiebreaker are correct, but the final order of
  players tied on both is not decided by the tiebreaker the screen names.

Impact: Pokémon events where players tie on points and Opp Win%. In the example above B and
C are such a pair, and so are D and E. What decides their order instead has not been tested;
from the code it is the order the players are read in, which is by name.

Workaround: none in the app; break such ties by hand.

## Evidence

Test output above. Found by reading the code and writing a test, not at an event.

## For the maintainer

- Status: Reproduced, open. **Needs a rules decision before it can be fixed:** should
  Pokémon's second tiebreaker be the opponents' opponents' win percentage, the same OGW%
  MTG uses, or should the column be removed? The repository does not say, so no fix was made.
- Regression test: `tests/test_rules.cpp` — `pokemonSecondTiebreakerIsWorkedOut`, marked as
  an expected failure. It reports XPASS (a failure) as soon as the figure is calculated, as
  a reminder to replace it with the worked figures. Manual: RG-704
- Fix: none yet
- Retest result: —

## Investigation, 2026-10-10

Re-read against the current source; the behaviour is unchanged and still reproduces
(`pokemonSecondTiebreakerIsWorkedOut` reports XFAIL).

What the code does for Pokémon (`src/core/swiss.cpp`):

- Points: 1 per match win, 0 for a draw or a loss (`swiss::points`).
- First tiebreaker, shown as "Opp Win%" (`omw_pct`): the average of the opponents'
  match-win rates, byes left out, each opponent counted as at least 25 % (`omwFloor`).
  Covered by the hand-worked example in `aPointsTieIsBrokenByOpponentsMatchWinPercentage`.
- Second tiebreaker, shown as "OGW%" (`ogw_pct`): `ranked()` sorts on it, but
  `figuresFromMatches` only works it out when the game is MTG and stores 0 otherwise.
- After that there is no further key, so players level on points and Opp Win% stay in
  the order they were read from the database (by points, then Opp Win%, then name).

So the sort key is missing for Pokémon, not stale. What it should be is still a policy
question, and no change was made:

1. **Opponents' opponents' win percentage** (the average of each opponent's own Opp
   Win%). This is how community descriptions of Play! Pokémon Swiss tiebreakers describe
   the second tiebreaker, and the 25 % floor and "Opp Win%" label already follow that
   scheme. Not verified against the official handbook: the PDF on pokemon.com could not
   be fetched (HTTP 403) when this was written. The secondary source read was RK9 Labs,
   "All about tiebreakers in the Pokémon TCG" (2021-05-25), which is not an official
   document. Note that the app's Pokémon points (1/0/0) are already not the handbook's,
   so "official rules" may not be the intent.
2. **The figure MTG uses under the same column name** (the average of the opponents' own
   win rates with byes counted).
3. **No second tiebreaker**: remove the column for Pokémon and document that such ties
   are left in name order or broken by hand.

In the six-player example above, options 1 and 2 both leave B and C level (0.4375 each
under option 1, 0.75 each under option 2), and likewise D and E, so that example cannot
tell them apart; a regression test for the chosen rule needs an asymmetric event.

## Resolution, 2026-10-10

Option 1 was chosen as club policy: for Pokémon players level on points and Opp Win%, the
average of their opponents' Opp Win% values decides, highest first. The column is now
headed "Opp Opp Win%". This is the club's setting; it is not presented as full compliance
with the official tournament rules (the points are still 1 / 0 / 0).

- Same opponents and counting as Opp Win%: byes are not opponents, an opponent met twice
  counts twice, the floor is 25 %, and a player with no real opponent gets the floor.
- Stored and compared at nine-decimal precision (rounded to nine decimal places), shown
  to three. This applies to both Pokémon figures, Opp Win% and Opp Opp Win%.
- Magic and One Piece are unchanged.
- Finished Pokémon tournaments keep their saved figures and placings; the column shows
  "—" for them. Tournaments still being played show the figure immediately (standings are
  worked out in memory) and save it with the next result, round end or finish.

Regression tests (all pass): `pokemonTiesOnOppWinAreBrokenByOppOppWin` (hand-worked, and
shows Magic's figure cannot separate the same two players),
`pokemonOppOppWinLeavesOutByesAndCountsARematchTwice`,
`pokemonPlayerWithNoOpponentsGetsTheFloor`,
`pokemonTiebreakersAreNotRoundedForDisplayBeforeComparing`,
`pokemonOppOppWinLeavesFinishedTournamentsAloneAndReachesLiveOnes`.
