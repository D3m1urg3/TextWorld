---
title: Blocked directions + truthful exit display — requirements
date: 2026-07-09
status: draft
tags: [world-generation, exits, blocked-directions, exit-display, architect, latent-exits, render, mutations, requirements]
modules: [world-gen, architect, systems, render, mutations, world]
related: [.lore/work/brainstorm/blocked-directions-and-exit-display.md, .lore/work/research/procedural-room-feel.md, .lore/work/specs/story-seed-architect.md, .lore/work/brainstorm/story-seed-and-lod-world.md]
req-prefix: EXITS
---

# Blocked directions + truthful exit display — requirements

## Context

The architect (spec [.lore/work/specs/story-seed-architect.md]) currently generates a
room for **any** invertible direction the player tries at an unmapped exit, and the
`Exits:` line lists only *realized* exits. Consequence: every room is silently an
eight-way junction, the world has no walls or dead ends, and the displayed exits are a
subset of what is actually walkable — they cannot be truthful. Established in
[.lore/work/brainstorm/blocked-directions-and-exit-display.md].

**This spec makes exits a property declared at a room's birth.** A room may only be
left through a declared exit; a declared exit may point at a room that does not exist
yet (**latent**, `dest = NULL`), and walking it triggers generation; anywhere else is a
**wall** (blocked, not shown). The architect **co-authors** the onward exits in the same
`create_room` call as the prose (structure follows setting + room content); the engine
owns the invariants (the return exit, direction legality, dedup). This **reverses** the
story-seed-architect deferral "Reference-leak onward exits" while keeping the others.

**Controlling invariant.** *Every direction shown is a standing invitation the player may
act on; every direction not shown is a wall.* Precisely: a **realized** exit always moves;
a **latent** exit is **attemptable** — walking it either generates-and-moves or, on a
transient generation failure (REQ-EXITS-3), walls *for that turn while remaining offered
and retryable*. So "displayed == walkable" holds as "displayed == a direction the engine
will honor as an attempt," never as "guaranteed to move this instant." The design problem
(blocked directions) and the player problem (honest display) are one: the exits line
cannot be truthful until "open" is defined, and defining it makes the line truthful. Once
the exit table holds latent stubs, the existing render query already lists the right set —
the display corrects itself.

**Scope: v1 tree.** Every declared exit spawns a *new* room; nothing reconnects. The
research ([.lore/work/research/procedural-room-feel.md]) shows a pure tree is the weakest
"feel" outcome — **loop closure is the primary feel lever** — but it is a strictly
additive follow-on. This spec ships the tree (truthful display + real walls + possible
dead ends); loop closure is out of scope (below).

**No schema change.** `exits.dest` already has no `NOT NULL` (`world.cpp:22`), so
"latent = NULL dest" needs no DDL and **no `SCHEMA_VERSION` bump**; existing worlds
(all non-null) stay valid. A shipped `world.db` generated under the old model simply has
no latent exits and behaves as a frozen tree until regenerated from the seed.

## Requirements

### Exit model

**REQ-EXITS-1** — An `exits` row is one of three states, and they are exhaustive:
**realized** (`dest` non-NULL → a generated room lies that way; move through it);
**latent** (`dest IS NULL` → an open direction whose room is not generated yet); **wall**
(no row for `(room, direction)` → the player cannot leave that way). No DDL change, no
`SCHEMA_VERSION` bump. Latent rows carry **only invertible directions** (REQ-ARCH-8),
guaranteed by the engine gate (REQ-EXITS-7) and by seed authoring (REQ-EXITS-9) — so realizing
one always has a reciprocal.

### Movement

**REQ-EXITS-2** — `resolveGo` (`systems.cpp`) **replaces** its former (a/b/c) branch
(REQ-ARCH-3) with, in this order: **(a)** exit row exists with non-NULL `dest` → move
(unchanged); **(b)** exit row exists with NULL `dest` **and** generation is enabled
(`aiNarrationEnabled()`, REQ-ARCH-2) → call `architectGenerate` to realize it, then move
through the now-realized exit; **(c)** otherwise (no row, or a latent row with generation
disabled, or generation failed) → the wall, unchanged text `"You can't go that way."`.
The former "generate for any bare invertible direction with no row" is **removed**:
generation now fires **only** on a pre-existing latent row, so a direction with no row is
always a hard wall. **This supersedes REQ-ARCH-3b in full** — the invertibility test moves
off `resolveGo` (every latent row is invertible by construction, REQ-EXITS-1) and onto the
gate/seed; REQ-ARCH-3a (realized → move, precedes any AI call) and REQ-ARCH-3c (the wall)
are retained.

