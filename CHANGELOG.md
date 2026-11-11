# Changelog

Changes in this fork (`tellistone/CircuitAI-wires3`, branch `zk`) on top of upstream
[rlcevg/CircuitAI](https://github.com/rlcevg/CircuitAI) `zk` (last upstream commit: `e51ba92` "Fix indentation", 2026-06-04).

## Unreleased

### Added

- **Chokepoints** ([doc/chokepoints.md](doc/chokepoints.md)). Every feature is off by default (`choke.enable`,
  `choke.spot.enable`, `choke.radar.enable`, `choke.objective.enable`, `choke.expand`, `quota.hold_choke`), and with
  all of them off the AI plays as before.
  - Each ally team learns the map's shape, shared by allied AIs and built on the area worker. It finds the chokes
    between regions the enemy's walkers can cross, the pockets of clusters behind them, and the current and forward
    lines. It also maps where enemy traffic goes, blended by the enemy's army mix, and how open the map is.
  - Choke defence (A): each cluster of a qualified pocket keeps its AA and its first tower, and the rest goes to the
    pocket's entrances, from a budget of what its held clusters would spend anyway. Modes switch at most every 3
    minutes. Narrow chokes keep a lane free of buildings.
  - Scored spots (F): towers, base towers and hold spots go to the best spot by covered traffic, height, threat and
    builder distance. Spots freeze once queued. The porc sweep visits points by uncovered traffic × income.
  - Radar planner (B): a builder between jobs that stands out of coverage builds a radar within 600 elmos first,
    when its AI's budget holds one, at the spot that adds the most weighted coverage (enemy ground counts
    `choke.radar.enemy` extra), if that is at least `choke.radar.min_gain` of it. So radar follows the builders as
    they expand. The first goes up once the first factory stands.
  - Holding the line (C), forward objectives (D) and enemy-held choke rankings (E).
  - Expansion bonus for clusters behind a defended cut.
  - `choke_overlay` chat command: draws chokes, pockets, lines, rankings, traffic, spots, radar and regions with map
    figures, and announces each threat-graph rebuild with the enemy types behind it. In games with no human
    opponents it tunes keys live and saves map fixtures.

- **Raid view of the threat map** (per-unit `raid_thr_mod` and `quota.raid_thr_role`, 0.1..4, default 1 = off;
  shipped `{"skirmish": 0.25, "assault": 0.34}`). Raiders swarm and dodge slow skirmisher and assault shots, but raid
  and scout tasks judged those units at full threat, as if they were standing still. Raid and scout tasks now read
  their own surface and amphibious threat layers, where each enemy's threat (thr_mod included)
  is x its type's `raid_thr_mod`, and it counts in `group_count_power` as thr_mod x raid_thr_mod (at most 1). A unit's
  value: its own `raid_thr_mod`, else `quota.raid_thr_role` for its main role (the first in its `role` list), else 1.
  Air threat is the same on both views.
  - One choice of view for every read: target picks, escape, firing spots, path caps and routes, raid and scout
    spots, contested clusters and the raider/scout mex bonus all follow the task (`IUnitTask::IsRaidView`), so a raid
    group never picks on one map and routes on the other. Two reads stay plain on purpose: the shared
    `SetThreatType` layer (mex and build checks read it) and the builders' wall in contested clusters. The hit
    retreat check of a raider follows the raid view.
  - Only types with a multiplier are drawn a second time, as the difference they make, and one pass after drawing
    builds the raid layers. With every multiplier at 1 the raid layers equal the plain ones bit for bit. Memory: two
    more layers per buffer (four with `group_count_power`).
  - Shipped per-unit `raid_thr_mod: 1.0` for the units Zero-K's own tactical AI doesn't let raiders swarm: Knight,
    Jack, Moderator, Scalpel (weapons that always hit), Nimbus (a gunship) and Siren.
  - Allied AIs share one threat map: the AI that built it decides the multipliers for all (as with `thr_mod`).

- **Attack groups vs raiders** (`quota.raid_attack_factor`, default and shipped `true`). The same value on our side:
  an attack group judges an enemy raider or scout (any role in its list) outside the base defence radius with its
  pick and keep strength x its units' `raid_thr_mod` (power-weighted, at most 1). A skirmisher group needs about 4x
  the strength to go after raiders in the field. Inside the base defence radius, within its own fire range + 64 elmos
  (raiders already fighting it), and on the route approach and the escape, it judges at full strength, so a raider
  target is never picked more loosely than it is kept, and a group never ignores raiders shooting it. The current
  target keeps the base and fire range exemptions out to 1.25x, so a raider stepping back and forth across either line
  doesn't flip the pick.

