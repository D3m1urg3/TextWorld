---
title: Combat system and enemies
date: 2026-07-10
status: resolved
tags: [combat, enemies, spells, puzzle, determinism, architect, bestiary, grimoires]
modules: [architect, systems, mutations, seed]
related: [.lore/work/brainstorm/mage-school-setting.md, .lore/vision.md, .lore/work/design/story-seed-architect.md]
---

# Combat system and enemies

Brainstorm on a turn-based, Zork-style combat system for TextWorld, and the
enemies it fights. The through-line: combat is a **deterministic puzzle**, not a
slot machine, and enemies are grown by the architect the same disciplined way
rooms already are.

## Starting premise (from the user)

Simple turn-based combat, Zork-style, with turns **synchronized to user
prompts**. Player has a basic attack plus spells with distinct effects
(elemental damage, crowd control, damage-over-time, and more). Enemies have
resistances and weaknesses. Start minimal — basic attack from player and enemy —
and lay a foundation for later complexity.

## The core reframe: combat is a puzzle, not a race

The vision bans **stochastic mechanics** outright (`.lore/vision.md`
anti-goals; combat is literally its example of "must be deterministic"). No hit
rolls, no damage ranges. Following that constraint to its end changes what
combat *is*:

- With no RNG, every enemy is a **lock** and every spell is a **key**.
- Resistances/weaknesses stop being stat-shaving and become **the puzzle**:
  "shrugs off fire, shatters to frost."
- This is *more* Thornmere, not less — it's a **school**. Fighting is figuring
  out. Grinding is the anti-pattern; deduction is the loop.

Deterministic enemies can be **telegraphed**: the wind-up on turn N always
precedes the strike on turn N+1. So the felt loop is:

> **read the telegraph → play the counter → HP is just the scoreboard.**

That call-and-response is what makes deterministic combat *feel* like combat
instead of a math quiz. It's also exactly the "react appropriately to enemy
actions" the user asked for, made mechanical.

## Spells are answers, not damage

The pivotal design principle: **in RNG combat spells compete on
damage-per-turn; in puzzle combat every spell is the answer to a specific enemy
behavior.** Fire isn't "good damage" — Fire is *the answer to the frost-thing*.
If a spell isn't the answer to something, it's dead weight.

Consequence for the content pipeline: **you design enemies and spells as pairs,
lock-and-key, in the same breath.** To add an enemy is to ask "what's the key?";
to add a spell is to ask "what's the lock?"

There turn out to be ~four **lock categories**, each answered by a kind of spell:

| Lock category | What the enemy does | The key (spell that answers it) |
|---|---|---|
| **Element lock** | Resists most damage, weak to one element | Bring the matching element (Fire vs the Rime-thing) |
| **Telegraph lock** | Winds up a heavy/timed attack over N turns | **Interrupt** (Stun/Bind) *or* **block** (Ward) before it lands |
| **Defense lock** | Shielded/warded — nothing damages it | **Dispel** first, *then* any damage (a two-key sequence) |
| **Multiplicity lock** | A swarm of low-hp bodies | **AoE / DoT** that hits all, or **CC** to freeze the swarm |

The multiplicity lock is what finally gives **DoT and AoE a reason to exist** —
in a single-target puzzle they were dead weight; against a swarm they're keys.

## Starter bestiary (illustrative, to design against)

- **Goblin grunt** — tutorial lock. Low hp, one telegraphed crude swing, no
  resistance, basic-attack-soluble, drops tier-1 grimoire. *Teaches: read the
  telegraph.*
- **Rime-touched goblin** — element lock, weak to Fire; telegraphs a freezing
  touch you can Ward. *Teaches: elements matter.*
- **Goblin shaman** — telegraph lock, no element weakness; winds up a spell,
  answered by Silence/interrupt in time or eaten through a Ward. *Teaches:
  timing, not damage type.*
- **Ironhide brute** — defense lock; Dispel the ward, then hit. *Teaches:
  sequencing two keys.*
- **Book-swarm** — multiplicity lock (the invaders stirred the library; nice
  bridge to the domestic-magic flavor). *Teaches: why DoT/AoE exists.*

Matching starter spells (each an answer): **Basic bolt** (always-available
floor), **Fire** (element), **Frost** (element + slow), **Ward** (block a
telegraph), **Silence/Bind** (interrupt a telegraph / caster), **Dispel** (strip
a defense lock).

## Progression: grimoires, not levels

