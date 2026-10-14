# One-on-one standings

How One Piece, Pokémon and Magic Modern tournaments are scored and ranked. Commander
events work differently; see [COMMANDER.md](COMMANDER.md).

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

## Late enrollment

What the app does today. It has not been decided whether this is how it should work.

No screen offers it: the registration page is only reachable before a tournament
starts. The core, however, still accepts a player enrolled into a tournament that has
started (it refuses finished and ended-early tournaments):

- **One-on-one:** the player joins with 0 points. The round already paired is not
  counted for them, neither as a loss nor as a bye. They are paired from the next round
  on, which can give someone else a bye if the number of players becomes odd.
- **Commander:** the player is added as checked in and active and would be seated in
  the next round, although check-in and taking a player off the list are both closed
  once the event has started.

`lateEnrollmentAsItWorksToday` in `tests/test_core.cpp` records this behaviour, so a
change of policy has to be made on purpose. Closing enrollment at the start would be a
status check in `tdb::enrollPlayer`.
