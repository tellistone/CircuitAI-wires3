# Chokepoints

CircuitAI can learn the shape of each map: which gaps in the terrain are chokes, which mex clusters sit behind which
chokes, where the enemy is likely to walk, and how open the map is. Several features use that to place towers, radar
and the army. Every feature is off by default, so with the shipped config the AI plays as it did before. The
`choke_overlay` chat command draws what the AI found, so you can check a map before you turn anything on.

The analysis is shared by allied AIs and runs on the area worker thread. It is built only when a feature reads it, or
when `choke_overlay` asks for it.

## The analysis

- **Threat graph.** The map is cut into 32-elmo cells that the enemy's *gating* movement types can cross (walkers,
  tanks, and hovers when the map has water). The open areas become regions, and the narrow gaps between them become
  chokes. A choke is *defensible* when it is at most `choke.defensible` main-tower ranges wide. Amphibious bots,
  spiders and jumpers are *leak* types: they don't shape the graph, they only make the clusters they can reach leaky.
  Jumpers cross a wall when it is no longer than their jump along a row or a column of cells. A leak type starts
  gating once its own cost reaches `choke.gate_share` of the enemy army seen plus a 300-metal prior, so a lone early
  unit can't flip it. It stays gating until it leaves the game.
- **Enemy types.** The opening default holds until a factory has been seen for every enemy team, or for 5 minutes. A
  type joins when a factory that builds it or one of its units is seen. It leaves only when every known factory for it
  is dead and none of its units has been seen for 5 minutes.
- **Sides and territory.** Map sides come from start boxes and enemy factories only. Territory comes from buildings
  in each region: a region joins 20 s after the first building is seen there and leaves 60 s after the last.
- **Pockets.** Each defence point (`CDefenceMatrix`) gets its entrances from the enemy side: the fewest chokes the
  enemy must pass to walk in. Points with more than `choke.entrances` entrances are *open*. Points that share a cut
  form a *pocket*. A pocket is *qualified* when every entrance is defensible, within `choke.near` ranges of its
  points, and has no enemy territory right outside it. A point is *leaky* when a leak type or enemy aircraft can reach
  it without passing the cut, or when ground outside the cut is within `choke.exposure` elmos of its mexes.
- **Lines.** The *current line* is the cut nearest our territory against enemy territory. The *forward line* is the
  cut nearest our territory against the enemy's side of the map. A region both sides hold counts as ours.
- **Traffic field.** On 64-elmo cells, one layer per enemy movement type: where its units are likely to walk from the
  enemy's factories (or start boxes) to our clusters and base. The layers are blended by the enemy's army mix, at most
  every 30 s and only when a share moves by more than 0.1.
- **Openness.** Half the share of mex income that is open from the enemy side, plus half the width of the narrowest
  separation between the sides over 4 main-tower ranges. Below `choke.openness.bands[0]` the map is *chokepointed*,
  above `bands[1]` it is *open*, and in between *semi-open*. The band only changes `bands[2]` past a boundary.
- **Lanes** (with `choke.enable`). A corridor `choke.lane` wide through every choke narrower than 3 lanes is kept
  free of buildings, except mexes, so our own towers and walls never close it.

Choke ids carry over when the terrain changes or the gating types change. A new choke waits 20 s before anything acts
on it.

## Features and their flags

| Key | Default | What it turns on |
|---|---|---|
| `choke.enable` | `false` | **Choke defence (A).** A qualified pocket with fewer entrances than defence points defends at its entrances. The pocket's towers move there, and AA stays on each cluster. A leaky pocket keeps each cluster's first tower too. Defence points are also split by region, and lanes are kept free of buildings. |
| `choke.spot.enable` | `false` | **Scored spots (F).** Every tower from `MakeDefence`, the base towers and the hold spots get the best spot within reach, scored by covered traffic, height, threat and builder distance. The porc sweep visits the points with the most uncovered traffic × income first. |
| `choke.radar.enable` | `false` | **Radar planner (B).** Radar goes where it covers the most uncovered weight for its cost, until `choke.radar.coverage` of the map is covered. It takes over `MakeDefence`'s radar. |
| `quota.hold_choke` | `false` | **Holding the current line (C).** The idle attack group holds at the top-ranked frontline choke whose towers are short of the enemy army near it. That's 1 choke, or 2 on chokepointed maps, and none on open maps. With `choke.enable`, that choke's pocket is fortified too. |
| `choke.objective.enable` | `false` | **Forward objectives (D).** When taking the forward line is worth it, idle attack groups go for its enemy-held chokes. |
| `choke.expand` | `false` | **Expansion bonus.** Mex expansion (`economy.mex_front`) scores clusters behind a cut with towers ×0.7, and behind a qualified cut without towers ×0.85. Lower is better. |

The rankings of enemy-held chokes (E) feed D and the overlay. They are computed when C or D is on, or when the
overlay is on.

## Tuning keys

All in the top-level `"choke"` block of `behaviour.json`. Sizes are in main-tower ranges, using the first
`porcupine.land` defender (or `porcupine.default`), unless noted.

