---
title: Story seed + architect — interface and fact-store design
date: 2026-07-09
status: draft
tags: [ai-integration, story-seed, world-generation, architect, bard, fact-store, schema, level-of-detail, persistence, bounded-context]
modules: [world-gen, architect, systems, mutations, world]
related: [.lore/work/brainstorm/story-seed-and-lod-world.md, .lore/vision.md, .lore/work/specs/ai-prose-renderer.md, .lore/work/specs/ai-resolver.md, .lore/work/design/engine-foundation.md]
---

# Story seed + architect — interface and fact-store design

## Scope

Technical design of two things from [.lore/work/brainstorm/story-seed-and-lod-world.md]:

1. The **bard↔architect interface**.
2. The **fact-store schema** (event-log projection vs materialized facts table; the stub-identity/dedup mechanism).

Build target: the **first atomic step** — a minimal architect sufficient to test the bard, bar = *"~4 mutually-coherent persisted rooms in one session."* Full LOD, prefetch, world-cap, dedup, and story-evolution are **designed in shape, deferred in build.**

## What the code already gives us (grounding)

Read of the current engine (`world.cpp`, `systems.cpp`, `mutations.hpp`, `prose.hpp`, `nlresolve.hpp`):

- **The fact-store is already here.** `description(entity, prose)` is commented *"canon: row exists = never regenerate"* — the persistence contract is a schema invariant, not a discipline. Established physical canon = the component tables (`description`, `name`, `exits`, `location`, `room`, `portable`). History = the append-only `events` log.
- **The seam is one branch.** `resolveGo` (`systems.cpp:49`): exit exists → `moveEntity`; else → `appendEvent(..., "failed", ..., "You can't go that way.")`. World-gen replaces that `else`.
- **Two AI units exist, both read-only by contract** (`prose.hpp`, `nlresolve.hpp`): SELECT-only, network egress, **no ids to the model, ids assigned mechanically**, total-catch fallback to a deterministic path. They share an `HttpTransport` seam and a build→request→transport→validate shape.
- **The tick is one transaction** (`loop.cpp:50`): `db.begin()` → increment turn → `resolve()` → `db.commit()`; any throw → `rollback` → EngineError.

## Decision 1 — No new facts table. The store *is* the interface.

**The bard and the architect never call each other. They communicate through the canon store** — exactly how every system in this engine already communicates (systems talk through component rows + events, never directly). The bard *writes* facts; the architect *reads* facts and *writes* rooms; the store is the seam.

This answers "event-log projection vs materialized facts table" directly: **both already exist, with distinct roles, and no third table is added.**

| Fact kind | Where it lives | Mutability | Status in brainstorm terms |
|---|---|---|---|
| **Setting** (tone, premise, scale) | `meta.setting` blob (loaded from a seed text file at init) | immutable for now | established, given |
| **Physical canon** (rooms, items, exits, prose) | existing component tables | append-only; `description` row = never regenerate | established |
| **History** (what happened) | existing `events` log (+ new `generated` verb) | append-only | the projection source |
| **Narrative-only assertions** ("the priests vanished") | — | — | **deferred** (arrives with story-evolution) |
| **Latent stubs / coarse sketches** | reserved `sketch` lane (see Decision 5) | mutable until realized | **deferred** |

A dedicated `facts` table of natural-language assertions is *not* built: for the atomic step every fact the architect needs is `meta.setting` + the immediate neighborhood, and a parallel assertion store would duplicate canon already expressed as component rows (violates vision principle 2).

### Setting loading

`openWorld` currently runs `seed/base.sql`. Add: read a `seed/setting.txt` (freeform prose describing world/tone/scale) and `INSERT INTO meta(key,value) VALUES('setting', <prose>)`. **Zero DDL** — `meta(key TEXT PRIMARY KEY, value)` already exists, so this is a new *row*, not a new *shape*. Swappable setting file without touching SQL; aligns with the vision's "starts from a user input file."

## Decision 2 — The bard is degenerate for the atomic step; the architect is the only live call.

The brainstorm named a "bard" (produces facts) and "architect" (emits rooms). But the seed setting is **static** (story-evolution deferred), so for the atomic step:

