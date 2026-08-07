---
title: "Implementation plan: combat-and-enemies"
date: 2026-07-10
status: executed
tags: [plan, combat, enemies, spells, cooldowns, determinism, architect, bestiary, grimoires]
modules: [systems, mutations, action, architect, render, prose, seed, combat]
related: [.lore/work/specs/combat-and-enemies.md, .lore/work/brainstorm/combat-and-enemies.md, .lore/vision.md, .lore/work/plans/ai-resolver.md]
---

# Implementation plan: combat-and-enemies

Turn-based, deterministic **puzzle** combat: enemies are locks, spells are keys,
the engine owns every number, no RNG. Source of truth:
**[.lore/work/specs/combat-and-enemies.md]** (39 requirements, prefix `COMBAT`).
This plan slices those requirements into atomic, mostly-deterministic bricks that
**mirror the shipped seams** — the ECS `SCHEMA_DDL` + `base.sql` seed, the
`runTurn` tick = transaction, the `Verb`/`Action` seam, `resolve`/`resolveImpl`
dispatch, the `mutations.cpp` write discipline, the `render()` /
`deterministicAppends()` / `buildFacts()` narration seams, the architect's
`create_room` tool/menu, and the `cannedToolUse` + injectable-`HttpTransport`
test discipline — rather than reinventing them (the resolver plan's method).

## Guiding constraints (from memory + spec + vision)

- **Deterministic skeleton first, LLM last.** Bricks 1–3 (Steps 1–18) are fully
  unit-testable with **no network**. The single live-LLM step (22, architect
  enemy spawning + combat NL/narration parity) is isolated, gated, and its
  assertions are **mechanical only** — no tune-retry loops. This is the
  [[token-risk-estimation]] / [[verification-must-be-bounded]] discipline: the
  token trap is open-ended live verification, not diff size.
- **The engine owns every number; no RNG; no growable stat.** All combat
  quantities are engine-owned constants (spec REQ-COMBAT-1, -14, -22, -29); the
  model only ever *selects* pre-authored content and writes prose. The schema
  carries **no** XP/level/growable column, ever.
- **Reuse proven seams, don't rebuild them** (table below).
- **One seam / one file / one testable behavior / one commit per step.** Target
  ≈ the resolver plan's granularity (~1.8 reqs/step).

## Seams this touches (verified in tree)

| Seam | File:line | What combat does with it |
|------|-----------|--------------------------|
| `SCHEMA_DDL` + `SCHEMA_VERSION` | `src/world.cpp:11`, `world.hpp` | Additive combat tables; one version bump per brick (micro-decision 1) |
| `base.sql` id ledger + seed | `seed/base.sql:1-10` | Hand-placed enemy (REQ-COMBAT-39), later bestiary/spell seed rows |
| `Verb` / `Action` | `src/action.hpp:11-17` | Add `Attack`, `Cast` (subject = spell/target); struct stays parser-free |
| `runTurn` tick transaction | `src/loop.cpp:50-60` | `resolveCombat(db, player)` inserted after `resolve(...)`, same txn (micro-decision 2) |
| `resolve` / `resolveImpl` | `src/systems.cpp:126-167` | New `Attack`/`Cast` cases; `resolveGo` gains the flee guard |
| mutation helpers (sole write path) | `src/mutations.hpp:30-58` | Add `damageEntity`, `applyStatus`, `defeatEnemy`, `downPlayer`, `learnSpell`, `dropGrimoire` |
| `render()` templates | `src/render.cpp:100-160` | Combat-event templates + shared `combatStatusLine` |
| `deterministicAppends` | `src/prose.cpp:118` | Status line append on the AI path (same seam as exits/inventory) |
| `buildFacts` | `src/prose.cpp:308` | Combat events → facts payload for AI narration |
| resolver ISA prompt + `emit_action` enum | `src/nlresolve.cpp` | Add `attack`/`cast` verbs (deterministic mechanical test; live in Step 22) |
| architect `create_room` tool + gate | `src/architect.cpp:200-302` | Extend to place ≤1 enemy from an engine-computed eligible menu |
| test discipline (`cannedToolUse`, fake `HttpTransport`, `testShippedSeedShape`) | `tests/tests.cpp:1349`, `:3267` | Every new `testCombat*`; live smoke gated by `TEXTWORLD_AI_LIVE_TEST=1` |

## Four micro-decisions (flagged, not blocking)

1. **Schema evolution = one additive `SCHEMA_DDL` edit + one `SCHEMA_VERSION`
   bump per brick** (4 bumps). `world.cpp:112-121` already documents the
   no-migrations / delete-and-recreate stance, so each bump makes a
   mid-implementation world file *detected as stale* rather than silently
   half-shaped. Recommended over a single big-bang schema so each table lands
   with the behavior that needs it.
2. **Enemy turn resolves in a new `resolveCombat(db, player)` called from
   `runTurn`** (`loop.cpp`, immediately after `resolve(...)`, inside the same
   transaction — mirroring how the loop, not `resolve`, owns `meta.turn++`). It
   keys on the hostile that shared the player's room **at tick start** (captured
   before the player action), so a flee still eats the enemy's single parting
   turn (REQ-COMBAT-26) even though the player has physically moved. Alternative
   (call inside `resolveImpl`) rejected: `resolveImpl` is per-verb and
   transport-threaded for `Go`; combat is verb-agnostic and deterministic.
3. **Combat logic lives in a new `src/combat.{cpp,hpp}` TU** added to `twcore`
   (mirroring the `nlresolve` / `architect` split), not grown into
   `systems.cpp`. It owns the enemy-turn system, damage/status math, and the
   gating menu. All *writes* still route through `mutations.cpp` — the sole
   sanctioned write path — so `grep -En "INSERT|UPDATE|DELETE" src/combat.cpp`
   stays empty except where it legitimately calls helpers (helpers do the SQL).