| Key | Default | Meaning |
|---|---|---|
| `choke.region.min_area` | `0.5` | Smallest region, in main-tower ranges squared. A smaller one merges into a neighbour. |
| `choke.region.merge` | `0.7` | Two regions merge when the choke's altitude reaches this share of the smaller region's highest. |
| `choke.defensible` | `2.0` | Widest choke that towers try to close. |
| `choke.entrances` | `3` | Most entrances before a cluster counts as open. |
| `choke.near` | `2.5` | Furthest useful entrance from a defence point. |
| `choke.exposure` | `600` | Elmos. Ground outside its cut this close to a cluster's mexes makes it leaky. |
| `choke.setback` | `[0.6, 0.8]` | Ideal tower distance behind a choke's line, in the tower's own weapon ranges. |
| `choke.lane` | `96` | Elmos. Width of the corridor kept free of buildings. |
| `choke.gate_share` | `0.25` | Share of the enemy army seen (plus a 300-metal prior) a leak type's cost must reach to start gating. |
| `choke.traffic.spread` | `0.15` | Width of the traffic corridors, as a share of the route's length. |
| `choke.openness.bands` | `[0.35, 0.65, 0.05]` | Chokepointed below, open above, and the margin around them. |
| `choke.radar.coverage` | `0.75` | Share of the map's weight the radar planner aims to cover. |
| `choke.radar.share` | `0.05` | Most of the metal income spent on planned radar. |
| `choke.objective.gain` | `1.5` | An objective's gain must be at least this × its cost. |
| `choke.objective.power` | `1.3` | Our attack power must be at least this × the objective's defence. |
| `choke.objective.retry` | `180` | Seconds before a failed objective may be tried again. |

## The `choke_overlay` command

Type it in chat. Every allied CircuitAI gets the line, but the ally team handles it once. Commands from enemy
players are ignored. A spectator may use it only in games with no human opponents.

| Command | What it does |
|---|---|
| `choke_overlay` or `choke_overlay toggle` | Shows or hides the overlay. The first time, it builds the analysis if no feature asked for it. The answer gives the map's openness and band. |
| `choke_overlay next` | Shows the next layer. |
| `choke_overlay <layer>` | Shows that layer: `chokes`, `pockets`, `lines`, `ranks`, `traffic`, `spots`, `radar` or `regions`. |
| `choke_overlay <movement class>` | Shows one movement class's own graph on the `chokes` and `regions` layers. Use a unit def name (`cloakraid`) or a class name. An unknown name lists the classes. |
| `choke_overlay all` | Back to the threat graph. |
| `choke_overlay labels` | Turns the text labels on or off. They are map markers, which human allies see too, so they start off when the AI has human allies. |
| `choke_overlay set <key> <value>` | Changes a tuning key or flag and rebuilds. Keys are named as in the config without `choke.`, for example `region.min_area 0.4`, `setback 0.5 0.7`, `spot.enable true`, and `hold_choke` for `quota.hold_choke`. `openness.bands` and `objective.retry` can't be set this way. Only in games with no human opponents. |
| `choke_overlay save` | Writes the chokes and point classes to `choke/<map>.json` in the AI's writeable data directory. Debug builds compare against that file at start and log what differs. Only in games with no human opponents. |

The AI answers in ally chat and in its log, as `CHOKE overlay: ...`.

### Layers

| Layer | What it draws |
|---|---|
| `chokes` | Each choke from end to end of its line: green when defensible, grey when not. Lanes are yellow rectangles. |
| `pockets` | Each pocket's entrances by mode: green at chokes, yellow mixed, white on cluster, grey not decided yet. Blue lines link its points to them. Points are red when open, orange leaky, green gated, grey unreached. Magenta marks the anchors of choke points. |
| `lines` | The current line orange, the forward line yellow, the objective magenta. Hold slots are cyan, with their spot. Labels give each frontline choke's rank, wanted power and tower power. |
| `ranks` | Enemy-held chokes: red when defended, white when not. Labels give the defence, the unarmed value behind and the share seen in the last 3 minutes. |
| `traffic` | The busiest cells of the blended traffic field: bigger and redder the busier. |
| `spots` | The last spot search's candidates: the best green, the next 4 cyan, the rest grey. |
| `radar` | The radar planner's candidates blue, and the edge of today's static radar coverage cyan. |
| `regions` | The graph itself: region centres linked to their chokes. On the threat graph, our side is green and the enemy's side red. |

Layers that other modules feed (`lines`, `ranks`, `spots` and `radar`) redraw every 10 s. The others redraw when the
analysis changes.

## How the features decide

### Choke defence (A)

When `MakeDefence` picks towers for a defence point, the pocket behind the point decides where they go. The pocket's
mode is fixed when its first tower is ordered:

- **On cluster**, as without `choke.enable`, when the pocket isn't qualified, has at least as many entrances as
  defence points, or can't afford a main tower per entrance.
- **At chokes** otherwise. The towers go to the entrances, and AA stays on each cluster.
- **Mixed** when the pocket is leaky. Each cluster also keeps its first tower.

