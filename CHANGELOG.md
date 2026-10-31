# Changelog

Changes in this fork (`tellistone/CircuitAI-wires3`, branch `zk`) on top of upstream
[rlcevg/CircuitAI](https://github.com/rlcevg/CircuitAI) `zk` (last upstream commit: `e51ba92` "Fix indentation", 2026-06-04).

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