**REQ-EXITS-3** — A **failed** realization (architect Phase-1 failure, REQ-ARCH-4) walls
for that turn but **leaves the latent row intact** (no write on Phase-1 failure), so the
exit is retryable next turn. Realization never deletes or downgrades the latent row; it
only fills its `dest` (REQ-EXITS-7). Re-crossing a realized exit takes branch (a) with no
AI call (persistence unchanged, REQ-ARCH-3a).

### Display

**REQ-EXITS-4** — The `Exits:` line (`render.cpp`, `roomBlock`) satisfies the controlling
invariant: it lists **all realized exits always**, plus **all latent exits when the
architect is enabled** (a latent exit is attemptable only if it can generate). When the
architect is disabled, latent exits are **omitted** (walking one would wall). A latent exit
reads **identically** to a realized one — no marker distinguishes "already explored" from
"not yet generated" (discovery is preserved). A room always has ≥1 exit (the realized return
written in REQ-EXITS-8), so the line is never empty for a generated room.

The predicate that gates latent-exit display is a **distinct, purpose-named
`architectEnabled()`** — NOT the raw `aiNarrationEnabled()` used for prose. Both may be
backed by the same env today, but display of a latent exit is an **ontological** fact about
the map (can this direction be traversed at all), whereas prose narration is **cosmetic**;
overloading one predicate for both means a cost/outage toggle silently re-shapes the map.
Naming them apart lets the two concerns diverge later and documents the intent. Note: the AI
switch is read at process start, not mid-turn, so a *within-session* flip is not a supported
runtime path — the concern is conceptual clarity and future-proofing, not a live race.

### create_room tool + prompt (architect co-authors exits)

**REQ-EXITS-5** — The `create_room` tool schema (REQ-ARCH-7b) gains one field: **`exits`**
— an array of **strings**, each an invertible direction name (REQ-ARCH-8). It is
**optional** (absent or empty array is legal — a **dead end**). `name` and `description`
remain required; no other fields. This supersedes REQ-ARCH-7b's "no other fields" clause
for `exits` only. Bare direction names carry no hint: the flavor of each exit reaches the
next room for free via the origin's prose (REQ-EXITS-6 requires the prose to describe its
exits, and the origin description is already in the child's generation context,
REQ-ARCH-7a).

**REQ-EXITS-6** — The architect system prompt (REQ-ARCH-7c) is amended: its former
prohibition on mentioning exits is **flipped into a requirement**. The model **declares
the directions that lead onward** in `exits`, chosen to fit the setting and this room —
**density is the model's call** (a sealed vault may declare none but the way back; a
crossroads several); the engine imposes no count. It must **exclude the entry-return
direction** (the way the player just came; the engine adds that). It must **describe the
exits it declares** in the prose and **name no opening it does not declare**. Unchanged:
no ids, no arrival narration, the description is of THIS room only.

**Prose↔exits consistency is a coherence concern, not a mechanical gate.** Nothing in the
engine verifies that the description actually mentions the declared directions (or mentions
no others); the mechanical Exits line is authoritative and always truthful, while the prose
is trusted-by-co-authorship (both written in one call by one author) and checked only by the
live coherence judge (REQ-ARCH-13), consistent with how all other prompt-quality claims are
handled. A cheap non-fatal lint (each surviving direction word appears in the description) is
noted as a possible future nicety but is **out of scope** here — the player-facing risk is a
prose/line mismatch, not a broken map.

### Validation gate

**REQ-EXITS-7** — `validateRoomProposal` (REQ-ARCH-9) is extended to carry a cleaned exit
set: `RoomProposal` gains `std::vector<std::string> exits`. The **name/description clauses
(a–c) remain the only fatal ones** — a bad exit never rejects an otherwise valid room.
The gate parses `exits` if present, **normalizes** each entry (trim, lowercase) before
matching so that `"North"` / `" up "` are kept rather than spuriously dropped, then
**sanitizes leniently** (dropping, with one stderr diagnostic per drop, never failing) by
dropping, in each case, an entry that is: not a string; not one of the eight invertible
directions (after normalization); a duplicate of an already-kept entry; or the **return
direction** — `inverse(direction-of-travel)`, i.e. the way back to the origin, which the
model was told to omit (REQ-EXITS-6) but may include anyway. Survivors form the cleaned
onward set (naturally ≤7). Absent/empty `exits` → empty set (dead end). This lenient-for-exits / strict-for-prose split is a
deliberate departure from REQ-ARCH-9's all-or-nothing gate, justified because exits are
advisory structure the engine sanitizes, whereas name/description are load-bearing canon.

### Write helper

