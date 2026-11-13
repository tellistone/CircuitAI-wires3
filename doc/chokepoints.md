# Chokepoints

CircuitAI can learn the shape of each map: which gaps in the terrain are chokes, which mex clusters sit behind which
chokes, where the enemy is likely to walk, and how open the map is. Several features use that to place towers, radar
and the army. Every feature is off by default, so with the shipped config the AI plays as it did before. The
`choke_overlay` chat command draws what the AI found, so you can check a map before you turn anything on.

The analysis is shared by allied AIs and runs on the area worker thread. It is built only when a feature reads it, or
when `choke_overlay` asks for it.

## The analysis

- **Threat graph.** The map is cut into 32-elmo cells that the enemy's *gating* movement types can cross (walkers,
  tanks, and hovers when the map has water). A cell counts as crossable when at least 3 of its 4 slope-map pixels
  are, so a cliff or a spike's side only one or two pixels thick stays a wall wherever it falls on the grid. Of two
  crossable neighbours that no pair of pixels crosses between (a thin diagonal cliff), one closes. The open
  areas become regions, and the narrow gaps between them become chokes. A choke is *defensible* when it is at most `choke.defensible` main-tower ranges wide. Amphibious bots,
  spiders and jumpers are *leak* types: they don't shape the graph, and each cluster keeps a tower of its own against
  them and anything else the cut doesn't stop. Jumpers cross a wall when it is no longer than their jump along a row
  or a column of cells. A leak type starts gating once its own cost reaches `choke.gate_share` of the enemy army seen
  plus a 300-metal prior, so a lone early unit can't flip it. It stays gating until it leaves the game. A gating
  spider type makes the graph close to the spiders' own, so its chokes move to where spiders are held too: the
  overlay says so each time the graph is rebuilt.
- **Enemy types.** The opening default holds until a factory has been seen for every enemy team, or for 5 minutes. A
  type joins when a factory that builds it or one of its units is seen. It leaves only when every known factory for it
  is dead and none of its units has been seen for 5 minutes.
- **Sides and territory.** Map sides come from start boxes and enemy factories only. Territory comes from buildings
  in each region: a region joins 20 s after the first building is seen there. It leaves 60 s after our last one, or
  120 s after the enemy's last. Only enemy towers and enemy buildings of at least 150 metal count, so a lone radar or
  wall doesn't flip a region.
- **Pockets.** Each defence point (`CDefenceMatrix`) gets its entrances from the enemy side: the fewest chokes the
  enemy must pass to walk in. Points with more than `choke.entrances` entrances are *open*. Points that share a cut
  form a *pocket*. A pocket is *qualified* when every entrance is defensible, within `choke.near` ranges of its
  points, and has no enemy territory right outside it.
- **Lines.** The *current line* is the cut nearest our territory against enemy territory. The *enemy line* is the
  same cut nearest enemy territory, its mirror. Where the narrowest gap between the sides is one set of chokes, the
  two are the same; where there is ground between them that either side could hold as cheaply, they part. The *front
  line* is the chokes they share, which every cheapest cut between the sides passes, and is empty while they don't
  touch. The *forward line* is the cut nearest our territory against the enemy's side of the map. A region both sides
  hold counts as ours. Nothing acts on the enemy or front lines yet; the overlay draws them.
- **Traffic field.** On 64-elmo cells (crossable by the same rule over their four 32-elmo cells), one layer per enemy
  movement type: where its units are likely to walk from the
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
| `choke.enable` | `false` | **Choke defence (A).** A qualified pocket with fewer entrances than defence points defends at its entrances. Each cluster keeps its AA and its first tower, and the rest of the pocket's towers go to the entrances. Defence points are also split by region, and lanes are kept free of buildings. |
| `choke.spot.enable` | `false` | **Scored spots (F).** Every tower from `MakeDefence`, the base towers and the hold spots get the best spot within reach, scored by covered traffic, height, threat and builder distance. The porc sweep visits the points with the most uncovered traffic × income first. |
| `choke.radar.enable` | `false` | **Radar planner (B).** A builder between jobs builds a radar nearby first when its budget holds one, if it adds at least `choke.radar.min_gain` of the map's weight (and, standing in coverage, `choke.radar.inside` of its view is new). It takes over `MakeDefence`'s radar. |
| `quota.hold_choke` | `false` | **Holding the current line (C).** The idle attack group holds at the top-ranked frontline choke whose towers are short of the enemy army in the region outside it. That's 1 choke, or 2 on chokepointed maps, and none on open maps. With `choke.enable`, that choke's pocket is fortified too. |
| `choke.objective.enable` | `false` | **Forward objectives (D).** When taking the forward line is worth it, idle attack groups go for its enemy-held chokes. |
| `choke.expand` | `false` | **Expansion bonus.** Mex expansion (`economy.mex_front`) scores clusters behind a cut with towers ×0.7, and behind a qualified cut without towers ×0.85. Lower is better. |

