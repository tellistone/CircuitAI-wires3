# Changelog

Changes in this fork (`tellistone/CircuitAI-wires3`, branch `zk`) on top of upstream
[rlcevg/CircuitAI](https://github.com/rlcevg/CircuitAI) `zk` (last upstream commit: `e51ba92` "Fix indentation", 2026-06-04).

## Unreleased

### Added

- **`guarded` unit attribute.** Any unit type with `"attribute": ["guarded"]` (for example artillery) gets a guard
  slot that follows all the `guard` rules, from when it finishes until it dies. A builder with the attribute gets a
  slot whatever `max_workers` says. Guarded units never guard others and don't count toward `max_share`. Flying units and static defences get no slot.
  The safe check leaves out the guarded unit's own influence, so an armed unit can't call its own spot safe.
  For an army unit the safe check also looks at its squad's attack or defend point, the way a builder's job location is
  checked, so artillery gathering at base for an attack takes a guard along.
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