**REQ-EXITS-8** — `writeGeneratedRoom` (`mutations.cpp`, REQ-ARCH-9) is extended, inside
the caller's transaction, to:
  1. **Realize the origin exit** — post-condition: `(originRoom, direction)` is realized to
     `newRoom`. In the production path this row **must pre-exist as latent** (resolveGo reaches
     generation only via a NULL-dest row, REQ-EXITS-2b), so the operation is an **update** of
     that row's `dest`. The write is expressed as an upsert purely for defensiveness; an
     *absent* row here is a precondition violation of REQ-EXITS-2b, not a normal path, and
     warrants a stderr diagnostic — it must never be relied on by callers.
  2. **Write the realized return**: `(newRoom, inverse(direction)) → originRoom` (unchanged).
  3. **Plant latent stubs**: for each direction in the cleaned `proposal.exits`, insert
     `(newRoom, dir, NULL)`. The set is already deduped and already excludes the return
     direction (REQ-EXITS-7), so it cannot collide with the return row written in step 2;
     no further filtering is needed here.
  4. **Append one `generated` event** (unchanged, REQ-ARCH-9 subject/object reading).
The `generated` event stays renderer-invisible and excluded from `buildFacts` (REQ-ARCH-10,
unchanged); latent stubs emit no events.

**Invertibility is guarded at use, not merely trusted at authoring.** Realizing a latent row
(step 1→2) requires `inverse(direction)`; `writeGeneratedRoom` already throws on a
non-invertible direction (existing parent behavior), which the tick transaction rolls back
(REQ-ARCH-5) — so a bad direction can never write a half-room. But a rolled-back realization
leaves a latent row the player can see yet never pass (a displayed≠walkable hole). The gate
(REQ-EXITS-7) makes this unreachable for AI-declared exits; the remaining exposure is
**hand-authored seed rows**, which REQ-EXITS-9 constrains to invertible directions and
REQ-EXITS-10's seed test exercises end-to-end (a typo'd seed direction fails that test rather
than shipping a dead exit).

### Seed frontier

**REQ-EXITS-9** — Because generation now fires **only** on latent rows, `seed/base.sql`
must plant a frontier or a fresh world cannot grow. The engineer edits `seed/base.sql`
(hand-authored SQL, not AI-generated) to add a **modest** set of latent exits (2–3 total)
`INSERT`ed with `dest = NULL` on the starting cell/corridor, coherent with Thornmere Hall
(`seed/setting.txt`) — e.g. the corridor leading on to an unseen stair or landing, a way
up to the floor above. The realized cell↔corridor exits are **unchanged**. These are
`dest = NULL` rows. Committed as part of this step.

