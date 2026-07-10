---
title: Blocked directions + truthful exit display — exits declared at birth
date: 2026-07-09
status: resolved
tags: [world-generation, exits, blocked-directions, exit-display, architect, level-of-detail, structure-content-split, graph-world]
modules: [world-gen, architect, systems, render, mutations]
related: [.lore/work/brainstorm/story-seed-and-lod-world.md, .lore/work/research/procedural-room-feel.md]
---

# Blocked directions + truthful exit display — exits declared at birth

## Context

Room generation (the architect, `create_room`) is working. Two problems surfaced,
posed as one design and one player problem:

1. **Design:** every room is silently an 8-way junction. `resolveGo` generates a
   room for *any* invertible direction with no exit yet, so the world has no walls,
   no dead ends, no authored shape — a uniform infinite grid.
2. **Player:** the `Exits:` line is built from the `exits` table, i.e. only the
   directions already walked and realized. It shows a subset of what's actually
   walkable and so cannot be truthful.

This descends directly from [[story-seed-and-lod-world]] — the reference-leak
frontier and the L0/L1/L2 stub idea sketched there but never wired into `resolveGo`.

## The key realization: these are one problem

The display can't be made truthful until "open" is *defined*. Today the honest
render of actual behavior would be `Exits: north, south, east, west, up, down,
in, out` on every room. Deciding which directions are blocked **is** the act of
making the exit line correct. One feature, one edit surface — not two.

## Current mechanics (grounded in code)

- `resolveGo` (`src/systems.cpp:53-82`): (a) exit row exists → move; (b) no exit,
  AI on, direction invertible → **generate a room and move**; (c) else wall.
  Case (b) is the bug — every invertible direction is a frontier.
- Eight invertible directions: `north/south/east/west/up/down/in/out`
  (`inverseDirection`, `src/architect.cpp:214-226`).
- `Exits:` line: `SELECT direction FROM exits WHERE room = ?`
  (`src/render.cpp:67-74`) — realized exits only.
- Architect prompt currently **forbids** the LLM from mentioning exits
  (`src/architect.cpp:151`): "Do NOT describe exits, directions, doorways leading
  onward." This was the structure/content split — engine owns structure, LLM owns
  flavor — but it exists partly *because* exits don't exist yet at gen time.
- Schema: `exits(room, direction, dest, PRIMARY KEY(room, direction))`
  (`src/world.cpp:22`). **`dest` has no `NOT NULL`.**

## The reframe that dissolves it

> A room's exits are **declared at birth**. The player may only leave through a
> declared exit. A declared exit may point at a room that **doesn't exist yet**
> (latent, `dest = NULL`). Walking a declared-but-latent exit triggers generation.
> Anywhere else is a wall — and correctly *not* displayed.

`resolveGo` becomes:

| exit row | dest | action |
|---|---|---|
| yes | realized | move |
| yes | **NULL** (latent) | generate the room behind it, realize, move |
| **no row** | — | wall (blocked) |

The free generation in today's case (b) simply deletes. A latent exit is exactly
the **L0 "referenced" stub** from [[story-seed-and-lod-world]].

## Decision: Fork C, refined — architect co-authors exits, engine owns invariants

Three forks were weighed for *who decides which directions open*:

- **A — engine picks procedurally.** Keeps the split pure, testable, theme-blind.
  Rejected: geography would have no authored meaning.
- **B — architect declares exits freely.** Coherent, but reverses the prohibition
  and needs validation.
- **C — engine picks the set, architect writes prose to agree.** Chosen direction.

**Refinement forced by the user's own reason** ("structure depends on the setting
and the content of the rooms"): a blind engine pick can't make structure depend on
theme. So the exits must be **co-authored in the same `create_room` call as the
prose** — the LLM writes "a low arch north, the east wall shelved solid" *and*
emits `exits: [north]` in one shot. Same author, same breath ⇒ they agree with no
second round-trip. This is simpler *and* more consistent than "decide set, then
prose."

Resulting split:

- **Architect owns:** which directions open + their flavor — *content-shaped
  structure.* Including **how many** — density is the AI's call, driven by setting
  (a dungeon branches, a road is a thread). No engine-imposed themed cap.
- **Engine owns:** graph integrity + identity — the graph can't lie even if the
  LLM does.

## Concrete shape

**Tool schema** — `create_room` grows one field:

```
exits: [ { direction, hint? } ]   // OPEN onward ways, excluding the way you came
```

`direction` constrained to the eight invertible names. **Empty array is legal** —
that's a dead end, and dead ends are exactly the shape a uniform grid lacks, so
they fall out for free.

**Prompt** — the blanket prohibition (`src/architect.cpp:151`) flips into a
requirement:

