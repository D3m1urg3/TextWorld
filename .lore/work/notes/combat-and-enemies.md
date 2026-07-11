---
title: "Implementation notes: combat-and-enemies"
date: 2026-07-11
status: in_progress
tags: [implementation, notes, combat, enemies, spells, determinism]
source: .lore/work/plans/combat-and-enemies.md
modules: [systems, mutations, action, architect, render, prose, seed, combat]
related: [.lore/work/specs/combat-and-enemies.md, .lore/work/brainstorm/combat-and-enemies.md]
---

# Implementation notes: combat-and-enemies

Executing the approved plan **inline** (no subagents, per standing rules
[[no-implement-subagents]] and [[verification-must-be-bounded]]). One step at a
time, each on branch `feat/combat-and-enemies`, gate run + shown, one commit per
step, STOP for go-ahead between steps.

Validation gate per step: `cmake --build build` + the step's exact `testCombat*`
in `./build/tests`.

## Progress tracker

**BRICK 1 — Foundation**
- [x] 1 — schema: health + hostile (+ version bump) → `testCombatSchema` ✅
- [x] 2 — seed hand-placed enemy → `testShippedSeedShape` ext. ✅
- [x] 3 — `Verb::Attack` + `resolveAttack` + `damageEntity` → `testCombatAttack` ✅
- [ ] 4 — enemy-turn system + chip lane → `testCombatChipClock`
- [ ] 5 — defeat + grimoire drop + downed → `testCombatDefeat`, `testCombatDowned`
- [ ] 6 — narration + template + HP status line → `testCombatRender`

**BRICK 2 — Telegraph / counter / Cast / cooldowns**
- [ ] 7 — schema: telegraph+cooldown+spells → `testCombatSchema` ext.
- [ ] 8 — telegraph→strike lane → `testCombatTelegraph`
- [ ] 9 — `Verb::Cast` + cooldown gate → `testCombatCastGate`
- [ ] 10 — counters Ward/Stun → `testCombatCounter`
- [ ] 11 — flee → `testCombatFlee`
- [ ] 12 — status line (cooldowns) → `testCombatStatusLine`

**BRICK 3 — Elements / four-lock closure / learning economy**
- [ ] 13 — schema: elements+locks+grimoire → `testCombatSchema` ext.
- [ ] 14 — element+resist+Fire/Frost → `testCombatElements`
- [ ] 15 — DoT → `testCombatDoT`
- [ ] 16 — defense lock (barrier/Dispel) → `testCombatDefenseLock`
- [ ] 17 — multiplicity lock (AoE/swarm) → `testCombatMultiplicity`
- [ ] 18 — grimoire→Read→learn → `testCombatLearn`

**BRICK 4 — Bestiary catalog / architect spawning / setting**
- [ ] 19 — bestiary catalog + refactor placement → `testBestiaryCatalog`
- [ ] 20 — eligible menu + gating + bootstrap + front → `testCombatGating`
- [ ] 21 — setting.txt invasion → `testArchitectSettingLoad`-style
- [ ] 22 — architect enemy spawn [HIGH — live LLM] → `testArchitectSpawn` (+ gated live)
- [ ] 23 — final validation sweep → `testCombatDeterminismReplay` + sweeps

## Log

### Setup
- Read plan/spec/brainstorm end to end. Verified Step 1 seams: `SCHEMA_DDL` in
  `src/world.cpp:11`, `SCHEMA_VERSION` in `src/world.hpp:12` (=1), test harness in
  `tests/tests.cpp` (`queryInt`, `TempDbFile`, `CHECK`, `main` runner ~:3241).
- Branch `feat/combat-and-enemies` created off `main`.

### Step 1 — schema: health + hostile (+ version bump) ✅
- `src/world.cpp`: added `health(entity PK, current, max)` + `hostile(entity PK,
  archetype TEXT, chip INTEGER)` to `SCHEMA_DDL`; updated `events.verb` comment to
  note combat verbs (comment-only, `events` is generic).
