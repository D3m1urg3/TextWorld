---
title: Combat system and enemies
date: 2026-07-10
status: implemented
tags: [combat, enemies, spells, cooldowns, determinism, architect, bestiary, grimoires]
modules: [systems, mutations, action, architect, render, prose, seed]
related: [.lore/work/brainstorm/combat-and-enemies.md, .lore/vision.md, .lore/work/brainstorm/mage-school-setting.md]
req-prefix: COMBAT
---

# Combat system and enemies

Turn-based, deterministic **puzzle** combat for TextWorld. Enemies are locks;
spells are keys; there is no RNG. Combat rides the existing tick model (one
prompt = one tick = one transaction), extends the ECS with combat components, and
respects the vision's boundary: the engine owns every number, the AI owns only
prose and the *selection* of pre-authored content.

This spec captures the **whole** design from
`.lore/work/brainstorm/combat-and-enemies.md`. It is deliberately comprehensive;
**atomic sequencing is a plan concern** — `/prep-plan` will slice these
requirements into shippable bricks (foundation → telegraph/cooldown → elements &
grimoires → bestiary & architect spawning). Nothing here mandates a big-bang
implementation.

## Context the requirements assume

- **ECS store.** Entities are integer ids; state lives in component tables
  (`room`, `name`, `description`, `player`, `portable`, `location`, …) in
  `world.db`. Combat adds new component tables.
- **Tick = transaction.** `runTurn()` opens one transaction, increments `turn`,
  calls `resolve(db, action, player)`, commits. Every state change pairs with an
  `events` row; all player-visible output is rendered from events + read-only
  lookups.
- **Extension seams.** The `Verb` enum + `Action` struct is the input seam; the
  AI resolver lowers NL to `Action`s with the fixed-verb parser as permanent
  fallback. AI narration renders from a facts payload with the template renderer
  as permanent fallback. The architect generates rooms at latent exits via
  tool-use, engine-enforced invariants, silent fallback when disabled/failed.

---

## Requirements

### A. Core model & determinism

<a id="req-combat-1"></a>
**REQ-COMBAT-1** — All combat quantities (health, base damage, chip damage,
cooldown lengths, telegraph durations, resistance multipliers, status-effect
magnitudes and durations) are **engine-owned constants**. No randomness enters
combat resolution anywhere. Given an identical starting world and an identical
input sequence, the full combat event stream and final state are byte-identical
on replay.

<a id="req-combat-2"></a>
**REQ-COMBAT-2** — Combat resolves **within the existing tick model**: one prompt
= at most one tick = one transaction. The enemy's turn resolves in the *same*
transaction as the player's action, immediately after it. Every combat state
change writes a paired `events` row (the transcript invariant holds for combat).