The pocket's pool is what its points would spend today less what each keeps on its cluster, so defending at the
chokes only concentrates the same metal. The pool is split across the entrances by their traffic, with at least one
tower each. Each call's towers go to the entrance with the most room left, and its tower list repeats while that
entrance's share has room. A point within one main-tower range of an entrance stays on its cluster, because there the
cluster and the choke are the same place. Mex builders count a cluster whose pocket has towers at an entrance as
defended, so they don't wait for an escort there.

The mode switches only on a hard event, and at least 3 minutes apart. Hard events are the pocket's cut changing, it
qualifying or no longer qualifying, it turning leaky, its pool first affording a tower per entrance, and it being
lost. Built towers never move. While enemy artillery has been seen in range of a choke in the last 5 minutes, no new
towers are queued there, and those not yet started are called off.

### Scored spots (F)

`score = cover × (1 + 0.5 × height) × setback ÷ (1 + threat ÷ own threat) ÷ (1 + builder distance ÷ a quarter of the map's diagonal)`

- *cover* is the traffic of the targets in range with a clear line of fire, halved for each of our towers that
  already covers a target.
- *height* is the height above the targets, up to 150 elmos.
- *setback* is the closeness to `choke.setback` ranges behind the line, for towers at chokes.

A spot freezes once its tower is queued. A tower that is cancelled before it starts unfreezes its spot and masks that
ground for 5 minutes. After 2 failures at a slot, that slot goes back to the old placement. Towers the AI calls off
itself (porc mode ending, artillery near a choke) keep their spot and count no failure.

The porc sweep scores every point of the clusters we hold when a round starts, then visits them in that order, one
every 5 s.

### Radar planner (B)

Each cell the enemy can reach weighs a floor plus its traffic, each scaled to add up to our mex income, plus the
income of our clusters' mexes in it. Coverage is the union of the viewsheds of known static radars, ours and our
allies'. Mobile radar is ignored. Every 15 s while under target, it queues the candidate with the most uncovered
weight for its cost × (1 + risk). Candidates are the highest buildable cell of each 512-elmo block on our side or in
our territory. Risk is the threat there in main towers, plus 1 for each radar lost nearby, halving every 5 minutes.
At most one planned radar waits for a builder at a time.

### Holding the line (C), objectives (D) and rankings (E)

- **Frontline.** The current line's chokes, ranked by our income behind each one × its traffic. Each of our points
  behind the line counts at its nearest line choke.
- **Hold slots.** A slot sits at the top-ranked choke whose towers are short of 1.2 × the enemy army seen near it in
  the last 3 minutes. It moves only to a choke ranked 1.3× higher. The first idle attack group to ask claims it, walks
  to its scored hold spot, and keeps the claim until the task ends. A slot with nowhere to stand (no traffic near the
  choke) stays free and is tried again after 30 s. A group that runs from a fight gives up its claim for 30 s.
- **Objectives.** When the forward line is shorter than the current line, or as long with more income behind it,
  its enemy-held chokes become an objective. That needs the gain to be `choke.objective.gain` × the cost, and our
  attack power to be `choke.objective.power` × their defence.
  - The gain is the towers and wanted power of the current-line chokes it would retire, plus 3 minutes of the income
    it brings behind the line.
  - The cost is their defence plus what holding them takes.
  - It lasts until taken, for at most 4 minutes in all (switches included), or until our attack power falls below its
    defence. A failed one waits `choke.objective.retry` seconds.
- **Rankings.** A choke is enemy-held when its outer side from us is enemy territory, or known enemy towers cover its
  line. Its defence is the threat of enemy towers reaching the line or our approach to it, plus enemy mobiles within a
  main-tower range. Its value is the cost of unarmed enemy units directly behind it.

## Logs

Lines in the AI's infolog, for tuning:

- `CHOKE graph`, `CHOKE masks` and `CHOKE traffic`: build sizes and times.
- `CHOKE openness`: the value, the band and its inputs.
- `CHOKE pocket [...]`: a pocket's first mode, and every switch.
- `CHOKE point for choke`: where a choke's towers anchor.
- `CHOKE spot`: each spot search, and each failed spot.
- `CHOKE artillery near choke`: towers called off.
- `CHOKE hold` and `CHOKE objective`: slots and objectives starting, moving and ending.
- `RADAR planned`: each planned radar, with coverage before and the share it adds.

## Known limits

- Traffic sources are the enemy's factory positions, not whole factory regions.
- Sight and fire lines start at the def's sight emit height for both towers and radar.
- Fortifying the frontline (C) only calls `MakeDefence` for pockets whose entrances are frontline chokes. A frontline
  choke with no pocket of ours behind it gets no towers of its own.
- New tower spending behind a line that moves forward doesn't pause.
- Objectives (D) only send idle attack groups to the chokes. E's rankings don't steer raids, scouts or artillery yet.
- A tower's cost is counted on the nearest defence point with room for it, as before. A tower at a choke can
  occasionally be counted on a cluster point near that choke.
- Viewsheds are cached until the terrain changes anywhere, not only near them.
