---
title: The dungeon master — a live bard, and why it takes no orders
date: 2026-08-03
status: resolved
tags: [dungeon-master, bard, story-evolution, agent-architecture, puzzles, cadence, cost, latency, npc, macro-micro, story-catalog, level-of-detail, irreversible-events, overture, model-tiering]
modules: [architect, pregen, world]
related: [.lore/vision.md, .lore/work/design/story-seed-architect.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/brainstorm/story-seed-and-lod-world.md, .lore/work/specs/combat-and-enemies.md, .lore/work/research/per-turn-latency-remedies.md, .lore/work/research/drama-manager-prior-art.md]
---

# The dungeon master — a live bard, and why it takes no orders

## Context

Session goal: design the two biggest remaining features — the dungeon/game master and the NPCs. NPCs were deliberately parked early on; this document is about the DM. The opening proposal was a **mastermind**: the DM owns story, puzzles, and challenges, and acts only through other systems (AI or not) rather than taking actions itself.

That proposal survives, but with its call graph inverted. The conclusion below is that the DM is not a new entity at all — it is the **bard** from [story-seed-architect.md](../design/story-seed-architect.md), grown from a static blob into a live writer, exactly as that design predicted.

**In one paragraph.** The bard runs once at world creation to author a story catalog — cast, faction, clock, abstract lock/key pairs, what an ending would look like — none of which exists as world entities. Thereafter it wakes only when something irreversible happens (`generated`, `defeated`, `learned`), reads its journal plus the events since, and decides what materializes next by *selecting from its own catalog*, never by inventing. It writes intent into the store and is called by nothing; the architect, and later the NPCs, read that intent and act. Macro authors who/what/why, micro decides where/when. Puzzles are engine tables with a selection menu, not a puzzle-writing agent.

## What exists today

Three AI call sites, enumerated in code as `enum class AiRole { Resolve, Narrate, Generate }` (`aihttp.hpp:20`). Every model default, profile record, and transport binding hangs off that enum.

| Entity | Seam | Model | Reads | Writes |
|---|---|---|---|---|
| **Resolver** | `nlresolve.cpp`, before the tick | Haiku 4.5 | input line + room/exits/items/inventory | nothing |
| **Narrator** | `prose.cpp`, after the tick commits | Opus 4.8 | this turn's event rows + room slice | nothing |
| **Architect** | `architect.cpp`, from `resolveGo` on a latent exit | Opus 4.8 | `meta.setting` + origin room name + prose + direction | world, only via `writeGeneratedRoom` |
| **Bard** | — | — | — | — |