> Declare which directions lead onward, chosen to fit the setting and this room —
> a vault may have only the way you came; a crossroads several. Do **not** include
> the way the player entered (`{inverse}`); the engine adds that. Describe the
> exits you declare in the prose, and name no opening you don't declare.

**Engine (`writeGeneratedRoom`, `src/mutations.cpp`)** enforces what the LLM can't
be trusted with:

- always wires the **return exit** (new → inverse → origin, realized) — every room
  has ≥1 exit and the player can always retreat;
- for each declared exit: insert `exits(new, dir, NULL)` — a latent stub; dedup
  against the return and against repeats;
- rejects direction names outside the eight. **No count cap** — density is the
  architect's, bounded naturally at 7 by dedup.

**`resolveGo`** splits case (b) per the table above; deletes the free-generation
path.

## Payoffs (why this shape and not another)

- **Display fix costs zero render code.** `roomBlock` already selects
  `direction FROM exits WHERE room = ?`. Once the table holds latent stubs, that
  query returns the truthful open set — realized *and* latent — automatically.
  Fix the model, the exit line corrects itself.
- **No DDL change, no `SCHEMA_VERSION` bump.** `dest` is already nullable
  (`src/world.cpp:22`); "latent = NULL dest" needs no migration and old worlds
  (all non-null) stay valid.
- **Dead ends fall out for free** from an empty `exits` array — the world gets
  shape without special-casing.

## Resolved sub-decisions

- **Count / density:** the architect decides, driven by setting. Engine imposes no
  themed cap — only the hard floor (return exit) and the natural ≤7 ceiling.
- **Blocked vs obstructed:** only **walls** (absent row) and **latent-open**
  (declared, NULL dest) now. Doors/locks/collapsed passages — *shown but
  impassable* — are a **later layer**. Guardrail: build nothing that assumes
  "row exists ⇒ passable," so an obstructed `state` column can slot in later.

## The loops-vs-trees problem (raised by [[procedural-room-feel]])

Research into how procedural games make space *feel authored*
([[procedural-room-feel]]) surfaced one finding that **re-prioritizes** the
graph-vs-grid thread below rather than confirming it. The most consistent lesson
across roguelike / Metroidvania practice (sharpest in *Unexplored*'s cyclic
generation): **tree-shaped space feels random; cyclic space feels designed.** A
tree — one path between any two points — produces dead-end/backtracking fatigue and
can't support lock-and-key (there's only ever one way forward). Loops give choice +
forward momentum and read as intentional.

**The catch:** the model we chose generates a **pure tree.** Every architect-declared
exit spawns a *brand-new* room; nothing ever reconnects. That is exactly the failure
mode designers avoid. Non-Euclidean-graph is fine; *never closing loops* is the part
that feels random.

This does **not** overturn the plan — exits-declared-at-birth still fixes the display
bug and adds blocked directions. It re-scopes the old "leave it a graph" thread:
**loop closure is not a nice-to-have, it is the primary feel lever**, and it becomes
the natural *next* pass. Cheapest path first:

1. **Ship the tree for v1.** It's honest and incremental — display truthful, walls
   real, dead ends possible.
2. **Then: occasional bind-to-existing.** When the architect declares an exit,
   *sometimes* wire it to an already-existing nearby room instead of a fresh latent
   stub (the dedup / stub-identity mechanism flagged in [[story-seed-and-lod-world]]).
   Even a low bind-rate turns the tree into a looped graph and buys most of the feel.
3. **Gold standard (probably overkill):** cycle-first authoring — generate *loops*
   as the unit, Unexplored-style.

## Deliberately left open

- **Graph vs grid.** Space is a pure graph today — north-east-south-west does *not*
  return you to start; no coordinates. Blocked directions work trivially in a graph
  (a wall is a missing edge). A mappable Euclidean world would need coordinate
  respect → the dedup/loop-closure problem from [[story-seed-and-lod-world]].
  Leaning: **stay a graph (accept non-Euclidean), but plan to close loops** — see
  the loops-vs-trees section above; the two are the same lever seen twice.
- **Seed as frontier root.** `base.sql` currently wires the start room's exits to
  real dests. It *could* instead plant NULL-dest stubs so the authored seed becomes
  the root of the reference-leak frontier. Nice, separable content change.
- **Trusted-by-co-authorship.** The engine validates the exit *list* (graph-legal)
  but can't cheaply verify the *prose* mentions exactly those exits. Prose/structure
  agreement is a **quality** concern — live-verified, not gated — consistent with
  how the existing prompt quality is handled (`src/architect.cpp:135-137`).
  Accepted.

## Next step

Ready for a spec/plan: schema semantics (NULL dest = latent), `create_room` tool
`exits` field, prompt flip, `writeGeneratedRoom` return-exit + latent-stub writes,
`resolveGo` three-case split, and confirming the render query already does the
right thing.