- `src/world.hpp`: `SCHEMA_VERSION` 1 → 2 (micro-decision 1: one additive DDL edit
  + one bump per brick).
- `tests/tests.cpp`: `testCombatSchema` — table existence + exact columns via
  `pragma_table_info`, fresh `openWorld` at bumped version; registered in `main`.
- Gate: `cmake --build build` clean; `./build/tests` → 1926 checks, 0 failures.
- Note: `pragma_table_info(...)` table-valued function used for column asserts
  (bundled sqlite supports it). No `chip` clamp/logic yet — that's Step 4.

### Step 2 — seed hand-placed enemy ✅
- `seed/base.sql`: entity 7 `goblin grunt` in the corridor (room 2) —
  `hostile(7,'goblin_grunt',1)`, `health(7,8,8)`, name, description, location.
  Tuning literals (micro-decision 4): player `health(3,12,12)`, goblin `8/8`,
  chip `1`, planned `kBasicAttackDamage=4` (2 hits to kill).
- **Minor divergence (approved-by-spec):** also added the player's `health(3,12,12)`
  row to `base.sql`. Plan Step 2 named only entity 7, but REQ-COMBAT-4 requires the
  player carry health and Step 4's chip gate needs it. Seed canon, not a mechanic.
- `tests/tests.cpp`: `testShippedSeedShape` extended — corridor has exactly one
  hostile with health row, chip>0, full health, name+description; player has a
  health row. Assertions scoped to hostile/player, so room/portable checks unaffected.
- Gate: build clean; `./build/tests` → 1932 checks, 0 failures.
- **Decision for behavior tests (Step 3+):** will add a dedicated
  `tests/combat_fixture.sql` (two rooms + goblin + player health) so combat
  behavior tests target a controlled world and stay robust as `base.sql` grows more
  enemies in Brick 3 — mirrors the existing fixture.sql/base.sql split.

### Step 3 — Verb::Attack + resolveAttack + damageEntity ✅
- `action.hpp`: `Attack` added to `Verb`; `subject` doc = target enemy (0 = the
  hostile in the room).
- **New TU `src/combat.{hpp,cpp}`** added to `twcore` (micro-decision 3). Owns
  `resolveAttack` + private `roomOf`/`hostileInRoom` (lowest-id living hostile in
  a room, deterministic). `kBasicAttackDamage = 4` exposed in the header for tests.
  `grep -En "INSERT|UPDATE|DELETE" src/combat.cpp` → empty (writes via mutations).
- `mutations.{hpp,cpp}`: `damageEntity(db, target, amount, actor, verb)` — clamps
  `current := clamp(current-amount,0,max)`, throws if no health row (before the
  event), then one paired event `(actor, verb, subject=target, object=amount)`.
- `systems.cpp`: `#include combat.hpp` + `Attack` case → `resolveAttack`.
- `parser.cpp`: `attack`/`hit`/`kill`/`fight` → `Verb::Attack` (no noun arg).
- `nlresolve.cpp`: `verbFromWord` + lowering switch gain `Attack` (argument-free);
  `emit_action` enum gains `attack`; ISA prompt now "eight verbs" + attack clause.
- **Event shape decision:** combat damage stores the amount in `events.object`
  (a per-verb number, read like moved's destination room) with `detail=NULL`.
- No render template yet — per the plan, Brick-1 verb templates (attacked/chip/
  defeat/downed) all land in Step 6; render emits nothing for unknown verbs.
- Tests: `tests/combat_fixture.sql` (new controlled world); `testCombatAttack`
  (empty-room refusal; exact floor damage; one event; player untouched pre-Step-4;
  stacking); `testNlResolveRequestBody` enum updated to the eight verbs.
- Gate: build clean; `./build/tests` → 1952 checks, 0 failures.
</content>
</invoke>
