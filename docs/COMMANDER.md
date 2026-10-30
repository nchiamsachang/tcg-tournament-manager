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
| Clear second place 2.5 (still a loss), with "no clear second" when the last players went out together | **Custom** club policy (rules `commander-rules-4`) |
| Optional top-four final pod; its placing order and the drawn-final fallback | **Custom** club policy; TopDeck seats a top cut in seed order, which the final follows |
| Final standings by total points: the final awards win 5, clear second 2.5, other losses 0, draw 1 on top of qualifying points (new events' default), or by knockout order (`final_ranking`) | **Custom** club policy (release 0.3.0, rules stay `commander-rules-4`) |
| Tiebreak floor of 20%; a bye adds 3 opponents at 20% | TopDeck addendum |
| Recommended number of rounds by player count | TopDeck addendum |
| The configured rounds are the whole event, plus one top-four final only when the organizer chose it before the start | **Custom** (TopDeck events normally cut to a playoff) |
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

**The configured number of rounds is the total of qualifying rounds.** A 3-round
event is Round 1, Round 2 and Round 3. Without a final it ends when Round 3 is
finalized, and the standings after that round are the final placings. With the
optional top-four final (see below) exactly one final pod follows; nothing else can
be created after the last round, and there is no semifinal or other playoff.

A suggested number is shown beside the rounds with the checked-in count it is for
(none below three players). Until the organizer sets the rounds, they follow the
suggestion as players check in. Once the organizer changes the rounds field, even to
the suggested number, check-ins leave them alone (`rounds_chosen` in the event
settings; older events count rounds that differ from the last suggestion as chosen).
Changing another setting never sets the rounds. **Use suggested rounds** sets the
suggestion for the players checked in now and lets the rounds follow again. The
number freezes when the event starts. Drops change later pods, never the number of
rounds.

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
  when the game was drawn take a loss (policy `ELIMINATED_LOSE`, shown as
  "Knocked-out players lose", the default for new events). Policy `ALL_DRAW`
  ("Whole pod draws") gives the whole pod the draw instead. Draw points are never
  split: "sharing" the draw means each drawing player gets the full 1, not a part of
  it. An event keeps the policy it was saved with.

Each player's own result is stored, so standings never infer anything from the
pod-level outcome. A bye is worth 5.

**Second place (club rules `commander-rules-4`, events created from this version).**
A won pod has a second-place decision, saved in `commander_pods.second_place_status`:

| State | Meaning | Points |
|---|---|---|
| `PENDING` | not recorded yet (what the **Winner** button on the pod card leaves) | none yet; the pod and the round say the points are provisional |
| `PLAYER` | `second_place_player_id` names another player of the pod | 2.5, and the result is still a loss |
| `NONE` | confirmed: no clear second (the last players went out at the same moment) | none |

A drawn or unreported pod, and a bye, has no second place (the state is empty). The
core refuses the winner, a player of another pod, a player on a bye and anyone not
seated in the pod. A round cannot be finalized while any of its won pods is
`PENDING` (checked in the core and by the trigger `commander_round_finalized_complete`),
and a correction in a finalized round must decide it. Points are added up as whole
half points, so 2.5 is never rounded; they are shown as 5 or 2.5. A second place
counts in match points and therefore in MW% and OMW% (both are points ÷ 5 per round),
but there is no second-place-count tiebreaker. Changing a second place is a score
change like any other: in a finalized round with later rounds it asks Keep or
Rebuild, and a finished event's placings follow it.

**Entering results.** Results are entered on the pod cards: **Winner +5** beside each
player, then **2nd +2.5** beside the others and **No clear second**; a draw is entered
on the card with the players already knocked out ticked. Each press saves at once
through the core and redraws only that pod. Finishing a pending second place is
reporting (`SECOND_PLACE_RECORDED`) and needs no reason. Anything else that changes a
saved pod result (another winner, another or a cleared second place, a draw instead
of a win, clearing the result) is a correction: the core refuses it without a
non-blank reason, and the audit entry keeps the reason with the values before and
after, written in the same transaction.

**Events created by an earlier version** (`commander-rules-3`) keep their rules: a
second place there is recognition only (no points), "no clear second" is not a
separate state, and its state column stays empty. Nothing is converted or awarded
afterwards. They also cannot add a final.

## Standings and tiebreakers

Always recomputed from saved results.

1. Match points.
2. **OMW%** — average match-win % of every opponent faced, counted once per
   shared pod. Each bye adds three opponents at the floor.
3. **MW%** — own points ÷ (5 × rounds played), where a bye counts as a round won.
4. A random order fixed by the event seed.

Every percentage has a floor of 20%. Dropped players stay in the standings,
marked. MW% and OMW% are point-based: a clear second place counts as 2.5 of 5, not
as a win.

## Top-four final (optional)

Chosen with **Add a top-four final** before the start (`commander_events.final_enabled`,
fixed once the event starts, also by the trigger `commander_final_enabled_frozen`).
Starting needs at least four checked-in players.

**Final standings** (`commander_events.final_ranking`) decides how the final's places
are worked out. It is chosen on the setup page, under the final, and only shown while
the final is checked:

- **Total points** (the default for new events): each finalist's total is their
  qualifying points plus the points the final awards. The most total points is 1st, so
  the winner of the final can place below a finalist with more combined points. Equal
  totals go to the better qualifying seed.
- **Knockout order**: the final's elimination order decides the places, as described
  below. The final adds no points. Every event created before release 0.3.0 is
  tagged this way, also one still in registration; the organizer can switch it before
  the start.

The choice is fixed once the event starts (the core refuses it, and so does the trigger
`commander_final_ranking_frozen`). Events from before second place scored points
(`commander-rules-3`) keep Knockout order. The two policies have their own calls in the
core and each refuses the other's events.

### Final standings by total points

The finalists are created as for a knockout final, and each one's qualifying points
are saved with the final (`commander_final_players.qualifying_points`, copied from
the standings the seeds came from; finals made before update 10 read them from
`qualifying_json`). Nothing about the final is recorded until it is over. On the final's
page, per finalist, the organizer chooses:

- **Winner +5**: one player. The 2nd pills are then available.
- **2nd +2.5**: the clear second place (a loss that earns the second-place points), or
  **No clear second** when the last opponents lost at the same moment.
- **Draw**: nobody won, and nobody is second. Where eliminated players take a loss in a
  drawn game, an **Already out** tick per finalist marks those who were out when it
  was drawn: they earn 0 and the others 1. Where the whole pod draws, all four earn 1.

Awards come from the same rules as a pod (`seatResults`), as whole and half points:
winner 5, clear second 2.5, other players 0, draw 1. The places are shown as you
choose ("qual 5 + final 2.5 = 7.5", "by seed" for equal totals), and nothing is
saved until **Confirm final results**. A win whose second place is still undecided is
shown as provisional ("Provisional: choose second place or No clear second") and the
core refuses to confirm it, so a half-decided result is never stored.

Saved on confirmation, in one transaction with the audit entry (`FINAL_CONFIRMED`) and
the completion of the event: each finalist's `final_result` (WIN, SECOND, LOSS, DRAW),
`final_points`, `out_at_draw`, place and basis (`TOTAL_POINTS`, or `TOTAL_POINTS_SEED`
when another finalist has the same total), and the final's second place
(`commander_finals.second_place_player_id`, `second_place_status` PLAYER or NONE). A
draw is stored as outcome `NO_WINNER`. The final's points are never added to the
qualifying points, `enrollments.match_points`, the percentages or the saved
qualifying standings; a total is always the saved qualifying points plus the stored
award. Places 1 to 4 are the finalists in total-points order and everyone else follows
in qualifying order; the champion is 1st, who need not be the winner of the final.
Final points are not a game win or loss in the club statistics, in either policy.

**Correcting the final.** A confirmed result is changed with **Correct final**, with a
required reason and the version it was opened with (a different version is a conflict,
as for pods). The new result replaces the awards, never adds to them (`FINAL_CORRECTED`,
with the earlier winner, second place, awards and places). Repeating the saved result
changes nothing and writes no audit entry.

**Qualifying corrections.** A correction in a qualifying pod that changes any
finalist's qualifying points is treated like one that changes the seeds: it asks to
discard the final (`REBUILD_FINAL`) while the final has not begun, and is refused once
it has. The question names what changes: finalists, seeds and qualifying points. The
saved qualifying points are never overwritten.

Ending the tournament early during a total-points final places nobody and names no
champion, as for a knockout final.

### Final standings by knockout order

1. The qualifying rounds are played and finalized as usual. Finalizing the last one
   does not complete the event.
2. **Create final** shows the four best players in the qualifying standings who are
   checked in and have not dropped, with their seeds, and asks for confirmation.
   Saving checks again that they are still the qualifiers. The standings are saved
   with the final (`commander_finals.qualifying_json`) and the players, seeds and
   seats in `commander_final_players`; seats follow the seeds, seed 1 first. One
   final per event (unique per tournament, and the trigger `commander_final_allowed`);
   a repeated request returns the same final.
3. Knockouts are recorded on the final's page as they happen, under **Record
   knockout**: select every player knocked out at the same moment (a pill each) and
   press **Confirm knockout**. Each is saved as it is confirmed, and the selection is
   cleared only then. A player who is out is no longer offered, to knock out or as
   the winner. A knockout always leaves at least one player in: the core refuses one
   that would remove everyone still in, and its message says to choose the winner
   (one player left) or **No winner / Draw** (the last players went out together).
   With one player left their knockout is disabled and the page says "One player
   remains. Confirm the winner to finish."; nothing is chosen or saved for the
   organizer. Taking the last knockout back is a correction and needs a reason
   (`FINAL_ELIMINATIONS_CORRECTED`).
4. The ending is chosen beside it, under **Determine winner**: one player won, or
   **No winner / Draw** (the game was drawn, or everyone still in went out at the
   same moment; they are placed by seed). The two sections sit side by side, or one
   above the other in a narrow window. The four places are shown before anything is
   saved, and **Confirm final results** asks once more, saves them and completes the
   event. The clock never decides the result.

Places: the first group out takes the lowest places, the next group the places above,
and so on; the winner is 1st. Inside a group the better qualifying seed takes the better
place, and only there: a better seed never moves above a player who stayed in longer.
With no winner (a drawn final, or one ended without a winner under the club's time
procedure), the players still in take the top places in seed order, marked
`QUALIFYING_RANK` ("by seed"); nobody is credited with a game win and no points are
added. Everyone else follows in qualifying order. The final never changes qualifying
points, percentages, second places or pairing inputs, and the earlier rule that puts
the best player who has not dropped first does not apply to an event with a final.

If fewer than four eligible players are left when the final is to be created, the app
says so and offers what already exists: undo a drop made after the last round, or end
the tournament early. It never fills the final with someone else, drops the option or
names a champion by itself.

**Corrections around the final.** Its seeds are fixed. A qualifying correction that
leaves the four finalists and their seeds as they were is saved, and the final stays.
One that would change them is refused unless the final has not begun and the
organizer chooses to discard it (`REBUILD_FINAL`, audited as `FINAL_DISCARDED`); the
final is then created again. Once the final has begun, such a correction is refused.
A final has begun from the first start of its clock, its first knockout or its
confirmation (`commander_finals.play_started_at`, update 9), and stays begun when the
knockout is taken back or the clock is reset. A confirmed final can be corrected with a
reason (`FINAL_CORRECTED`, with the earlier places).

**Drops during the final.** A finalist cannot be dropped while the final is being
played, because the club's rules do not say how a finalist who leaves is placed; the
organizer records the final as it was played or ends the tournament early. Other
players can still drop; it changes nothing in the final. A drop cannot be undone once
the final exists.

## Finishing

Without a final: when every pod of the last configured round has a result, the
round screen offers **Finish tournament**. On confirmation, in one transaction, the app:

1. finalizes that round;
2. recomputes the standings with the event's scoring and tiebreakers;
3. saves each player's final placing (`enrollments.final_placement`), a full
   snapshot of the final standings (`commander_events.final_standings_json`),
   the first-placed player and the completed status;
4. shows the final standings.

The first place goes to the best-placed player who has not dropped; everyone
else follows in standings order. Finalizing the last round through any other
path does the same thing. Finishing twice changes nothing. With a final, confirming
the final does the same, with the final's places for the four finalists.

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

**Corrections.** Every change to a saved result, in a live round, a finalized round
or the final, needs a non-blank reason; the core checks it and the audit entry keeps
it. In a live round, changing a saved result also needs the version the screen last
loaded, so two windows cannot silently overwrite each other. In a
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
- `7 commander_second_place` adds `commander_pods.second_place_player_id`.
- `8 commander_scored_second_and_final` adds `commander_pods.second_place_status`,
  `commander_events.final_enabled`, the tables `commander_finals` and
  `commander_final_players`, and triggers that keep a second place inside a won pod,
  stop a round being finalized with a result or second place to report, allow only
  the one final of an event that chose it, fix its players and seeds, and refuse a
  confirmed final without four places. Existing rows get no second-place state and
  no final. Like every update it runs after a backup of the database.
- `10 commander_final_ranking` adds `commander_events.final_ranking` (KNOCKOUT or
  TOTAL_POINTS; every existing event is tagged KNOCKOUT, also one still in
  registration), `commander_finals.second_place_player_id` and `second_place_status`,
  and rebuilds `commander_final_players` the way update 4 rebuilds tournaments (rows
  copied and counted, index and triggers put back) to allow the placement bases
  `TOTAL_POINTS` and `TOTAL_POINTS_SEED` and to add `qualifying_points`,
  `final_result`, `final_points` and `out_at_draw`. Existing finals, including
  unfinished ones, keep every value they had; the trigger
  `commander_final_players_fixed` now also fixes `qualifying_points`, and
  `commander_final_ranking_frozen` stops the choice changing after the start. It runs
  after a backup of the database.

The columns `playoff_cut`, `recommended_cut`, `playoff_seeds_json` and the
table `commander_advancements` remain only to hold what earlier events saved.

Code: rules and pairing in `src/core/commander.cpp`, persistence in
`src/core/commander_db.cpp`, screens in `src/ui/screens_commander.cpp`,
tests in `tests/test_core.cpp` and `tests/test_ui.cpp`.

## Limitations

- The app is a single-user desktop program with a local SQLite file. There are
  no accounts, so "organizer" is whoever is at the keyboard and is the actor
  recorded in the audit log. Several windows on the same file are safe.
- Standings can be saved as PDF or CSV. Pairing sheets and pod signs print from the
  round's Print menu, also for the final pod.
- Pairing uses local search, so for large fields it finds a good pairing, not a
  provably optimal one.
- A total-points final records only its result: the order players were knocked out is not
  asked for or saved. Its awards are the club's fixed values (5, 2.5, 0, 1); they cannot
  be changed per event, and the final's points are not part of the club statistics.
- Switching Final standings is only possible before the event starts; an event cannot
  change policy afterwards, and a finalist's qualifying points are fixed when the final
  is created.
- The only cut is the optional top-four final. Larger cuts (Top 10, Top 16) are not
  offered.
- The final uses the tournament's round length for its clock. TopDeck's addendum
  gives a top cut 180 minutes; the club has not set a separate final length.
- A playoff that was part-way through when the app was upgraded cannot be
  resumed (see above).
