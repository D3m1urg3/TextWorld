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
- [x] 4 — enemy-turn system + chip lane → `testCombatChipClock` ✅
- [x] 5 — defeat + grimoire drop + downed → `testCombatDefeat`, `testCombatDowned` ✅
- [x] 6 — narration + template + HP status line → `testCombatRender` ✅ **(Brick 1 done)**

**BRICK 2 — Telegraph / counter / Cast / cooldowns**
- [x] 7 — schema: telegraph+cooldown+spells → `testCombatSchema` ext. ✅
- [x] 8 — telegraph→strike lane → `testCombatTelegraph` ✅
- [x] 9 — `Verb::Cast` + cooldown gate → `testCombatCastGate` ✅
- [x] 10 — counters Ward/Stun → `testCombatCounter` ✅
- [x] 11 — flee → `testCombatFlee` ✅
- [x] 12 — status line (cooldowns) → `testCombatStatusLine` ✅ **(Brick 2 done)**

**BRICK 3 — Elements / four-lock closure / learning economy**
- [x] 13 — schema: elements+locks+grimoire → `testCombatSchema` ext. ✅
- [x] 14 — element+resist+Fire/Frost → `testCombatElements` ✅
- [x] 15 — DoT → `testCombatDoT` ✅
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

### Step 4 — enemy-turn system + chip lane, wired into the tick ✅
- `combat.{hpp,cpp}`: `tickStartHostile(db, player)` (living hostile in the
  player's room, read-only) + `resolveCombat(db, player, hostile)` — no-op if
  hostile 0 or already at 0 health (player's action ended combat; removal is
  Step 5); else enemy idles (Brick 1) + chip lane `damageEntity(player, chip,
  hostile, "chip")`. New private readers `healthOf`/`chipOf`.
- `loop.cpp` `runTurn`: capture `startHostile` BEFORE the turn increment/resolve
  (micro-decision 2), then call `resolveCombat` after `resolve`, same transaction
  → player-then-enemy in one tick (REQ-COMBAT-2).
- Key mechanism: enemy turn keyed on the tick-START hostile, so a future flee
  (Step 11) still eats the parting turn even though the player has moved.
- `testCombatChipClock` (via runTurn): moving in doesn't chip (was in cell at tick
  start); Wait in the hostile room chips (non-combat verb still triggers enemy);
  attack lands strike+chip same tick; meta.turn +1/tick; chip event paired at the
  turn, actor=enemy→subject=player, and player's event precedes enemy's by id.
- Gate: build clean; `./build/tests` → 1981 checks, 0 failures. combat.cpp raw-write
  grep still empty.

### Step 5 — defeat + grimoire drop + downed model ✅
- `mutations.{hpp,cpp}`: `dropGrimoire(archetype, room)` mints a portable grimoire
  (fixed archetype→flavor map; goblin_grunt → "fire grimoire") and returns its id;
  `defeatEnemy(enemy, droppedItem, actor)` deletes hostile/health/location (entity
  + name survive, REQ-COMBAT-30) and emits one 'defeated' event (object=grimoire);
  `downPlayer(player, enemy, safeRoom, actor)` drops carried portables at the fall
  room, relocates player to safeRoom, restores player + enemy to full health, emits
  'downed'.
- **Event-shape decision:** grimoire drop is recorded by the 'defeated' event
  (object = minted grimoire), NOT its own verb — mirrors writeGeneratedRoom's
  mint-under-one-event, and keeps the Brick-1 verb set = {attacked,chip,defeated,
  downed} exactly as Step 6 enumerates.
- `combat.hpp`: `kDormitoryCell = 1` (seed safe room). `combat.cpp` resolveCombat
  now branches: tick-start hostile at 0 hp → defeat+drop (no enemy turn); else
  idle+chip, then if the player hit 0 → downPlayer. `archetypeOf` reader added.
- Tests: `testCombatDefeat` (rows gone / entity+name survive / grimoire in corridor
  / defeated event refs a portable); `testCombatDowned` (carry wand in, chip to 0,
  wake in cell at full HP, wand at fall room, enemy restored & room-bound, one
  downed event).
- Gate: build clean; `./build/tests` → 2052 checks, 0 failures. combat.cpp raw-write
  grep empty. AI-Validation items 3 & 11 (known_spells clause deferred to Step 18).