- **Raid tuning logs**. Once a minute: `THREAT raid view` (how many enemies have a multiplier, the cell where the
  raid view differs most from the plain one, and the threat map update time) and `RAID` (raid group escapes, and the
  damage our ground raid and scout units took by attacker type, next to that type's `raid_thr_mod`; an attacker we
  can't see is named by its weapon when only one type has it). With `DEBUG_VIS`, a
  SURFACE RAID window joins the threat map windows.

- **Grid consumers get linked to a grid with enough energy** (`economy.energy.grid_link`, default and shipped
  `max_pylons` 6, `margin` 1.1; `max_pylons` 0 = off). Structures with `customParams.neededlink` (Big Bertha,
  Cerberus, Desolator, Lucifer, strider hub, Disco, Zenith, Starlight) only work on a grid with that much energy, and
  were often built unpowered. Every 10 s each of ours, from its first nanoframe on (the game keeps a consumer's nanoframe on
  the grid, so the line is built alongside it and it works once finished), is read for the game's `lowpower`; after 2 unpowered reads in
  a row it gets energypylons, one at a time, from its own grid towards the nearest other grid with margin x
  neededlink (the game's `gridNumber` and `OD_gridMaximum`), until it is powered or the cap is reached. Never towards
  its own grid, and a failed attempt waits 60 s, doubling up to 5 min. Buildings the game has taken off the grid
  for now (paralysed, disarmed, morphing, or not active: a solar closes for 4 s when hit) still count: their
  gridNumber is stale, so each joins the grids it overlaps, which it links again when back on, and adds its energy
  to their capacity. Consumers in one grid share a pylon line instead of
  each laying their own (Big Berthas stand together), and consumers an ally gives us are watched too.
  - The superweapon dice only draws a grid consumer while a grid with margin x its need is within `max_pylons` reach
    of where it would be built; a queued gun is kept.
  - Porc grid consumers are only built at a defence point with such a grid within `max_pylons` reach.
  - Removed the 4 hub pylons of raveparty, mahlazer and zenith and the strider hub's `"pylon": true` in
    `build_chain.json`: they didn't reach an energised grid.

- **`economy.chat` switch for the test chat lines** (default `false`). The threat floor's `Defending` / `Attacking` line
  and the `ms_pull` line (`economy.ms_pull_chat`) only post with `"chat": true`. The resign line `I am defeated.` and
  the start-up line are unchanged.

- **Anti-heavy cloak routes** (per-unit attribute `cloak_route`, default off). Cloaked anti-heavies (Ultimatum,
  Widow) used to route on the decloak layer with no `antiheavy.path_threat` cap and an unchecked stand-off spot,
  so they walked through enemy weapon range and died once they decloaked. Without the attribute they now route,
  pick stand-off spots and flee on the normal threat map like Lance; with it, the old behaviour. Mines (Skuttle)
  always keep cloak routes. Under water without sonar stealth cloak doesn't count (one `IsCloakRoute` check, also
  used by artillery and scouts).
  - Once a heavy is seen, anti-heavies also leave support tasks (Phantom, Lance), and leave a fighting group when a
    heavy they can shoot is within their weapon range; a heavy already in weapon range (x1.2 for the current target)
    is attacked from where the squad stands, whatever escorts it.
  - Squad units keep their own `fire_state` (default fire at will); cloakers are no longer held to return fire.

- **Geothermal build priority** (`economy.energy.geo.priority`: `"low"`, `"normal"` or `"high"`, default and
  shipped `"high"`). Before, an energygeo job was NORMAL unless energy was short. It now takes this priority while
  energy isn't short (job choice weight and travel cap, and ZK build priority), and drops to normal while metal
  stalls so mex jobs get the scarce metal first. Energy short: HIGH, as before. Only one geo job exists at a time.
  `CBEnergyTask` re-applies the same rule (`CEconomyManager::GetEnergyPriority`), which also returns any energy job
  queued during a shortage to NORMAL once it ends.

- **Geothermal vents defended like mex clusters** (`porcupine.geo` in `build_chain.json`, default off; shipped on).
  Each vent that no mex defence point covers becomes a defence point of its nearest cluster, sharing that cluster's
  preventive roll. Our geo there is defended like a mex: when it finishes and on a sighting. Towers for a vent stay
  at the vent, and vents we don't hold get none. Vent points don't count as the cluster's defence (mex escort) or as
  its front positions, and count as safe positions only once defended.

- **Scout share floor** (`quota.scout_share`, 0..1, default 0 = off; shipped 0.25). This share of our
  scout-attribute units of a movement type (scouting or not, player-controlled ones left out), rounded down, may
  scout even when `scout` + bonus is lower. `scout_max` caps it too: scouting raiders don't fill the raider floor, so
  an uncapped high share would keep factories building raiders that all go scouting (share 1 with `scout_max` 0 is
  read as 0.9). Like the rest of the scout quota it is checked when a unit gets a new task, so it never pulls a raider
  off a raid. With 0.25: 4 Glaives -> at least 1 scouting, 8 -> 2.

- **Fractional preventive defences** (`porcupine` block in `build_chain.json`). `prevent` (towers a cluster gets
  outside porc mode) can be a fraction: count = its whole part, plus 1 with the odds of its fraction, so 0.5 = a 50%
  chance of 1 tower and 1.5 = 1 tower and a 50% chance of a 2nd. Before, it was read as a whole number and 0.5 meant 0.
  - The chance is `prevent` x `prevent_map` x `prevent_mex`: `[[x, multiplier], ...]` curves, linear between points,
    flat beyond the ends, missing or `[]` = x1. `prevent_map` x = map area in 512-elmo squares / players on our team
    (fixed for the game); `prevent_mex` x = ally-team finished mexes / all metal spots, read at each roll.
  - Each cluster rolls the first time it is defended and again every `prevent_reroll[0]` s (default 180); the new
    count replaces the old. A roll never cancels a tower already queued or built.
  - Sightings: every 5 s, an enemy in LOS that can hit ground (air included, no cost floor) within mex LOS of one of
    our finished mexes makes that cluster roll and defend at that mex right away, at most once per
    `prevent_reroll[1]` s (default 30) since its last roll or sighting. Half-taken clusters count, but below 10 m/s a
    sighting there never starts porc mode (the mex chain only porcs half-taken clusters above 10 m/s). A sighting's
    roll can only raise the count, so being raided never lowers a cluster's defence. After a sighting of a flying
    enemy the first tower is `prevent_air` (shipped `turretmissile`, which hits air and ground; `""` = off).
  - Shipped: `prevent` 0.5, `prevent_reroll` [180, 30], `prevent_map` [[32, 1.5], [128, 0.7]], `prevent_mex`
    [[0.2, 0.6], [0.5, 1.2]] (starting numbers to tune). `PREVENT` log line on each roll with every factor.
  - Preventive towers are now queued each on its own rather than as a chain. A count of 2+ would otherwise have been
    cancelled and re-queued on every call by the porc-chain abort, which cancels any unstarted chain outside porc mode.

- **Threat floor breakdown in the log**: every 30 s with the `THREAT FLOOR` line, `THREAT FLOOR ours:` (our army's
  unit count, its power with `pwr_mod`, and the types carrying most of its metal) and `THREAT FLOOR enemy:` (the
  known enemy mobile army's metal split into enemies in radar or LOS and hidden ones, faded and unfaded, and the
  types carrying most of it), to check the army ratio behind the floor and `ms_pull_army`.

- **ms_pull in chat**: the threat floor line adds the army factor (`ms_pull: 1.21 (army x1.50)`, `(army off)` before
  `ms_pull_army.since` or with no enemy army known), and a line `ms_pull: 0.85 = base 1.20 (mex 35%) x income 0.90 (82.0 m/s) x army 0.79
  (enemy 1450 / ours 2340 metal = 0.62)` (each factor with the measure it reads) posts whenever `ms_pull` moves by `economy.ms_pull_chat` (default 0.25 = 25%, 0 = off) since the last one, at most
  every 20 s.

- **ms_pull scales with income and the army balance** (`economy.json`; [#105](https://github.com/tellistone/CircuitAI-wires3/pull/105)).
  Two `[[x, multiplier], ...]` curves multiply `ms_pull` (lower = more metal to factories while metal stalls), linear
  between points and clamped at the ends; a missing key uses the defaults, `[]` turns one off.
  - `ms_pull_income` (default `[[30, 1.0], [160, 0.7]]`): x = metal income without reclaim (mex + overdrive +
    constructor share, as the response gates' `unlock_after`), from the start.
  - `ms_pull_army` (default `{"since": 300, "curve": [[0.5, 1.3], [2.0, 0.7]]}`): x = known enemy mobile threat per
    enemy team / our army threat (commanders on neither side), from `since` seconds and while any enemy army is known.
  - The old army cost clamp on `ms_pull` (`ClampMobileCostRatio`) is removed; `ms_pull_army` replaces it.
  - `MS_PULL` log line every 30 s with each factor.

- **Known enemy army fades out of sight** (`quota.enemy_half_life` in `behaviour.json`, default 180 s, 0 = off). The
  enemy army count that `ms_pull_army` and `quota.attack_threat_floor` read counts an enemy in radar or LOS in full and
  one out of both at half per `enemy_half_life` seconds since last seen; enemies seen dying or forgotten don't count.
  Before, the threat floor counted every enemy ever seen and not seen dying, for up to 20 minutes.

- **Priority repairs leave dangerous spots** (`retreat` block; code defaults keep the old behaviour, the shipped
  config turns both on).
  - `repair_safe_held` (default `true`): with `false`, the `repair_safe` / `repair_safe_static` check ends or skips a
    priority repair on threat alone. Before, it also needed the spot to be enemy-held, so a repair on a front where
    our own army stood counted as safe even under heavy fire.
  - `repair_hit_exit: [builder hit, unit hit, cooldown sec]` (default `[false, false, 5]`, shipped `[true, false, 5]`):
    drop a priority repair as soon as one of its builders takes damage, and/or when the mobile unit being repaired takes
    damage while builders are on it. That unit then gets no new priority repair for `cooldown` seconds.

- **Line formation for attack groups** (`formation` block in `behaviour.json`; code default off, shipped on; per unit
  `formation_row` and the `no_formation` attribute). Attack groups of 3 or more land or sea units used to send every
  unit to the same path points, so they walked single file and clumped at turns. While a group walks its path and
  isn't engaging, it now walks as a line across the path its leader walks, centred on the leader:
  neighbours `spacing` apart (closer as the group grows), at most `width_range` x its median weapon range wide,
  narrower where terrain or buildings in the next 3 s of walking leave less room, or where a flank would step into
  more enemy threat than the path itself is in (between two turrets the line narrows to the gap). Rows shift sideways
  to fit, and no slot lies past a spot that isn't free, seen from the path. Spacing squeezes to 0.7 before a second row
  is added; rows fill front to back by `row_order` (riots and assaults in front, artillery at the back) and follow the
  leader's path (behind the leader, the track of its spot on that path; a re-plan that turns back by more than 120°
  from where the line was heading lays the rows behind the new heading instead; a sharp corner of the same route
  doesn't count, since it turns the heading gradually). In a row units take places in the order they stand across the path,
  then swap where that shortens two units' walks to their places, so they don't walk across each other; places are
  matched again when the group, its columns or its leader change, or the path turns back. Where not even 2 fit abreast the units follow
  the path as before, until 2 fit again for 3 s. The line walks at the speed of its slowest unit (support units such as
  capture cars included) and slows in proportion as its furthest-behind unit, or a busy leader, falls behind its slot,
  stopping when that unit is `lead_sec` behind, so even the slowest can catch up (but not while an enemy is in LOS of
  any unit in the group, or was in the last 5 s: near the enemy the line walks on at its slowest unit's speed); each unit gets a move a few seconds
  ahead of its slot and a wanted speed that lets it catch up, re-sent only when it nears that move's end or the slot
  moves off its way. At the end of its walk the group stands in line. A unit out of its slot for 12 s catches up at full
  speed and isn't waited for (the usual regroup still stops the group if it falls far behind); while the line walks,
  one that hasn't moved 64 elmos in 30 s while out of its slot is released to find a new task (the leader too, and the
  line re-centres on the next one). Units with the `jump` attribute (jump on the move; none in the shipped config)
  still jump along the path whenever their jump is ready, then walk back into line; jumps in combat and when fleeing
  don't go through the line and are unchanged. If the group's most restricted mover is a `no_formation` unit, the
  group walks as before. While the line is on, the regroup check (stop and gather when units spread too far)
  ignores units the line steers that keep up (in line, or stragglers at least 10% faster than the line), so a group in
  line no longer stops to gather for them. When the group does regroup (formation on, `min_units` or more), it regroups
  in line: the line stops with its front row where it stands, and the units walk to their slots in it, so laggards come
  up into their rows instead of everyone packing onto the leader. If the line was off (often just after a fight, with
  no way it came to go by), the front row stands at the leader facing the nearest strong enemy group (the most threat
  per distance), else away from our base; standing, it still turns to a clear threat around it. Where no line fits, the
  units walk to that spot, as the gather did. A regroup in line ends as soon as every unit the line steers stands in its
  slot and the rest are near (else at the usual check, every 16 updates); the walk then resumes in line on the same path,
  unless a unit asked for a new route. A holding group that is hit and steps away ends its regroup, so the step isn't undone. A holding group (nothing to attack, no front to walk to, no safe route)
  stands in line where it is, as a regroup's line stands, instead of every unit walking straight to one spot at each
  decision. It moves only to a spot with at most 80% of the threat here, and keeps that spot until another has at most
  80% of its threat, so two spots can't take turns. To a spot within 256 elmos a block standing facing the threat
  slides there as it stands, its front still to the threat and no unit changing slot (where it fits there; else the
  line walks straight there); farther, the line walks a threat-capped path and faces anew at its end. A group already
  beside the group it reinforces stands in line the same way (it used to gather: units within 600 elmos stood where
  they were, the rest walked straight in), and units that join walk into its slots. A unit pulled back from danger while the line stands rejoins
  with the next path rather than walk back into the slot that endangered it, and a line that switches off while
  standing (no room left for 2 columns) stands again at the next decision. A group with no line (formation off, or
  fewer than `min_units` units that take a slot) holds with the same 80% rule, straight to a quiet spot within 256
  elmos, else along a threat-capped path (ending bunched on its last point), and gathers beside the group it reinforces as
  before.
  Where no line fits, a stand's units stand together at its end. Stepping out of an attacker's range while holding,
  and escapes where no path was found, stay straight moves. Once the line stands at the end of its
  walk it faces the threat (`face_range`, 0 = off): it reads the enemy threat 8 ways at 1 and 2 x its fire range
  around the block and, if the threat has a clear side, turns the block about its centre to face it, its front row
  short of more threat than at the centre (else it faces the way it came, not the path's last jog). It turns only
  where the block fits that way (walkable, 2 columns wide), and again only for a clear threat more than 45° off for
  2 s, and keeps facing across new paths that end within 256 elmos of the same spot, so
  re-plans to the same spot don't turn it to and fro (a route to a target is always walked). Units keep their places on a turn of up to 120°. Fleeing groups, and cornered groups moving to a quieter spot, also
  run as a line (they used to run single file along the escape path, which splash weapons punished): rows in reverse
  `row_order` while they walk (riots at the back, towards the pursuers; artillery and skirmishers lead; normal order
  again once a cornered line stands and faces), at the slowest unit's speed, waiting for no one and with no unit
  dropping back towards the pursuers. With splash near (`splash_spacing`: a finished enemy that can hit the ground, or a mobile bomb, whose weapon area of
  effect or blast is 16 or more, mobile ones seen in the last 10 s, within its range + 600 of the group, held 10 s), neighbours and rows stay at least 1.2 x its
  radius apart (at most 256), adding rows where the width runs out. Other squads
  (raid, defend, anti-air, anti-heavy) and units' own facing come in later steps.
- **Reload scoot for any squad unit** (per unit `reload_scoot`, default 0 = off; `true` = 0.8; per unit
  `reload_scoot_margin`, default 100, or the anti-heavy flee margin in anti-heavy squads). While engaged, a unit whose
  main weapon is less than `reload_scoot` reloaded and that stands in ground threat steps straight away from the
  target to just outside the target's ground range + margin (a margin further back each second while still in
  threat), but never to a hotter spot, and fights again once reloaded that far; the rest of the squad fights on. A unit
  outside all ground threat doesn't scoot (it fights on); a scooting unit that gets there keeps its order. It turns back
  early when walking into range would take as long as the reload it has left, doesn't start when there isn't time to
  get out and back, and stays on hold position while scooting (else the engine's idle auto-attack walks it straight
  back in). Scoots end when the squad stops being engaged; a scooting leader keeps anti-air and anti-heavy squads
  engaged on their target. Anti-heavy's scoot uses the same rules now. A squad that is aborted or done gives its scooting units their
  move state back (it used to leave them on hold position for good). Works in attack,
  raid, defend, anti-air and anti-heavy squads (not mines or aircraft). It is anti-heavy's shoot and scoot, moved to
  `ISquadTask`; anti-heavy units without `reload_scoot` behave as before. No unit has it set yet. A scooting unit's
  move is re-sent once a second only when its spot has moved.
- **Anti-heavy threat-capped routes and flee** (`antiheavy` block in `behaviour.json`; code defaults off, shipped
  `path_threat` 1.0, `flee_margin` 200, `flee_ally_threat` 10, `flee_ally_radius` 500; per unit `antiheavy_flee_margin`).
  Anti-heavy squads used to path to heavies, safety and home with no threat cap, and had no flee. Their routes now use
  anti-air's cap (squad strength x base bonus / `thr_mod.distance`, x `path_threat`); the route to a heavy ends at its
  stand-off point (the first spot outside the heavy's own threat ring that is under the cap, read from the threat map),
  and the squad attacks from there. They flee like anti-air
  squads from armed enemies in reach (gunships included), unless escorts cover them, and stop once out of reach (with
  a little hysteresis). The heavies they hunt don't set off the flee; instead, after a shot with a long reload
  (5 s or more), each unit steps out of the target's reach on its own and comes back once reloaded while the rest of
  the squad fights on (shoot and scoot), instead of the whole squad walking back to the attack group. Heavies that
  could only be attacked from inside an uncovered flee zone aren't picked, so escorted heavies wait for the army.
  Mines and aircraft never flee; aircraft get no cap. Per unit `antiheavy_max_group` (default 0 = no limit) caps how
  many units of that type one anti-heavy squad holds: squads only grow by merging, and a merge that would go over it
  is skipped. The path cap, squad flee check and cover check are now shared by anti-air and anti-heavy
  (`ISquadTask`); anti-air behaves as before.
- **Capturer hides when loaded** (per unit `capture_hide`, default 0 = off; shipped 2.0 on `vehcapture`). A unit whose
  captured units (Zero-K's `capture_controller` rules param) cost more than `capture_hide` x its own metal cost retreats
  to the closest haven and stays there, ignoring the retreat wait timeout, until enough of them die or are lost to drop it
  back under that line. Its captured units go back to the enemy if it dies.
- **Raider production floor** (`quota.raider`, `raider_per_mex`, `raider_max`; all default 0 = off; shipped 1, 1.0, 12).
  The scout quota's twin for production: while we have fewer raider units (main role `raider` or a role bound to it,
  alive and not on a scout task, plus queued in factories) than `raider` + bonus, a factory that can build a raider builds one, picked by its
  `factory.json` tier weights. The bonus is the scout formula (reachable enemy mexes / enemy players x `raider_map_size`
  curve (missing = `scout_map_size`; shipped `[[8, 1.0], [20, 4.0]]`) / all players x `raider_per_mex`), with reachability judged for a starting raid group (`quota.raid[0]` power,
  `thr_mod.raid`, `thr_mod.distance.raid`). `raider_max` caps `raider` + bonus but never cuts `raider` itself.
- **Raid roaming spots** (`quota.raider_spot`, missing = old roaming; same keys and defaults as `scout_spot`). Raid groups
  with no enemies known roam to the best-scored spot (near the enemy, unseen for longest, not defended by known static
  enemies, not on our side) instead of the shared farthest-mex list. Groups claim spots separately from scouts; the
  group re-picks once the spot is in our LOS, turns dangerous (then it is blocked for 10 s) or its destination was
  changed by something else. Contested clusters still come first; with no spot left it roams the old way.

- **Guards interpose** (`guard.interpose`, default `[120, 600]`). While a mobile armed ground enemy or an unknown radar dot, seen now, is
  within 600 of the guarded unit, its guard moves to a spot 120 towards the nearest one (at most halfway), instead of
  only following with the engine Guard order. A Guard order is queued behind the move, so from the spot it follows the
  guarded unit, staying on the enemy's side. It is re-sent a spot when the enemy or the guarded unit shifts it. `[0, ...]` turns it off.
- **Anti-air leash** (`antiair` block in `behaviour.json`). Mobile AA squads (main role `anti_air`) used to chase the
  nearest known aircraft anywhere on the map, so they followed planes home into enemy territory alone.
  - `leash` (default -1 = off): only chase an aircraft within this distance of home, meaning one of our attack or
    defend group leaders or the base. An aircraft already in weapon range is always engaged. With no aircraft
    near home, the squad stays with the nearest attack group, as it already did with no target.
  - `own_ground` (default `false`): also counts as near home when the spot the squad would shoot from is covered
    by our buildings or static defences (the area defend groups treat as ours, radius `defence.infl_rad`), or when
    our armed units other than the squad have influence under the aircraft and beat the enemy's, not counting the
    plane's own share.
  - `fire_spot` (default -1 = off): check enemy threat where the squad would stand to shoot, this fraction of its
    shortest weapon range short of the aircraft, instead of under the aircraft.
  The shipped `behaviour.json` sets `leash` 800, `own_ground` true and `fire_spot` 0.9.
- **Anti-air early flee** (`antiair.flee_margin`, default -1 = off; shipped 300). AA squads used to move away only once
  hit. Now, on every update, when a mobile ground enemy we see now that can hit ground targets (or a moving radar blip
  low enough not to be a plane) is within its ground weapon range + this margin of any squad unit, the squad paths (threat-capped) to the nearest
  safe spot further from it: an attack/defend group leader, a defended mex cluster or the base. It re-plans every 2 s
  while the danger stays and goes back to normal work once it is gone. With `antiair.flee_ally_threat` (default -1 = off; shipped 10)
  and `flee_ally_radius` (500) there is no early flee while our and allied mobile armed units (not AA, not aircraft)
  and static defences that can hit ground (weapon range up to 2000, so no superweapons) within that radius of the leader, each at its power × √(health %) (nanoframes don't count), minus the threat of the enemies that set off the flee, add up to that much power
  (artillery's `ally_threat`, net of the enemy): AA moving with an army stays with it, and flees once the army leaves,
  dies or is outweighed. If `flee_margin` is set and `flee_ally_threat` is missing, it defaults to 10.
- **Artillery flee cover is net of the enemy.** The `artillery_flee_margin` check now needs the `ally_threat` escort
  power at the artillery's spot minus the summed threat of the enemies that set off the flee (each counted once) to
  reach `ally_threat`, so an escort the enemy outweighs no longer holds artillery in place. For this check, static
  defences that can hit ground (not superweapons) count as cover too, at full-health power. Firing-spot cover is unchanged.
- **Mobile AA no longer joins attack groups.** In the shipped `behaviour.json`, cloakaa, amphaa, shieldaa, vehaa,
  jumpaa and tankaa had the `support` attribute, which adds the support role. Support units walk to the nearest attack
  or defend group and join it, so these AA never entered the anti-air task and never chased aircraft. The attribute is
  removed from them; they now use the AA task (leash, own ground, and staying with the nearest attack group when there
  is no aircraft to chase).
- **Anti-air moves are always threat-aware.** AA squads no longer fall back to plain engine orders that ignore threat:
  - no path to the target: go to a safe spot (attack group, defended mex cluster or base) instead of fight-moving at it;
  - no path to the disengage spot: same safe-spot search instead of a straight move;
  - hit with no safe path: threat-aware path to base instead of a straight fight-move;
  - last resort: threat-aware path to the commander instead of a Guard order;
  - regroup: each unit paths to the gathering point instead of fight-moving to it.
  When even that finds no path, the squad holds where it is (weapons still fire) and retries on the next update.
- **Anti-air firing spots.** AA squads now judge the attack task's firing spots (`IFighterTask::Attack`)
  against their own strength: a spot or the walk to it with more threat than the squad can take counts as dangerous,
  so they hold or pick the least dangerous spot instead of walking in. Target choice and firing spots use the same
  allowance (squad strength x `thr_mod.base_bonus` / `thr_mod.distance.aa` at the spot).
  At a firing spot AA now fights from the spot instead of taking an Attack order, so the engine never chases a
  plane past it (`IFighterTask::isFireSpotChase`, new `CCircuitUnit::FightFrom`; other tasks unchanged).
- **Anti-air target checks.** An unidentified radar blip is taken for a plane only when it is at least 50 elmos above
  the ground, so AA no longer walks to a ground unit's dot it can't shoot. The ground is the highest terrain within the
  radar error (96 elmos) around the dot, and never below the water surface, so ships and units by cliffs don't pass. When judging threat at a firing spot, only
  the plane's own threat at that spot is taken off (none outside its ground range, none for a pure fighter), so a
  plane can no longer make real ground threat under it disappear.
- **Anti-air path threat cap.** Every AA path request passes a threat cap (squad strength with the strictest
  `thr_mod.distance` and loosest `base_bonus` of start and goals, never below the threat where the unit stands),
  so routes go around known defences instead of through them. With no capped route the squad holds; only the
  last escape to base, after being hit, may ignore the cap.
  A unit that can't find a capped route back to its squad when regrouping leaves for its own AA task, so it
  doesn't keep the whole squad in regroup (no targets) or get dropped as stuck. AA walks to the firing spot it
  judged when picking a target, not to the aircraft itself. `own_ground` leaves out the squad's own influence, so
  AA can't call its own surroundings in enemy land "ours" and creep forward.
  A unit split off that way can't merge back for 30 s. While engaging, AA units farther than 2x their range from
  the aircraft get no firing spot and keep following the squad path (`IFighterTask::fireSpotReach`, off for other tasks).
  During regroup only units outside half the regroup distance get a path back; the rest hold. While engaging, AA
  re-orders on a target tile change only once the aircraft moved 1/4 of its air range (`IFighterTask::retargetMove`,
  off for other tasks). Leash, `fire_spot` and the engage check use the AA range against aircraft, not the longest
  weapon range.
  Only `leash`, `own_ground` and `fire_spot` default to the old behaviour; the other AA changes above apply to every config.
- **`guarded` unit attribute.** Any unit type with `"attribute": ["guarded"]` (for example artillery) gets a guard
  slot that follows all the `guard` rules, from when it finishes until it dies. A builder with the attribute gets a
  slot whatever `max_workers` says. Guarded units never guard others and don't count toward `max_share`. Flying units and static defences get no slot.
  The safe check leaves out the guarded unit's own influence, so an armed unit can't call its own spot safe.
  Guarded-unit slots have their own `max_share` pool (`guarded_max_share`, -1 = same as `max_share`), so builder slots
  can't use up the whole share. Their safe check also leaves out squadmates and every other guarded unit, so a squad
  or a group of artillery can't call its own spot safe. For a support unit, which follows the nearest attack or
  defend squad, the members of those squads count as squadmates.
  The safe check (all slots) now leaves out every slot's guard, not only its own, so two vips side by side don't each
  count the other's guard, release their own together, then pull again.
  For an army unit the safe check also looks at its squad's attack or defend point, the way a builder's job location is
  checked, so artillery gathering at base for an attack takes a guard along. A point within the unit's weapon range is
  treated as its target, not a destination, and is ignored.
- **Configurable builder guards** (`guard` block in `behaviour.json`, [#3](https://github.com/tellistone/CircuitAI-wires3/pull/3)).
  Early unarmed builders get a guard slot that one combat unit fills.
  - `roles` (default `["raider", "riot"]`): main roles that may guard. Before, only raiders and riots could, and that was hardcoded.
  - `max_workers` (default 3): a builder gets a slot if the worker count, commander included, is below this when it
    finishes. Before, this was a hardcoded 3. 0 turns guards off.
  - `max_share` (default 1.0 = no limit): the most of each role's units that can guard at once, counted by
    acting main role. For example, 0.3 means 30% of raiders and 30% of riots, rounded up so a single unit of a role
    can still guard.
  - `pull_range` (default 800): an empty slot pulls the nearest eligible unit within this distance. Eligible means idle
    or on a raid or defend task, never a scout or a member of an attack squad. Before, a slot only filled when a unit
    asked for a new task.
- **Guard exit conditions.** The guard goes back to normal duty when:
  - its builder retreats;
  - its builder has been safe for `safe_time` seconds (default 5). Safe means allied influence, minus the guard
    itself, is at least `safe[0]` and enemy influence is at most `safe[1]`, both where the builder stands and
    where its current job is;
  - it stands in enemy threat above `threat_ratio` times its own threat. The builder then gets no guard for
    `threat_cooldown` seconds (default 10, -1 turns this off).
- **Guard damage exit** (`guard.damage_exit`, default `true`). A guard that takes damage is released at once, and its
  builder gets no guard for `threat_cooldown` seconds.
- **Geothermal plants** ([#29](https://github.com/tellistone/CircuitAI-wires3/pull/29)). The energy pick used to skip
  every building that needs a vent, so `energygeo` was never built. Now, when a known vent is free, has no more enemy
  surface threat than `economy.energy.geo.threat` (default 1.0), is outside allied zones and the builder can reach it,
  and the geo is under its `economy.energy` limit and quick enough to build, it is built exactly on that vent, before
  fusion and singu are considered. No free vent, or the limit reached, never stops other energy. Every 10 s one finished
  geo at a time morphs into `energyheavygeo` once average metal income and game time pass `economy.energy.geo.morph`
  (`[<income>, <second>]`, default off; shipped `[30.0, 900]`), metal isn't empty, energy isn't stalling, and the geo
  is at 90% health or more with no more threat on it than `geo.threat`. A morph still running after 5 minutes stops
  blocking the next one.
  `behaviour.json` gains `energygeo` and `energyheavygeo` entries.

### Changed

- **Constructors freeze in every stall again** (reverts that part of #108). In a metal stall a constructor within
  `ms_pull` x the factory pull no longer starts new spending: constructors hold all new spending in any stall, as
  before #108, and the flee-turret skip is back to its old test. The factory side of #108 stays.

- **ms_pull_army compares metal value** (`economy.json`). Its x is now the known enemy mobile army's metal value per
  enemy team over our army's metal value, constructors and commanders left out on both sides, hidden enemies fading
  as before. It was the two armies' threat, which weights units by `thr_mod` (artillery and bombers at 0 counted as
  nothing on either side) and by health and damage, so a big army of light units could read smaller than a few
  enemy heavies. The `MS_PULL` log line shows both metal values.

- **The threat floor, resign and base gather compare metal value too** (`behaviour.json`; the keys keep their names).
  - `quota.attack_threat_floor`: our army's metal value against the known enemy mobile army's per enemy team (faded,
    no commanders or constructors), the same two numbers as `ms_pull_army`. The `THREAT FLOOR` log lines and the
    Defending / Attacking chat line show metal (`enemy metal: X | our metal: Y`), with the types carrying most of it.
  - `resign.threat`: the game reads lost when the enemy army seen alive is worth more than this x our army's metal
    (enemy constructors no longer count). The `RESIGN` log line shows both.
  - `quota.attack_merge.base_gather` (when `true`): the army at base gathers until it has `quota.attack` power and the
    known enemy mobile army's metal value per ally. It was power against the enemy's total threat, statics and
    commanders included.
  - An enemy nanoframe in LOS (a unit still in its factory) no longer counts in any of these, as our own count only
    once finished. One seen being built that left LOS counts, since it may have finished.

- **Anti-heavy units leave the army while a heavy is around.** Anti-heavy hunting used to switch on for good at the
  first sighting of any enemy heavy (the enemy cost only drops when we see it die), and units already in the army
  never left it. Now it follows sightings: while an enemy heavy has been in radar or LOS within the last 10 s, new
  anti-heavy units get anti-heavy squads and, every 2 s, anti-heavy units in attack and gathering groups that aren't
  fighting or fleeing leave for one; a squad disbands (its units back to the army) once none has been seen for 60 s.
  The gap stops a heavy dipping in and out of radar from sending units back and forth. A unit joins an anti-heavy squad
  of its type nearby with room (within 1000 elmos) rather than walking alone until a merge. Mines are unchanged: always
  their own squad, which disbands once no enemy heavy is known.
- **Raid and attack drop outranging runners sooner.** Both tasks skip an enemy moving away from the leader at 80%
  of the squad's top speed or more; an enemy that outranges us is now skipped from 75% (except anti-heavy, support and artillery
  units, which are chased as before). Outranges: its ground/water
  weapon range (anti-air only units never count, as in the flee's `IsGroundDangerDef`) is over 1.1x ours, so a
  constructor's few extra elmos don't count. Ours: attack's weighted median weapon range (as for siege targets), a
  raid group's highest weapon range. Raid's near-base allowance (which doubles the squared speed limit) no longer
  applies to an outranging enemy. Enemies of unknown type (radar dots) are treated as not outranging. Both use
  `ISquadTask::IsRunningAway`.
- **Squad flee: one state machine, cheaper checks.** Anti-air and anti-heavy squads now share one flee state
  machine (`ISquadTask::UpdateFlee`) in place of two copies; behaviour is unchanged (anti-heavy keeps its wider end
  margin and stops on arrival, anti-air neither). The escort cover check no longer forces a rebuild of the allied unit
  list each time a squad looks (it uses the one the influence map refreshes every few frames; units that died since
  count 0). Anti-heavy target search tests the heavy role before any map or engine read.
- **Commander flee and hide, follow-up to #62.** Jobs near a fight: a job inside an enemy's flee zone is taken when
  allies there plus the commander cover 1.25x the threat reaching it (it used to be skipped whatever the cover), so
  the commander works alongside a winning army. A home fight also needs 1.25x cover, the bar a flee ends at, so a
  fight it starts doesn't turn into a flee at the first hit. Jobs and morph starts keep an enemy for 5 s after it left
  radar and LOS (the flee itself still counts only enemies seen now). No cornered defence while metal is stalling.
  Cover checks no longer force a refresh of the friendly-unit list, and the flee checks in DefaultMakeTask wait for
  the cost map like the rest of it. One danger evaluation per commander per frame feeds every flee, fight, job and
  morph decision. With every flee spot inside a flee zone the commander now counts as cornered (it used to head for
  base anyway). `hide.latch` (commander.json; true in the shipped profiles, missing = on when the flee is on, so the
  "previous" profile keeps a live trigger) replaces the latch that followed `flee_margin`. A flee also ends only once
  no enemy seen in the last 5 s is within reach, so one stepping out of sight doesn't end it mid-way.

- **Repair chases end when the builder doesn't close in** (`retreat.repair_chase: [sec, elmos]`, default `[0, ...]` =
  off; shipped `[3, 64]`). This replaces dropping a repair job by sampling the target's speed (`repair_flee` now only
  has the speed share, used when picking jobs). A builder out of reach of a moving repair target that hasn't cut the gap
  by 64 elmos in 3 s counts as stalled; when no builder on the job is in reach and all are stalled, the job is dropped
  with the `repair_leash` cooldown. The speed test missed units moving at their group's (slowest member's) pace and
  restarted on every pause in a fight.
- **Escort and patrol builders drop the engine's own repair chases.** While patrolling, the engine repairs damaged
  units nearby by itself and chases any that is no faster than the builder (up to about 500 elmos from the patrol
  path). The same `repair_chase` check now catches that: an escort builder then moves instead of patrolling for the
  `repair_leash` cooldown, and a patrol fallback job moves to its spot for the rest of the job, so the engine doesn't
  pick the same chase again.

- **Builders stop chasing units they can't catch** (`retreat.repair_flee: [speed share, sec]`, default `[0, ...]` = off;
  shipped `[0.5, 2]`). A mobile repair target that is not retreating, is out of a builder's reach and is moving away
  from it faster than 0.5 x its own max speed is no longer picked for repair or pulled onto. A repair job is dropped
  once its target keeps doing that to all of its builders for 2 s, and then no new priority repair is made for it
  during the `repair_leash` cooldown, so a unit that stops and starts in a fight doesn't make builders take and drop
  its job over and over. The old leash only fired after a builder had reached build range and then fell 300 elmos
  behind, so a builder as fast as or faster than the target chased it forever.
- **Builders don't follow a repair target into a fight** (with `retreat.repair_safe`). While a builder repairs a
  mobile unit that is not retreating (priority or retreat repair), it leaves the job once the unit's spot has threat
  above the builder's strength in enemy-held area, the same rule job selection already used. The job stays open and
  is picked up again once the spot is safe.
- **Repair leash covers retreat repairs.** `repair_leash` now also applies to HIGH repair jobs (made when a unit
  retreats) once the unit is no longer retreating; before, it only covered priority (NOW) repairs.
- **Retreat repair ends with the retreat.** When a unit leaves its retreat (repaired, or its haven wait timed out), its
  leftover HIGH repair job is dropped, so its builder doesn't follow it back to the front. Priority (NOW) repairs stay,
  under the leash and flee checks.

- **Cheaper attack decisions and firing spots, same behaviour.** Squads read each unit's health (engine calls) once
  per frame for their strength, instead of at every strength check; the firing-spot search used to read the health of
  every unit in the group for each unit it ordered. Firing spots are tried cheapest first, and the walk check (the
  costly part) only runs until a spot passes, so it usually runs once per unit instead of for all ~15 candidates; the
  pick is the same. Threat checks skip working out the allowance where there is no threat (it is always above 0).
  Attack and raid groups share one escape check (`ISquadTask::IsInDanger`, `PullBackEndangered`; they were copies),
  which asks the engine whether a unit is cloaked only for units standing in danger. Raid and anti-air use the shared
  strength sum. In the attack task, `Update()` is split into the per-update part and `Decide()`, the route helpers
  take the group's strength themselves, and the approach check reuses the line check.
- **Cheaper anti-air checks, same decisions except one.** AA target choice now judges aircraft nearest first and stops
  at the first that passes the leash and firing-spot threat checks, instead of judging every aircraft; leash home
  points and squad strength are only gathered once a candidate gets past the cheap checks (blip height, walkable). The ground-height check for
  unidentified radar blips (up to 9 elevation reads) stops at the first read that shows the blip is low, so a blip
  near sea level needs none and most ground blips one; the flee scan runs it only for blips within reach. The
  early-flee scan skips enemies too far from the leader before checking each squad unit. Path requests share one
  helper. One behaviour change: a hit on a squad unit no longer replaces the leader's path while one is still being
  computed (it checked the hit unit's query, so every hit on a non-leader sent a new path request and could drop an
  in-flight flee path).
- **Anti-air resumes hunting as soon as it reaches its retreat spot.** After retreating from a hit, an AA squad
  checked whether it had arrived only every 32 updates (about 30 s), and picked no targets until then. It now checks
  arrival on every update; only the re-plan of the path stays on the 32-update timer.
- **Anti-air flees from mobile bombs.** The early-flee check skipped enemies without an anti-land or anti-water
  weapon, meant for enemy AA, but mobile bombs (imp, snitch, skuttle) have no weapon either, only a death explosion.
  It now skips only units that can shoot aircraft and nothing else, so AA runs from a bomb within its blast radius
  + `flee_margin`.

- **Cheaper geothermal checks, same decisions except one 10 s delay.** Geo buildings are recognised from a list read
  once at start instead of an engine call per energy type, job and scout check. Vent search tries vents nearest
  first and stops at the first one that passes, and a vent the engine says can't be built on (a geo already stands
  there) is skipped for 10 s instead of being asked again on every energy pick, so a vent that frees up is noticed up
  to 10 s later. The vent scan reads features by id and checks each feature type once per game: one engine call per
  feature, where before it cost two wrapper objects and two calls (ZK maps have thousands of trees). A randomised model
  of old against new (200,000 cases each) picked the same vent and the same energy every time, and the vent search
  made about 70% of the old engine calls before the 10 s skip.
- **Faster constructor job choice and reclaim scans, same decisions.**
  - Job choice works out a job's travel cost first, and runs the reach and threat checks only for a job that
    would beat the best so far (nearby or far fallback). A randomised model of old against new picked the same
    job in all 200,000 cases, and in that model ran about a third of those checks (a sixth for a hiding commander).
  - Reclaim scans (new reclaim jobs, a running job picking its next wreck, and the constructor-count reclaim rule)
    read features by id and look up each feature type's metal once per game. Before, every feature cost two wrapper
    objects and up to five engine calls; one with no metal (ZK map trees) now costs one call. A running job checks reach only for a wreck that
    would beat its current pick, so it no longer runs a threat and path check on every tree around it.
  - No reclaim scan when every reclaim job is full and the cap on untaken jobs is reached.
  - `mex_escort` points that aren't `[number, number]` are skipped instead of raising a JSON error.
- **Faster spot picking for scouts and raid groups.** The score of every candidate spot is worked out first, and
  only a spot that would beat the best so far is checked for being ours (an engine query for geothermal vents) and
  for enemy defences. The pick is the same; in a randomised check of old against new it matched in all 200,000 cases
  and did about a fifth of those checks.
- **Roaming raid groups re-plan their route less often.** A group roaming with no enemies known asked for a new
  route to the same destination about every second. It now keeps the route it is walking for up to 3 s, unless there
  is danger at the destination, on the stretch just ahead or to any of its units, or the destination changed.
- **Attack groups re-plan routes to buildings, units standing still and the front less often.** A group planned a
  new route at every decision (about every 2 s), even when it was still walking to the same target or front. It now
  keeps the route it is walking for up to 3 s, while the target moved less than one path square (64 elmos on most
  maps), no unit is in danger, nothing unsafe lies just ahead and the ground it would fight the target from is still
  safe for its current strength. A moving target is re-planned as before, and so is any decision where a unit asked
  for a re-plan (danger ahead, a sudden threat) or right after a unit joined or left the group.
- Attack group target picking checks the cheap things (type, speed) before threat, reachability and height, and the
  front search no longer copies the map's cluster list on every call. Same picks.
- Attack group code reorganised for reading: the near-base threat reference is passed explicitly instead of being
  swapped in and out of the task, one helper runs every route, and the escape, flee destination and target
  selection are separate steps. Same behaviour.
- Scout quota config errors (a negative or non-number `scout` or `scout_max`, a non-number `scout_per_mex`, or a
  `scout_spot` that isn't an object) now log and use the default instead of raising a JSON error. A negative
  `scout_per_mex` used to turn the mex bonus off; it now logs and uses the default 1.0 (use 0 for off), the same
  rule as the raider keys. An explicit `null` for a scout key now means its default too (it used to read as 0, so
  `"scout_max": null` meant no cap), and `true`/`false` are no longer read as 1/0.

- Retreated units waiting to be repaired (at a haven or by builders) now patrol in place instead of back and forth
  between two points, so they stay in range of whatever is repairing them. They still return fire.
- Retreated units now stop well inside the repairer's range (60% of it plus the unit's own radius) before waiting.
  Before, the old patrol point could fall outside a sanctuary's nano tower range, so the unit was never repaired.
  When waiting for a builder, its own build range is used instead of the nano tower's.

### Fixed

- **Scout slots are per movement type and only for raiders that scout.** The scout quota (`quota.scout` + mex bonus)
  is worked out per movement type, but a raider with the scout attribute compared it with every scout task, so Ticks
  (`mine` role), Fleas, Darts, Swifts, Blastwings and scouts of other movement types used up its slots. It now counts
  only units of its own movement type that scout through the attribute. A `QUOTA scout|raider bonus` log line shows
  each bonus (reachable enemy mexes, enemy players, `*_map_size` multiplier / all players, `*_per_mex`) when it changes.

- **Defence cost refunds go to the right defence point.** `CDefenceMatrix::GetDefPoint` never kept the nearest
  distance it found (`tmp = dist` instead of `dist = tmp`), so in a cluster with several defence points a destroyed or
  cancelled tower's metal could be refunded to the wrong point: one point then looked defended while it wasn't, and
  another would rebuild a tower it already had. A destroyed tower's refund also no longer takes a point's cost below 0
  (`GetDefPoint` falls back to the cluster's first point even when that point holds less metal).

- **ms_pull splits metal again** (`economy.json` `ms_pull` and its multipliers). It had little or no effect:
  - A factory's own build power was left out of the factory pull when it picked its next unit (it had just left its
    last job), so with one factory and no caretakers on it the factory never waited, whatever `ms_pull` said.
  - Since the stall rule, constructors held all new spending in a stall whatever `ms_pull` said.
  - Now, in a metal stall (the constructors' latch, for both sides), a constructor may start new spending while
    constructors pull at most `ms_pull` x the factories, its own build power included; a factory waits 3 s while
    factories, it included, x `ms_pull` pull more than the constructors (not while no constructor spends). An energy
    stall still holds all new constructor spending. The constructors' flee-turret skip uses the same share.

- **Attack groups in line merge again.** A route to another group (reinforcing, idle merging, a rally) ends where the
  line's front row stands, but merging is judged at the group's leader, which stands up to the line's depth behind
  (its row, or turned by facing). A group whose leader walked in a back row could park just outside merge reach
  (`travel_sec` at its slowest speed, with the 20% margin of `reinforce_range`) and never merge. Such routes now end
  the line's depth closer, the merge reach allows for it too, and with no line nothing changes.
- **Attack groups in line engage again.** Two things kept a group in line standing just outside engage range,
  facing the enemy without attacking. A group engages once its leader is within the group's highest weapon or sight
  range of the target, but a route to a target ends where the line's front row stands, and the leader stands its
  row's offset behind (rows are 64 to 90 elmos apart, up to 256 with enemy splash near). So a group whose leader
  walked in a back row stopped short, and a new route from there ended at the same spot. Target routes now end the
  leader's row offset closer; with the leader in the front row, or no line, nothing changes. And a line standing
  facing (a hold, the end of a walk) ignored a new path ending within 256 elmos of where it faced from, so it didn't
  move for a target just out of range; it now always walks a route to a target. Since holding groups stand in line,
  facing the threat, this happened often.
- **Geothermal plants are actually queued.** Three checks shared with other energy kept geo off the build list in
  practice. The energy-job cap allowed a 500-metal job only while no energy job at all was queued (below 62.5 build
  power), and solar or wind jobs nearly always were; geo now has its own cap of one geo job at a time. The energy
  pick ran only during an energy stall or for an idle constructor with spare metal and nothing else to do; the vent
  check now also runs on every economy pass, like mex. And in a stall the quick-build test used a quarter of the time
  budget, which needed an income of about 22 at 10 minutes; geo now always uses the full 40 s budget (income about 11
  at 10 minutes, 15.5 at the 400 s `since`).
- **Guard slots fill again with riots.** Since `quota.attack_merge.base_gather` false (the shipped default), new
  riots no longer wait in a defend group at base: they walk out alone to reinforce an army. The guard pull skipped
  every unit in an attack task, so riots could only get a slot in the moment they left the factory, and only if a
  builder needed a guard right then. The pull now also takes a unit that is alone in its attack task, isn't fighting
  and isn't the rally host. Squads of two or more are still left alone.

- The `retreat.repair_pull` share cap now also applies when idle builders choose their next job. Before, it only
  limited builders pulled off other work. Urgent jobs outrank everything in job selection, so every idle builder
  took a priority repair whenever there were enough of them, and other work such as metal extractors stalled.
  The cap now applies even when the pull is turned off (`range` 0).
- A guard slot that cancelled itself left a stale entry behind in the guard list.
- The guard safe check subtracted the guard's full power from the allied influence at the builder. It now subtracts
  only what the guard actually adds there, so builders next to turrets count as safe as intended.

## 2026-10-31: Priority repair and escort builders ([#1](https://github.com/tellistone/CircuitAI-wires3/pull/1))

### Added

- **`prio_repair` unit attribute.** Any unit type in `behaviour.json` can now get the urgent builder repair
  that only units with the `heavy` main role got before, without changing its role:
  ```json
  "tankassault": { "role": ["assault"], "attribute": ["prio_repair"] }
  ```
  - Mobile units: when damaged below 90% health but still above their retreat threshold, a builder is sent to repair them.
  - Static units: a finished building with `prio_repair` gets the same urgent repair when damaged below 90% health.
  - Commanders have `prio_repair` by default (every morph level, including dynamic commanders).
- **Builder pull for priority repair** (`retreat.repair_pull: [range, share]`). If nobody takes an urgent
  repair job, the nearest reachable builder within `range` drops its current work for it. The commander is
  included. Builders already on urgent work or another repair are left alone. At most `share` of all builders
  (minimum 1) are on priority repair at once. `range` 0 turns it off.
- **Safety options for priority repair** (`retreat.repair_safe`, `retreat.repair_safe_static`, default `false`).
  When `true`, builders won't take, be pulled onto, or keep a priority repair where enemy threat is higher than
  the builder's own strength in enemy-held ground. Set separately for mobile and static targets.
- **Repair leash** (`retreat.repair_leash: [distance, cooldown]`, `[300, 20]` in the shipped config). Once a
  builder has reached a unit, the priority repair is cancelled if the unit drives more than `distance` beyond
  build range, so builders stop chasing units back into battle. That unit gets no new priority repair for
  `cooldown` seconds. Retreating units are exempt. `distance` 0 turns it off.
- **Retreat wait timeout** (`retreat.wait_timeout`, seconds, default 60, 0 = no limit). A retreated unit waiting
  at its haven with nobody repairing it goes back to duty when the timeout runs out. The timer restarts while a
  builder is assigned to repair it. Planes are not affected.
- **Escort builders** (new `escort` section). Strong attack groups get a builder that follows them, repairing and
  reclaiming as it goes:
  - `min_power`: attack power a group needs to get and keep an escort (0 = feature off).
  - `share`: max share of builders on escort duty (rounded down).
  - `behind`: how far behind the group leader, toward base, the escort stays.
  - `follow`: past this distance from its spot, the escort catches up along a threat-aware path.
  - `range`: only builders within this distance of a group member are picked (default 600).
  - `comm`: allow the commander as an escort.
  - `take_from`: highest job priority an escort can take a builder off: `"idle"`, `"low"`, `"normal"` or
    `"high"` (default). Urgent jobs, repairs and other escorts are never interrupted.

  The escort patrols (auto repair and reclaim) while the group holds or advances, and plain-moves while the group
  falls back toward base. It ends when the group is gone, its power drops below `min_power`, or it goes somewhere
  the builder can't reach.

### Fixed

- Urgent repair jobs were cancelled as soon as a builder started moving toward a unit in combat, because the
  general path safety check treated the combat area as unsafe. This is why heavy-unit repairs rarely happened.
  Urgent repairs now use the same rule as job selection.
- Commanders and builder units with the `heavy` role (such as Funnelweb) never asked for priority repair, because
  the request was only made while on a fighting job. Build, repair and wait jobs now make it too.
- A repair job nobody took now closes once its target is back to full health, instead of lingering.
- An existing repair job for a unit is now raised to the higher priority when a more urgent request comes in.
- Builders no longer take repair jobs targeting themselves.

### Notes

- `behaviour.json` documents all new settings. Unknown `escort.take_from` values are logged and treated as `"high"`.
- Built and compiled for Linux and Windows. Not yet tested in a full game.