- **Resolver** — natural language into the closed `Action` ISA via a schema-enforced `emit_action` tool. It translates, never decides; the engine still validates and can refuse. Fixed-verb parser is the permanent fallback.
- **Narrator** — event rows to prose, read-only by contract: no output claim without a sourcing event row. A mechanical pre-display gate requires canon room text and `failed` details verbatim; gate failure falls back to the template renderer. Deliberately starved of engine-internal tags on `burned`/`froze` so it cannot echo an archetype name into prose as a noun.
- **Architect** — the only read-write AI unit, with the write confined to one mutation helper (`grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` must be empty, and that check is part of the build's validation). The model proposes flavor; the engine mints ids, owns exit reciprocity, and copies every enemy stat from the frozen bestiary.
- **Bard** — not an agent. `meta.setting`, a static blob loaded once from `seed/setting.txt`. Called "degenerate for the atomic step" in the design, which also states the interface will not change when a live bard lands: the bard simply becomes a *writer* into the store the architect already reads.

Two pieces of supporting machinery, neither an agent, both load-bearing for what follows:

- **The pregen worker** (`pregen.cpp`) — one thread, one job at a time, zero database access, everything snapshotted on the main thread at queue time. Its own header already anticipates this feature: *"coherent story generation — planned, out of scope here — cannot author rooms in ignorance of one another, and a serial worker is what can later be taught to consult shared story state without races"* (`pregen.hpp:120`).
- **The eligible-menu pattern** (`combat.cpp`) — the engine computes which archetypes are legal here, hands the model *blurbs only*, and re-checks the choice against a live menu at commit. The model picks; it never invents.

## Decisions

### 1. The bard is rare and asynchronous, never per-turn

A turn is ~4000 ms and ~99.9% model TTFB; the engine tick is 2–4 ms ([turn-latency-polish notes](../notes/turn-latency-polish.md)). A per-turn story call roughly triples per-turn cost and doubles latency for no gameplay gain. The bard rides the pregen threading pattern: off the critical path, nothing waits on it, and if it fails the game is exactly today's game.

### 2. Hierarchical authority, flat communication

"Hierarchy vs. flat" turned out to be two questions wearing one name:

- **Who has authority** — whose decisions constrain whose.
- **Who calls whom** — the runtime call graph.

The engine already separates them. `resolveCombat` never calls `resolve`, but tick ordering in `loop.cpp:126` gives the player's action authority over the enemy's. Authority is expressed by ordering and by what each party may read, not by a call.

So: the bard has total authority over story and zero callers. It is the only unit permitted to write intent; every other unit may read intent and may never write it. That is a grep-checkable invariant of the same kind `architect.cpp` already lives under.

The two designs also converge on one implementation. The mastermind version requires threading every AI task and holding coherence in the database — which *is* store-mediated communication. The hierarchy survives only as a fact about who writes what, which is the useful part and is free.

### 3. Store-mediation is the mechanical guarantee against railroading

The vision's anti-goal is a railroading narrator. A bard that can only place pressures into the store, with nothing downstream taking orders from it, **physically cannot force a beat**. A mastermind with a command channel is one weak prompt away from doing so.

Framed as tabletop practice: good GMs load the board with pressures and respond to what players do; bad GMs have a plot and push. The flat structure is structurally the former.

### 4. Blandness comes from thin context and plot-shaped prompts, not from flatness

The stated fear was that a flat structure yields uninspired, irrelevant stories. Two real causes, neither fixed by a hierarchy:

- **Thin context.** `buildArchitectContext` sends setting + one room name + canon prose + direction. If the bard writes one sentence of intent, the story is one sentence good — under a mastermind too, since a mastermind issuing one-sentence orders produces one-sentence-quality rooms. Richness is a function of what is on the wire, and that knob is identical in both designs.
- **Plot-shaped prompts.** A bard asked *"what happens next?"* writes a plot, and a plot needs obedient executors. A bard asked *"who wants what, what clock is running, what is about to go wrong, what does the player not know yet"* writes a **situation** — which needs no obedience, because it becomes story only on collision with the player.

### 5. What flatness genuinely costs, and the fix

A write-ahead bard never sees whether its intent landed. A mastermind reviewing executor output could say *"colder, more menace."*

The fix is not a command channel but a feedback loop: **write ahead, read back**. The bard observes the append-only event log — already the permanent transcript — and adjusts its next intent. A control loop rather than a command chain. It converges a few turns later than a review gate would, which at one generated room every few minutes is nothing.

### 6. The bard is the deliberate exception to narrow context, bounded by cadence

Vision principle 4 prefers narrow, targeted context. A story brain that sees only the current room is not a brain. The bard is the stated exception, and its cost is bounded by **cadence** (rare calls) rather than by context size.

The journal pattern keeps that flat: each wake, read the bard's own journal plus the event rows since the last wake, then rewrite the journal. A 500-turn session costs the same per wake as a 50-turn one.

### 7. One agent, three lanes — not a second entity

"Director" was introduced mid-session as a name for something the design already had. Structurally a live bard and a director are identical: both write into the store, both are read by the architect, neither is called, both are safe to fail. What survives the merge is a distinction between the **kinds of fact written**, not between two agents.

Started as two lanes; the macro/micro split (decision 12) forced a third out of the middle.

| | Setting | Arc + catalog | Pressures |
|---|---|---|---|
| Answers | what the world *is* | what *could* happen, and who exists in potential | what is about to happen *next* |
| Tense | describes | promises | anticipates |
| Written by | overture, rarely appended after | overture; **appended** by micro wakes | rewritten freely each wake |
| Mutability | canon — never rewritten, **append-only** | append-only (see unresolved question) | free — it is a sketch |
| Needs the event log? | no | no | yes, it is the whole input |
| LOD tier | — | L0/L1 | — |
| Failure mode | bland world | incoherent cast | railroading |

Mutability is the load-bearing row. `meta.setting` is canon under principle 2. Intent must be revisable, because *"the invasion is looking for something under the library"* has to survive the player burning the library down. This is the immutable `description` vs. mutable `sketch` split the LOD design already reserves.

The setting lane should be **append-only rather than frozen**: never edit the world's tone or premise, but allow new world-facts as they become true ("the east wing burned in the third week"). Without an append path the world's description still calls it a peaceful school while goblins hold the lower halls. This falls inside the tension table's existing exception for explicit player action that logically rewrites.

Engine enforces the per-lane write rules; the agent is not trusted with them.

**Test for splitting them later.** Two agents earn separation when they want different cadence, different context, or different guardrails. Cadence: the setting lane writes a handful of times per session, the intent lane every wake — same wake serves both. Context: the intent lane needs the setting anyway, and a setting-writer blind to current intent would contradict it. Guardrails: genuinely different, but enforced in engine code either way. None forces a split today. Revisit if you ever want to wake one without the other.

### 8. No puzzle-writing agent — and no new puzzle system either

**Revised 2026-08-03.** The general claim stands and is the strongest of the session. The lock/key generalization that originally accompanied it was an overreach and has been dropped.

**Stands:** a puzzle-writing AI will invent unsolvable puzzles, puzzles referencing affordances that do not exist, or puzzles the engine cannot arbitrate — and checking solvability would require an LLM judge, i.e. the open-ended live-LLM verification loop this project has already ruled out. The engine owns the mechanic; the model selects from an engine-computed menu.

**Dropped:** the proposal to extend the schema with a general lock/key puzzle model. *"Enemies are locks, spells are keys"* is **combat's** framing (`specs/combat-and-enemies.md`), authored for combat, and it should stay there. Generalizing it was reaching for the puzzle shape that is easiest to make engine-arbitrable rather than the one that fits Thornmere — whose non-combat affordances (staircases that move when unobserved, doors with opinions, restless books) are navigational, social, and knowledge-shaped, not lock-shaped.

**What is actually true:** combat *is* the puzzle system, and it is already built — `resistance` integer ratios, `bestiary.barrier` for the dispel-then-damage sequence, `effect='aoe'` for swarms, `drop_table` for key acquisition, and `eligibleArchetypes` refusing to offer an enemy whose keys the player does not already hold. Enemies are easy when approached correctly and a wall otherwise. Nothing to add.

**The bard's contribution to puzzles is teaching, not authoring.** `discoveredResistances` derives weakness knowledge purely from the transcript, so the only way to learn one is to take damage experimenting. A catalog knowledge-beat — a scorched study, a muttering portrait, a torn bestiary page — makes a weakness learnable through fiction, grants nothing mechanical, and is **validated against `resistance` at admission** so the bard cannot assert a falsehood about the rules. See [bard-fact-store.md](../design/bard-fact-store.md).

### 9. Everything else is a context change, not a new agent

The architect gets two additional fields: current bard pressures, and a short digest of nearby rooms it already built. Today it authors every room in ignorance of every other one, which is why a tree of individually good rooms does not add up to a *place*. Same call, same cost, no added latency.

### 10. Explicit non-additions

- **A critic/judge agent** over the architect or narrator. Doubles cost, adds critical-path latency, and makes quality an unbounded live-LLM loop. Existing mechanical gates (canon verbatim, no forbidden nouns, blurb re-check) catch the failures that matter.
- **A command channel** from the bard to anything. It is the railroading vector.
- **A separate summarizer.** The journal is the summary, maintained as a side effect of waking.

### 11. Cadence — the bard wakes on irreversible change, and on nothing else

Three candidates were considered and rejected: event-count thresholds (mechanical, magic numbers), frontier coupling to pregen (ties story pace to exploration pace, and inverts the bard's authority by making it a *pull* from the architect), and a self-spent call budget (chicken-and-egg — to decide whether to wake, it must be awake).

The answer is a different axis than *when*: **what**. The bard wakes because the world became different, and for no other reason. Cost then scales with the player's progress rather than with the clock.

The predicate needs no heuristic — `mutations.hpp:9` already draws the line: *"`appendEvent` alone (no component write) is legal ONLY for the no-write verbs: `looked`, `waited`, `failed`."* Every other verb is paired with a component write by construction.

But not every mutation deserves a wake. `moved` fires nearly every turn; `took`/`dropped` are inventory shuffling; `chip` and `attacked` fire several times per fight. Those are repeatable churn. The trigger is narrower: **irreversible** change.

| Verb | Written by | Why irreversible |
|---|---|---|
| `generated` | `writeGeneratedRoom` | canon prose, never regenerated |
| `defeated` | `defeatEnemy` | hostile/health/location rows gone for good |
| `learned` | paired with `learnSpell` | canon and permanent, idempotent |

Those three exist today, so this is buildable now without waiting on NPCs. Conversations and solved puzzles add a fourth and fifth when they exist.

Two mechanics fall out:

- **Coalesce, never queue.** Triggers arrive in bursts — clearing a room of three enemies yields three `defeated` in three consecutive ticks, which is one fight, not three story beats. The bard is a **singleton** and has no key to dedupe on (unlike pregen), so: at most one wake in flight ever; a trigger arriving mid-wake sets a dirty flag and the next post-commit check fires once. Simpler than pregen's per-key state machine.
- **A hard ceiling is the main cost control.** Early game, an exploring player triggers `generated` every few turns and ends a fight every 3–6, so realistic cadence is a wake every 2–4 turns at the start, tapering as the map fills. A *never more than one wake per N turns regardless of triggers* rule belongs in engine code, not in the prompt.

Queuing follows `architectQueuePregen`'s rule: main thread, after the tick's transaction commits and after the player's text is flushed, so it can never delay the turn they waited on.

### 12. The overture — one cold wake per world file

The bard also runs once at the beginning, to set the stage. This is the only wake with **no event log to read**: it reads `seed/setting.txt` and writes all three lanes cold. Different input, different prompt, its own thing.

`openWorld` initializes only when the world file does not exist, so the overture runs **exactly once per world, at creation**. A resumed session never pays it — which makes a blocking 5–10 s call at first launch acceptable in a way a per-turn cost never is.

It should **not** live in `world.cpp`: that translation unit has no network and no AI, and `initialize()` runs inside a transaction. It belongs in `main.cpp` immediately after `openWorld` returns — which means `openWorld` must report whether it just created the world, since today it returns a `Db` either way. Small API change, known now rather than discovered later.

### 13. Macro and micro — the story catalog

The overture exists to solve the weakness of a purely reactive bard: with only reactive wakes, nothing can ever happen *to* the player, and the story can only ever be a response.

The split: **the macro authors *who, what, and why*; the micro decides *where and when*.** The overture cannot author the map, because the map does not exist yet — but everything map-independent is fair game.

| Overture can author (map-independent) | Cannot author |
|---|---|
| Who exists, and what each wants | Where anyone is |
| The faction / antagonist force | Which room holds what |
| The clock — what is getting worse | When anything is encountered |
| Puzzle *kinds* — lock/key pairs, abstract | Which door the key opens |
| What would constitute an ending | The path to it |

Everything in the right column is exactly what an irreversible event settles.

**The mechanism is the bestiary, one level up.** `bestiary` is a frozen catalog of archetypes that exist as data long before any instance exists in the world; `placeEnemy` casts an instance from the mold; the engine computes an eligible menu and the model picks a blurb. Do the same for story: the overture writes a catalog (cast, faction, clock, lock/key pairs, the question the story is asking), and a micro wake does not *invent* — it **selects from its own catalog and places**. Same proposes/disposes shape, one level up.

What that buys:

- **Coherence by construction.** A room cannot be off-theme if the only things it may contain are things the arc already named.
- **It answers the blandness fear directly.** The literary work happens once, with the whole picture in view and a generous budget — not improvised in a 400-token reaction while the player waits.
- **Placement is checkable.** Selection from a closed set is testable; "did it write a good story" never will be.
- **Still no command channel.** The architect reads the catalog and picks from it. It is not told what to do; it is told what exists.

**Where enforcement stops, stated honestly.** The engine *can* enforce that nothing appears in the world the bard did not first write into its catalog — mechanical, exactly like `architectCommitProposal` re-checking an enemy blurb against the live eligible menu before placing. The engine *cannot* enforce that a newly appended catalog row is thematically coherent. And the catalog must be able to grow, or a fixed cast runs dry and eventually the player does something the overture never imagined. That hole is the subject of the unresolved question below.

### 14. The LOD ladder generalizes, and closes the loop

The story-seed design defines the ladder for rooms: **L0 referenced** (entity + name), **L1 sketched** (mutable `sketch` prose), **L2 realized** (canon `description`, never rewritten). A latent exit is L0; a generated room is L2.

It generalizes to the whole cast. A catalog character is L0. One the bard has fleshed out but the player has not met is L1. One the player has spoken to is L2 — canon, unrewritable. Same for puzzles: a lock/key pair in the catalog is L0, placed in the world it is L2.

Which closes a loop with decision 11: **an irreversible event *is* a materialization.** `generated` = a room reached L2; `defeated` = something left the world permanently. The wake trigger and the promise/redemption structure are the same mechanism seen from two sides — the bard wakes when something materializes, and its job is to decide what materializes next. One existing table split carries both.

### 15. Model tiering falls out of macro/micro

The split maps straight onto cost, the way `AiRole` already does:

- **Overture** — once per world file, quality matters enormously, latency is a one-time startup cost. Opus 4.8 (or Fable 5 to see the ceiling), high effort, generous `max_tokens`. The one call where being expensive is obviously correct.
- **Micro wake** — selection from a menu plus a short journal update. Classification-shaped work, which is what Haiku already does for the resolver; Sonnet 5 if more judgment is wanted.

### 16. The bard authors situation, never urgency — escalation is spatial

Added after the prior-art research surfaced Apocalypse World countdown clocks as a fix for the purely-reactive gap. **Rejected, and the reason is a design constraint worth stating positively.**

`seed/setting.txt` already settles it: *"At the safe edges the only tension is that of being out of bed after curfew; nearer the breach it is the goblins themselves. The deeper and more inward the halls, the more contested; the further out, the safer and the stiller."* And decisively: *"The invasion is a spreading front, **not a flood**."*

Tension in this game is **a gradient in space, not in time**, and it is already implemented — `distanceFromSeed` does a BFS over realized exits and gates enemy tier on the result. The player controls their own exposure by choosing how deep to go.

A countdown clock converts that gradient from space to time. Once the invasion advances on a timer the outer edges stop being safe, and the authored structure collapses into the flood the setting explicitly rejects.

This also dissolves most of the "purely reactive means nothing happens *to* the player" worry: the player moves, and moving changes exposure. **Movement through space is the clock**, and that mechanism is already built and tested.

The constraint, stated for the prompt: **the bard may create situations, but not urgency.** No ticking threats, no "before it is too late," nothing that makes standing still or talking at length feel expensive. Pressure exists in exactly two places — combat, and depth — both opt-in by the player's own movement. This is precisely the instinct a story model violates by default; asked for dramatic tension, it reaches for a deadline.

## Cost and latency

Current per-turn spend, approximate: resolve on Haiku 4.5 (~$0.0009) + narrate on Opus 4.8 (~$0.014) ≈ **1.5 ¢/turn**, ~$3 for a 200-turn session.

The irreversible-change trigger fires more often than the every-10-turns figure this session started from — realistically a wake every 2–4 turns early on, tapering as the map fills and fights lengthen. At every 3 turns, with a micro wake around 4k in / 300 out (selection plus a journal update, not composition):

| Micro-wake model | Per wake | Amortized | vs. today |
|---|---|---|---|
| Opus 4.8 | ~$0.040 | ~1.3 ¢/turn | ~+90% |
| Sonnet 5 (intro $2/$10) | ~$0.011 | ~0.4 ¢/turn | ~+25% |
| Haiku 4.5 | ~$0.0055 | ~0.2 ¢/turn | ~+12% |

So decision 15 is not a micro-optimization — it is the difference between the story layer costing as much as the rest of the game and costing a rounding error. The **hard ceiling** (decision 11) is the backstop for the tail case where triggers cluster.

The overture is a one-time ~$0.10 on Opus 4.8 per world file, which amortizes to nothing over a session.

Either way, **zero added latency** after startup, because nothing waits on the bard.

For contrast, the rejected shapes: a per-turn bard is ~4 ¢/turn, roughly triple the cost of the whole rest of the turn. N NPCs each thinking every tick multiplies both cost and, if serialized, latency (four NPCs ≈ 16 s turns) — which is why lazy reconstruction, not simulation, is the NPC answer.

## Correction to an existing note

[per-turn-latency-remedies.md](../research/per-turn-latency-remedies.md) concludes prompt caching silently no-ops because the minimum cacheable prefix is 4,096 tokens while our prompts are ~1,100–1,600. **That is now stale for Opus 4.8, where the minimum is 1,024** — the narrator and architect prompts clear it. Haiku 4.5 is still 4,096, so the resolver stays uncacheable.

This matters more once the bard exists: a large stable story prefix consumed by every downstream agent is exactly the shape caching exists for, with reads at ~0.1× input price. Re-measure `cache_read_input_tokens` before designing around "caching does not help us."

## Carried forward to design

Settled enough to stop exploring, not settled enough to build. These are design decisions, not open explorations.

1. **Storage.** Freeform blob in `meta` (zero DDL, mirrors the `setting` precedent) vs. structured tables. The three lanes may not want the same answer: setting is already a `meta` row, but the catalog is a *selectable menu* and therefore wants rows with ids the way `bestiary` does, and pressures are freeform. The schema currently has no home for narrative-only assertions — that row in the fact-store table is explicitly marked *deferred*.
2. **Readership.** Architect only, or NPCs too? Different features with very different blast radii. Note the catalog makes this sharper than it was: an NPC reading *pressures* is a very different grant from an NPC reading *the cast list*.
3. **Does the bard see raw player input, or only event rows?** The log is the engine's truth, but *"the player typed 'I don't trust the headmaster' and then did nothing"* is precisely what a GM reads a table for — and it exists nowhere in `events`. Adding it means the bard sees things the engine does not consider real: either its most valuable signal or a hallucination vector.
4. **The eligible-menu function for placement.** The enemy analog is `eligibleEnemyBlurbs`, gated on distance from seed, the spawn ledger, and whether a drop teaches a spell the player lacks. The story analog does not exist and is not obvious: what makes a catalog character or a lock/key pair *eligible here, now*?

## Unresolved

**The retcon boundary — the part of this design trusted least.**

The catalog must be able to grow: a fixed cast runs dry, and eventually the player does something the overture never imagined (burning down the library the arc's secret was buried under). So a micro wake needs an append path. But appending is exactly where incoherence — and self-justifying revision — re-enters.

Two bad options and one untested third:

- A bard that **can rewrite its own arc**: retcon risk. It quietly edits what it always wanted so that it was never wrong, and the player can never catch it being wrong.
- A bard that **cannot**: brittle. The story is now about a thing that no longer exists.
- **Append-only arc that records its own breakage** — the bard may write *"the library is gone; the secret is unreachable by that route"* but may never delete the original promise. The player can then catch it being wrong, which is arguably a feature.

The third is the working answer and the reason all three lanes are marked append-only or free-sketch, with nothing rewritable in between. It has not been tested and it is the first thing to attack in design.

Related and equally untested: intent is mutable, but the moment intent touches the world it becomes an event row, which is immutable. That may be the whole guarantee, or it may only look like one.

## Parked (NPCs)

Raised for context, deliberately not pursued:

- **The ISA is the work, not the AI.** `Verb` (`action.hpp:11`) is closed and has no `Talk`. Player→NPC needs a verb whose payload is free text rather than a closed vocabulary; NPC→world needs a *strict subset*, gated by eligible menu, or an NPC can pocket the key a puzzle depends on.
- **Is dialogue canon?** If an NPC says "my brother died in the flood," is there now a brother and a flood? Dialogue is a firehose of world-assertions with no gate, and the narrator's "nouns must exist" rule cannot apply to it and stay interesting.
- **A latency trick worth testing:** if NPC speech is printed **verbatim** rather than re-prosed by the narrator — the way canon room descriptions already are — then the NPC call and the narrate call are independent and can run concurrently, keeping the turn at ~4 s and saving the narrator's tokens. Unverified: the narrator still needs to know *something* happened, and the two agents cannot see each other.
- **`resolveCombat` is the precedent** for an NPC turn system: a system running after the player's action inside the same transaction, owned by `loop.cpp`. But that puts a model call inside the open tick transaction — the wrinkle the architect accepted once and then spent a whole feature undoing.
