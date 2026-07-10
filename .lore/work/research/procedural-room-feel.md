---
title: How procedural games make rooms and space *feel* authored
date: 2026-07-09
status: active
tags: [procedural-generation, world-generation, level-design, roguelike, cyclic-generation, dead-ends, loops, level-of-detail, llm-generation, exits]
modules: [world-gen, architect, systems]
related: [.lore/work/brainstorm/blocked-directions-and-exit-display.md, .lore/work/brainstorm/story-seed-and-lod-world.md]
---

# How procedural games make rooms and space *feel* authored

Gathered 2026-07-09 to ground [[blocked-directions-and-exit-display]]. The question:
when space is generated, what makes it feel *designed* rather than a uniform grid or
a random sprawl? Findings pulled from roguelike/Metroidvania level-design practice
and recent LLM world-gen work.

## Key findings

1. **Loops beat trees — this is the single biggest "feel" lever.** The most
   consistent lesson across sources: *tree-shaped* space (every room hangs off a
   parent, one path between any two points) feels random and produces
   backtracking + dead-end fatigue. *Cyclic* space (loops, multiple routes between
   two points) reads as intentionally designed. **This directly challenges our
   current plan**, which generates a pure tree of latent stubs (see mapping below).

2. **Abstract-first, refine last.** Good generators fix high-level *topology and
   intent* first, then resolve specifics (biome, theme, contents) last, in layers.
   This is exactly the L0→L1→L2 level-of-detail model from
   [[story-seed-and-lod-world]] — the research independently validates it.

3. **Hybrid: handcrafted anchors + procedural connective tissue.** Nobody ships
   pure generation. Prefabs / set-pieces / authored vignettes are dropped in as
   *memorable reference points* ("the ones players discuss"); procedure fills the
   space between. Uniqueness of a hand-made piece draws attention and makes it
   meaningful. Validates **seed-as-frontier-root**.

4. **Room *purpose* drives room *shape*.** Deciding a room's function first (vault,
   barracks, cell, dining hall) tailors its size, contents, and connections. A
   prison cell is small; a hall is large. Purpose → structure, not the reverse.
   Validates the decision that **the architect sets exit density by what the room
   *is*** in the given setting.

5. **Kill *most* dead ends, keep a *few*.** Maze generators produce dungeons packed
   with dead-end corridors — "a certain sadistic appeal" but tedious. Standard fix:
   remove dead ends until only room-connecting passages remain, then *add back* a
   few unneeded connectors to create loops. A handful of deliberate dead ends give
   texture (a vault, an edge); a world made *of* dead ends does not.

6. **Theme-appropriate connection geometry.** Cogmind uses wide corridors/big rooms
   because it's a ranged-combat game; melee roguelikes use tight spaces. The
   *connection design itself* should express the intended experience — not a
   one-size default. For us: exit count/shape should express setting (dungeon vs
   road), which is the call already made.

7. **LLMs buy coherence but drift over long sessions.** LLM generation "feels
   authored rather than random" precisely because it understands context and can
   interconnect ("the mill where travelers have vanished since the miller died").
   But documented failure mode: **consistency decays across long play** — repetition,
   contradiction, forgotten state. Safeguards (milestones, bounded state, explicit
   canon) are needed. Matches our bounded-context law and trust-but-verify stance.

## The loops-vs-trees problem, in detail (most important for us)

*Unexplored*'s **cyclic dungeon generation** (Joris Dormans) is the sharpest source.
Its thesis: a dungeon built as a **graph of cycles** feels hand-designed; a dungeon
built as a **tree** feels generated.

- It starts by drawing a **loop** with entrance + exit nodes splitting it into two
  arcs, then picks from ~24 **"major cycle types"** that define "the narrative ebb
  and flow of the level." Both arcs reach the same goal by different obstacles.
- Loops enable **lock-and-key** and **one-way valves** — the vocabulary of authored
  design — because there's more than one route to reason about. A tree can't do
  meaningful lock-and-key; there's only ever one way forward.
- Why it *feels* designed: loops give **choice + forward momentum**; trees give
  **dead ends + backtracking**. Players read the difference even if they can't name
  it.

Cogmind agrees from the opposite direction: minimize backtracking by managing **loop
density** — "areas where multiple routes converge" create pacing and prevent forced
repetition. "A map that looks good from above often plays poorly from within" — feel
is a property of *traversal*, not the top-down picture.

## What this means for our design