- **Bard = the setting blob**, loaded once at init. No live agent. There is nothing for a live bard to *do* until narrative evolves, and it has no consumer yet.
- **Architect = the sole live AI call**, at the `resolveGo` frontier.

The bard is tested *through* the architect: if the setting is rich enough that four independently-generated rooms cohere, the "bard" (the setting) is sufficient shared context. When a live bard lands later, **the interface does not change** — the bard simply becomes a *writer* into the store the architect already reads.

> **This reframes the ask, and the reframe is confirmed (2026-07-09).** You said "start with feature 1 (storytelling/bard)." This design delivers feature 1's *testable core* — a persistent setting that seeds world-gen — while deferring the *live, evolving* storyteller (which has no consumer until #2 and #4 exist). **Decided: build setting + architect; live bard deferred.**

## Decision 3 — Architect TU: mirror the resolver, but end in a write.

New translation unit `architect.{hpp,cpp}`, shaped like `nlresolve` — with one deliberate break: it is the **first read-write AI unit**, so the write is isolated in one mechanical, engine-owned helper. The model *proposes*; the engine *disposes*.

| Function | Effect | Mirrors |
|---|---|---|
| `buildArchitectContext(db, room, direction)` → payload | SELECT-only, no ids | `buildResolveContext` |
| `buildArchitectRequestBody(payload)` → request | pure; one `create_room` tool (name, description, optional items[]) | `buildResolveRequestBody` |
| `validateRoomProposal(response)` → `optional<RoomProposal>` | pure, no DB, never throws | `validateAndLower` (minus the DB lookup) |
| `writeGeneratedRoom(db, origin, direction, proposal, actor)` | **WRITE** — mint entity, tags, name, description, reciprocal exits, items, `generated` event; in the caller's transaction | new `mutations.hpp` helpers |
| `architectGenerate(db, room, direction, actor[, transport])` → `bool` | orchestrates; **total try/catch → false on any failure** | `aiResolve` |

**Context (bounded, O(1), independent of world size):** `meta.setting` + the origin room's name + canon description + the direction. Nothing else — not the neighborhood, not history. The bounded-context law holds trivially at this stage; the graded LOD neighborhood (still bounded) arrives later.

**Engine-owned invariants before any write** (vision principles 1 & 3 — AI translates, engine decides):
- proposal has non-empty name and description;
- **reciprocal exit is created mechanically** (dest→origin in the opposite direction: n↔s, e↔w, u↔d, in↔out) — never model-specified;
- the new room advertises **only** the back-exit initially (**decided 2026-07-09: pure tree**, no latent onward stubs in the atomic step); further exits are discovered by trying them (→ more generation);
- entity ids minted here, **never read from the model** (inherits the resolver/renderer discipline).

## Decision 4 — Seam: synchronous generation inside the tick, fall back to the wall.

`resolveGo` becomes:

```
room = roomOf(player)
if dest = exitDest(room, direction):        # exit already exists →
    moveEntity(player, dest, "moved")        #   never regenerate (persistence)
elif aiNarrationEnabled()
     and architectGenerate(db, room, direction, player):
    moveEntity(player, exitDest(room, direction), "moved")   # exit now exists
else:
    appendEvent(player, "failed", "You can't go that way.")  # today's behavior
```

- **Atomicity for free:** the generated room + reciprocal exit + move all commit (or roll back) with the tick.
- **Persistence falls out of existing code:** once the exit row exists, `exitDest` finds it and no regeneration is possible — the `description`-is-canon contract now covers *generated* rooms too.
- **Graceful degradation:** `architectGenerate` catches all its own failures (network, validation, throwing transport) and returns `false`, so an AI-less or failed turn is exactly today's wall — no EngineError, the turn commits as a normal `failed` event.
- **Known wrinkle (accepted for the atomic step):** the network call sits inside the open tick transaction, so the player *stalls* at the frontier for the call duration. Single-player / single-process makes the held write-lock a non-issue; the stall is the brainstorm's known cost, whose full-feature answer is **background prefetch + LOD** (deferred).

## Decision 5 — Forward-compatible shape for the deferred machinery.

Nothing below is built now, but the atomic-step schema must not trap us (vision tension: ship the step unless it creates an expensive-to-unwind trap).

