# Changelog

Changes in this fork (`tellistone/CircuitAI-wires3`, branch `zk`) on top of upstream
[rlcevg/CircuitAI](https://github.com/rlcevg/CircuitAI) `zk` (last upstream commit: `e51ba92` "Fix indentation", 2026-06-04).

## Unreleased

### Added

- **Reload scoot for any squad unit** (per unit `reload_scoot`, default 0 = off; `true` = 0.8; per unit
  `reload_scoot_margin`, default 100, or the anti-heavy flee margin in anti-heavy squads). While engaged, a unit whose
  main weapon is less than `reload_scoot` reloaded and that stands in ground threat steps straight away from the
  target to just outside the target's ground range + margin (a margin further back each second while still in
  threat), but never to a hotter spot, and fights again once reloaded that far; the rest of the squad fights on. A unit
  outside all ground threat doesn't scoot (it fights on), and a scooting unit stops once it gets there. It turns back
  early when walking into range would take as long as the reload it has left, doesn't start when there isn't time to
  get out and back, and stays on hold position while scooting (else the engine's idle auto-attack walks it straight
  back in). Scoots end when the squad stops being engaged; a scooting leader keeps anti-air and anti-heavy squads
  engaged on their target. Anti-heavy's scoot uses the same rules now. A squad that is aborted or done gives its scooting units their
  move state back (it used to leave them on hold position for good). Works in attack,
  raid, defend, anti-air and anti-heavy squads (not mines or aircraft). It is anti-heavy's shoot and scoot, moved to
  `ISquadTask`; anti-heavy units without `reload_scoot` behave as before. No unit has it set yet.
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