The rankings of enemy-held chokes and the enemy line's (E) feed D and the overlay. They are computed when C or D is
on, or when the overlay is on.

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
| `choke.setback` | `[0.6, 0.8]` | Ideal tower distance behind a choke's line, in the tower's own weapon ranges. |
| `choke.lane` | `96` | Elmos. Width of the corridor kept free of buildings. |
| `choke.gate_share` | `0.25` | Share of the enemy army seen (plus a 300-metal prior) a leak type's cost must reach to start gating. |
| `choke.traffic.spread` | `0.15` | Width of the traffic corridors, as a share of the route's length. |
| `choke.openness.bands` | `[0.35, 0.65, 0.05]` | Chokepointed below, open above, and the margin around them. |
| `choke.spot.radius` | `300` | Elmos from a defence point its towers may stand. |
| `choke.spot.height` | `0.5` | The most a spot's height adds to its score: 0.5 is up to +50%, 1 up to double. |
| `choke.spot.height_cap` | `150` | Elmos above the ground it covers at which a spot earns all of `choke.spot.height`. |
| `choke.spot.enemy` | `1.0` | Weight of the enemy value a tower can shoot from a spot, per the tower's own cost: at 1, enemies worth the tower double the score. 0 turns it off. |
| `choke.spot.forward` | `0.5` | Lean of a defence point's, the base's and the radar guard's towers the way the enemy comes from: a spot a radius toward the point's approach scores up to this much more than one a radius away from it. 0 turns it off. |
| `choke.radar.min_gain` | `0.02` | Share of the map's weight a radar must add to the coverage. |
| `choke.radar.share` | `0.05` | Share of each AI's metal income its radar budget grows by. The budget starts with one radar and holds two at most. |
| `choke.radar.enemy` | `2.0` | Extra weight of enemy ground (the enemy's half of the map, and enemy territory on ours), × our mex income. |
| `choke.radar.inside` | `0.25` | A builder standing in coverage builds a radar only if at least this share of its view (by weight) is new. |
| `choke.radar.guard` | `"turretmissile"` | Tower queued once a new radar stands, at the best spot near it. `""` turns it off. |
| `choke.radar.guard_radius` | `300` | Elmos from the radar the guard tower may stand. |
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
| `choke_overlay save` | Writes the chokes and point classes to `choke/<map>.json` in the AI's writeable data directory, for comparing maps by hand. Only in games with no human opponents. |

The AI answers in ally chat and in its log, as `CHOKE overlay: ...`. While the overlay is on, it also says when the
threat graph is rebuilt, with the enemy types behind it, for example `threat graph rebuilt: 14 regions, 19 chokes;
enemy types: TANK3(gating 0.60) TKBOT3(promoted to gating 0.35)`.

### Layers

| Layer | What it draws |
|---|---|
| `chokes` | Each choke from end to end of its line: green when defensible, grey when not. Lanes are yellow rectangles. |
| `pockets` | Each pocket's entrances by mode: green at chokes, white on cluster, grey not decided yet. Blue lines link its points to them. Points are red when open, green gated, grey unreached. Magenta marks the anchors of choke points. |
| `lines` | The current line orange, the forward line yellow, the enemy line red, the front line white, the objective magenta. Hold slots are cyan, with their spot. Labels give each frontline choke's rank, wanted power and tower power. |
| `ranks` | Enemy-held chokes and the enemy line's: red when defended, white when undefended, grey when not seen enough to say. Labels say which, and give the defence, the unarmed value behind and the share seen in the last 3 minutes. |
| `traffic` | The busiest cells of the blended traffic field: bigger and redder the busier. |
| `spots` | The last spot search's candidates: the best green, the next 4 cyan, the rest grey. |
| `radar` | The radar planner's candidates blue, and the edge of today's static radar coverage cyan. |
| `regions` | The graph itself: region centres linked to their chokes. On the threat graph, our side is green and the enemy's side red. |

Layers that other modules feed (`lines`, `ranks`, `spots` and `radar`) redraw every 10 s. The others redraw when the
analysis changes.

## How the features decide

### Choke defence (A)

When `MakeDefence` picks towers for a defence point, the pocket behind the point decides where they go:

- **On cluster**, as without `choke.enable`, when the pocket isn't qualified, has at least as many entrances as
  defence points, or its budget has never covered its entrances.
- **At chokes** otherwise. Each cluster keeps its AA and its first other tower, against raiders, leak types and
  anything else the cut doesn't stop. The rest goes to the entrances.

The budget is what the pocket's held points would spend today beyond what they keep, less what they already spent
beyond that on their clusters. So defending at the chokes only moves the same metal, and a pocket that porced its
clusters first doesn't pay twice. The budget covers the entrances once it buys enough main towers to span each
entrance's width at the setback. That only has to happen once.

The budget is split across the entrances by their traffic, with at least one tower each. Each call's towers go to the
entrance with the most room left, and its tower list repeats while that entrance's share has room. An entrance that
is new (under 20 s old) or that enemy artillery covers waits. While one waits and no other has room, the point keeps
today's towers on its cluster. A point within one main-tower range of an entrance stays on its cluster, because there
the cluster and the choke are the same place. So does a point with an enemy sighting at its cluster. Mex builders
count a cluster whose pocket has towers at an entrance as defended, so they don't wait for an escort there.

The mode switches at most every 3 minutes, and a new cut is a new pocket. Built towers never move. While enemy
artillery has been seen in range of a choke in the last 5 minutes, no new towers are queued there, and those not yet
started are called off.

### Scored spots (F)

`score = cover × (1 + choke.spot.height × height) × (1 + choke.spot.enemy × enemy value ÷ own cost) × setback × lean ÷ (1 + threat ÷ own threat) ÷ (1 + builder distance ÷ a quarter of the map's diagonal)`

- *cover* is the traffic of the targets in range with a clear line of fire, halved for each of our towers that
  already covers a target. At a choke the targets are its line and the band in front of it. At a defence point, the
  base or a radar, they're the point's own approach: the cells on near-shortest routes from the enemy to it, by
  walking distance, each weighted by its traffic. Traffic piles up where routes narrow, so the ramp up to a plateau
  outweighs the open ground before it, and the ground below the plateau's cliffs, which leads elsewhere, counts
  for nothing.
- *height* is the height above the targets as a share of `choke.spot.height_cap`, up to 1. So a spot that high
  or higher scores `1 + choke.spot.height` times as much (1.5× by default).
- *enemy value* is the cost of the known enemies a tower there could shoot: in range, in a clear line of fire, and
  of a kind it can hit (fliers need anti-air). Buildings count at their last known position, and mobile units only
  while in radar or sight. As towers skip threatened spots, an armed enemy only counts where the tower outranges
  it, so in practice this finds unarmed targets (mexes, energy, radar, constructors) and outranged ones.
- *setback* is the closeness to `choke.setback` ranges behind the line, for towers at chokes.
- *lean* pulls a defence point's, the base's and the radar guard's towers the way the enemy comes from. It is
  `1 + choke.spot.forward × (1 + ahead) ÷ 2`, where *ahead* (from −1 to 1) is how far the spot sits toward the
  point's approach, as a share of the radius, times how one-sided that approach is. The approach is the targets
  above, walked from the enemy, so on a plateau the lean points down its ramp, not at a cliff edge facing the
  enemy. By default a spot a radius toward an approach from one side scores 1.5×, one a radius away from it 1×,
  and an approach from all sides leans nowhere.
- Hold spots skip the builder term.

A tower skips spots that enemy threat reaches, because a builder only builds out of threat and would move it to the
nearest safe site, often down the slope of the ledge it was meant for. It's built where it was scored, without the
usual random shake, and so are radars and their guards.

Spots stay on the goal's side: a choke's inner region or the defended point's region, any region the enemy only walks
into through it (high ground or a dead end off it), or a wall cell no enemy walks (a ledge). A tower's spot must also
be within build range of ground that one of our builders able to build it can walk to from where it stands, on that
builder's own movement mask; otherwise no builder would take the job, and it would wait 5 minutes, holding up the
porc chain behind it, before being dropped. AA towers aren't scored, they go to the point as before. A point keeps
its spots by tower type, and a new tower takes the first one of its type that no tower stands on or is queued at, so
a lost tower is rebuilt at its old spot.

A spot freezes once its tower is queued. A tower that is cancelled before it starts unfreezes its spot and masks that
ground for 5 minutes. Builders lost or chased off on the way count too, so once a spot fails twice within 10 minutes,
the point's new towers of that type go back to the old placement until 10 minutes after the last failure.
Towers the AI calls off itself (porc mode ending, artillery near a choke) keep their spot and count no failure.

The porc sweep scores every point of the clusters we hold when a round starts, then visits them in that order, one
every 5 s. Our towers are listed once for the whole round.

### Radar planner (B)

One AI per ally team keeps the radar map, every 15 s: the one that builds the analysis. Each cell the enemy can reach
weighs a floor plus its traffic, each scaled to add up to our mex income, plus the income of our clusters' mexes in
it. Enemy ground (the enemy's half of the map, and enemy territory on ours) weighs `choke.radar.enemy` × our mex
income more, spread over its cells, because artillery, bombers and raids only fire at what radar or sight shows.
Coverage is the union of the viewsheds of known static radars, ours and our allies', unfinished ones too. Mobile
radar is ignored. Candidates are the highest buildable cell of each 512-elmo block on our half of the map (outside
enemy territory) or in our territory.

Radar is built by builders as they expand, on every AI of the team, with no walks for it. Once the AI's first
factory stands, a builder that finishes a job builds a radar nearby first when its AI's budget holds one, unless a
NOW job is waiting. Standing in coverage, it only does so where at least `choke.radar.inside` (25%) of the radar's
view, by weight, is new, so radar near the coverage's edge still goes up but no radar doubles another. On open
ground 25% lets radars stand about 830 elmos apart, 0.4 about 1350 and 0.5 about 1700 (radius 2100). It's a NOW
job, and it goes ahead even while metal stalls, because it's cheap and it's what lets the AI see raids coming. The
budget is `choke.radar.share` of the AI's income, starts with one radar, so the first goes up in the opening, and
holds two at most. To hold radar back early on, give the radar's unit entry a `since` in seconds, as for any unit,
for example `"since": 120` in the `staticradar` entry (shipped as 0).

The spot is one of the candidates within 600 elmos or the highest buildable cell there: the one with the most
uncovered weight in its view for 1 + risk, if that is at least `choke.radar.min_gain` of the total weight and it
passes the inside rule. So where radars died lately, a spot must see more to be worth another. It must be
reachable, have no threat on it (to the builder, or to a building there), and stand 500 elmos from other radars
(closer, the job cancels itself). Risk is 1 for each radar lost nearby, halving every 5 minutes. A radar a builder
was sent to build counts as coverage until a radar stands within 500 elmos of it, or for 90 s, so two builders
never go for the same ground. A radar job its builder leaves is dropped after 3 s, so no other builder crosses the
map for it. Once little is left uncovered near a builder, it skips the search: one sum over the cells any spot
near it could see.

Each new radar gets a guard: once it stands, a `choke.radar.guard` tower (turretmissile) is queued as a HIGH job
at the spot scorer's best spot within `choke.radar.guard_radius` (300 elmos). That's the spot that covers the
radar's approaches best, keeps the radar in range and in sight, and favours height. If no spot passes, it goes
beside the radar. No guard is queued where a tower of ours or an ally's already reaches the radar. The guard is
outside the radar budget and waits out metal stalls like any tower, so it never slows the radar down. If the radar
is never built, the guard is dropped with it.

Why not a queued job: a radar queued for any builder lost the pick to nearer work every time, waited out the stall
rule, and was given up. Picking the spot for the builder at hand gets it built at once.

### Holding the line (C), objectives (D) and rankings (E)

- **Frontline.** The current line's chokes, ranked by our income behind each one × its traffic. Each of our points
  behind the line counts at its nearest line choke.
- **Enemy army at a choke.** An enemy mobile seen in the last 3 minutes counts at the line chokes that open into the
  region it stands in: at the one nearest it in full, and at the others by half. One in a region no line choke opens
  into, ours or further out, doesn't count.
- **Hold slots.** A slot sits at the top-ranked choke whose towers are short of 1.2 × the enemy army at it. The first
  idle attack group to ask while that choke is short, and whose power makes up the shortfall, claims it, walks to its
  scored hold spot, and keeps the claim while it keeps asking. Once claimed, the slot keeps its choke until 30 s
  after it was last short, so an enemy army that steps out of its region or dies doesn't walk the group away at once,
  unless another short choke ranks 1.3× higher. Then it moves to the top-ranked short choke, or ends if none is. A
  group that hasn't asked for a minute (it went to fight elsewhere) loses its claim. A slot with nowhere to stand (no
  traffic near the choke) stays free and is tried again after 30 s. A group that runs from a fight gives up its claim
  for 30 s.
- **Objectives.** When the forward line is shorter than the current line, or as long with more income behind it,
  its enemy-held chokes become an objective. That needs the gain to be `choke.objective.gain` × the cost, and our
  attack power to be `choke.objective.power` × their defence. Their defence is what we saw there, over the share of
  their lines we saw in the last 3 minutes (at least a quarter), so chokes we haven't watched count as stronger.
  - The gain is the towers and wanted power of the current-line chokes it would retire, plus 3 minutes of the income
    it brings behind the line.
  - The cost is their defence plus what holding them takes.
  - It lasts until taken, for at most 4 minutes in all (switches included), or until our attack power falls below its
    defence as it is now. A failed one waits `choke.objective.retry` seconds.
- **Rankings.** A choke is enemy-held when its outer side from us is enemy territory, or known enemy towers cover its
  line. Every choke of the enemy line is ranked too, held or not. Its defence is the threat of enemy towers reaching
  the line or our approach to it, plus enemy mobiles within a main-tower range. Its value is the cost of unarmed
  enemy units directly behind it. A ranked choke is *undefended* when no armed enemy is in reach and we saw at least
  a quarter of its line in the last 3 minutes; with less seen, no defence only means we don't know. Objectives (D)
  still take only enemy-held chokes.

## Logs

Lines in the AI's infolog, for tuning:

- `CHOKE graph`, `CHOKE masks` and `CHOKE traffic`: build sizes and times. Traffic layers are built one movement
  class at a time (classes with the same mask share one), so other work on the worker thread never waits long.
- `CHOKE type set`: each change to the enemy's types, with each type's share and role.
- `CHOKE openness`: the value, the band and its inputs.
- `CHOKE pocket [...]`: a pocket's first mode, and every switch.
- `CHOKE point for choke`: where a choke's towers anchor.
- `CHOKE spot`: each spot search, and each failed spot.
- `CHOKE artillery near choke`: towers called off.
- `CHOKE hold` and `CHOKE objective`: slots and objectives starting, moving and ending.
- `RADAR by`: each radar a builder is sent to build, with the coverage before it.

## Known limits

- Traffic sources are the enemy's factory positions, not whole factory regions.
- Towers' sight and fire lines start at the def's sight emit height. Radar's start at half the model's height
  plus it, as the engine emits from the unit's middle; the AI can't read `radarEmitHeight`.
- Fortifying the frontline (C) only calls `MakeDefence` for pockets whose entrances are frontline chokes. A frontline
  choke with no pocket of ours behind it gets no towers of its own.
- New tower spending behind a line that moves forward doesn't pause.
- Objectives (D) only send idle attack groups to the chokes. E's rankings don't steer raids, scouts or artillery yet.
- A tower's cost is counted on the nearest defence point with room for it, as before. A tower at a choke can
  occasionally be counted on a cluster point near that choke.
- Viewsheds are cached while their cell stays a candidate, and aren't made again when terraform changes the ground.
