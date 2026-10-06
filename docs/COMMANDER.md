# Commander multiplayer events

How this app runs MTG Commander tournaments. Modern (and One Piece, Pokémon)
events are unchanged and use the original one-on-one code.

**This is not an official Wizards of the Coast pairing algorithm.** Where a rule
comes from a published guideline it is marked *TopDeck*; everything marked
*Custom* is this club's own policy.

References: [TopDeck multiplayer addendum](https://topdeck.gg/mtr-ipg-addendum),
[Running a Commander tournament](https://topdeck.gg/help/running-commander-tournament).

## Where things come from

| Rule | Source |
|---|---|
| Win 5, loss 0, draw 1; a bye counts as a win | TopDeck addendum |
| Tiebreak floor of 20%; a bye adds 3 opponents at 20% | TopDeck addendum |
| Recommended number of rounds by player count | TopDeck addendum |
| The configured rounds are the whole event: no playoff, top cut or final | **Custom** (TopDeck events normally cut to a playoff) |
| Any number of three-player pods so everyone plays | **Custom** (TopDeck allows at most one per round) |
| Three-player pods go to the lowest standings by default | **Custom** default; TopDeck's software offers low or high as an option |
| How match-win % is computed and the tiebreaker order | **Custom** reading — the addendum does not spell it out |
| Final random tiebreaker | **Custom** |
| Pairing cost weights and search | **Custom** |
| Eliminated players take a loss in a drawn pod (configurable) | **Custom** |

## Pods

Four players by default. For `n` active players the app solves `4a + 3b = n`
with the most four-player pods. Pods are only ever three or four players.

- 5 players: one four-player pod and one bye (the only count with no solution).
- Fewer than 3: pairing is blocked with an explanation.
- A three-player event is allowed.

## Rounds

**The configured number of rounds is the total.** A 3-round event is Round 1,
Round 2 and Round 3; it ends when Round 3 is finalized and the standings after
that round are the final placings. There is no playoff, top cut, semifinal or
separate final, and nothing can be created after the last round.

The number is recommended from the checked-in count when registration closes;
the organizer can override it before starting. It freezes when the event
starts. Drops change later pods, never the number of rounds.

| Players | Rounds |
|---|---:|
| 3–16 | 3 |
| 17–34 | 4 |
| 35–128 | 5 |
| 129–208 | 6 |
| 209–304 | 7 |
| 305–540 | 8 |
| 541–960 | 9 |
| 961+ | 10 |

## Pairing — algorithm `cmdr-swiss-2`

Each round's seed is `sha256(event seed : round number)`. The seed, the
algorithm version, the full input (players, points, ranks, byes, three-pod
counts, seat history, previous meetings) and the resulting cost are saved with
the round, so any published pairing can be reproduced exactly.

**Hard constraints** — never traded away: pods of exactly 3 or 4, and every
active player assigned exactly once (one pod, or the single five-player bye).

**Round one** is a seeded shuffle.

**Later rounds** start from standings order and are improved by bounded local
search: pairwise swaps between pods inside a standings window, with seeded
restarts and a cap on evaluations. The cost being minimised is:

| Term | Cost | Weight |
|---|---|---:|
| Placement | match points on the wrong side of the short-pod cutoff, per player in a three-player pod | 200 |
| Score | squared match-point difference, per pair sharing a pod | 4 |
| Rematch | squared number of earlier meetings, per pair sharing a pod | 30 |
| Three-player pod | earlier three-player pods of each player placed in one | 12 |

**Where three-player pods go** is an organizer setting, frozen at start:

- **Lowest standings** (default) — short pods are filled from the lowest
  match-point groups, so the leaders play four-player pods. A three-player pod
  is a 1-in-3 game, so this avoids handing the leaders better odds. The costs
  are that the top pod sometimes mixes point totals, and that in a small field
  the same low-standing players can sit in short pods more than once.
- **Highest standings** — short pods are filled from the top instead.
- **Best points match** — no preference; short pods land wherever points line
  up best, which can be the top pod. Events started before this setting existed
  keep this behaviour.

Players tied on points are interchangeable for this rule, so rematch avoidance
and sharing short pods fairly still apply inside the group.

With these weights, mixing a full win (5 points) costs more than a rematch, and
a rematch costs more than mixing a one-point draw gap. Rematches are minimised,
not forbidden: in a small field they are unavoidable and the search allows them.

**Seats** are then chosen per pod to minimise the squared count of times each
player has already sat in that seat, with seat 1 (first turn) weighted ×3.

**Byes** go to the player with the fewest earlier byes, then the lowest standing.

## Results and scoring

One game per pod. A pod is *pending* until reported, then either:

- **Win** — one winner (5); everyone else loses (0).
- **Draw** — players still in the game each get 1. Players already eliminated
  when the game was drawn take a loss (policy `ELIMINATED_LOSE`, the default).
  Policy `ALL_DRAW` gives the whole pod the draw instead.

Each player's own result is stored, so standings never infer anything from the
pod-level outcome. A bye is worth 5.

## Standings and tiebreakers

Always recomputed from saved results.

1. Match points.
2. **OMW%** — average match-win % of every opponent faced, counted once per
   shared pod. Each bye adds three opponents at the floor.
3. **MW%** — own points ÷ (5 × rounds played), where a bye counts as a round won.
4. A random order fixed by the event seed.

Every percentage has a floor of 20%. Dropped players stay in the standings,
marked.

## Finishing

When every pod of the last configured round has a result, the round screen
offers **Finish tournament**. On confirmation, in one transaction, the app:

1. finalizes that round;
2. recomputes the standings with the event's scoring and tiebreakers;
3. saves each player's final placing (`enrollments.final_placement`), a full
   snapshot of the final standings (`commander_events.final_standings_json`),
   the first-placed player and the completed status;
4. shows the final standings.

The first place goes to the best-placed player who has not dropped; everyone
else follows in standings order. Finalizing the last round through any other
path does the same thing. Finishing twice changes nothing.

## Workflow and safeguards

1. Create the event, register and check in players, start. Configuration freezes.
2. Report each pod. Results are provisional until the round is **finalized**.
3. After finalizing, **Start Round N** generates and publishes the next round.
   There is no pairing preview; drops made between rounds apply to the round
   that is generated next.
4. On the last configured round the action is **Finish tournament** instead.

The next round cannot be published until the current one is finalized.

**No rounds past the total.** This is enforced in three places, not just by
hiding a button:

- the workflow refuses to publish once the configured number of rounds exists,
  or once the event is finished, and a repeated request for an existing round
  returns that round instead of creating another;
- trigger `commander_round_limit` makes the database reject any round whose
  number is above the configured total, any round that is not a normal round,
  and any round for an event that is not in play;
- trigger `commander_round_count_frozen` stops the total being raised after
  the event has started, and `commander_no_new_playoff` rejects a new event
  with a playoff configured.

If drops leave fewer than three active players between rounds, no pod can be
formed; the organizer can finish the event early on the current standings. That
ends it with fewer rounds than configured and never adds one.

**Corrections.** In a live round, changing a saved result needs the version the
screen last loaded, so two windows cannot silently overwrite each other. In a
finalized round that later rounds were already published from, the organizer
must choose:

- **Keep** — standings are recomputed; the later rounds stay as published.
- **Rebuild** — later rounds are discarded and the event reopens after the
  corrected round. Refused once a later round has any result.

Correcting a finished event updates its saved final standings and never
reopens it or adds a round (except through an explicit Rebuild as above).

Every report, correction, drop, publish, finalize and finish is written to an
audit log (History in the app).

## Events from the earlier version that had a playoff

Earlier versions could add a Top 4 / 10 / 16 playoff after the rounds.
Migration 3 does not delete anything that was played. It sorts existing events
into three groups:

| Event when upgraded | What happens |
|---|---|
| Playoff only configured (registration, or rounds still in play) | The playoff setting is removed and recorded in History (`PLAYOFF_SETTING_REMOVED`). The event runs its rounds and finishes normally. |
| Finished, with playoff rounds played | Flagged `legacy_playoff`. Rounds, playoff pods, winner and saved placings are kept exactly as played and shown read-only, labelled "earlier version". |
| Rounds complete, playoff seeded or part-way through | Flagged `legacy_playoff` and marked **Needs finishing**. No playoff round can be published, reported or finalized. The organizer chooses **Finish tournament**, which saves the final standings from the configured rounds. Playoff rounds already created stay on record and do not count. |

Flagged events show an "Earlier-version playoff" notice on their round screen.
If the organizer would rather keep a part-played playoff's result, the options
are to leave the event unfinished as a record, or to finish it on its standings
and note the playoff outcome elsewhere; the app will not resume the playoff.

## Data

Migrations are numbered, additive and recorded in `schema_migrations`.

- `1 commander_multiplayer` adds `enrollments.checked_in` and the tables
  `commander_events`, `commander_rounds`, `commander_pods`, `commander_seats`,
  `commander_byes`, `commander_advancements` and `commander_audit`, with
  uniqueness constraints and triggers that stop duplicate rounds and a player
  being in two places in one round.
- `2 commander_round_timer` adds the saved round clock to `commander_rounds`.
- `3 commander_fixed_rounds` adds `legacy_playoff`, `final_standings_json` and
  `completed_at` to `commander_events`, flags the events described above and
  adds the three triggers that cap the number of rounds.

The columns `playoff_cut`, `recommended_cut`, `playoff_seeds_json` and the
table `commander_advancements` remain only to hold what earlier events saved.

Code: rules and pairing in `src/core/commander.cpp`, persistence in
`src/core/commander_db.cpp`, screens in `src/ui/screens_commander.cpp`,
tests in `tests/test_core.cpp` and `tests/test_ui.cpp`.

## Limitations

- The app is a single-user desktop program with a local SQLite file. There are
  no accounts, so "organizer" is whoever is at the keyboard and is the actor
  recorded in the audit log. Several windows on the same file are safe.
- No standings PDF export on Commander screens yet (CSV export works). Pairing sheets and pod signs
  print from the round's Print menu.
- Pairing uses local search, so for large fields it finds a good pairing, not a
  provably optimal one.
- There is no playoff or top cut. An event that needs one has to be run as a
  separate tournament.
- A playoff that was part-way through when the app was upgraded cannot be
  resumed (see above).
