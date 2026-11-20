# Chokepoints

CircuitAI can learn the shape of each map: which gaps in the terrain are chokes, which mex clusters sit behind which
chokes, where the enemy is likely to walk, and how open the map is. Several features use that to place towers, radar
and the army. Every choke feature is off by default, so with them off the AI places towers, radar and the army as
it did before. The exception is the turret picker's callers (`turret.after_flee`, `turret.near_enemy`, `turret.short_choke`,
`turret.enemy_line`, `turret.artillery`), which are on: they add light turrets for constructors in the field, and read the analysis for it. The `choke_overlay` chat
command draws what the AI found, so you can check a map before you turn anything on.

The analysis is shared by allied AIs and runs on the area worker thread. It is built only when a feature reads it,
when `choke_overlay` asks for it, when the turret picker's first order comes (which waits up to 10 s for it), or,
with `turret.short_choke` or `turret.enemy_line` on, once a constructor could order a turret.

## The analysis

- **Threat graph.** The map is cut into 32-elmo cells that the enemy's *gating* movement types can cross (vehicles
  until an enemy army is seen, then the types its army is made of, below). A cell counts as crossable when at least 3 of its 4 slope-map pixels
  are, so a cliff or a spike's side only one or two pixels thick stays a wall wherever it falls on the grid. Of two
  crossable neighbours that no pair of pixels crosses between (a thin diagonal cliff), one closes. The open
  areas become regions, and the narrow gaps between them become chokes. A choke is *defensible* when it is at most `choke.defensible` main-tower ranges wide. A type
  that doesn't shape the graph is a *leak* type, and each cluster keeps a tower of its own against leaks and anything
  else the cut doesn't stop. Jumpers cross a wall when it is no longer than their jump along a row or a column of
  cells. The graph is built from a few *masks*, one per way of walking: each ground type's, the amphibious bots',
  the hovers'. Spiders and jumpers count for the kbots' mask (a jumper type for its own type walking, a spider type
  for the ground type that climbs the steepest, bots), so the cliffs they cross stay walls and they can't collapse
  the map into one region; their own cells still drive their traffic and reach. A mask's share is the faded army cost
  of the types that walk as it (armed mobiles: constructors and commanders don't count) over the enemy army seen plus
  a 300-metal prior. A mask gates once its share reaches `choke.gate_share`, or once it is the biggest of an army of
  at least 300 metal, and stops when its share falls below half of `choke.gate_share`. So the graph follows the
  army's mix both ways, a lone early unit or a new factory's first units can't flip it, and a mix near the line
  doesn't flip it back and forth. Until a mask first gates (no army seen yet), the graph assumes vehicles, the ground
  types that climb the least, so it starts with the most regions. After that, an army that died or faded under the
  prior keeps the masks it had.
- **Enemy types.** The opening default holds until a factory has been seen for every enemy team, or for 5 minutes. A
  type joins when a factory that builds it or one of its units is seen. It leaves only when every known factory for it
  is dead and none of its units has been seen for 5 minutes.
- **Sides.** Map sides come from start boxes and enemy factories only.
- **Pockets.** Each defence point (`CDefenceMatrix`) gets its entrances from the enemy side: the fewest chokes the
  enemy must pass to walk in. Points with more than `choke.entrances` entrances are *open*. Points that share a cut
  form a *pocket*. A pocket is *qualified* when every entrance is defensible, within `choke.near` ranges of its
  points, and has none of the enemy's held ground (below: their buildings make it, or their side of the map) right
  outside it, so an army passing doesn't move the towers.
- **Held ground.** Who owns each region, as the `sides` overlay tints it: the side with buildings in it. Both
  sides' buildings make it contested when each side's are worth at least a third of the other's metal there (so an
  enemy radar by our base doesn't), and so does the other side's threat there (armed units) when it is at least a
  third (`CChokeMap::CONTEST_SHARE`, 0.33) of the owner's; with no guns of its own, any threat contests it. A region
  with no buildings is nobody's, whatever armies stand in it. Enemy buildings count until seen dead, enemy mobiles
  where they were last seen, for a minute, and none once the AI has seen that spot empty. A region's owner holds it
  once it has owned it for 20 s, so a raid passing through doesn't move the lines. The enemy also holds its side of
  the map where nobody owns it, so an unscouted base still counts as theirs. Each side's ground counts only where it joins that side's main ground (its regions on its side
  of the map) without crossing the other's, so an army camped deep in the other's ground is an island, not a new
  front. Held ground is the one ground model: the lines, pocket qualification and the radar planner go by it.
- **Lines.** They go round held ground. The *current line* (our line) is every choke out of our ground into a
  region the enemy can walk to from theirs without crossing ours; a choke into ground only we can reach isn't on it.
  The *enemy line* is its mirror: every choke out of the enemy's ground into a region we can walk to from ours
  without crossing theirs. Where our ground meets theirs a choke is on both; contested and nobody's ground between
  them keeps them apart. The *front line* is the chokes where our ground meets theirs, neither side contested;
  only the overlay draws it.
- **Traffic field.** On 64-elmo cells (crossable by the same rule over their four 32-elmo cells), one layer per enemy
  movement type: where its units are likely to walk from the
  enemy's factories (or start boxes) to our clusters and base. The layers are blended by the enemy's army mix, at most
  every 30 s and only when a share moves by more than 0.1.
- **Openness.** Half the share of mex income that is open from the enemy side, plus half the width of the narrowest
  separation between the sides over 4 main-tower ranges. Below `choke.openness.bands[0]` the map is *chokepointed*,
  above `bands[1]` it is *open*, and in between *semi-open*. The band only changes `bands[2]` past a boundary.
- **Lanes.** A corridor `choke.lane` wide through every choke narrower than 3 lanes. The turret picker and scored
  spots never put a tower on one; with `choke.enable` it is also kept free of every building, except mexes, so our
  own walls never close it.

The graph is rebuilt when the gating masks change, and when the terrain changes what they can cross. Craters and the
ground levelled under buildings change it through every fight, so the terrain alone rebuilds it at most every 5 minutes,
unless the change opens a new crossing: newly crossable cells joining two of the graph's regions away from their border
(a wall blown through, a terraformed ramp), which rebuilds it at once. A crater widening a choke, or on a cut across
open ground, touches both regions where they already meet, so it waits (the masks are made again at most every 30 s).
Choke ids carry over either way. A new choke waits 20 s before anything acts on it.

## Features and their flags

| Key | Default | What it turns on |
|---|---|---|
| `choke.enable` | `false` | **Choke defence (A).** A qualified pocket with fewer entrances than defence points defends at its entrances. Each cluster keeps its AA and its first tower, and the rest of the pocket's towers go to the entrances. Defence points are also split by region, and lanes are kept free of buildings. |
| `choke.spot.enable` | `false` | **Scored spots (F).** Every tower from `MakeDefence`, the base towers, the armed hub towers of build chains and the hold spots get the best spot within reach, scored on the turret picker's value model: covered traffic and enemies in metal, height, view, threat and builder distance. The porc sweep visits the points with the most uncovered traffic × income first. |
| `choke.radar.enable` | `false` | **Radar planner (B).** A builder between jobs builds a radar nearby first when its budget holds one, if it adds at least `choke.radar.min_gain` of the map's weight (and, standing in coverage, `choke.radar.inside` of its view is new). It takes over `MakeDefence`'s radar. |
| `quota.hold_choke` | `false` | **Holding the current line (C).** The idle attack group holds at the most short choke of our line: short is one our ground-hitting towers don't cover all of, or whose towers are short of the enemy army in the region outside it, and the most short has the biggest shortfall, else the least cover. That's 1 choke, or 2 on chokepointed maps, and none on open maps. With `choke.enable`, the top-ranked short choke's pocket is fortified. |
| `choke.expand` | `false` | **Expansion bonus.** Mex expansion (`economy.mex_front`) scores clusters behind a cut with towers ×0.7, and behind a qualified cut without towers ×0.85. Lower is better. |
| `turret.after_flee` | `true` | **Turret picker (G), after a flee.** A constructor whose flee from nearby enemies ends clear or covered orders a light turret where the flee started. |
| `turret.near_enemy` | `true` | **Turret picker (G), near an enemy building.** A mobile ground constructor within `turret.enemy_reach` of a known enemy building, with no armed ally within `turret.ally_radius`, orders a light turret around itself. |
| `turret.short_choke` | `true` | **Turret picker (G), at a short choke.** A mobile ground constructor within `turret.short_choke_reach` of a short choke of our current line (as C judges it, below) orders one centred on the choke's gap. Only defensible chokes, settled and with no enemy artillery on them, as for C. |
| `turret.enemy_line` | `true` | **Turret picker (G), near the enemy line.** A mobile ground constructor within `turret.enemy_line_reach` of a choke of the enemy line orders one centred on the choke's gap. The same chokes as `turret.short_choke`. |
| `turret.artillery` | `true` | **Turret picker (G), by our artillery.** A mobile ground constructor within `turret.artillery_reach` of our artillery shooting a target in its range, or of an engaged anti-heavy squad with a unit in range of its target (not mines or aircraft), with no ground-hitting turret within `turret.defended_radius` of it, orders one centred on the artillery (an anti-heavy squad: its leader). Anti-heavy squads and anti-heavy artillery count only against a building. |

The rankings of enemy-held chokes and the enemy line's (E) feed only the overlay. They are computed only while the overlay is on.

## Tuning keys

All in the top-level `"choke"` block of `behaviour.json`. Sizes are in main-tower ranges, using the first
`porcupine.land` defender (or `porcupine.default`), unless noted.

| Key | Default | Meaning |
|---|---|---|
| `choke.region.min_area` | `0.5` | Smallest region, in main-tower ranges squared. A smaller one merges into a neighbour. |
| `choke.region.merge` | `0.7` | Two regions merge when the choke's altitude reaches this share of the smaller region's highest. Near 1, open ground splits wherever the distance to the nearest wall dips (on a mirrored map, down the mirror line), into chokes as wide as the regions. |
| `choke.region.min_count` | `20` | Fewest regions wanted. A build with fewer runs once more with `merge` at 1, kept if it has more regions. That build still merges across any choke wider than `choke.defensible`, so it only finds narrow chokes the first build merged away. With still too few (open ground, which has no choke to split at), seeds go on a grid of the map, centred so a mirrored map gets mirrored pieces, and the grid gets a column or a row finer until there are this many. Each big region's cells go to its nearest seed (walking), and a piece under the floor (`min_share`) goes to its neighbours. The grid depends only on the map's size, so a crater moves no seed and ids carry over rebuilds. A line between two pieces of one region is a cut: a choke as wide as the ground it crosses, which counts like any other (held ground, the lines, holds, the turret picker's C). The graph log line and the overlay say when it rebuilt or cut, and how much. 0 turns it off. |
| `choke.region.min_share` | `0.5` | Neither step of `min_count` makes a region under this share of the ground per `min_count` region: the rebuild and the cut use it as `min_area` where it is bigger. A shard between two lines is of no use as a region, so they come out of a size (on a 12x8 map with craters, a half to one and a half times the average). Regions of the first build keep `min_area`. |
| `choke.defensible` | `2.0` | Widest choke that towers try to close. |
| `choke.entrances` | `3` | Most entrances before a cluster counts as open. |
| `choke.near` | `2.5` | Furthest useful entrance from a defence point. |
| `choke.setback` | `[0.6, 0.8]` | Ideal tower distance behind a choke's line, in the tower's own weapon ranges. |
| `choke.lane` | `96` | Elmos. Width of the corridor through a narrow choke: no picker or scored tower on it, and with `choke.enable` no building but a mex. |
| `choke.gate_share` | `0.25` | Share of the enemy army seen (plus a 300-metal prior) a graph mask's cost must reach to start gating; it stops below half of this. Spiders and jumpers switch the turret picker's T off and on at the same shares. |
| `choke.traffic.spread` | `0.15` | Width of the traffic corridors, as a share of the route's length. |
| `choke.openness.bands` | `[0.35, 0.65, 0.05]` | Chokepointed below, open above, and the margin around them. |
| `choke.spot.radius` | `300` | Elmos from a defence point its towers may stand. |
| `choke.spot.height` | `0.5` | The most a spot's height adds to its score: 0.5 is up to +50%, 1 up to double. |
| `choke.spot.height_cap` | `150` | Elmos above the ground it covers at which a spot earns all of `choke.spot.height`. |
| `choke.spot.enemy` | `1.0` | Weight of E, the cost of the known enemies a tower can shoot from a spot, added to the covered traffic in metal. 0 turns it off. |
| `choke.spot.view` | `1.0` | A spot scores up to 1 + this × its score by V, the share of its range circle in a clear line of fire, as `turret.view` does for the picker. 0 turns it off. |
| `choke.spot.forward` | `0.5` | Lean of a defence point's, the base's and the radar guard's towers the way the enemy comes from: a spot a radius toward the point's approach scores up to this much more than one a radius away from it. 0 turns it off. |
| `choke.radar.min_gain` | `0.02` | Share of the map's weight a radar must add to the coverage. |
| `choke.radar.share` | `0.05` | Share of each AI's metal income its radar budget grows by. The budget starts with one radar and holds two at most. |
| `choke.radar.enemy` | `2.0` | Extra weight of enemy ground (the enemy's half of the map, and the enemy's held ground on ours), × our mex income. |
| `choke.radar.inside` | `0.25` | A builder standing in coverage builds a radar only if at least this share of its view (by weight) is new. |
| `choke.radar.guard` | `"turretmissile"` | Tower queued once a new radar stands, at the best spot near it. `""` turns it off. |
| `choke.radar.guard_radius` | `300` | Elmos from the radar the guard tower may stand. |
| `choke.radar.heavy` | `"staticheavyradar"` | Long-range radar the planner builds when one sees a lot that is still dark, offered to a builder (not a commander) before a small radar. It stands within 1500 elmos of the builder; radars lost within 1500 elmos of a spot count against it. One job per AI at a time, and none for 90 s after the ally team lost one; then it is rebuilt (its `behaviour.json` limit counts living ones). Its guard is the build_chain `radar` hub of its type. While the planner is on, the porc lists leave it out. A failed search logs its best score once a minute. `""` turns it off. |
| `choke.radar.heavy_income` | `40` | Metal/s of the ally team's income (ZK's overdrive totals: mex, overdrive, misc) before one is considered. |
| `choke.radar.heavy_min_gain` | `0.15` | Share of the map's weight a heavy must add to the coverage. |
| `choke.radar.heavy_depth` | `1500` | Elmos a heavy must stand from the nearest enemy ground cell. |
| `choke.radar.chain` | `true` | A build_chain hub `radar` step goes to the highest free spot within 300 elmos of its offset, or is skipped where its view would add less than `min_gain`. With `"upgrade": true` on the step it builds the heavy there instead, when that AI's heavy gates and score pass. |

The turret picker has its own top-level `"turret"` block, so the two pickers are tuned apart. Sizes are in elmos.

| Key | Default | Meaning |
|---|---|---|
| `turret.radius` | `600` | Elmos from the order's centre a turret may stand. |
| `turret.min_value` | `1.0` | A spot's value (T + A + E + C, × height) must reach this × the turret's cost. |
| `turret.traffic` | `1.0` | Weight of T, the enemy traffic in range. T is left out while spiders and jumpers are `choke.gate_share` of the enemy army (until they fall below half of that): then a turret pays for itself in A, E and C alone. |
| `turret.horizon` | `120` | Seconds of threatened income T counts, and of an enemy mex's income in E. |
| `turret.ally` | `1.0` | Weight of A, our and allied unarmed buildings in range. |
| `turret.enemy` | `1.0` | Weight of E, the enemies a turret could shoot. |
| `turret.front` | `1.0` | The spot with the most enemy building influence, out of threat, ranks up to 1 + this × its value. 0 turns the lean off. |
| `turret.view` | `1.0` | A spot ranks up to 1 + this × its value by *V*, the share of its range circle's ground in a clear line of fire from it. A cliff top over the ground below scores high and its foot low; flat open ground scores 1. Like the lean, it ranks spots and never decides whether one is worth its cost. 0 turns it off. |
| `turret.height` | `0.5` | The most a spot's height adds to its value. |
| `turret.choke` | `100` | Metal a choke of our current line or the enemy line in range adds (C). |
| `turret.choke_econ` | `1.0` | A choke of our current line adds this × our economy to C: the cost of our and allied energy, pylons and factories on our held ground, split among the current line chokes by the traffic through each (evenly with no traffic). Mexes are left out; they steer the traffic. 0 = off. |
| `turret.choke_enemy` | `400` | Metal a choke of the enemy line adds to C, on top of `turret.choke` (and of the economy share for a choke on both lines). |
| `turret.choke_cap` | `1000` | The most C one choke carries, before depth and halving, so the busiest pass doesn't soak up every turret. 0 = no cap. |
| `turret.choke_depth` | `1.0` | C × (1 − this × the choke's distance ÷ range). At 1, a choke is worth all of its C right under the turret and nothing at the edge of its range; at 0, the same anywhere in range. A tower covering the choke halves its C for the next by as much: fully when it stands on it, by a quarter when the choke is halfway out at 1. |
| `turret.not_defence` | `["staticheavyarty", "zenith", "mahlazer", "starlight_satellite", "raveparty", "staticnuke", "staticantinuke"]` | Armed statics that defend nothing in the field: Big Bertha, Zenith, Starlight and its Glint, Disco Rave Party, Trinity and the Antinuke. They never cover a target, and the callers don't count them as a defence or as allied power. Claw mines and missiles are left out the same way, by their `mobilebuilding` mark. |
| `turret.after_flee_since` | `0` | Seconds of game time before the after-flee caller starts. |
| `turret.defended_radius` | `600` | A defence this near where a flee started means no after-flee turret; a ground-hitting one (ours or an ally's, finished, being built or queued) this near our artillery means no artillery turret. |
| `turret.light` | `["turretlaser", "turretmissile"]` | The light turrets both callers may build: Lotus and Picket. |
| `turret.tiers` | `[{"mex": 0.4, "defs": ["turretriot", "turretgauss"]}, {"mex": 0.55, "defs": ["turretheavylaser"]}]` | Heavier turrets, by map control. A tier is unlocked once our ally team's finished mexes hold `mex` of the map's metal spots: Stardust and Gauss at 40%, HLT at 55%. An order tries the heaviest unlocked tier first and builds from it if any of its defs has a spot worth its cost; otherwise it drops a tier, down to `turret.light`. Within a tier the most value per metal wins. |
| `turret.enemy_reach` | `700` | Distance to a known enemy building that starts the near-enemy caller. |
| `turret.short_choke_reach` | `600` | Distance from a short choke's line that starts the short-choke caller. |
| `turret.enemy_line_reach` | `800` | Distance from an enemy line choke's line that starts the enemy-line caller. |
| `turret.enemy_line_budget` | `0.5` | An order at a choke of the enemy line (taken by either choke caller) needs only this × the dearest def's cost in the turret account; 0 to 1, 0 = never refused. The turret is still charged its full cost, which may take the account below zero, ahead of every other spender. |
| `turret.artillery_reach` | `600` | Distance to our attacking artillery that starts the artillery caller. |
| `turret.ally_radius` | `500` | Distance within which any armed ally counts as allied power, as the flee counts cover. |
| `turret.cooldown` | `30` | Seconds between one constructor's orders, whatever the result. |
| `turret.share` | — | Removed: the picker spends from the turret account, `porcupine.budget` in build_chain.json. `choke_overlay set turret.share` sets the account's share. |

## The `choke_overlay` command

Type it in chat. Every allied CircuitAI gets the line, but the ally team handles it once. Commands from enemy
players are ignored. A spectator may use it only in games with no human opponents.

| Command | What it does |
|---|---|
| `choke_overlay` or `choke_overlay toggle` | Shows or hides the overlay. The first time, it builds the analysis if no feature asked for it. The answer gives the map's openness and band. |
| `choke_overlay next` | Shows the next layer. |
| `choke_overlay <layer>` | Shows that layer: `chokes`, `pockets`, `lines`, `ranks`, `traffic`, `spots`, `radar`, `regions`, `sides` or `turrets`. |
| `choke_overlay <movement class>` | Shows one movement class's own graph on the `chokes` and `regions` layers. Use a unit def name (`cloakraid`) or a class name. An unknown name lists the classes. |
| `choke_overlay all` | Back to the threat graph. |
| `choke_overlay labels` | Turns the text labels on or off, and the `sides` layer's info boxes. Labels are map markers, which human allies see too, so they start off when the AI has human allies. |
| `choke_overlay set <key> <value>` | Changes a tuning key or flag and rebuilds. Keys are named as in the config without `choke.`, for example `region.min_area 0.4`, `setback 0.5 0.7`, `spot.enable true`, and `hold_choke` for `quota.hold_choke`. `openness.bands` can't be set this way. The `turret.*` keys take their full name (`turret.front 2`), change only the AI that answers, and need no rebuild; `turret.light`, `turret.tiers` and `turret.not_defence` can't be set. Only in games with no human opponents. |
| `choke_overlay turret <x> <z> [def]` | Our nearest free constructor orders a turret at (x, z), with no cooldown or budget, at HIGH priority. With no def the picker chooses from the porcupine list (AA left out). Shows the `turrets` layer. Only in games with no human opponents. |
| `choke_overlay save` | Writes the chokes and point classes to `choke/<map>.json` in the AI's writeable data directory, for comparing maps by hand. Only in games with no human opponents. |

The AI answers in ally chat and in its log, as `CHOKE overlay: ...`. While the overlay is on, it also says when the
threat graph is rebuilt, with the enemy types behind it, for example `threat graph rebuilt: 14 regions, 19 chokes;
enemy types: TANK3(gating 0.40) KBOT2(gating 0.45) TKBOT3(as KBOT2 0.35)`: each type's graph mask and its share
(a type walking as another shows its own share, which counts for that mask), `leak` for a mask that doesn't gate.
Before an army is seen, the vehicles show as `(gating, assumed)` once the opening default has left the set.

### Layers

| Layer | What it draws |
|---|---|
| `chokes` | Each choke from end to end of its line: green when defensible, grey when not. A cut across open ground (`choke.region.min_count`) is labelled `cut`. Lanes are yellow rectangles. |
| `pockets` | Each pocket's entrances by mode: green at chokes, white on cluster, grey not decided yet. Blue lines link its points to them. Points are red when open, green gated, grey unreached. Magenta marks the anchors of choke points. |
| `lines` | The current line orange, the enemy line red, the front line white. Hold slots are cyan, with their spot. Labels give each frontline choke's rank, wanted power and tower power. |
| `ranks` | Enemy-held chokes and the enemy line's: red when defended, white when undefended, grey when not seen enough to say. Labels say which, and give the defence, the unarmed value behind and the share seen in the last minute. |
| `traffic` | The busiest cells of the blended traffic field: bigger and redder the busier. |
| `spots` | The last spot search's candidates: the best green, the next 4 cyan, the rest grey. |
| `radar` | The radar planner's candidates blue, and the edge of today's static radar coverage cyan. |
| `regions` | The graph itself: region centres linked to their chokes. On the threat graph, our side is green and the enemy's side red. |
| `sides` | Who controls each region and choke, and the lines by tower cover and shortfall, on the threat graph whatever class is shown. Drawn by a widget (below): regions tinted blue where we control them and red where the enemy does, both stronger where the lines take the region as that side's held ground, striped blue and red where contested, and clear where nobody is; thin region borders. Each choke is a gate across its gap, wall to wall. Its core is its owner: blue or red where that side's towers cover all of its gap, dashed where that is contested, thin grey where no side's do. Posts at its ends mean towers reach part of it: blue ours, red the enemy's, half and half both; no posts, none. A casing round the core marks a choke on a line: green our current line where our ground-hitting towers cover all of its gap and outgun the enemy army outside it, orange where it's short (either fails); the enemy line dark red where the enemy's ground-hitting towers cover all of its gap (defended), pink where they don't (undefended); a white rim round the casing on the front line. The lines go round held ground, so they follow the tint once it has held 20 s and joins its side's main ground; a region's info box says when the lines still take it otherwise, or that it is cut off. Each enemy unit the AI counts is marked where it believes it is: a circle for an armed unit, a square for a building (red armed, pale unarmed), filled while in its sight or radar and an outline where it was last seen, so a spectator with full view can see which enemies the AI knows of. When the AI's LOS cheating option is on, the bottom line says so in red (the infolog says `LOS cheating: on` at start). A legend is printed bottom left. With labels on, info boxes give the numbers behind each owner: hostile and allied threat (total, mobile, static) and eco, the ranked chokes' defence and seen share, and our line's wanted and tower power. The cursor's region and choke get boxes beside the cursor; zoomed in, owned chokes and regions near the camera get one beside them, as many as fit. Without the widget it shows the `lines` layer. |
| `turrets` | The last turret order: its radius ring and centre white, the candidates that passed the hard rules grey, the best green, the next 4 cyan, and those outside our buildings' influence dark red. Tested spots are labelled with their value ÷ cost and F, the chosen one with its def and the four terms. |

Layers that other modules feed (`lines`, `ranks`, `spots`, `radar` and `turrets`) redraw every 10 s, and `sides` every
4 s. The others redraw when the analysis changes.

### The sides layer and its widget

Buildings decide who owns a region, and towers who defends a choke; armies only contest them. Each side has three
numbers at a region or a choke: mobile threat and static threat (its armed units', power for ours and threat for the
enemy's, as the influence map counts them) and eco (its unarmed buildings' metal cost). A building is armed only as a
finished tower the turret picker counts as defence (`CTurretPicker::IsDefence`), so a nanoframe or a Big Bertha (and
the rest of `turret.not_defence`) is an unarmed building. The units are ours where they are now, enemy buildings
until seen dead, and armed enemy mobiles where they were seen in the last minute, aircraft, constructors and mobile
buildings (Claw mines, missiles) aside.
Enemies whose spot the AI has seen empty since (hidden, as the rest of the AI skips them) don't count. At
a region they are the units in it. At a choke they are the guns that reach its line, as `CFrontline` counts towers:
buildings whose range reaches it, mobiles within their range or a main-tower range of it (whichever is longer, as they
walk), and unarmed buildings within a main-tower range of it.

- **A region** is the side's with buildings in it, armed or not. Both sides' buildings make it contested, each
  side's worth at least a third of the other's metal there, and so does the other side's threat there when it is at least a third of the owner's (any threat, if the owner has no guns).
  With no buildings it is nobody's.
- **A choke** is defended by a side whose towers that hit ground cover all of its line, wall to wall: every cell of
  it within one of their ranges. Both sides covering it, the other side's towers reaching any of it, or its mobiles
  there at a third of the defender's threat or more make it contested. Where no side covers all of it, it is
  nobody's, whatever towers reach part of it (its posts show those). A choke's info box gives the share of its line
  each side's towers cover.

A region's owner is what held ground settles on; nothing but the overlay reads a choke's.

The layer needs the `dbg_circuit_sides.lua` widget in `util/LuaUI/Widgets/`. Copy it into the Zero-K data folder's
`LuaUI/Widgets/` on the machine that runs the AI (local widgets are on by default). The AI finds it through
`Lua_callUI` each time it draws, so it works if the widget is turned on mid-game, and the `lines` layer stands in
while it's off. In games with human opponents the AI never calls it, as the machine running the AI may be theirs.
When several AI ally teams send (spectating AI vs AI) the widget keeps showing one, the local player's or else the
lowest ally team id, until it clears or goes 30 s without an update, and names it bottom left. The messages are
described in `ChokeSides.cpp`.

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

Scored spots use the turret picker's value model (see "Turret picker (G)"), with each goal's own targets and terms:

`score = (T + E + A + C) × (1 + choke.spot.height × height) × (1 + turret.front × F) × (1 + choke.spot.view × V) × setback × lean ÷ (1 + threat ÷ own threat) ÷ (1 + builder distance ÷ a quarter of the map's diagonal)`

There is no cost threshold: a goal's tower goes on its best spot.

- *T* is the traffic of the targets in range with a clear line of fire, in metal as the picker counts it (× `turret.horizon`, once per route; `turret.traffic` is the picker's alone). Each target is halved for each tower that already covers it, defends (not a Claw mine, a missile or `turret.not_defence`) and hits it. At a choke the targets are its line and the band in front of it. At a defence point, the
  base or a radar, they're the point's own approach: the cells on near-shortest routes from the enemy to it, by
  walking distance, each weighted by its traffic. Traffic piles up where routes narrow, so the ramp up to a plateau
  outweighs the open ground before it, and the ground below the plateau's cliffs, which leads elsewhere, counts
  for nothing.
- *height* is the height above the mean of the targets it counts, as a share of `choke.spot.height_cap`, up to 1.
  So a spot that high or higher scores `1 + choke.spot.height` times as much (1.5× by default).
- *V* is the share of the range circle's ground in a clear line of fire from the spot, as the picker's. A spot on a
  cliff edge sees the plateau and the ground below, and its foot only the open side.
- *E* is `choke.spot.enemy` × the picker's enemy targets that a tower there could shoot, in range and in a clear
  line of fire: known enemy buildings seen in the last 3 minutes (no fakes, Claw mines or missiles), at cost, an
  enemy mex at its income × `turret.horizon`. They are halved for covering towers as T is. Mobile units don't count:
  their routes are T already, and a spot frozen once its tower is queued mustn't follow a passing raid. As towers
  skip threatened spots, an armed building only counts where the tower outranges it.
- *A*, *C* and *F* are the picker's, as one of its orders counts them (`CTurretPicker::GetSpotShare`), so the
  `turret.ally`, `turret.choke*` and `turret.front` keys tune them here too. *A* is our and allies' unarmed
  buildings in range with a clear line of fire, at cost × `turret.ally`, our economy only above what *C* carries of
  it. *C* is the best choke of our line or the enemy line with its middle in range, as the picker's *C* (by its depth
  in range, `turret.choke_depth`). Both are halved for covering towers as T is. *F* is the known enemy
  buildings' influence at the spot over the highest among the candidates, so towers lean toward the enemy's
  buildings on top of *lean*.
- *setback* is the closeness to `choke.setback` ranges behind the line, for towers at chokes.
- *lean* pulls a defence point's, the base's and the radar guard's towers the way the enemy comes from. It is
  `1 + choke.spot.forward × (1 + ahead) ÷ 2`, where *ahead* (from −1 to 1) is how far the spot sits toward the
  point's approach, as a share of the radius, times how one-sided that approach is. The approach is the targets
  above, walked from the enemy, so on a plateau the lean points down its ramp, not at a cliff edge facing the
  enemy. By default a spot a radius toward an approach from one side scores 1.5×, one a radius away from it 1×,
  and an approach from all sides leans nowhere.
- Hold spots skip the builder term, E, A, C and F.

A tower's spot is moved onto its build site, buildable facing south and open on the blocking map (lanes, queued
jobs), as the picker's are, so it never lands on another job's footprint. A tower skips spots that enemy threat reaches, because a builder only builds out of threat and would move it to the
nearest safe site, often down the slope of the ledge it was meant for. It's built where it was scored, without the
usual random shake, and so are radars and their guards.

Spots stay on the goal's side: a choke's inner region or the defended point's region, any region the enemy only walks
into through it (high ground or a dead end off it), or a wall cell no enemy walks (a ledge). A tower's spot must also
be within build range of ground that one of our builders able to build it can walk to from where it stands, on that
builder's own movement mask; otherwise no builder would take the job, and it would wait 5 minutes, holding up the
porc chain behind it, before being dropped. AA towers aren't scored, they go to the point as before. A build chain's
`hub` tower that shoots the ground is scored the same way, guarding the building that started the chain (within
`choke.spot.radius`), and its site goes on the blocking map as soon as it is queued, as a turret picker job's does, so
the hub's next tower takes another spot; it keeps its configured offset when no spot passes. Its shields, storages and
AA keep their offsets. A tower queued in any chain counts as covering its approach, also one waiting behind a hub's
radar. A point keeps
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
it. Enemy ground (the enemy's half of the map, and the enemy's held ground on ours) weighs `choke.radar.enemy` × our mex
income more, spread over its cells, because artillery, bombers and raids only fire at what radar or sight shows.
Coverage is the union of the viewsheds of known static radars, ours and our allies', unfinished ones too. Mobile
radar is ignored. Candidates are the highest buildable cell of each 512-elmo block on our half of the map (off the
enemy's held ground) or on our held ground.

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

### Turret picker (G)

The turret picker finds the best spot for a turret within `turret.radius` of any centre, with no defence point,
choke or cluster behind it, and the constructor builds it on that exact spot. The choke spot scorer (F) guards one
thing; this one weighs everything a turret there would cover, each term in metal so it compares with the turret's
cost:

`base = (T + A + E + C) × (1 + turret.height × height)`, `value = base × (1 + turret.front × F) × (1 + turret.view × V) ÷ (1 + walk ÷ 1000)`

- *T*, traffic: the traffic field carries each destination's income across every cross-section of its enemy routes.
  The heaviest 64 cells within reach are summed over those in range and divided by the cell lengths the range spans
  (2 × range ÷ 64), which gives the income whose routes pass under the turret, in metal/s; × `turret.horizon`. None while spiders and
  jumpers are `choke.gate_share` of the enemy army (until they fall below half of that): the threat graph walks them
  as kbots, and their routes over cliffs are left out.
- *A*, our and allied unarmed buildings in range (mexes, energy, radar, factories, storage, nanoframes too), at cost.
  Energy, pylons and factories in our territory count only above what the spot's C already carries of our economy.
- *E*, known enemy buildings in range seen in radar or sight in the last 3 minutes (so the ghost of one that died
  out of sight drops out), at cost; an enemy mex counts its income × `turret.horizon`, which is what killing it
  takes from them. Mobiles don't count: they move on, and their routes are in T already.
- *C*, `turret.choke` when a choke of our current line or the enemy line has its middle in range. A current line
  choke adds `turret.choke_econ` × our economy (the cost of our and allied energy, pylons and factories on our
  held ground), split among the current line's chokes by the traffic field across each one's line: the enemy's routes
  to what we hold show which pass shuts it in, a base one region back or behind two passes too. An enemy line choke
  adds `turret.choke_enemy`; a choke on both lines adds both. The whole is capped at `turret.choke_cap` and × (1 −
  `turret.choke_depth` × its distance ÷ range). A choke gets none until it has settled after a graph rebuild, or while
  enemy artillery is on it, as choke defence waits; its part of the economy isn't moved onto the others. A spot that
  counts a choke's economy doesn't count the same economy buildings in A again: A keeps only what is left over. One choke's worth at most. Every other target counts in full anywhere in range, so without the depth a
  turret only has to reach the choke with its outer range, and walk, threat, view and the targets behind it hold it
  back there.
- Each target counts only if the turret hits it (ground, and under deep water for one that hits there), in range
  and in a clear line of fire (high-arc turrets skip that). It is halved for each tower of ours or an ally's,
  queued ones included, that already covers it and hits it, so an AA tower doesn't cover a mex. Claw mines,
  missiles and `turret.not_defence` never cover.
- *height*: above the mean height of the targets it counts, up to 150 elmos.
- *F*, the front: each known enemy building, armed or not, fakes, Claw mines and missiles left out, adds
  2 × (1 − d ÷ 2048) (the influence map's enemy territory), over the highest among the candidates. The spot reaching furthest toward the enemy while
  out of threat ranks up to (1 + `turret.front`) × its value. *walk* is the ordered constructor's distance.
- *V*, the view: the share of the range circle's ground, by area, in a clear line of fire from the spot, sampled
  along 24 bearings every 16 elmos (high-arc turrets: 1). A spot on a cliff edge sees the plateau and the ground
  below; its foot sees only the open side.

Only the base is tested against the cost: a spot is worth building when its base is at least `turret.min_value` ×
the turret's cost, so the lean, the view and the walk only rank spots that already pay. The def with the best value per metal
wins, so the type follows the enemy buildings in range and the choke through E and C.

A spot is skipped when it is outside the radius; more than `defence.infl_rad` (2048 elmos) from every building of
ours or an ally's (constructors left out, so the ordering constructor can't make its own ground pass, and Claw
mines and missiles, so a minefield doesn't stretch our ground); not buildable facing south or closed on the
blocking map (lanes and queued jobs, picker jobs included, are on it); on a lane; in the constructor's threat; out of its
reach; not safe for a builder whose flee is on (`IsCommJobSafe`); or masked in the last 5 minutes, by a build the
engine refused there or a picker job dropped there. Candidates are the 64-elmo cell centres within the radius, the
same for every def, snapped to the build grid, tried best upper bound first; the best 5 are tried again 16 elmos to
each corner.

The job is a DEFENCE job at the spot with no shake, its footprint on the blocking map from the start. It is dropped
30 s after its last builder leaves, and on arrival if threat (the builder's) has reached the spot or its base has
fallen below 0.75 × `turret.min_value` × cost (the enemy building it was for died, say). The margin keeps a spot
that only just paid from flipping on one new tower. A dropped job masks its spot, as a build the engine refuses
does, so the next order looks elsewhere. Picker and cornered towers
(`MakeCommDefence`) stay out of the defence points' counts, so a forward picker turret dying doesn't make the porc
sweep rebuild a home tower.

**Callers.** One update a second runs them, each turret from `turret.light`, or a heavier tier of `turret.tiers`
once our share of the map's mexes unlocks it, at NORMAL priority:

1. After a flee that ends clear or covered, centred where the flee started, with only the light defs that hit what
   it fled from (after gunships, only Picket). Not before `turret.after_flee_since` seconds of game time, and not
   when a defence that hits what it fled from (ours or an ally's, finished, being built or queued; Claw mines,
   missiles and `turret.not_defence` left out) stands within `turret.defended_radius` of where the flee started.
   Only builders whose flee is on: the commander by default, other constructors with `retreat.builder_flee`. A
   cornered flee keeps the cornered defence, a hurt one goes for repair.
2. A mobile ground constructor with a known enemy building seen in the last 3 minutes (not a Claw mine or a
   missile) within `turret.enemy_reach`, and no armed ally (a mobile or a finished tower, its own power, Claw mines,
   missiles and `turret.not_defence` left out) within `turret.ally_radius`, centred on itself. A finished picker
   turret counts as allied power, so it stops once its turret stands.
3. A mobile ground constructor within `turret.short_choke_reach` of the line of a short choke of our current line
   (our towers that hit ground don't cover all of its gap, or are short of 1.2 × the enemy army outside it: C's
   reading, every 10 s), centred on the choke's gap.
4. A mobile ground constructor within `turret.enemy_line_reach` of the line of a choke of the enemy line, centred
   on the choke's gap.

   3 and 4 take only defensible chokes (`choke.defensible`: a gap towers can close), settled for 20 s since a
   rebuild and with no enemy artillery seen in range of them in the last 5 minutes, the chokes C counts; the nearest
   in reach.
5. A mobile ground constructor within `turret.artillery_reach` of our artillery (its artillery task, anti-heavy
   artillery included) that is shooting a target in its range and isn't fleeing, or of an anti-heavy squad that is
   engaged with a unit in range of its target (not mines or aircraft), with no turret that hits ground (ours or an
   ally's, finished, being built or queued; Claw mines, missiles and `turret.not_defence` left out) within
   `turret.defended_radius` of it, centred on the artillery or the squad's leader. Anti-heavy squads and anti-heavy
   artillery count only while their target is a building: a fight with a mobile heavy is over before a turret is up.

A constructor takes the first of 2 to 5 that holds for it; one that fails 2 only for allied power around may still
take 3 to 5. 3 and 4 wait for the choke analysis, which they ask for. In 3 to 5, centred away from the
constructor, it helps only a job within `turret.radius` of itself too.

Gates, in order: the caller's switch; the constructor isn't player-led, retreating, fighting, morphing, on a
defence job or on a HIGH or NOW one; its `turret.cooldown`; metal isn't stalling; the trigger; a light def it can
build whose water depths suit the centre (after a flee: that hits what it fled from). Then, if one of our turrets
of a kind that hits it is being built, or a picker job is queued, within `turret.radius` of the centre, whoever
queued it, the constructor helps build it instead (no budget, but the cooldown starts). Else, after a flee, the
defence check; the choke analysis, asked for on the first order, which waits for it up to 10 s; the budget; and
one order a second for the AI, so a group arriving together orders once and the rest help. An order starts the
cooldown whatever the result.

The budget is the AI's turret account (`porcupine.budget` in build_chain.json), which porc-mode towers spend from
too. It is paid in as the metal comes: `share` x the average income x min(ms_pull, 1), held up to `bank_sec` seconds
of that (or the dearest tower a waiting spender asked for, whichever is more). An order needs the dearest light def's
cost in it and takes the turret's cost out. Cornered, base timetable, radar guard, mex escort and hub towers are
charged too, but never wait, so they can take it below zero; preventive towers are free. A finished turret of ours
that dies to a known mobile killer that doesn't outrange it pays `loss_refund` (2) x its cost back, past the cap if
need be. So at 0.1, ms_pull 1 and 10 m/s it fills at 1 m/s: a Lotus about every 90 s if nothing else spends.

### Holding the line (C) and rankings (E)

- **Frontline.** The current line's chokes, ranked by our income behind each one × its traffic. Each of our points
  behind the line counts at its nearest line choke.
- **Enemy army at a choke.** An enemy mobile seen in the last minute (as held ground counts it) counts at the line
  chokes that open into the region it stands in: at the one nearest it in full, and at the others by half. One in a
  region no line choke opens into, ours or further out, doesn't count.
- **Short.** A choke of our line is short when our towers that hit ground don't cover all of its gap, wall to wall
  (the enemy reaches every choke of our line without crossing our ground, by its definition), or when its towers are
  short of 1.2 × the enemy army at it.
- **Hold slots.** A slot sits at the most short choke no other slot has: the biggest shortfall (the enemy army
  outside it over its towers), and where no choke has one, the smallest share of its gap covered, the higher rank on
  a tie. The first idle attack group to ask while that choke is short, and whose power makes up the shortfall (any
  group, where the choke is short only for its cover), claims it, walks to its scored hold spot, and keeps the claim
  while it keeps asking. Once claimed, the slot keeps its choke until 30 s after it was last short, so an enemy army
  that steps out of its region or dies doesn't walk the group away at once, unless another choke is 1.3× as short
  (against its last shortfall within 30 s, for the same reason).
  Then it moves to the most short choke, or ends if none is short. A
  group that hasn't asked for a minute (it went to fight elsewhere) loses its claim. A slot with nowhere to stand (no
  traffic near the choke) stays free and is tried again after 30 s. A group that runs from a fight gives up its claim
  for 30 s.
- **Rankings.** A choke is enemy-held when its outer side from us (more chokes from our ground) is the enemy's
  ground, or known enemy towers cover its line. Every choke of the enemy line is ranked too, held or not. Its defence
  is the threat of enemy towers reaching the line or our approach to it, plus enemy mobiles seen in the last minute
  within a main-tower range (aircraft aside, as held ground counts them: the lines and their ranks forget an army at
  the same time). Its value is the cost of unarmed enemy units directly behind it, aircraft aside. What we saw of it
  is the lower of two shares in our LOS in the last minute: of its line, and of the ground beyond it on the
  enemy's side, up to a main-tower range out, where its defenders would stand. A ranked choke is *undefended* when no
  armed enemy is in reach and we saw at least a quarter of it; with less seen, no defence only means we don't know.

## Logs

Lines in the AI's infolog, for tuning:

- `CHOKE graph`, `CHOKE masks` and `CHOKE traffic`: build sizes and times. Traffic layers are built one movement
  class at a time (classes with the same mask share one), so other work on the worker thread never waits long.
- `CHOKE type set`: each change to the enemy's types or the gating masks, with each type's mask and share.
- `CHOKE openness`: the value, the band and its inputs.
- `CHOKE pocket [...]`: a pocket's first mode, and every switch.
- `CHOKE point for choke`: where a choke's towers anchor.
- `CHOKE spot`: each spot search, and each failed spot.
- `CHOKE artillery near choke`: towers called off.
- `CHOKE hold`: slots starting, moving and ending.
- `RADAR by`: each radar a builder is sent to build, with the coverage before it.
- `TURRET <caller>, by <builder>`: each turret order (`after flee`, `near enemy` or `test`) with its def, spot, the
  four terms, height, F, the nearest building of ours, the walk, base and value per cost, candidates tested and the
  time; or why nothing (no def, no spot, nothing in range worth its cost, no buildable spot among the best, or the
  best at less than its cost) with the hard-rule counts, which say how many spots were outside our influence,
  threatened, out of reach, unbuildable, unsafe or masked. `helps the ...` when it assists a turret instead, and `dropped on arrival` when a job's
  spot stopped paying or came under threat.

## Known limits

- Traffic sources are the enemy's factory positions, not whole factory regions.
- Towers' sight and fire lines start at the def's sight emit height. Radar's start at half the model's height
  plus it, as the engine emits from the unit's middle; the AI can't read `radarEmitHeight`.
- Fortifying the frontline (C) only calls `MakeDefence` for pockets whose entrances are frontline chokes. A frontline
  choke with no pocket of ours behind it gets no towers of its own.
- New tower spending behind a line that moves forward doesn't pause.
- E's rankings only feed the overlay. They don't steer attacks, raids, scouts or artillery yet.
- A tower's cost is counted on the nearest defence point with room for it, as before. A tower at a choke can
  occasionally be counted on a cluster point near that choke.
- Viewsheds are cached while their cell stays a candidate, and aren't made again when terraform changes the ground.
- The turret picker only answers the AI's own callers and the test command; human allies can't order from it. It
  places no AA turrets. A centre with no building of ours within 2048 elmos (a constructor deep in enemy land) gets
  nothing until something of ours stands near.