### Step 6 — combat narration + template fallback + HP status line ✅ (Brick 1 done)
- `combat.{hpp,cpp}`: `combatStatusLine(db, player)` — "" outside combat, else
  "HP: cur/max\n" (self-gating on a living hostile in the room). Cooldowns join
  Step 12.
- `render.cpp`: templates for `attacked`/`chip`/`defeated`/`downed` (object carries
  the engine-owned number; the model never sets it). Appends `combatStatusLine`
  unconditionally at the end (self-gating). Read-only preserved.
- `prose.cpp`: `deterministicAppends` appends the SAME `combatStatusLine` helper →
  byte-identical status line on template + AI paths (Step 12 verifies). buildFacts
  needed NO change — its `verb <> 'generated'` filter already surfaces combat events,
  and `object`(amount) is already excluded so the model never sees numbers.
- **Standing template rule now in force** (binds Steps 8–18): every step adding a
  combat event verb adds its render template + AI-disabled render assertion in the
  same commit.
- `testCombatRender`: no HP line out of combat; HP line on entering combat; attacked
  + chip lines + HP on an attack tick; defeated line + no HP after the kill; downed
  wake-line on the downing tick. Full Brick-1 loop playable end to end, AI-disabled.
- Gate: build clean; `./build/tests` → 2078 checks, 0 failures.

## BRICK 1 COMPLETE — the loop exists, someone can die, fully deterministic/offline.

### Step 7 — schema: telegraph + cooldowns + spells + status effects ✅
- `world.cpp` SCHEMA_DDL: `hostile` gains `telegraph_period`; five new tables —
  `spell_catalog(spell PK, element, cooldown, tier, effect)`, `known_spells`,
  `cooldowns(entity, spell, ready_turn)`, `pending_strike(entity PK, damage,
  element)`, `status_effects(entity, kind, magnitude, remaining)`. Version 2 → 3.
- Seed (base.sql + combat_fixture.sql): `spell_catalog` rows ward(cd2)/stun(cd3),
  both tier 1, no element; player `known_spells` = {ward, stun}; goblin
  `telegraph_period = 2`. **Spell keys are lowercase** to match the lowercasing
  parser/resolver input; status line will capitalize for display.
- `testCombatSchema` now opens `combat_fixture.sql` (DDL shapes + seed content):
  five tables' columns, hostile now 4 cols, player knows exactly {ward,stun},
  catalog rows have cooldown>0/tier=1, goblin telegraph_period>0.
- Gate: build clean; `./build/tests` → 2098 checks, 0 failures.

### Step 8 — telegraph → strike lane ✅
- `combat.hpp`: `kStrikeDamage = 5`. `combat.cpp`: `resolveCombat` enemy turn is
  now the state machine — pending_strike exists → land it (`struck`) + clear; else
  if `currentTurn % telegraph_period == 0` → `setPendingStrike` (`telegraph`, one-
  tick wind-up, no damage); else idle. Chip always applies (land tick = strike +
  chip). Helpers: `currentTurn`, `telegraphPeriodOf`, `pendingStrikeDamage`.
- `mutations`: `setPendingStrike(enemy, damage, element)` (upsert + 'telegraph'
  event, actor=enemy) and event-free `clearPendingStrike`; `defeatEnemy` also
  deletes pending_strike; `downPlayer` clears the enemy's pending (fight reset).