4. **Instance stat provenance is a layered concern.** Bricks 1–3 seed the
   hand-placed enemy's numbers as **literals** in `base.sql`. Brick 4 introduces
   the `bestiary` catalog and refactors *placement* to copy archetype constants,
   adding a test that the seed enemy's literals **equal** the catalog row
   (REQ-COMBAT-30). The catalog is thus a clean later layer, not a brick-1
   dependency.

---

## Brick & step sequence

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>BRICK 1 — Foundation</b> <span style="color:#666;">(the loop exists, someone can die — deterministic)</span><br>
&nbsp;&nbsp;<b>1</b> schema: health + hostile ─▶ <b>2</b> seed enemy ─▶ <b>3</b> Attack verb ─▶ <b>4</b> enemy-turn + chip ─▶ <b>5</b> defeat/drop + downed ─▶ <b>6</b> narration + status(HP)<br>
<b>BRICK 2 — Telegraph / counter / Cast / cooldowns</b> <span style="color:#666;">(deterministic)</span><br>
&nbsp;&nbsp;<b>7</b> schema: telegraph+cooldown+spells ─▶ <b>8</b> telegraph→strike ─▶ <b>9</b> Cast + cooldown gate ─▶ <b>10</b> counters Ward/Stun ─▶ <b>11</b> flee ─▶ <b>12</b> status line (cooldowns)<br>
<b>BRICK 3 — Elements / four-lock closure / learning economy</b> <span style="color:#666;">(deterministic)</span><br>
&nbsp;&nbsp;<b>13</b> schema: elements+locks+grimoire ─▶ <b>14</b> element+resist+Fire/Frost ─▶ <b>15</b> DoT ─▶ <b>16</b> defense lock (barrier/Dispel) ─▶ <b>17</b> multiplicity lock (AoE/swarm) ─▶ <b>18</b> grimoire→Read→learn<br>
<b>BRICK 4 — Bestiary catalog / architect spawning / setting</b> <span style="color:#666;">(deterministic body + one live tail)</span><br>
&nbsp;&nbsp;<b>19</b> bestiary catalog + refactor placement ─▶ <b>20</b> eligible menu + gating + bootstrap + front ─▶ <b>21</b> setting.txt invasion ─▶ <b>22</b> architect enemy spawn <span style="color:#b00;">[HIGH — live LLM]</span> ─▶ <b>23</b> final validation sweep<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified, no network · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span> gated live verification, mechanical assertions only.
**Size** = implementation complexity (S/M/L). **Token-risk** = verification token cost (the trap is live-LLM loops, not diff size), so every deterministic step is LOW regardless of Size; only Step 22 is HIGH.

---

## BRICK 1 — Foundation

> Goal: the combat loop exists and someone can die, entirely deterministically.
> An empty spellbook still beats the seed enemy (basic-attack floor); chip makes
> HP a clock; defeat drops a grimoire item; player death downs-and-relocates.