<a id="req-combat-3"></a>
**REQ-COMBAT-3** — Combat is entered **implicitly**: while a hostile entity with
health > 0 shares the player's room, combat is active and the enemy takes exactly
one turn per tick. There is no explicit "engage" command. Combat ends when the
enemy is defeated ([REQ-COMBAT-20](#req-combat-20)), the player is downed
([REQ-COMBAT-23](#req-combat-23)), or the player leaves the room
([REQ-COMBAT-26](#req-combat-26)).

### B. Health & entities

<a id="req-combat-4"></a>
**REQ-COMBAT-4** — A `health` component (`entity`, `current`, `max`) exists. The
player and every enemy carry it. `current` is clamped to `[0, max]`.

<a id="req-combat-5"></a>
**REQ-COMBAT-5** — An enemy is an entity marked hostile (a `hostile` component)
carrying `health`, a `location`, `name`/`description`, and a reference to its
bestiary archetype ([REQ-COMBAT-28](#req-combat-28)).

### C. Player & enemy actions

<a id="req-combat-6"></a>
**REQ-COMBAT-6** — A `Verb::Attack` (basic attack) exists: always available, **no
cooldown**, fixed damage, no element. It deals non-zero damage to the hostile in
the room — the anti-deadlock floor ([REQ-COMBAT-18](#req-combat-18)).

<a id="req-combat-7"></a>
**REQ-COMBAT-7** — A `Verb::Cast` (with a spell subject) resolves a **known**
spell against the enemy. Casting a spell the player has not learned
([REQ-COMBAT-21](#req-combat-21)) is not a valid action and is declined **without
consuming a turn**, consistent with the resolver/parser contract for
unrecognized actions.

<a id="req-combat-8"></a>
**REQ-COMBAT-8** — The player takes **exactly one action per tick**. Offense and
defense are mutually exclusive: the player may attack, cast (offensive *or*
defensive), or flee in a tick — never both attack and defend in the same tick.
Non-combat verbs (Look, Wait, Take, Drop, Inventory) remain legal during combat;
each still consumes the tick and therefore counts as a non-counter action
([REQ-COMBAT-11](#req-combat-11)) — the enemy still takes its turn.

<a id="req-combat-9"></a>
**REQ-COMBAT-9** — The enemy takes **exactly one turn per tick**, resolved by an
enemy-turn system that fires after the player's action within the same
transaction ([REQ-COMBAT-2](#req-combat-2)). Until combat, only the player acted;
this system is the first non-player actor. The turn is a single chosen *action*
(telegraph a strike, land a pending strike, cast, or idle); passive chip damage
([REQ-COMBAT-12](#req-combat-12)) is a separate lane, not the turn.

### D. Telegraph & counter (the timing puzzle)

<a id="req-combat-10"></a>
**REQ-COMBAT-10** — A telegraphed enemy attack has a **one-tick wind-up**: on the
telegraph tick the engine emits a telegraph event (narrated as a wind-up) and
records pending-strike state on the enemy. The strike resolves on the enemy's
*next* turn (the player's next tick) unless countered.

<a id="req-combat-11"></a>
**REQ-COMBAT-11** — The counter window is the **single player action** between
telegraph and strike. In that window: an **interrupt** (e.g. Stun/Silence)
cancels the pending strike; a **block** (Ward) negates its damage. A wrong action
or no counter → the strike lands for its fixed damage. "In time" means exactly
this one-action window.

<a id="req-combat-12"></a>
**REQ-COMBAT-12** — Enemies may also deal **untelegraphed chip damage** each tick
(a fixed per-archetype constant), making HP a **clock**: even optimal play costs
health, so every fight is a race. This is the mechanism that keeps a *solved*
encounter non-trivial. Chip is a **passive lane applied every tick in addition
to** the enemy's turn action ([REQ-COMBAT-9](#req-combat-9)); countering a
telegraph never prevents chip — that irreducibility is what makes it a clock. (A
tick where a telegraphed strike lands therefore deals strike + chip.)

### E. Spell economy — cooldowns

<a id="req-combat-13"></a>
**REQ-COMBAT-13** — Each spell has a **fixed cooldown** (a spell-catalog
constant) measured on the **global tick clock**: cast at tick `T` → unavailable
until tick `T + cooldown`. Walking between fights therefore clears cooldowns
naturally. Basic attack has no cooldown. Casting a known spell that is still on
cooldown is declined **without consuming a turn** (as with an unknown spell,
[REQ-COMBAT-7](#req-combat-7)) — cooldown gates *availability*; it does not punish
the player with a lost turn.

<a id="req-combat-14"></a>
**REQ-COMBAT-14** — Cooldowns are **immutable constants**, never reduced by
progression or any runtime effect. (Guards the no-number-growth anti-goal —
[REQ-COMBAT-22](#req-combat-22).)

<a id="req-combat-15"></a>
**REQ-COMBAT-15** — During combat the engine **deterministically appends** a
status line reporting each known spell's readiness (e.g. `Fire: ready · Ward: 2 ·
Stun: 1`) and current/max health. This line is engine-authored and never left to
the model — the same always-appended pattern as exits/inventory.

### F. Lock categories, elements & resistances

<a id="req-combat-16"></a>
**REQ-COMBAT-16** — Each attack/spell carries an **element** (or none, for
basic). Each enemy archetype has a **resistance/weakness table** mapping element →
damage multiplier (engine constants). Applied damage = base × multiplier,
deterministic. The multiplier applies to **elemental** attacks only; a basic
attack (element = none, [REQ-COMBAT-6](#req-combat-6)) is not looked up in the
table and deals its fixed base damage unmodified — which is what makes the
non-zero floor of [REQ-COMBAT-18](#req-combat-18) hold against every archetype.

<a id="req-combat-17"></a>
**REQ-COMBAT-17** — The mechanics must express all **four lock categories**:
*element lock* (resistance table), *telegraph lock*
([REQ-COMBAT-10](#req-combat-10)–[11](#req-combat-11)), *defense lock* (a
**barrier** state on the enemy — distinct from the player's Ward spell of
[REQ-COMBAT-11](#req-combat-11) — that blocks damage until stripped by Dispel), and
*multiplicity lock* (AoE and/or DoT reaching several bodies or ticking over
time — [REQ-COMBAT-19](#req-combat-19)).

<a id="req-combat-18"></a>
**REQ-COMBAT-18** — Basic attack always deals non-zero damage to **every** enemy
(no enemy is fully immune to basic). This guarantees no encounter is ever a hard
deadlock, even with an empty spellbook.

### G. Status effects (DoT & crowd control)

<a id="req-combat-19"></a>
**REQ-COMBAT-19** — Damage-over-time and crowd-control effects are **components**
applied to an entity that tick down deterministically each tick within the
combat system: DoT applies fixed damage per tick for a fixed duration; CC
suppresses the affected entity's action (or cancels a pending strike) for a fixed
duration. In scope, CC is cast **by the player on enemies**; enemy-inflicted
action-denial on the *player* is out of scope for this spec — the
prompt-synchronous tick has no coherent "skipped player action."

### H. Progression — grimoires & knowledge

<a id="req-combat-20"></a>
**REQ-COMBAT-20** — A defeated enemy (health = 0) is removed from play and
**drops a grimoire** item per its archetype drop table into the room. Grimoires
are portable items. The drop table is a **deterministic fixed mapping** (archetype
→ item[s]), never probabilistic ([REQ-COMBAT-1](#req-combat-1)).

<a id="req-combat-21"></a>
**REQ-COMBAT-21** — Reading a grimoire adds its spell to a `known_spells`
component (**canon**). A learned spell is permanent — never removed by any
mechanic — and persists across restarts. Reading an already-known grimoire is a
no-op success.

<a id="req-combat-22"></a>
**REQ-COMBAT-22** — Progression happens **only** by learning new spells (keys).
No experience, levels, or any growable numeric stat exists. Power is *keys
known*, never *numbers grown*. (Vision anti-goal enforcement.)

### I. Death — the "downed" model

<a id="req-combat-23"></a>
**REQ-COMBAT-23** — When player health reaches 0 the player is **downed, not
dead**: within the tick, the player is relocated to the dormitory cell (the seed
safe room) with health restored to max. Being downed also **clears the player's
active combat status effects** (DoT/CC); spell cooldowns are not specially reset —
they expire normally on the global tick clock ([REQ-COMBAT-13](#req-combat-13))
and will have lapsed by the time the player walks back.

<a id="req-combat-24"></a>
**REQ-COMBAT-24** — On being downed the player **drops all carried items** at the
location where they fell; those items remain there to be recovered. `known_spells`
is **never** dropped or lost — knowledge is permanent, possessions are droppable.

<a id="req-combat-25"></a>
**REQ-COMBAT-25** — On being downed the enemy that downed the player is
**restored** to its initial combat state (health and any pending-strike/effect
state reset); the fight resets. No world content is deleted.

### J. Fleeing & movement

<a id="req-combat-26"></a>
**REQ-COMBAT-26** — The player may flee by moving (`Go`) through an **already
generated** (non-latent) exit. Fleeing through a latent/ungenerated exit is **not
permitted** while a hostile is present (prevents mid-combat room generation).
Fleeing counts as the player's (non-counter) action for that tick, so the enemy
resolves its **single ordinary turn once** as the player leaves — a pending
telegraphed strike **lands** as that turn ([REQ-COMBAT-11](#req-combat-11)),
otherwise the turn is the enemy's normal action; there is no separate "parting
hit" constant and no double strike (chip still applies per
[REQ-COMBAT-12](#req-combat-12)).

<a id="req-combat-27"></a>
**REQ-COMBAT-27** — Enemies are **room-bound**: a hostile never follows the player
between rooms. A fled enemy remains in its room in its current state and can be
re-engaged on return.

### K. Bestiary catalog

<a id="req-combat-28"></a>
**REQ-COMBAT-28** — A `bestiary` catalog of enemy **archetypes** is defined as
**seed data** (SQL, loaded into `world.db` at init exactly like `base.sql` and
`setting.txt`). Each archetype is a frozen record: health, resistance table, chip
amount, telegraph pattern, drop table, an integer **tier**, and a short thematic
**blurb**. Spells likewise carry a **tier** in the spell catalog; tiers are the
ordinal the bootstrap and gating rules key on
([REQ-COMBAT-32](#req-combat-32)–[33](#req-combat-33)).

<a id="req-combat-29"></a>
**REQ-COMBAT-29** — Archetype stat blocks are **engine-owned constants**; the AI
never sets or alters any archetype number. Placing an enemy copies its
archetype's constants into a new instance. The **blurb** is the *only* archetype
field the model ever sees (for selection and narration).

<a id="req-combat-30"></a>
**REQ-COMBAT-30** — Enemy **instances are canon**: a spawned enemy, its current
health, and its defeat are world state in `world.db`. A defeated enemy stays
defeated across restarts. A live world keeps the archetype constants it was seeded
with; a fresh world gets the current catalog.

<a id="req-combat-39"></a>
**REQ-COMBAT-39** — The seed world **hand-places at least one enemy instance** (an
archetype from the catalog) in `base.sql`, mirroring how the seed hand-places
rooms and items — so combat is fully exercisable in a fresh world *without* the
architect. This is the enemy the foundation validation drives
([AI Validation](#ai-validation) item 3), and the substrate on which
architect-spawned enemies ([REQ-COMBAT-31](#req-combat-31)) are the later layer.

### L. Architect spawning & gating

<a id="req-combat-31"></a>
**REQ-COMBAT-31** — When the architect generates a room it may place **at most
one** enemy, **selecting** an archetype from an engine-computed eligible menu. It
**never invents** an archetype or any stat, and never sees ids or numbers — the
same boundary as room/exit generation.

<a id="req-combat-32"></a>
**REQ-COMBAT-32** — The eligible menu is **gated by player state** so the
lock-and-key economy cannot deadlock: the engine offers only archetypes whose lock
the player can already solve (**all** keys the lock requires are in `known_spells`
— both keys, for a two-key defense lock per [REQ-COMBAT-17](#req-combat-17)) **or**
that are basic-attack-soluble and drop a key the player lacks.

<a id="req-combat-33"></a>
**REQ-COMBAT-33** — **Bootstrap rule:** the first enemy ever placed in a world
must be basic-attack-soluble and must drop a grimoire teaching a **tier-1
(starter) spell** ([REQ-COMBAT-28](#req-combat-28)), so the key chain can start.

<a id="req-combat-34"></a>
**REQ-COMBAT-34** — **Front intensity is one knob.** Enemy-spawn eligibility
density *is* the invasion front: contested (inner) rooms are eligible for enemies;
safe-edge rooms have none. Setting tone, gating, and difficulty pacing are
expressed through this single signal, not three separate systems. Front intensity
must be a **deterministic, engine-computed** property of a room's position (e.g. a
function of graph distance from the seed — far = safe edge, near the invaded core
= contested); the exact metric is tuning, but it must be repeatable so eligibility
and the "safe-edge yields an empty menu" check are well-defined.

<a id="req-combat-35"></a>
**REQ-COMBAT-35** — With AI disabled or on any generation failure, the architect
spawns **no** enemy (combat content appears only where generation succeeds); the
seed's hand-placed enemy and all combat mechanics still function deterministically.

### M. Setting

<a id="req-combat-36"></a>
**REQ-COMBAT-36** — `seed/setting.txt` gains the **invasion premise** (goblins
have breached the lower/inner halls seeking the grimoires) framed as a **spreading
front** — safe edges, dangerous core — while preserving the existing hushed tone.
This authorizes the architect's DNA to produce combat-inflected rooms
([REQ-COMBAT-34](#req-combat-34)).

### N. Narration & fallback

<a id="req-combat-37"></a>
**REQ-COMBAT-37** — Combat output follows the existing narration contract: AI
prose grounded in the turn's combat events when enabled and valid; the **template
renderer is the permanent fallback** on any failure. Engine-appended deterministic
lines (health/cooldown status, exits) are never left to the model, and protected
canon (room descriptions, refusals) is passed verbatim and mechanically checked.

<a id="req-combat-38"></a>
**REQ-COMBAT-38** — NL combat input resolves through the existing resolver ("burn
it", "swing at it", "shield") lowering to `Attack`/`Cast`; the **fixed-verb parser
is the permanent fallback**. The model recognizes intent/nouns only — whether an
action *applies* (combat active, spell known, cooldown ready, element legal) stays
the engine's deterministic decision.

---

## AI Validation

The AI verifies "done" by driving the engine and observing events — favoring
**deterministic, network-free** checks (template + fixed-verb mode) so validation
is cheap and repeatable; only the two AI-path items exercise Claude, and they
assert the *fallback/boundary*, not prose quality.

1. **Determinism replay ([REQ-COMBAT-1](#req-combat-1)).** From a fresh seeded
   world, run an identical scripted input sequence twice; assert the combat
   `events` streams and final `world.db` state are byte-identical.
2. **Tick integrity ([REQ-COMBAT-2](#req-combat-2), [-9](#req-combat-9)).** Each
   combat prompt increments `turn` exactly once; player-then-enemy state changes
   share one transaction; every change has a paired event row.
3. **Foundation loop ([REQ-COMBAT-4](#req-combat-4)–[6](#req-combat-6),
   [-20](#req-combat-20), [-39](#req-combat-39)).** Attack the seed-placed enemy:
   its `health` drops by the
   fixed damage each tick; at 0 it is removed and a grimoire item appears in the
   room.
4. **Chip clock ([REQ-COMBAT-12](#req-combat-12)).** Against a chip enemy, player
   `health` decreases by the fixed chip each tick even when the player plays
   optimally.
5. **Telegraph/counter ([REQ-COMBAT-10](#req-combat-10)–[11](#req-combat-11)).**
   Script telegraph→Ward: strike deals 0. Script telegraph→(non-counter): strike
   deals its fixed damage. Script telegraph→Stun: pending strike is cancelled.
6. **Offense XOR defense ([REQ-COMBAT-8](#req-combat-8)).** No single tick records
   both an attack-damage event and a ward event from the player.
7. **Cooldowns ([REQ-COMBAT-13](#req-combat-13)–[15](#req-combat-15)).** After
   casting spell X (cooldown N), casting X is declined for exactly N ticks and the
   status line counts down `X: N…1` then `X: ready` at tick `T+N`; basic attack is
   never blocked; the status line is present and engine-authored.
8. **Elements & resistance ([REQ-COMBAT-16](#req-combat-16),
   [-18](#req-combat-18)).** Right element applies the weakness multiplier; wrong
   element applies the resist multiplier; basic attack always deals >0 to every
   catalogued archetype.
9. **Status effects ([REQ-COMBAT-19](#req-combat-19)).** A DoT applies its fixed
   damage for exactly its duration then stops; a CC suppresses the enemy's action
   / cancels its strike for exactly its duration.
10. **Progression & persistence ([REQ-COMBAT-21](#req-combat-21)–[22](#req-combat-22)).**
    Reading a dropped grimoire adds the spell to `known_spells`; the spell is still
    present after a restart; no XP/level/growable-stat column exists anywhere in
    the schema.
11. **Downed model ([REQ-COMBAT-23](#req-combat-23)–[25](#req-combat-25)).** Drive
    player health to 0: player is in the dormitory cell at full health; all
    previously carried items are at the fall location; `known_spells` is unchanged;
    the enemy is back at full/initial state; nothing was deleted.
12. **Fleeing ([REQ-COMBAT-26](#req-combat-26)–[27](#req-combat-27)).** Fleeing via
    a latent exit while a hostile is present is refused; via a generated exit it
    succeeds, applies one parting hit, leaves the enemy in its room, and the enemy
    is unchanged on return.
13. **Bestiary boundary ([REQ-COMBAT-28](#req-combat-28)–[30](#req-combat-30)).**
    A placed instance's stats equal its archetype seed constants; defeat persists
    across restart; the AI/generation path performs no write of any archetype
    number.
14. **Gating (deterministic parts of
    [REQ-COMBAT-31](#req-combat-31)–[34](#req-combat-34)).** Given a `known_spells`
    lacking Fire, the engine-computed eligible menu contains no fire-locked
    archetype; after learning Fire, fire-locked archetypes become eligible; the
    first-ever spawn is basic-soluble and drops a tier-1 grimoire; safe-edge rooms
    yield an empty menu.
15. **AI-path fallback parity ([REQ-COMBAT-35](#req-combat-35),
    [-37](#req-combat-37)–[38](#req-combat-38)).** With AI disabled, combat is
    fully playable via fixed verbs + templates and the architect spawns no enemy.
    With AI enabled, an induced resolver/narration/generation failure falls back
    silently to fixed verb / template / no-spawn without corrupting combat state.

## Out of scope (explicitly deferred)

- Multi-enemy encounters and overlapping telegraphs; multi-tick wind-ups.
- Any spell resource other than cooldown (mana is **ruled out**, not deferred).
- Balancing pass: exact per-archetype numbers (health, chip, cooldowns,
  multipliers) are tuning data settled during implementation, not requirements.
- Non-combat NPCs, dialogue, and the waking of the school.