- `render.cpp`: `telegraph` ("winds up a heavy blow") + `struck` ("lands its blow
  … for N damage") templates (standing rule; asserted in testCombatTelegraph).
- **Schedule = global-tick modulo:** deterministic, replayable; no per-enemy turn
  counter needed. period 2 → telegraph on even ticks, strike on odd.
- `testCombatChipClock` (Step 4) updated: its tick-3 now accounts for the strike
  landing (attack + strike + chip in one tick) — the enemy no longer idles.
- `testCombatTelegraph`: robustly finds the telegraph tick (chip-only, pending set,
  "winds up" rendered), then the strike tick (strike+chip, pending cleared, "lands
  its blow"). Gate: build clean; 2097 checks, 0 failures. combat.cpp raw-write empty.

### Step 9 — Verb::Cast + cooldown gate ✅
- `action.hpp`: `Cast` verb + a dedicated `std::string spell` field. **Divergence
  (noted):** plan said "subject carries the spell" but subject is int64_t and
  spells are string-keyed (spell_catalog PK is TEXT), so the spell rides its own
  field — cleaner than overloading subject.
- `lookup.hpp`: `lookupSpell(db, word)` (catalog recognition, shared by parser +
  resolver). `parser.cpp`: `cast <spell>` and bare `<spell>` → Cast. `nlresolve`:
  ninth verb `cast` (enum + ISA clause + subject-is-spell), validateAndLower Cast
  case resolves subject→spell key.
- **Pre-tick gate** in `loop.cpp`: `castDenialReason(db, player, spell)` — unknown/
  unlearned or still-recharging → declined with NoTick (no turn, no enemy turn),
  mirroring Quit/tier-a. Cooldown gates availability, never costs a turn.
- `resolveCast` (in-tick): sets `cooldowns.ready_turn = executionTurn + catalog
  cooldown` (immutable, REQ-COMBAT-14) + 'cast' event. Effect (Ward/Stun) is Step
  10. `setCooldown` mutation helper. render 'cast' template (standing rule).
- **Cooldown timing:** gate (pre-tick) ready iff `ready_turn <= currentTurn+1`;
  resolveCast (post-increment) sets `now + cd`. Both reference the same execution
  turn — consistent; testCombatCastGate verifies cast at T → ready at T+cd.
- `testNlResolvePrompt`/`testNlResolveRequestBody` updated to nine verbs.
- Gate: build clean; `./build/tests` → 2125 checks, 0 failures. combat.cpp clean.

### Step 10 — counters: Ward (block) + Stun (interrupt/CC) ✅
- `mutations`: `applyStatus`/`clearStatus`/`tickStatusEffects` (event-free status
  bookkeeping, shared with DoT in Step 15); `downPlayer` now clears both combatants'
  status_effects (completes REQ-COMBAT-23).
- `resolveCast` dispatches on `spell_catalog.effect`: `ward` → one-tick `ward`
  status on the player; `stun` → `stun` status (kStunDuration=2) on the enemy +
  cancel its pending strike + `stunned` event.
- `resolveCombat`: enemy turn action SUPPRESSED while `hasStatus(enemy,'stun')`;
  a landing strike is negated + consumed if `hasStatus(player,'ward')` (`warded`
  event), else lands. `tickStatusEffects(player)`+`(enemy)` after the enemy turn
  (a same-tick ward/stun still applies this tick, then counts down). Chip always,
  even while stunned (REQ-COMBAT-12 irreducibility).
- **Timing model:** telegraph on tick T-1, strike resolves in resolveCombat on
  tick T (after the player's tick-T action) → the counter is the tick-T cast. Ward
  is set and consumed within tick T (no cross-tick persistence needed).
- render `warded` + `stunned` templates (standing rule). `testCombatCounter`: ward
  blocks (chip only, no attacked event same tick — offense XOR defense), no-counter
  strike lands (strike+chip), stun cancels+suppresses for the duration then resumes.
- Gate: build clean; `./build/tests` → 2173 checks, 0 failures. combat.cpp clean.
  Covers AI-Validation items 5, 6, and the CC half of 9.

### Step 11 — fleeing + room-bound enemies ✅
- `combat.{hpp,cpp}`: `hostileInRoom` promoted to public (moved out of the anon
  namespace) so `resolveGo` can consult it.
- `systems.cpp` `resolveGo`: a **latent** exit with a hostile present is refused
  BEFORE any architect call ("You can't flee into the unknown…") — no mid-combat
  generation (REQ-COMBAT-26). A **realized** exit needs no special code: the
  enemy's single parting turn falls out of resolveCombat keying on the tick-start
  hostile (micro-decision 2). Guard triggers only with a hostile → hostile-free
  worlds unchanged.
- Room-bound (REQ-COMBAT-27) is emergent: nothing moves the enemy, so it stays
  put and is re-engageable, unchanged, on return.
- `testCombatFlee`: (1) AI-ON + fake transport → latent flee refused, transport
  never called (calls==0), no move/no new room; (2) AI-off → realized flee moves
  the player, one chip event marks the parting turn, enemy stays in the corridor
  at unchanged health across return.
- Gate: build clean; `./build/tests` → 2201 checks, 0 failures. Covers
  AI-Validation item 12 (generation-disabled parts).

### Step 12 — engine-appended status line (cooldowns + HP) ✅ (Brick 2 done)
- `combatStatusLine` extended: `HP: c/m · Spell: ready|N · …` for each known spell,
  alphabetical (deterministic), remaining = ready_turn - now, no cooldown row ⟹
  ready. `capitalize` first letter for display. Same helper both paths append.
- **Gate/line consistency fix:** `castDenialReason` cooldown check changed from
  `> currentTurn+1` to `> currentTurn`, so "ready" on the line ⟺ castable next
  action; cast at T declined for ticks T+1..T+cd, castable at T+cd (matches
  "declined for exactly N ticks" + "ready at T+N"). Updated Step-9 test's resume
  loop (`turn() < readyTurn`).
- `testCombatStatusLine`: absent outside combat; HP + both spells ready on entry;
  "Ward: N" on cast counting down to "Ward: ready" at T+N; render() output ends
  with exactly `combatStatusLine(db,3)` (byte-identity by shared helper). Covers
  AI-Validation item 7.
- Gate: build clean; `./build/tests` → 2215 checks, 0 failures.

## BRICK 2 COMPLETE — telegraph/counter/Cast/cooldowns; the timing puzzle works.

### Step 13 — schema: elements, resistances, barrier, grimoire (+ bump) ✅
- `world.cpp`: `resistance(archetype, element, multiplier_num, multiplier_den)`
  (integer ratio — no floats/RNG), `barrier(entity PK)`, `grimoire(entity PK,
  spell)`. Version 3 → 4.
- Seed (both): spell_catalog += fire('damage'), frost('frost'), dispel('dispel',
  tier2); resistance rows rime_touched weak-fire 2/1, resist-frost 1/2 (archetype
  data — instance placed Step 14). barrier/grimoire tables start empty (ironhide's
  barrier row is Step 16 per plan; grimoire rows written by dropGrimoire in Step 18).
- **Divergence (noted):** plan had Step 13 seed the ironhide/book_swarm INSTANCES
  into base.sql. Deferred: archetype-level catalog/resistance data seeded now in
  both seeds; enemy instances go in combat_fixture.sql at their behavior steps
  (14/16/17) and base.sql's full roster is assembled in Step 19 (where the plan
  already re-expresses all four via placeEnemy). Keeps the curated Thornmere world
  + testShippedSeedShape stable until the coherent Step-19 assembly.
- `testCombatSchema`: three tables' columns; resistance rows are non-zero integer
  ratios; rime fire ratio == 2. Gate: build clean; 2228 checks, 0 failures.

### Step 14 — element + resistance multiplier; Fire/Frost; floor ✅
- `combat.hpp`: kSpellDamage=4, kSlowDuration=2. `combat.cpp`: `resistedDamage`
  (base × num/den, integer, neutral when no row, unmodified when element ""),
  `enemyIncapacitated` (stun OR slow suppresses the enemy turn). `resolveCast`
  extended: fire → 'burned' (resisted damage); frost → 'froze' (resisted damage +
  'slow' CC); ward returns early, others target hostileInRoom.
- Basic attack unchanged (no element → never hits the resistance table) — that IS
  the non-zero floor (REQ-COMBAT-18).
- render `burned` + `froze` templates (standing rule).
- `combat_fixture.sql`: **frost study = entity 6** (initially collided with the
  player at id 3 — UNIQUE name.entity crash; fixed) holding rime-touched (entity 8,
  10/10, weak fire). corridor east↔study west realized pair.
- `testCombatElements`: fire on rime → kSpellDamage×2 (weakness); frost on rime →
  kSpellDamage/2 (resist) + slow status; basic attack deals exactly
  kBasicAttackDamage (>0) to BOTH seeded archetypes. Covers AI-Validation item 8.
- Gate: build clean; `./build/tests` → 2256 checks, 0 failures. combat.cpp clean.

### Step 15 — DoT status effect ✅
- `combat.hpp`: kDotDamage=2, kDotDuration=2. `combat.cpp`: extracted
  `defeatHostile` (shared by both defeat checks); `applyDot` (fixed magnitude via
  damageEntity, not resistance-scaled, independent of CC). resolveCast 'dot' effect
  → applyStatus(enemy,'dot',mag,dur). resolveCombat DoT lane: applyDot → **defeat
  check after DoT** (a DoT kill removes the enemy same-tick, never lingers at 0) →
  status countdown.
- render 'dot' template ("smoulders"). Seeds gain `ember` (effect 'dot', element
  fire, cd 3).
- `testCombatDoT`: cast ember burns tick 1, stun (survive) → tick 2, wait → expired;
  exactly kDotDuration 'dot' events each of magnitude kDotDamage. With Step 10's CC
  this closes AI-Validation item 9.
- Gate: build clean; `./build/tests` → 2276 checks, 0 failures. combat.cpp clean.
</content>
</invoke>