Spells are learned from **grimoires dropped by enemies**. On-architecture and
already half-planned — the mage-school brainstorm deferred a `known_spells`
component. The loop:

> enemy dies → drops a grimoire **item** → read/take it → `known_spells` grows →
> that's **canon**, persisted forever.

**Progression axis is the spellbook, not a level counter.** Power comes from
*knowing more keys*, never from bigger numbers.

Rejected on purpose: **XP and levels.** They reintroduce stat-growth and let the
player out-stat the puzzle into a grind. There is no RNG and no number-growth in
this vision.

## Enemies are grown by the architect (the big decision)

The user's call: **the architect spawns enemies**, not hand-placement. Reasoning
that overrode the earlier "hand-place it" position: this project's soul is
AI-grown territory. Combat confined to two seed rooms is a dead end. If combat
matters, it lives wherever the world grows. So the two objections
(determinism, gating) must be *solved*, not dodged — and both are solvable with
machinery already trusted for rooms.

### Determinism — the architect selects, it never invents

Reuse the exact room-generation boundary (model co-authors prose, "never invents
structure, never sees an id"; engine mints ids and enforces invariants). For
combat:

- The engine owns a fixed **bestiary** — a catalog of archetypes, each with
  constant hp / resistances / telegraph timing / drop table.
- The architect's only combat job: *given this room + setting, is there an enemy
  here, which archetype fits, and write its entrance.*
- The engine instantiates the archetype's frozen stat block.

The model picks the **costume and the casting**; the engine owns every
**number**. No RNG enters because the numbers were authored into the catalog.

**Reject the forbidden door:** letting the architect set hp/damage "to fit the
room." The moment the model owns a number, determinism dies and the same goblin
is a pushover in one room and lethal in the next.

### Gating — lazy, player-state-aware generation (the gem)

Rooms are minted **lazily, at the moment you walk into them**, so at spawn time
the generator can see your `known_spells`. Gate the eligible bestiary on player
state:

> The architect may only place a lock whose key you already hold — *or* a lock
> beatable by basic attack that drops a key you don't yet have.

The lock-and-key economy becomes **self-balancing by construction**: no Fire →
the world *cannot* generate a fire-lock in front of you; it generates a basic
goblin dropping the Fire grimoire instead. Learn Fire → frost-things become
*eligible*. The world grows to match your capability. This is the project's core
thesis again — the procedural "weakness" becomes the balancing mechanism — and
it is uniquely available here *because* generation is lazy and state-aware.

Two rules fall out:

1. **Bootstrap rule** — the first combat ever placed must be basic-attack-
   soluble and must drop a tier-1 grimoire, or the chain can't start.
2. **Anti-deadlock floor** — basic attack works on everything, slowly. (This was
   the optional "fork #3" pressure valve; the grimoire economy makes it
   mandatory — otherwise a missing key hard-locks the player.)

## Where the enemy data lives (three layers, three homes)

An enemy is the same two-halves split as a room — a **mechanical half** (hp,
resistances, telegraph timing, drops) and a **presentational half** (what it is,
how it enters). They don't live in one file.

Hard technical reason `setting.txt` is the wrong home for the mechanical half:
the architect's eligible menu is **dynamic** (depends on `known_spells`),
computed per generation call. A static DNA file can't express that — so the
menu must be an engine-computed handshake, not prose the architect "picks up."

```
setting.txt      → TONE ONLY. Gains the invasion premise so the world feels
 (AI reads)         combat-inflected. No numbers, ever.

bestiary (data)  → THE CATALOG. Structured, engine-owned: per archetype
 (engine reads)     { hp, resistances, telegraph_turns, drop_table, + a short
                    thematic blurb }. Deterministic. Model never sees numbers.

tool schema      → THE HANDSHAKE. At generation time the engine computes the
 (engine → AI)      *eligible* subset (gated by known_spells / depth) and offers
                    it: "place one of {these}, name+blurb each, pick and narrate."
```

- The **blurb is the one bridge**: a prose field living inside the structured
  bestiary, the only piece handed to the model (so it can pick a fitting
  archetype and narrate it). Numbers never cross the bridge.
- **Data file, not hardcoded C++.** Difficulty now lives *in the catalog, not
  the corridors* — tuning it must not mean recompiling. Leaning toward a
  `bestiary` **seed table** loaded at init the way `base.sql` / `setting.txt`
  already are, to keep the "the world *is* the database file" story intact
  (fork: SQL seed table vs JSON config).
- **Instances are canon.** The archetype is a template (catalog/seed); *this
  goblin, in this room, at 4 hp, now dead* is world state in `world.db` and stays
  dead across restarts. The catalog is the mold; the db holds the castings.

## Architecture fit (mostly free)

- **Turn = tick = transaction** is already the engine's heartbeat, and "every
  command consumes a turn" is already Zork's troll model. "The enemy acts every
  prompt" isn't something to build — it's something to *stop suppressing*.
- **Spells are words**: the AI resolver already lowers utterances to actions, so
  "burn it" / "freeze it" / "swing at it" reach combat verbs nearly free. The
  resolver recognizes nouns; whether an action applies stays the engine's call —
  boundary holds.

## Minimal skeleton (foundation brick vs second brick)

**Brick 1 — the loop exists and someone can die:**

- an `hp` component (on the player, and on one enemy entity in a room)
- one `attack` action, fixed damage, no element
- enemy retaliates each tick, fixed damage
- 0 hp → enemy despawns / player is **downed** (see Resolutions → death model)
- AI narrates the blow; template fallback holds

**Brick 2 — the whole game in miniature (vertical slice):**

> goblin (telegraphs a basic hit) → basic attack kills it → drops **Fire**
> grimoire → read it, learn Fire → next enemy is a frost-thing only Fire cracks.

Elements, resistances, DoT, CC are all **clean layers** on brick 1: element is a
tag on an attack, resistance a multiplier table on the enemy, DoT/CC are
components that tick down each turn (the tick loop already does this).

## Resolutions (loose ends tied)

All seven open forks are settled. The knot ties itself: **death → tuition →
discovery → chip-clock → fleeing-to-learn → safe-edge/dangerous-core → one front
knob.** Every loose end pulls on the same thread.

### 1. Death = "downed" (knowledge permanent, possessions droppable)

Neither raw option fit: permadeath **discards the AI-grown canon** (against
"persisted content is canon" as a felt value); a free soft-reset **removes all
stakes**. Synthesis:

- Death **downs** the player, who wakes in the **dormitory cell** (safe seed
  room).
- Cost: you **drop what you were carrying** where you fell — non-destructive, a
  trek back to recover it. Nothing is deleted.
- The enemy you were fighting is **restored** — the fight resets.
- **Learned spells are never lost.** Once a grimoire is *read*, the spell is in
  `known_spells` forever; you only ever drop *unread* grimoires and items.

Principle: **knowledge is permanent, possessions are droppable.** The spellbook
(the progression axis) only moves forward, so death bites without un-teaching you
or discarding the world.

**Death is tuition.** First time you meet an archetype you don't know its
weakness or telegraph — you learn them by fighting, and sometimes by dying and
waking in your cell. Punishment becomes pedagogy. Very "school." The soft-reset
and the discovery loop reinforce each other.

### 2. Telegraph/counter timing model (the heart)

Tick-based, deterministic:

- A telegraphed attack has a **one-tick wind-up.** On the telegraph tick ("the
  brute hauls its cleaver back") the strike lands on your **very next action**
  unless countered.
- **"In time" = your next prompt** — exactly one action-window. Miss it or play
  the wrong action → fixed-damage hit lands.
- **Offense and defense are mutually exclusive uses of your turn** — you can't
  Ward *and* attack on the same tick. *That mutual exclusion is the whole
  tension:* push damage, or answer the tell?
- **Pre-loading a Ward is allowed but self-punishing** — no rule needed. A ward
  eats your turn and blocks one hit; spammed, you deal zero damage and any
  enemy with baseline pressure out-attrites you. The economy bans spam.

**HP is a clock.** Enemies deal **small untelegraphed chip damage every tick**,
so even perfect play costs HP — every fight is a race to solve the pattern before
the chip wears you down. This is what kills the "solved puzzle stays solved" tax:
a known fight is still an execution race, and a *first* encounter (weakness
unknown) costs more chip → the tuition loop.

*Later brick:* multi-tick wind-ups with multiple interrupt windows; two enemies
whose telegraphs overlap so you can't answer both (variety from **patterns**, not
**more enemies**).

### 3. Fleeing / movement mid-combat

Spend your action on `go`; the enemy gets its parting hit as you turn. You may
flee **only through an already-generated exit** (normally back the way you came)
— which dodges the architect collision (no room generation mid-combat) and is
intuitive (flee toward safety, not deeper into the unknown). **Enemies are
room-bound** and don't chase, so fleeing is a real tactic: reset, go learn the
missing spell, return. Feeds the discovery loop.

### 4. setting.txt — spreading front, not total warzone

Keep the existing hushed DNA; add that the *lower/inner halls* have been breached
by goblins hunting the grimoires. Yields **safe edges** (where you wake and flee
*to*) and a **dangerous core** — a natural difficulty gradient — while preserving
what made Thornmere special. Reconciles the invasion debt with the
`mage-school-setting` tone.

### 5. Difficulty tiers — one knob, not three systems

`known_spells` is the **correctness gate** (already guarantees no deadlock). The
**front** is the **pacing knob** — and elegantly, *the front just IS where
enemies are eligible to spawn.* Enemy-density = "how invaded this room is." So
setting, gating, and pacing collapse into a single knob (front intensity) instead
of three separate systems. Graph depth is available as a deterministic refinement
if pacing needs more resolution later.

### 6. Bestiary storage — SQL seed table

A `bestiary` seed table baked into `world.db` at init, exactly like `base.sql`
and `setting.txt`. Keeps the "world *is* the db file" story, and a world
correctly carries the rules it was born with (fresh worlds get the latest
catalog; live worlds keep theirs).

### 7. Staleness tax — resolved

Handled by the chip-clock (every fight is a race) + discovery/tuition loop +
overlapping-telegraph patterns. "More enemies ≠ more combat" stays a design rule,
now with teeth.

### 8. Spell economy = per-spell cooldown, not mana

Chosen over mana because it **reinforces the timing axis** the whole system is
built on, rather than running orthogonal to it. Mana budgets scarcity; cooldown
governs *availability over ticks* — which is exactly what the telegraph/tick
model already is.

- **The upgrade it buys:** counters (Stun/Silence/Ward) on cooldown turn the
  answer to a telegraph from "play the counter" into "read the enemy's
  **cadence** and schedule your limited counters against it." A foe that
  telegraphs every other turn can't be interrupted every time → you solve a
  **rotation**, filling gaps with Ward or basic attack. Fully deterministic,
  meaningfully deeper.
- **Fixes the shallowest lock:** a cooldown on an element key kills the
  degenerate "spam the weakness" solve — knowing the answer becomes *executing*
  it over time (the same job the chip-clock does for telegraphs).
- **Composes:** offense-XOR-defense per turn stays (cooldown is a cross-turn
  constraint layered on top); ward-spam is now double-guarded (economy +
  availability); HP-clock deepens (race the chip *while your keys recharge*).
- **Floor intact:** basic attack has **no cooldown** — always available, and it's
  the natural gap-filler while spells cycle.

**Guard rail (keeps it in-vision):** cooldowns are **fixed constants per spell**,
never reduced by progression. "Lower your cooldowns" as a growable stat would
reintroduce the banned number-growth. Progression stays *more keys* — a new
grimoire may offer a different **cooldown profile** (fast-weak vs slow-strong),
which is a *choice between keys*, not a stat upgrade.

**Two consequences:**
1. Cooldown state must be **engine-appended and visible** during combat
   (`Fire: ready · Ward: 2 · Stun: 1`) — same deterministic always-appended
   pattern as exits/inventory, never left to the model.
2. Measure cooldowns on the **global tick clock**, so walking between fights
   clears them naturally (no reset logic) and they only bite *within* a fight.

More minimalist than mana (no pool, no regen, no per-spell cost to balance — one
number per spell) *and* deeper on the axis that matters.

## Deferred to design/implementation time

Not open *questions* — settled in principle, details land when we build:

- Exact bestiary schema (resistances/drops as structured columns vs child rows).
- Chip-damage magnitudes and per-archetype HP/telegraph tuning (this *is* the
  difficulty curve — tune the catalog, not the corridors).
- Multi-tick wind-ups and multi-enemy encounters (the "later brick" of timing).
- Exact cooldown values per spell (this is a tuning axis, like chip magnitudes —
  settled *that* cooldowns exist and how, per Resolution 8; the numbers land at
  design time). Mana is ruled out, not deferred.

## Rejected on purpose

- **Full HP/damage JRPG combat with a wandering wolf** — punctures the tone; the
  enemy has to *belong to the building* (or the invasion).
- **AI deciding damage numbers** — vision principle #1's exact forbidden example.
  Engine owns every number; AI owns only the sentence.
- **XP / levels** — power is *keys known*, not *numbers grown*.
- **Enemy data (numbers) in `setting.txt`** — wrong channel (prose, AI-facing)
  and technically impossible for a dynamic gated menu.