Mapping each finding onto [[blocked-directions-and-exit-display]]:

<table>
  <tr><th align="left">Research finding</th><th align="left">Our current plan</th><th align="left">Verdict</th></tr>
  <tr>
    <td>Loops &gt; trees for feel</td>
    <td>Architect declares latent stubs → each becomes a fresh room → <strong>pure tree</strong>, no loops (we chose "stay a graph, non-Euclidean, don't apologize")</td>
    <td><strong>Tension.</strong> The cheapest model is exactly the one designers avoid. "Non-Euclidean graph" is fine; "tree with no loops" is the part that feels random.</td>
  </tr>
  <tr>
    <td>Abstract-first, refine last</td>
    <td>L0/L1/L2 LOD from prior brainstorm</td>
    <td><strong>Validated.</strong> Independent confirmation.</td>
  </tr>
  <tr>
    <td>Handcrafted anchors + procedural fill</td>
    <td>Seed room; "seed as frontier root" (open thread)</td>
    <td><strong>Validated.</strong> Promote from "nice, separable" toward "load-bearing for feel."</td>
  </tr>
  <tr>
    <td>Purpose → shape; theme-driven density</td>
    <td>Architect sets exit count by setting/room content</td>
    <td><strong>Validated.</strong> This is the right call.</td>
  </tr>
  <tr>
    <td>Keep a few dead ends, not all</td>
    <td>Empty exits array = dead end, falls out free</td>
    <td><strong>Validated,</strong> with a caveat: dead ends should be <em>occasional</em>, not the default a tree produces.</td>
  </tr>
</table>

### The one real challenge to our brainstorm

We parked loop-closure as "deliberately left open — stay a graph." The research says
**loop closure is not a nice-to-have; it is the primary thing that separates
authored-feeling space from random space.** A world where every declared exit always
spawns a *brand-new* room is a tree, and trees are the failure mode.

This doesn't overturn the plan — it re-prioritizes the open thread. Options, cheapest
first:

- **Accept the tree for v1, close loops later.** Ship exits-declared-at-birth as a
  tree; it already fixes the display bug and adds blocked directions. Add loop
  closure as the *next* feel pass. Honest and incremental.
- **Occasional dedup / bind-to-existing.** When the architect declares an exit,
  sometimes bind it to an *already-existing* nearby room instead of a fresh stub
  (the loop-closure / stub-identity mechanism flagged in [[story-seed-and-lod-world]]).
  Even a low bind-rate converts a tree into a graph with cycles and buys most of the
  feel.
- **Cycle-first authoring (Unexplored-style).** Generate *loops* as the unit, not
  rooms — heaviest, probably overkill for a text world, but the gold standard.

The abstract-first + bounded-context + handcrafted-anchor findings all fit our
existing model cleanly. **Loop closure is the finding that should change what we
build next.**

## Sources

- [Dungeon Generation in Unexplored — Boris the Brave](https://www.boristhebrave.com/2021/04/10/dungeon-generation-in-unexplored/) — cyclic generation, loops vs trees, abstract-first.
- [Procedural Map Generation — Cogmind / Grid Sage Games](https://www.gridsagegames.com/blog/2014/06/procedural-map-generation/) — feel is traversal not top-down; prefabs as memorable anchors; theme-driven geometry; loop density.
- [Rooms and Mazes — journal.stuffwithstuff.com](https://journal.stuffwithstuff.com/2014/12/21/rooms-and-mazes/) — dead-end removal + adding connectors for imperfect (looped) mazes.
- [Building the Level Design of a procedurally generated Metroidvania — Game Developer](https://www.gamedeveloper.com/design/building-the-level-design-of-a-procedurally-generated-metroidvania-a-hybrid-approach-) — hybrid handcrafted/procedural, purpose-driven rooms, room-size variety.
- [How to Procedurally Generate and Decorate 3D Dungeon Rooms — Archmage Rises](http://www.archmagerises.com/news/2021/6/12/how-to-procedurally-generate-and-decorate-3d-dungeon-rooms-in-unity-c) — room purpose tailors generation; small material palette for consistency.
- [AI Procedural Content Generation — Muddy Terrain Games](https://muddyterrain.com/blog/ai-procedural-content-generation-unreal-engine) — LLM PCG feels authored via context/interconnection.
- [GenQuest: An LLM-based Text Adventure Game (arXiv 2510.04498)](https://arxiv.org/html/2510.04498v1) — LLM coherence-drift over long sessions; milestones/state safeguards.