### Step 1 — Combat schema: `health` + `hostile` (+ version bump)
**Requirements:** REQ-COMBAT-4, -5 (partial). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add to `SCHEMA_DDL` (`src/world.cpp:11`), and bump `SCHEMA_VERSION` (`world.hpp`):
```sql
CREATE TABLE health(entity INTEGER PRIMARY KEY, current INTEGER, max INTEGER);
CREATE TABLE hostile(entity INTEGER PRIMARY KEY, archetype TEXT, chip INTEGER);
```
`current` is clamped to `[0, max]` in code (Step 4's helper), not by DDL. `hostile`
carries the archetype **tag string** (REQ-COMBAT-5's "reference to bestiary
archetype"; the catalog it references arrives in Brick 4) and the per-instance
`chip` constant (REQ-COMBAT-12; literal per micro-decision 4). No new event verbs
need DDL — `events` is generic (`world.cpp:25`); the combat verb vocabulary is a
comment update only.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> succeeds; extend
<code>testWorld</code> (or a new <code>testCombatSchema</code>) to assert the
<code>health</code> and <code>hostile</code> tables exist with the expected columns
(query <code>sqlite_master</code> / <code>PRAGMA table_info</code>), and that a
fresh <code>openWorld</code> succeeds with the bumped <code>SCHEMA_VERSION</code>.
No network.
</blockquote>

### Step 2 — Seed a hand-placed enemy in `base.sql`
**Requirements:** REQ-COMBAT-39, -5, -12 (seeded chip), -30 (canon substrate). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Extend the `base.sql` id ledger (`seed/base.sql:1-10`) with **entity 7**, a
`goblin grunt` hand-placed in the **corridor (room 2)**, mirroring how the seed
hand-places rooms/items: `entities(7)`, `hostile(7,'goblin_grunt', <chip>)`,
`health(7,<max>,<max>)`, `name(7,'goblin grunt')`, `description(7, …)`,
`location(7,2)`. Numbers are literals (micro-decision 4). This is the enemy the
foundation validation drives and the substrate architect-spawned enemies layer
onto (REQ-COMBAT-39).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> extend <code>testShippedSeedShape</code>
(<code>tests.cpp:3267</code>): assert the corridor contains one <code>hostile</code>
entity carrying a <code>health</code> row, a non-zero <code>chip</code>, and a
<code>name</code>/<code>description</code>. Deterministic, no network.
</blockquote>

### Step 3 — `Verb::Attack` + `resolveAttack` + `damageEntity`
**Requirements:** REQ-COMBAT-6, -8 (offense = one action), -18 (non-zero floor, single archetype), -38 (resolver ISA gains `attack`). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- `action.hpp`: add `Attack` to the `Verb` enum. `subject` reused as the target
  enemy id (0 → "the hostile in the room").
- `parser.cpp` (permanent fixed-verb fallback): map `attack`/`hit`/`kill`/`fight`
  → `Attack`.
- `nlresolve.cpp`: add `attack` to the `emit_action` **verb enum** and one crisp
  ISA-prompt clause (mechanical test only here; live NL in Step 22).
- `mutations.cpp/.hpp`: add `damageEntity(db, target, amount, actor, verb)` —
  the sole damage write path: `UPDATE health.current = clamp(current-amount,0,max)`
  **plus** a paired combat event. Never begins/commits (caller owns the txn).
- `combat.cpp`: `resolveAttack(db, player)` — find the hostile sharing the
  player's room; none → `failed` event ("There's nothing here to attack."); else
  `damageEntity(enemy, kBasicAttackDamage, player, "attacked")`. Fixed damage,
  no element, non-zero (REQ-COMBAT-6/-18). Wire the `Attack` case into
  `resolveImpl` (`systems.cpp`). Defeat handled in Step 5.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatAttack</code>: attacking the seed enemy
drops its <code>health.current</code> by exactly <code>kBasicAttackDamage</code>
and writes one <code>attacked</code> event; attacking in a hostile-free room →
<code>failed</code> event, no health change. Plus a mechanical
<code>testNlResolveRequestBody</code> extension: <code>emit_action</code> verb enum
now contains <code>attack</code>. Deterministic.
</blockquote>

### Step 4 — Enemy-turn system + chip lane, wired into the tick
**Requirements:** REQ-COMBAT-1 (no-RNG multi-actor tick), -2, -3, -9 (idle turn in brick 1), -12. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`combat.cpp`: `resolveCombat(db, player)`, called from `runTurn`
(`loop.cpp:53`, immediately after `resolve(...)`, **inside the same
transaction**). Per micro-decision 2 it takes the hostile that shared the
player's room **at tick start** (capture the id before `resolve` runs, pass it
in). Combat is active ⟺ that hostile has `health>0`. When active:
- **Turn action** (REQ-COMBAT-9): in Brick 1 the only choice is **idle** (no
  telegraph/strike lane yet — that is Step 8). The system exists and fires; it is
  the first non-player actor.
- **Chip lane** (REQ-COMBAT-12): always, in addition to the turn action, apply
  `damageEntity(player, hostile.chip, enemy, "chip")` — the irreducible clock.

Downed/defeat checks are Step 5; here the tick just applies chip and idles.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatChipClock</code>: one tick against the
seed enemy decreases <code>player.health.current</code> by exactly
<code>hostile.chip</code>; <code>meta.turn</code> increments exactly once; every
combat change has a paired <code>events</code> row with that turn number
(player-then-enemy in one transaction). A non-combat verb (<code>Wait</code>) in a
hostile room still applies chip (REQ-COMBAT-3/-8). Covers spec AI-Validation
items 2 &amp; 4. Deterministic.
</blockquote>

### Step 5 — Defeat + grimoire drop + the "downed" model
**Requirements:** REQ-COMBAT-20 (drops an item), -23, -24, -25. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Two death checks run at the end of `resolveCombat` (after both lanes):
- **Enemy `health.current == 0`** → `defeatEnemy(db, enemy, player)` (mutations):
  remove from play by deleting its `hostile` + `health` + `location` rows (the
  **entity id survives** — nothing is deleted from `entities`; defeat = absence of
  hostile/location, which persists across restart, REQ-COMBAT-30) and
  `dropGrimoire(db, enemy_archetype, room, player)` — mint a **portable** grimoire
  item (`portable`+`name`+`description`, located in the room) per a fixed
  archetype→item map. The `grimoire→spell` component + Read/learn is Step 18; here
  the *item* appears (satisfies AI-Validation item 3).
- **Player `health.current == 0`** → `downPlayer(db, player, enemy, player)`:
  relocate player to the **dormitory cell (room 1)**, restore `health.current = max`,
  **drop all carried portables** at the fall location, and **restore the enemy** to
  initial combat state (`health.current = max`; pending-strike reset is a no-op
  until Step 8). Clearing player status effects is a no-op until Brick 2/3 adds
  them (forward-ref). `known_spells` is untouched (table arrives Brick 2;
  invariant re-asserted in Step 18).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatDefeat</code>: drive the seed enemy to
0 → its <code>hostile</code>/<code>location</code> rows are gone, its
<code>entities</code> row remains, and a portable grimoire item is in the corridor.
<code>testCombatDowned</code>: drive player to 0 (script many chip ticks) → player
is in room 1 at full health; all previously-carried items are at the fall room; the
enemy is back at full health. Covers AI-Validation items 3 &amp; 11 (the
<code>known_spells</code> clause of 11 is completed in Step 18). Deterministic.
</blockquote>

### Step 6 — Combat-aware narration + template fallback + HP status line
**Requirements:** REQ-COMBAT-37 (template path), -15 (HP portion). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- `render.cpp`: templates for the combat event verbs (`attacked`, `chip`, enemy
  defeated, player downed) in the same dumb-template style as `took`/`waited`.
- `prose.cpp` `buildFacts` (`:308`): include combat events in the facts payload so
  AI prose is grounded in the turn's combat events (no new nouns/numbers invented).
- **Status line seam:** add a shared `combatStatusLine(db, player)` helper appended
  by **both** paths whenever a hostile shares the player's room — from `render()`
  (template) and from `deterministicAppends` (`prose.cpp:118`, the AI path, same
  seam that already appends exits/inventory). Brick 1 content: `HP: 12/12`
  (cooldown fields join in Step 12). This line is engine-authored, never the
  model's (REQ-COMBAT-15/-37).
- **Standing template rule (binds Steps 8–18):** every subsequent step that
  introduces a new combat event verb (telegraph, struck, ward-block,
  stun-interrupt, cast-declined, Fire/Frost/Dispel, DoT tick/expire,
  barrier-block, AoE-hit, swarm-defeat, grimoire-read) **adds its `render.cpp`
  template + a template-path (AI-disabled) render assertion in the same commit**
  — the permanent-fallback obligation (REQ-COMBAT-37) is discharged verb-by-verb
  as the verb is born, never deferred to the live tail (Step 22 is
  mechanical-only and cannot catch a missing template). Step 23 closes the loop
  with an exhaustive check.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatRender</code>: an attack tick renders a
combat line via the template renderer; the <code>HP: n/max</code> status line is
present on combat ticks and <b>absent</b> on non-combat ticks; with AI disabled
(<code>testLoop</code>-style, no key) the full Brick-1 loop is playable end to end.
Deterministic (template path); the AI path is exercised live in Step 22.
</blockquote>

---

## BRICK 2 — Telegraph / counter / Cast / cooldowns

> Goal: the timing puzzle. Enemies wind up a telegraphed strike; the player's
> single action between telegraph and strike is the counter window; Ward blocks,
> Stun interrupts; spells are gated by per-spell cooldowns on the global tick
> clock; the status line reports readiness. Flee becomes a real tactic.

### Step 7 — Schema: telegraph, cooldowns, spell catalog, known_spells, status effects
**Requirements:** REQ-COMBAT-13 (cooldown substrate), -19 (CC substrate). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`SCHEMA_DDL` additions + version bump:
```sql
CREATE TABLE spell_catalog(spell TEXT PRIMARY KEY, element TEXT, cooldown INTEGER,
                           tier INTEGER, effect TEXT);      -- engine-owned constants
CREATE TABLE known_spells(entity INTEGER, spell TEXT, PRIMARY KEY(entity, spell));
CREATE TABLE cooldowns(entity INTEGER, spell TEXT, ready_turn INTEGER,
                       PRIMARY KEY(entity, spell));         -- cast at T → ready_turn = T+cooldown
CREATE TABLE pending_strike(entity INTEGER PRIMARY KEY, damage INTEGER, element TEXT);
CREATE TABLE status_effects(entity INTEGER, kind TEXT, magnitude INTEGER,
                            remaining INTEGER, PRIMARY KEY(entity, kind));  -- DoT/CC
```
`base.sql` seed additions: `spell_catalog` rows for **Ward** (block, no element,
cooldown) and **Stun** (CC, cooldown); seed the **player's `known_spells`** with
Ward + Stun (a student starts knowing basic counters — makes telegraph/counter
exercisable per the confirmed Brick 2/3 boundary); add a `telegraph_period`
constant to the seed enemy (columns on `hostile`, literal). No growable columns
anywhere (guards REQ-COMBAT-22, asserted in Step 18).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatSchema</code> extended: the five new
tables exist with expected columns; the seed player's <code>known_spells</code>
holds exactly {Ward, Stun}; <code>spell_catalog</code> holds their constant rows;
fresh <code>openWorld</code> succeeds at the new version. Deterministic.
</blockquote>

### Step 8 — Telegraph → strike lane in the enemy-turn system
**Requirements:** REQ-COMBAT-9 (real turn action), -10, -12 (strike + chip on land tick), -17 (telegraph lock). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Replace Step 4's "idle" turn action with the deterministic telegraph schedule
(driven by `telegraph_period`, a per-archetype constant — no RNG):
- **Telegraph tick:** emit a `telegraph` event (narrated as a wind-up) and write a
  `pending_strike` row (row exists = strike pending, damage/element from
  constants). One-tick wind-up (REQ-COMBAT-10).
- **Strike tick** (the enemy's *next* turn): if a `pending_strike` row still
  exists and was not countered, `damageEntity(player, pending.damage, enemy,
  "struck")` then delete the row. Chip still applies **in addition** (a land tick
  deals strike + chip, REQ-COMBAT-12).
- `downPlayer` (Step 5) and `defeatEnemy` now also clear `pending_strike`
  (fight-reset, REQ-COMBAT-25).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatTelegraph</code>: script telegraph tick
→ a <code>pending_strike</code> row + <code>telegraph</code> event appear and no
strike damage lands that tick; next tick (no counter) → player loses
<code>strike + chip</code> and the row is cleared. Deterministic.
</blockquote>

### Step 9 — `Verb::Cast` + cooldown gating (declined without consuming a turn)
**Requirements:** REQ-COMBAT-7, -13, -14, -8 (cast = the action), -38 (resolver ISA gains `cast`). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- `action.hpp`: add `Cast`; `subject` carries the spell (resolve the spell name to
  a catalog key — reuse the `lookupNoun`-style pattern but against `spell_catalog`/
  `known_spells`, not entities).
- `parser.cpp`: `cast <spell>` / `<spell>` → `Cast`. `nlresolve.cpp`: add `cast` to
  the `emit_action` enum + ISA clause (mechanical test here; live in Step 22).
- **Availability gate — the key discipline** (REQ-COMBAT-7/-13): casting a spell
  **not in `known_spells`**, or **still on cooldown** (`cooldowns.ready_turn >
  now`), is **not a valid action** — declined *without consuming a turn*,
  consistent with the resolver/parser unrecognized-action contract. This gate runs
  in `runTurn`/resolution **before** the tick opens (mirroring how `Quit` and the
  tier-a "no Action" path avoid a tick), so no turn/`meta.turn`/enemy turn occurs.
- On a valid cast, `resolveCast` (combat.cpp) applies the spell effect (Brick 2:
  Ward/Stun — Step 10) and writes `cooldowns.ready_turn = now + catalog.cooldown`.
  Cooldowns are **immutable constants**, never reduced (REQ-COMBAT-14).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatCastGate</code>: casting an
unknown spell → declined, <b>no tick</b> (<code>meta.turn</code> unchanged, no
enemy turn); casting Ward → tick occurs, <code>cooldowns.ready_turn == now +
cooldown</code>; recasting Ward while on cooldown → declined, no tick; basic
<code>Attack</code> is never blocked. Mechanical resolver test: enum contains
<code>cast</code>. Deterministic.
</blockquote>

### Step 10 — Counter resolution: Ward (block) + Stun (interrupt/CC)
**Requirements:** REQ-COMBAT-11, -19 (CC), -17 (telegraph-lock keys). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

The counter window is the **single player action** between telegraph and strike:
- **Ward:** casting Ward sets a one-tick block flag on the player (a
  `status_effects` row `kind='ward'`, `remaining=1`); when the pending strike
  resolves in `resolveCombat`, a present ward **negates its damage** (block) and is
  consumed. Ward does not delete `pending_strike` conceptually, but the damage is 0.
- **Stun:** casting Stun writes a CC `status_effects` row on the **enemy** and
  **cancels the pending strike** (delete `pending_strike`) / suppresses the enemy's
  turn action for the CC duration (interrupt). CC ticks down deterministically each
  tick in `resolveCombat`.
- A wrong action or no counter → the strike lands (Step 8). Offense XOR defense
  holds: a tick is one player action (attack **or** cast), never both
  (REQ-COMBAT-8). `downPlayer` now also clears the player's `status_effects`
  (completes REQ-COMBAT-23's "clears active status effects").

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatCounter</code>: telegraph→Ward → strike
deals 0 (block); telegraph→(Attack) → strike deals its fixed damage; telegraph→Stun
→ <code>pending_strike</code> cancelled and enemy action suppressed for exactly the
CC duration, then resumes. No single tick records both a player attack-damage event
and a ward event (REQ-COMBAT-8). Covers AI-Validation items 5, 6, and the CC half of
9. Deterministic.
</blockquote>

### Step 11 — Fleeing + room-bound enemies
**Requirements:** REQ-COMBAT-26, -27. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Edit `resolveGo` (`systems.cpp`): when a hostile shares the player's room,
- through a **realized** (already-generated) exit → move succeeds; because
  `resolveCombat` keys on the tick-start hostile (micro-decision 2), the enemy
  resolves its **single ordinary turn once** as the player leaves — a pending
  telegraphed strike **lands** as that turn, else its normal action; chip still
  applies. No separate "parting hit" constant, no double strike.
- through a **latent** (NULL-dest) exit → **refused** (`failed` event), preventing
  mid-combat room generation (REQ-COMBAT-26). The architect is never called while a
  hostile is present.
- Enemies are **room-bound** (REQ-COMBAT-27): a fled enemy stays in its room in its
  current state (no chase), re-engageable on return — this falls out of the enemy
  having a fixed `location` and combat being room-scoped; assert it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatFlee</code>: fleeing via a latent exit
with a hostile present → refused, no generation, no move; fleeing via the realized
back-exit → succeeds, the enemy takes exactly one turn as you leave (pending strike
lands; chip applies), the enemy remains in its room, and on return its state is
unchanged. Covers AI-Validation item 12 (generation-disabled parts). Deterministic.
</blockquote>

### Step 12 — Engine-appended status line (cooldowns + HP)
**Requirements:** REQ-COMBAT-15. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Extend Step 6's `combatStatusLine` to the full deterministic form: for each known
spell, its readiness computed from `cooldowns` vs `meta.turn` (`Fire: ready · Ward:
2 · Stun: 1`) plus current/max health — the same always-appended, never-model
pattern as exits/inventory, from both `render()` and `deterministicAppends`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatStatusLine</code>: after casting a
spell with cooldown N, the status line counts down <code>Spell: N…1</code> then
<code>Spell: ready</code> at tick <code>T+N</code>; HP is shown; the line is
byte-identical on the template and AI-append paths; absent outside combat. Covers
AI-Validation item 7. Deterministic.
</blockquote>

---

## BRICK 3 — Elements, four-lock closure, learning economy

> Goal: elements/resistance make weakness the puzzle; the two remaining lock
> categories (defense, multiplicity) ship so all four of REQ-COMBAT-17 are
> expressed and unit-tested; DoT/CC round out REQ-COMBAT-19; the grimoire →
> Read → `known_spells` economy makes power = keys known, persisted forever.

### Step 13 — Schema: elements, resistances, barrier, grimoire component (+ bump)
**Requirements:** REQ-COMBAT-16 (substrate), -17 (substrate), -20 (grimoire→spell). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`SCHEMA_DDL` additions + bump:
```sql
CREATE TABLE resistance(archetype TEXT, element TEXT, multiplier_num INTEGER,
                        multiplier_den INTEGER, PRIMARY KEY(archetype, element)); -- integer ratio: no floats, no RNG
CREATE TABLE barrier(entity INTEGER PRIMARY KEY);   -- defense-lock state (row exists = warded)
CREATE TABLE grimoire(entity INTEGER PRIMARY KEY, spell TEXT);  -- the item→spell bridge
```
Resistance multipliers are **integer ratios** (num/den) to keep damage math exact
and deterministic (REQ-COMBAT-1 — no floats). `base.sql` seed: expand
`spell_catalog` with **Fire, Frost, Dispel** (elements + a multiplicity key —
finalized in Steps 14/16/17); add `resistance` rows for the seed archetypes; add
one seed enemy per remaining lock category (defense-lock and swarm — Steps 16/17)
as **bare hostile instances only**. Crucially, do **not** write the ironhide's
`barrier` row here — that row is added in Step 16 alongside the negation logic, so
Step 14's basic-attack floor test (which loops every seeded archetype) is not
silently satisfied by a barrier whose mechanics do not yet exist.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatSchema</code> extended: the three tables
exist; resistance rows are integer ratios; fresh world opens at the new version. No
network.
</blockquote>

### Step 14 — Element + resistance multiplier; Fire/Frost; basic-attack floor
**Requirements:** REQ-COMBAT-16, -18 (all archetypes), -17 (element lock). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`combat.cpp` damage path: applied damage = `base × resistance(archetype, element)`
(integer ratio), looked up **only for elemental attacks**; a basic attack
(element = none, Step 3) skips the table and deals its fixed base unmodified —
which is exactly what keeps the non-zero floor holding against **every** archetype
(REQ-COMBAT-18). Add Fire (element damage) and Frost (element damage + a slow, a
`status_effects` CC of small magnitude) as castable spells resolving through
`resolveCast`. Seed a **rime-touched** archetype (element lock, weak to Fire) so
the weakness/resist branches are exercised.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatElements</code>: Fire on the
Fire-weak archetype applies the weakness multiplier; a wrong element applies the
resist multiplier; basic <code>Attack</code> deals &gt;0 to <b>every</b>
seeded archetype tag (loop over every seeded archetype — the formal
<code>bestiary</code> catalog itself arrives in Step 19, so do not couple to it
here). The floor claim is scoped to archetypes without an active <code>barrier</code>
row; Step 16 re-verifies it holds after a barrier is stripped. Covers
AI-Validation item 8. Deterministic.
</blockquote>

### Step 15 — DoT status effect
**Requirements:** REQ-COMBAT-19 (DoT half). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

DoT is a `status_effects` row (`kind='dot'`, fixed `magnitude`, fixed
`remaining`) applied by a spell and ticked down deterministically each tick in
`resolveCombat`: fixed damage per tick via `damageEntity`, decrement `remaining`,
remove at 0. `applyStatus(db, entity, kind, magnitude, remaining, actor)` mutation
helper (shared by DoT and CC). Enemy-inflicted action-denial on the *player* stays
out of scope (spec REQ-COMBAT-19).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatDoT</code>: a DoT applies its fixed
damage for exactly its duration then stops; combined with Step 10's CC test this
closes AI-Validation item 9. Deterministic.
</blockquote>

### Step 16 — Defense lock: barrier state + Dispel (two-key sequence)
**Requirements:** REQ-COMBAT-17 (defense lock). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add the ironhide's `barrier` row here (deferred from Step 13) together with its
negation logic: while a `barrier` row is present (distinct from the player's Ward,
REQ-COMBAT-17), `damageEntity` against that enemy is negated. **Dispel** (a
castable spell) strips the barrier (delete the row); only then does damage land — a
two-key sequence (Dispel → any damage). This is the step that makes the barrier
mechanically real, so it also **re-asserts the REQ-COMBAT-18 floor** in its
post-strip form (basic attack deals >0 once the barrier is gone).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatDefenseLock</code>: attacks/spells on a
barriered enemy deal 0; after Dispel the barrier is gone and the next hit lands;
basic attack alone can never damage it through the barrier but can after Dispel
(floor still holds post-strip). Deterministic.
</blockquote>

### Step 17 — Multiplicity lock: AoE / DoT vs a small swarm
**Requirements:** REQ-COMBAT-17 (multiplicity lock), -19 (AoE reaching several bodies). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Seed a **book-swarm**: multiple low-HP hostile bodies co-located in one room
(spec's out-of-scope note excludes *overlapping telegraphs*, not multiple bodies,
so keep their turns simple — chip only, no telegraph). Add an **AoE** spell that
damages **all** hostiles in the room, and let DoT (Step 15) reach each body — the
keys that answer the multiplicity lock. `resolveCombat` already iterates the
room's hostiles for chip; extend the enemy-turn/defeat handling to the multi-body
case (each body defeated independently; drops per body).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatMultiplicity</code>: an AoE cast
damages every swarm body in one tick; a DoT applied to the swarm ticks each body;
single-target basic attack thins them one at a time. With Steps 8/14/16 this
proves all four lock categories of REQ-COMBAT-17 are expressed. Deterministic.
</blockquote>

### Step 18 — Grimoire → Read → `known_spells` learning economy
**Requirements:** REQ-COMBAT-20 (archetype→grimoire→spell mapping), -21, -22. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- `dropGrimoire` (Step 5) now also writes the `grimoire(entity, spell)` row per a
  **deterministic fixed** archetype→spell drop map (REQ-COMBAT-20; goblin_grunt →
  Fire grimoire, per the vertical slice). Never probabilistic.
- Reading a grimoire (extend the existing item-interaction path — a `read`/`Take`+
  use verb; wire through parser + resolver ISA) calls `learnSpell(db, player,
  spell)`: add to `known_spells` (**canon**, persisted). A learned spell is
  **permanent** — never removed by any mechanic, survives restart, survives
  `downPlayer` (re-assert). Reading an already-known grimoire is a **no-op
  success** (REQ-COMBAT-21).
- **Anti-goal guard (REQ-COMBAT-22):** progression is *only* new `known_spells`
  rows. Assert **no** XP/level/growable numeric column exists anywhere in the
  schema (a `sqlite_master` sweep test).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatLearn</code>: killing the seed enemy
drops a Fire grimoire; reading it adds Fire to <code>known_spells</code>; Fire is
still present after a simulated restart (reopen the db); reading it again is a no-op
success; a schema sweep finds no growable-stat column. Completes AI-Validation
items 10 and the <code>known_spells</code> clause of 11. Deterministic.
</blockquote>

---

## BRICK 4 — Bestiary catalog, architect spawning, setting

> Goal: the catalog becomes the mold instances are cast from; the architect
> spawns enemies by *selecting* from an engine-computed, player-state-gated
> eligible menu — the self-balancing lock-and-key economy — with the setting
> authorizing combat-inflected rooms. Steps 19–21 are deterministic; Step 22 is
> the single isolated live-LLM tail.

### Step 19 — `bestiary` catalog seed table + refactor placement to copy constants
**Requirements:** REQ-COMBAT-28, -29, -30. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add a `bestiary` seed table (SQL, loaded like `base.sql`): one frozen record per
archetype — `health, chip, telegraph_period, tier, blurb` scalar columns +
`resistance`/`drop`/`telegraph` detail via the child tables already added
(resistance in Step 13; a `drop_table(archetype, spell)` child row). The **blurb**
is the only archetype field the model ever sees (REQ-COMBAT-29). Refactor
placement into a `placeEnemy(db, archetype, room)` mutation helper that **copies
the catalog constants into a new instance** (`hostile`/`health`/name/desc rows).
**All four** seed archetype instances — goblin_grunt (Step 2), rime-touched
(Step 14), ironhide (Step 16), book-swarm (Step 17) — are re-expressed via
`placeEnemy`/catalog-copy in `base.sql`, replacing their scattered literals, so the
gate's "every seed instance equals its catalog row" claim is real and not narrower
than stated (micro-decision 4, REQ-COMBAT-30). The AI never sets or alters any
archetype number.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBestiaryCatalog</code>: every seed instance's
stats equal its <code>bestiary</code> row; defeat persists across a reopen; a
spawned instance is canon in <code>world.db</code>. Covers AI-Validation item 13
(deterministic parts). Deterministic.
</blockquote>

### Step 20 — Eligible menu + gating + bootstrap + front intensity
**Requirements:** REQ-COMBAT-32, -33, -34, -35 (disabled/failure → no spawn, deterministic parts). **Size:** L · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

The gem, and **wholly deterministic** — no LLM here. Kept as one step (against the
plan's ~1.8-req granularity) because the three sub-rules are not independently
shippable: the menu (gating) and the front (which rooms get a non-empty menu at
all) are two inputs to the *same* `eligibleArchetypes` return, and the bootstrap
rule is a special case of the menu for the empty-ledger world — splitting them
would produce steps that can't be validated in isolation. `combat.cpp`:
`eligibleArchetypes(db, room)` → the engine-computed menu:
- **Gating (REQ-COMBAT-32):** offer only archetypes whose lock the player can
  already solve — **all** keys the lock requires are in `known_spells` (both keys
  for a two-key defense lock) — **or** that are basic-attack-soluble and drop a
  key the player lacks. No deadlock by construction.
- **Bootstrap (REQ-COMBAT-33):** the first enemy the **architect** ever places in a
  world must be basic-attack-soluble and drop a **tier-1** grimoire. The ledger is
  scoped to **`placeEnemy` (architect) spawns only** — it must NOT count the
  seed-placed goblin_grunt (Step 2), or the rule would read "already bootstrapped"
  from tick 1 and never fire. Concretely: an `architect_spawn_count` in `meta` (or a
  provenance flag on placed instances) incremented only by architect placement, not
  seed inserts; the seed enemy is REQ-COMBAT-39's guarantee, the architect check is
  a distinct guarantee for grown worlds.
- **Front intensity (REQ-COMBAT-34):** a deterministic, engine-computed function of
  the room's **graph distance from the seed** — far = safe edge (empty menu), near
  = contested. One knob; the exact metric is tuning but must be repeatable.
- With generation disabled / on failure the caller spawns **no** enemy
  (REQ-COMBAT-35, deterministic half).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testCombatGating</code>: with <code>known_spells</code>
lacking Fire, the menu contains no fire-locked archetype; after learning Fire, they
become eligible; the first-ever <b>architect</b> spawn in a fresh world (with the
seed goblin_grunt already present) is still basic-soluble + tier-1 drop — asserting
the ledger ignores the seed enemy; a safe-edge room yields an empty menu. Covers
AI-Validation item 14. Deterministic, no network.
</blockquote>

### Step 21 — `setting.txt`: the invasion premise
**Requirements:** REQ-COMBAT-36. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Edit `seed/setting.txt`: add the invasion premise (goblins have breached the
lower/inner halls seeking the grimoires) as a **spreading front** — safe edges,
dangerous core — while preserving the existing hushed Thornmere tone. Tone only,
**no numbers** (the mechanical menu is the engine handshake, not prose the
architect "picks up"). This authorizes the architect's DNA to produce
combat-inflected rooms (REQ-COMBAT-34/-36).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectSettingLoad</code>-style check: the
seeded <code>meta.setting</code> contains the invasion premise and still contains
the hushed-tone anchors; contains no numeric stat. Deterministic.
</blockquote>

### Step 22 — Architect enemy spawning + combat NL/narration parity (live tail)
**Requirements:** REQ-COMBAT-31, -35, and the **live** halves of -37, -38. **Size:** M (code) · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>

> ⚠️ **The one high-token-risk step** ([[verification-must-be-bounded]]). Last,
> isolated, mechanical assertions only — never a prompt-tuning loop against live
> output. If a prompt needs work, that is a bounded, separate effort.

Extend the architect `create_room` tool (`architect.cpp`) so that, when the
engine's `eligibleArchetypes(db, room)` (Step 20) is non-empty, the tool schema
offers an **optional** `enemy` field constrained to a **schema-enforced enum of
the eligible archetype names** (blurbs only, never ids or numbers). The model may
**select at most one** or none; it **never invents** an archetype or stat. On a
valid selection the engine calls `placeEnemy` (Step 19) — the model picks the
costume, the engine mints the instance. Disabled/failed generation → no enemy
(REQ-COMBAT-35), room still created, same boundary as exits. The deterministic
selection→placement mapping is unit-tested with a **fake `HttpTransport`**
(`cannedToolUse`-style, extended for the enemy field); the **live** smoke (gated
by `TEXTWORLD_AI_LIVE_TEST=1`, structured like `testArchitectLiveSmoke`,
`tests.cpp:3250`) asserts only: a spawned enemy's stats equal the catalog; the
model wrote no number; NL combat ("burn it" → `Cast`, "swing at it" → `Attack`)
lowers or falls back cleanly; and an induced resolver/narration/generation failure
falls back silently to fixed-verb / template / no-spawn without corrupting combat
state.

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> deterministic: <code>testArchitectSpawn</code> with a
fake transport selecting an eligible archetype → <code>placeEnemy</code> writes a
catalog-equal instance; selecting an ineligible/absent enemy → no spawn; AI
disabled → no spawn, room still made. Live (gated, rare):
<code>testCombatLiveSmoke</code> asserts the mechanical invariants above only —
never model wording. Covers AI-Validation item 15 and the deterministic half of
13's "no archetype-number write" (<code>grep -En "INSERT|UPDATE|DELETE"
src/architect.cpp</code> stays empty; <code>src/combat.cpp</code> writes only via
helpers).
</blockquote>

### Step 23 — Final validation sweep against the spec checklist
**Requirements:** REQ-COMBAT-1 (determinism replay) + all (validation sweep). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** items 1–15 end to end and confirm each maps to a
green test. The one item without a dedicated home above is **item 1 (determinism
replay):** add `testCombatDeterminismReplay` — from a fresh seeded world, run an
identical scripted combat input sequence **twice**; assert the combat `events`
streams and final `world.db` state are **byte-identical** (the resolver plan's
db-byte-identity discipline). Also, against the **final** schema (after Brick 4's
`bestiary`/`drop_table`/tier columns land): re-run the REQ-COMBAT-22 growable-stat
`sqlite_master` sweep (no XP/level/growable numeric column anywhere), and grep the
whole combat surface for any RNG call (`rand`, `random`, time-seeding) → none
(REQ-COMBAT-1). Add an **exhaustive-template** assertion (Fix for REQ-COMBAT-37):
enumerate every combat event verb the schema can emit and assert `render()`
registers a template for each — no verb falls through to empty/generic output on
the AI-disabled path.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>./build/tests</code> green including every
<code>testCombat*</code>; <code>testCombatDeterminismReplay</code> byte-identical
across two runs; no RNG in the combat surface; each of the 15 AI-Validation items
maps to a named passing test. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-COMBAT-1 (determinism, no RNG) | 4, 23 |
| REQ-COMBAT-2 (tick = transaction) | 4 |
| REQ-COMBAT-3 (implicit combat entry) | 4, 5 (defeat/down ends), 11 (leave ends) |
| REQ-COMBAT-4 (`health` component) | 1 |
| REQ-COMBAT-5 (enemy = hostile + health + loc + archetype) | 1, 2 |
| REQ-COMBAT-6 (`Verb::Attack`, floor) | 3 |
| REQ-COMBAT-7 (`Verb::Cast` known spell) | 9 |
| REQ-COMBAT-8 (one action/tick, offense XOR defense) | 3, 4 (non-combat verb still triggers enemy), 9, 10 |
| REQ-COMBAT-9 (enemy one turn/tick) | 4, 8 |
| REQ-COMBAT-10 (telegraph wind-up) | 8 |
| REQ-COMBAT-11 (counter window) | 10 |
| REQ-COMBAT-12 (chip clock) | 2, 4, 8 |
| REQ-COMBAT-13 (per-spell cooldown, global clock) | 9 |
| REQ-COMBAT-14 (cooldowns immutable) | 9 |
| REQ-COMBAT-15 (engine-appended status line) | 6, 12 |
| REQ-COMBAT-16 (element + resistance) | 14 |
| REQ-COMBAT-17 (four lock categories) | 8/10 (telegraph), 14 (element), 16 (defense), 17 (multiplicity) |
| REQ-COMBAT-18 (basic non-zero to every enemy) | 3, 14 |
| REQ-COMBAT-19 (DoT + CC) | 10 (CC), 15 (DoT), 17 (AoE/multi-body) |
| REQ-COMBAT-20 (defeat → grimoire drop) | 5 (item), 18 (spell mapping) |
| REQ-COMBAT-21 (read → `known_spells`, permanent) | 18 |
| REQ-COMBAT-22 (only keys, no growable stat) | 18 |
| REQ-COMBAT-23 (downed, not dead) | 5, 10 (clears status) |
| REQ-COMBAT-24 (drops items, keeps knowledge) | 5 |
| REQ-COMBAT-25 (enemy restored on down) | 5, 8 (pending reset) |
| REQ-COMBAT-26 (flee via generated exit) | 11 |
| REQ-COMBAT-27 (enemies room-bound) | 11 |
| REQ-COMBAT-28 (bestiary catalog seed) | 19 |
| REQ-COMBAT-29 (archetype constants engine-owned) | 19 |
| REQ-COMBAT-30 (instances are canon) | 19 |
| REQ-COMBAT-31 (architect places ≤1, selects) | 22 |
| REQ-COMBAT-32 (menu gated by player state) | 20 |
| REQ-COMBAT-33 (bootstrap rule) | 20 |
| REQ-COMBAT-34 (front intensity, one knob) | 20 |
| REQ-COMBAT-35 (disabled/failure → no spawn) | 20 (det.), 22 (live) |
| REQ-COMBAT-36 (`setting.txt` invasion) | 21 |
| REQ-COMBAT-37 (narration + template fallback) | 6 + standing template rule (8–18, verb-by-verb), 23 (exhaustive check), 22 (live) |
| REQ-COMBAT-38 (NL combat via resolver) | 3, 9 (ISA ext.), 22 (live) |
| REQ-COMBAT-39 (seed hand-places an enemy) | 2 |

All 39 COMBAT requirements are stepped. Bricks 1–3 (Steps 1–18) and Steps 19–21
are deterministic and unit-testable with no network; **Step 22 is the single
isolated live-LLM tail** with mechanical-only assertions. Every step names a
concrete file/table/function and a concrete validation gate; every schema change
is one additive `SCHEMA_DDL` edit + one `SCHEMA_VERSION` bump (micro-decision 1);
no step introduces RNG, a growable stat, an id/number on the wire, or a write
outside the `mutations.cpp` helpers.