**Migration.** `world.db` is currently a *live persistent save*, not a template — `openWorld`
seeds it once and thereafter it accumulates play. Since `SCHEMA_VERSION` is unchanged, any
pre-existing `world.db` (the committed one, or a player's) opens silently under the new model
but has **no latent rows at all**, so it is a **permanently frozen tree** — every unexplored
direction is now an indistinguishable wall, with no backfill. For this early-stage prototype
that is acceptable **only by regenerating from the seed** (delete `world.db`, let it rebuild);
the committed `world.db` is regenerated as part of this step. Explicitly decided: **do not**
bump `SCHEMA_VERSION` to force-refuse old worlds (it would also discard any real progress);
the frozen-tree/regenerate tradeoff is documented instead. Revisit if `world.db` becomes a
precious save worth a real migration.

### Testing

**REQ-EXITS-10** — Unit tests (no network, default `tests` target) cover, via the existing
canned-transport fixtures extended to carry an `exits` array:
  - **resolveGo three-case** — realized → move; latent + AI-on (canned) → realized + move
    + origin exit now non-NULL; **undeclared direction → wall**; latent + AI-off → wall;
    failed generation → wall with the latent row still present (verified by two consecutive
    attempts at the same latent exit with Phase-1 forced to fail both: each walls and the
    row stays NULL, provable-retryable).
  - **display invariant (REQ-EXITS-4)** — a room with both realized and latent exits: AI-on
    lists both; AI-off lists only realized; latent renders identically to realized.
  - **gate (REQ-EXITS-7)** — a proposal with valid `exits` yields the cleaned set; junk
    entries (non-string, `"northeast"`, a duplicate, the entry-return direction) are each
    dropped and the room is still created; name/description remain fatal.
  - **write helper (REQ-EXITS-8)** — a canned proposal `exits:["east","down"]` realized from
    a latent `origin→north` yields: origin north realized to the new room, new south→origin
    realized, new east/down as latent NULL rows, one `generated` event.
  - **persistence** — after realizing a latent exit, re-crossing takes branch (a), no
    regeneration.
Existing REQ-ARCH-12 cases are updated to the new contract (the creation test seeds a latent
origin exit; the disabled-mode/REQ-ARCH-2 test additionally asserts latent exits are hidden
and walls are byte-identical).

**REQ-EXITS-11** — The gated live smoke (REQ-ARCH-13, `TEXTWORLD_AI_LIVE_TEST=1`) is
extended to assert, **mechanically only**:
  - a generated room's declared exits become latent rows; walking one continues generation;
  - the displayed==walkable invariant, in both directions and **failure-tolerant** per the
    controlling invariant: **every** invertible direction *not* on the exits line walls when
    walked; and **every** direction *on* the line, when walked, either moves/generates **or**
    walls-while-leaving-its-row-intact (a transient Phase-1 failure, REQ-EXITS-3, is a
    *conforming* outcome, not a violation — the oracle must not flag it). A shown direction
    that walls **and drops its row**, or an unshown direction that moves, is the only failure.
  - **anti-degeneration** (guards the silent all-dead-ends failure of REQ-EXITS-7): across the
    generated chain, assert a **nonzero** fraction of rooms declare ≥1 surviving onward exit —
    i.e. the world is not collapsing to a straight corridor of dead ends because the model's
    exit shape is being wholesale-dropped. A run where *every* room declares zero onward exits
    fails, surfacing a systematically bad exit format that per-drop stderr lines alone would
    bury.
No wording assertions; coherence remains the single bounded judge call.

## Out of scope (explicit deferrals)

- **Loop closure / dedup / bind-to-existing.** The v1 world stays a **tree** — every
  declared exit spawns a new room, no two nodes denote the same place. Binding a declared
  exit to an existing room (the primary "feel" lever, [.lore/work/research/procedural-room-feel.md])
  is the next increment, strictly additive.
- **Obstructed exits (doors / locks / keys).** Only **walls** (absent row) and
  **latent-open** (NULL dest) exist. A shown-but-impassable exit needs a state column and
  an unlock action — deferred. Guardrail: nothing may assume "row exists ⇒ passable" (a
  latent row with AI off is already a non-passable existing row), so an obstructed state
  can slot in later.
- **Exit hints fed to child generation.** Bare direction names only; the origin's prose
  carries flavor via `origin_description`. A stored per-exit hint honored at child
  generation is deferred.
- **Euclidean / mappable geometry.** Space stays a pure graph (going N-E-S-W need not
  return you home); coordinates are not tracked.
- **LOD sketch lane (L1), prefetch, background generation.** Latent stubs are bare L0
  (name-less: just a direction). Sketching and prefetch remain deferred (story-seed-architect).
- **World-size cap / latent eviction.** No cap read, no eviction.

## AI Validation

How the AI verifies completion, behaviorally. Each item names the requirements it verifies.

1. **Build, no schema change (REQ-EXITS-1):** `cmake --build build` succeeds; `grep -n
   "SCHEMA_VERSION" src/world.hpp` still shows `1`; no `ALTER`/new column in the DDL.
2. **Three-case movement (REQ-EXITS-2, -3):** with a canned transport — walking a realized
   exit moves; walking a latent exit generates and moves and the origin row is now non-NULL;
   walking a direction with **no row** walls with the unchanged text; a forced Phase-1
   failure walls and the latent row is still present (a second attempt retries).
3. **Display invariant (REQ-EXITS-4):** build a room with one realized and one latent exit;
   with AI enabled the `Exits:` line names both, with AI disabled only the realized one; the
   two exits render with identical wording.
4. **Tool + prompt (REQ-EXITS-5, -6):** inspect the `create_room` schema for the optional
   string-array `exits`; read the prompt constant and confirm it now *requires* declaring
   onward exits, excludes the return direction, and requires the prose to describe them.
5. **Gate leniency (REQ-EXITS-7):** a canned proposal whose `exits` mixes valid directions
   with a non-string, `"northeast"`, a duplicate, and the return direction still creates the
   room, keeping only the valid non-return directions; a blank name/description still walls.
6. **Write helper (REQ-EXITS-8):** after realizing `exits:["east","down"]` from a latent
   `origin→north`, query `exits`: `origin→north` realized, `new→south→origin` realized,
   `new→east` and `new→down` present with NULL dest, and exactly one `generated` event.
7. **Seed frontier (REQ-EXITS-9):** a fresh world from `seed/base.sql` has ≥1 latent exit on
   the starting rooms; with AI enabled, walking it generates a room; the exits line shows it;
   the realized cell↔corridor exits are unchanged.
8. **Regression + disabled parity (REQ-EXITS-10):** `./build/tests` passes including the
   updated REQ-ARCH cases; with `ANTHROPIC_API_KEY` unset, walls are byte-identical to the
   pre-feature text and latent exits are hidden.
9. **Live end-to-end (REQ-EXITS-11 — manual, optional):** with a real key and
   `TEXTWORLD_AI_LIVE_TEST=1`, generate a short chain; assert each room's declared exits
   become latent rows, walking them continues generation, and the rendered exits line equals
   the walkable set at every step.