**LOD levels are *derived from which rows exist*** — no `level` column, and the `description`-is-canon contract stays pristine because coarse prose lives in a *separate, mutable* lane:

| Level | Rows present | Prose lane | Mutable? |
|---|---|---|---|
| **L0 Referenced** | `entities` + `room` + `name` (+ reference metadata) | none | — |
| **L1 Sketched** | + `sketch(entity, prose)` | `sketch` | yes (latent) |
| **L2 Realized** | + `description(entity, prose)` + items + exits | `description` | **no (canon)** |

Realization = a `sketch` becoming a `description`. **Monotonic refine-never-overwrite becomes a schema invariant**: sketches are rewritable *because they aren't canon*; descriptions never are. This adds one table (`sketch`) → a SCHEMA_VERSION bump, cheap now (migration story = "delete the world file," content is disposable).

- **World cap** → `meta.world_cap` (reserve the key now, unused). Cap counts **L2 rooms only**; latent L0/L1 stubs are evictable (LRU by distance) because they never happened — established rooms never are.
- **Dedup / stub-identity** → deferred and *not needed yet*: the atomic step grows a **tree** at unmapped exits (no reference-leak, no loops), so no two nodes can denote the same place. Dedup becomes load-bearing only when reference-leaks + the cap force loop-closure. Its future home: a resolution step in `writeGeneratedRoom` that binds a proposed reference to an existing node instead of minting one.

## Atomic step — scope and validation

**In:** setting blob loaded at init; architect at `resolveGo`; synchronous in-tick generation of one L2 room with a mechanical reciprocal exit; persistence + wall fallback.
**Out:** live bard, LOD/sketch lane, prefetch, world-cap/eviction, dedup/loop-closure, reference-leak exits, feeding events to the architect.

**Validation** (honors [[verification-must-be-bounded]] — no open-ended live-LLM loops):

- **Mechanical, deterministic (CI), injected canned transport** — the load-bearing tests:
  - canned `create_room` → going north from the hall creates a room with the canned name/description, a reciprocal south exit, a `generated` event; player moves there.
  - **persistence / no-regeneration:** south then north again → the *same* room, and the **transport is invoked zero times** on the second crossing.
  - **fallback:** transport error / invalid proposal / AI disabled → the `failed` wall, turn commits, no partial room written (rollback leaves no orphan entity).
  - **ids not from model:** a proposal carrying an `id`-looking field changes nothing about the minted id.
- **Gated live smoke (manual, not CI)** — mirrors the resolver's gated smoke: walk four rooms against the real API; a **bounded** coherence check (feed setting + the four descriptions to one judge call, "coherent with setting and each other? y/n") — single call, no loop.

## Decision summary

1. **No new facts table** — the component tables + event log are the fact-store; setting is a `meta.setting` blob loaded from a seed text file (zero DDL).
2. **Store-mediated interface** — bard and architect communicate through canon, never directly; bard is degenerate (static setting) for the atomic step.
3. **Architect = first read-write AI unit**, shaped like the resolver, write isolated in one engine-owned helper enforcing reciprocity/invariants; ids minted mechanically.
4. **Seam = `resolveGo`**, synchronous in-tick generation, total-catch fallback to today's wall; persistence and atomicity fall out of existing code.
5. **Deferred machinery is shape-compatible** — LOD via a separate `sketch` lane (keeps `description`-canon pristine), cap via `meta.world_cap` counting L2 only, dedup unneeded while the world is a tree.

## Decided (2026-07-09)

- **Bard scope:** build the static setting + architect; live/evolving bard deferred (Decision 2).
- **New-room exits:** pure tree — new rooms show only the way back; no latent onward stubs in the atomic step (Decision 3 invariants).

## Open questions

- **Setting storage:** `meta.setting` blob (zero DDL, ship now) vs a structured `setting(prose, tone, scale, cap)` table (needs a bump). Leaning `meta` now; promote when the sketch lane forces a bump anyway. *(Spec REQ-ARCH-1: `meta.setting`, loaded via a `settingPath` param mirroring `seedPath`.)*
- **Coherence judge:** ~~human eyeball vs the one-shot bounded LLM-judge~~ **Resolved (spec REQ-ARCH-13):** a single bounded LLM-judge call in the gated live smoke — one call, human reads the result, no tuning loop.
