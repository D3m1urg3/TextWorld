---
title: Story seed + LOD world generation — the fact-store model
date: 2026-07-09
status: resolved
tags: [ai-integration, story-seed, world-generation, level-of-detail, persistence, bounded-context, architect, bard, canon-facts]
modules: [world-gen, story, render, mutations]
related: [.lore/work/brainstorm/ai-integration-points.md, .lore/vision.md, .lore/work/specs/ai-prose-renderer.md, .lore/work/specs/ai-resolver.md]
---

# Story seed + LOD world generation — the fact-store model

## Context

Prose renderer (#0) and NL resolver (#1) are implemented. Next in the dependency order is #3 story seed, which the prior brainstorm ([.lore/work/brainstorm/ai-integration-points.md]) argued must land before world-gen (#2) because generated content is theme-less without it. This session refined that: the user wants to build **the storyteller (bard/author)** first but needs **a minimal architect (world-gen)** to test it. NPCs (#3 in the user's numbering) are explicitly deferred. The session converged on a single model that unifies story, world-gen, persistence, and performance.

## The reframe: "story" is not an object, it is a growing fact-store

A room is *done* when made; a story is by nature *unfinished*. Persisting "the story" as a blob forces a choice between rewriting it (breaks persistence) and ignoring the player (breaks agency). Way out:

**There is no story object. There is a growing pile of canon facts. "Story" is the name for reading them in the order they became true.**

- The **seed file is setting**, not plot — the initial set of facts (world, tone, premise, what's expected). Confirmed by the user: the seed describes the setting; it is not a script.
- Play **appends new facts** as events happen. Never rewritten. Persistence becomes trivial.
- This drops onto the shipped renderer, whose contract is already *"no claim without a sourcing fact."* The bard's job = **produce the facts the renderer is allowed to cite.** Same substrate, one layer up.
- **The railroading anti-goal evaporates:** facts don't force beats, they only constrain what can be true next. The architect can't put a swamp beside the library because a fact says "marble city." Coherence is a side effect of everything reading one store — no director agent needed.

### Two fact sources, both canon, both persistent

- **Setting facts** — from the seed. Static, given, true before play.
- **Event facts** — emergent. "The player smashed the bell." Appended as play happens. The **foundation's event log is already this machinery** — feature 1 decides what *setting* adds on top.

Persistence (user's words): "once an event happens within a session it carries out in the rest. The world should be persistent." The whole world persists — a **sandbox**, not a corridor.

## The distinction that does all the work: established vs latent

Pure facts give a coherent sandbox with no arc. Split them:

- **Established** — true now, canon, immutable. What the renderer cites.
- **Latent** — could become true; provisional; triggerable.

**Progression = a latent becoming established.** Player can ignore latents forever (agency preserved). The same distinction covers *space*: a generated-but-unvisited room is a latent room; it establishes on first visit. One idea — **provisional vs realized** — spanning both plot and geography.

## The only real performance blocker

Sort every cost by *does it grow with world size?*

| Cost | Per what | Scales with world size? | Verdict |
|---|---|---|---|
| SQLite storage | per room | No, O(1) each | **Non-blocker.** Don't over-engineer. |
| LLM cost to generate a room | per room, once (persistence) | No, amortized | Manageable; vision principle 2 caps it. |
| LLM latency of one generation | per frontier crossing | No, fixed | Real but local; hidden by prefetch. |
| **Context fed to each AI call** | per call | **YES if naive — O(whole world)** | **The only dangerous one.** |

**The sandbox death spiral:** if "generate room 400" means "here's everything that ever happened," every call gets slower and pricier the longer someone plays. The more fun they have, the worse it runs.

### The design law

**Every AI call's context is bounded independent of total world size. Never O(world).** SQLite is the memory; the prompt is the cache line. The whole world persists; no single operation looks at more than a local window. Corollary: **coherence only has to be local, because perception is local** — you can't contradict a room 50 hops away, but the player can't see it either.

### Latency is hidden by a fact about players

A player moves **one room per turn** and dwells 10–30s reading/acting; an LLM room-gen is ~3–8s. Generate the frontier in the background during dwell time. The synchronous stall only happens to a *sprinter* running through unexplored territory faster than generation. Design for the explorer; accept the rare stall.

## The world must be bigger than the path

User: for a sense of discovery, there must be **parts of the world the player never visits**. Pure lazy-on-visit generation is solipsistic — the world is exactly the size of your footprints, no roads not taken.

Key separation: **a *sense* of a larger world needs *evidence* of it, not *realization* of it.** The feeling comes from perceiving edges — an exit not taken, a road sign, a named district. Interiors can stay unwritten; imagination fills them (better than an LLM).

**Reference-leak frontier:** realizing a room emits prose that *names other places* ("a road climbs north to Keld"). Those references spawn latent stubs. Visit A → spawns B, C, D; visit B → spawns E, F. The frontier of known-but-unvisited **expands faster than the player consumes it.** The world is permanently bigger than the path; you pay full cost only for ground underfoot.

**How solid must the unvisited world be?** Spectrum: `named exit only → sparse reference-graph of stubs → fully-realized unvisited rooms`. The right end (interiors furnished and unseen) is only needed for an **alive** offscreen world (tide rising whether you're there or not, NPCs living) — that is **feature 3 territory, deferred.** For story-seed + architect, the middle buys the whole "big world, real discovery" feeling at stub prices.

## Level-of-detail (LOD) — the unifying mechanism

Borrowed from graphics: far rooms hold a coarse description, sharpened as the player approaches, **enriched by ongoing events/story** at the moment of sharpening.

LOD attacks the one real blocker. The bounded-context law as a *hard cutoff* ("include the 1-ring, drop the rest") has an arbitrary, incoherent edge. **LOD replaces the cutoff with a smooth falloff:** near rooms enter context at full detail (few, heavy), far rooms coarse (many, light), very-far as bare names. Total tokens stay bounded; coherence degrades gracefully instead of cliff-edging. It is the *continuous* version of the structure/content split.

### The rule that makes narrative-LOD work: refinement is monotonic

Graphics LOD **pops**; the narrative equivalent is a **contradiction**. Constraint:

**Sharpening a room may only *add* detail consistent with its coarser form — never overwrite it.** Like a mipmap, the coarse level must be a faithful average of the fine one. This is the persistence rule (*once real, it stays*) applied *inside* one room's lifecycle. A room is not binary latent/established — it is a **stack of ever-finer canon layers, each appended, never rewritten.** No contradiction = no popping.

**Disanalogy with graphics:** graphics LOD is bidirectional (objects re-coarsen as they recede). Narrative LOD **cannot coarsen** — that would forget canon, breaking persistence. So it is **refine-in, never-out.** Behind the player, everything stays full-detail forever (cheap — stored text, zero recompute). Distance controls how far *ahead* you sharpen, never any downgrade.

### The payoff: LOD answers the prefetch fork

Earlier fork: "does a pre-generated, never-visited room exist / did it happen?" LOD resolves it: **hold coarse, realize the fine layer as late as possible — on approach.** The user's clause "enriched by ongoing events/story" is the thing graphics LOD *can't* do — the sharpening pass weaves in *what has happened since* ("word of the bell you smashed has reached even the market"). Realizing too early freezes a room before the story that should color it exists. So keep far things coarse **deliberately**, so the last-moment fine layer absorbs the story. Late realization = richer, story-aware rooms.

### Level taxonomy (starting point)

| Level | Content | Cost | Trigger |
|---|---|---|---|
| **L0 Referenced** | name + who mentioned it + direction. "The Salt Market, east." | ~free | reference-leak |
| **L1 Sketched** | 1–2 sentences: kind of place, rough contents, coarse connections. Enough to render "a road toward a crowded market" and plan the graph. | cheap | enters near-ahead ring |
| **L2 Realized** | full prose + concrete canon items the resolver can act on; read verbatim after. Story-enriched at this moment. | full | imminent approach / first entry |

*(L3 Alive — offscreen events actively mutating a realized room — is feature 3, deferred.)*

### LOD policy (user's call): a spotlight that rides the player

Detail is set by **player proximity for new rooms**, and increases as the player moves:

- **1 hop (immediately accessible) → L2** (fully realized before entry ⇒ zero on-entry stall)
- **2 hops → L1**
- **3+ hops → L0**

The gradient slides forward with the player; monotonic (never downgrade). Minor accepted tradeoff: making adjacents L2 freezes them ~one move before arrival rather than at the moment of arrival — one move of story-staleness, negligible.

## Hard limit on world size (orthogonal to LOD)

LOD bounds the *slice* (per-call context); it does nothing to bound the *whole*. The reference-leak frontier guarantees the total keeps growing — stubs spawn faster than the player consumes them, so the stub graph trends to infinity even while every prompt stays small. **LOD bounds the lens; a hard cap bounds the thing being looked at.** Both are needed.

**What gets capped — two budgets, don't conflate:**

- **Established (L2) rooms = the real "world size."** Canon, immune to deletion (persistence), what matters for completeness and sense of place. **The hard limit governs this.**
- **Latent (L0/L1) stubs = a working buffer, not "the world."** They trend to infinity via reference-leaks, but they never *happened*.

**Latents are evictable; established rooms never are.** The established-vs-latent line pays off again: garbage-collect latent stubs (LRU by distance/recency) — safe because a stub never happened. Established rooms can never be evicted (the persistence wall). So the stub buffer stays bounded around the player automatically, and only canon counts against the cap.

**What happens *at* the cap** (a naive hard wall is bad — the frontier stops and the player hits an invisible fence):

1. **Loop-closure bias (required, not optional).** Near the cap a leaked "road east" cannot spawn a new room — but references were promised to be *real places*. The only way to honor that without growing is to **bind the reference to an existing node** (the stub-identity/dedup mechanism). So the architect flips from **expansion to interconnection**: stop leaking new places, start closing loops. The world **rounds itself into a closed graph** instead of being severed. Dedup becomes **load-bearing at the boundary.**
2. **Narrative edges for the remainder.** Some exits genuinely terminate; the **seed's setting defines the boundary shape** — sea, cliff, washed-out road, fog. The edge is authored by tone, not an error.

**The cap is a setting parameter, not a constant.** The seed declares scale ("a small drowned village" vs "a sprawling port city" vs "a continent"). Same file that sets tone sets size. A *finite* world is also what gives the **"win-condition vs endless"** question a home — you can't end an infinite sandbox, but a bounded, closeable world can be *completed*.

**Tension to log:** loop-closure can produce weird long-range geography (the road east quietly loops across town). Coherence is local, so the player rarely *sees* it — but a careful mapper will. Bug vs dream-logic flavor is **seed/tone-dependent** (a labyrinth wants it; a realist village doesn't).

## Two consequences to carry into design

1. **Every move triggers a wave of background upgrades.** Stepping X→Y re-centers the gradient: Y's other neighbors L1→L2, the new 2-hop ring L0→L1, fresh references spawn L0s. ~*b−1* full realizations per move (branching factor b) — the expensive ones. Hideable because they run in dwell time, are independent (parallelize), and the L1 ring can be **batch-sketched in one call** (mipmap-for-a-region). **Batch the cheap layer, parallelize the expensive layer.**

2. **Stub identity / dedup turns a tree into a world.** References to "the Salt Market" from both X and Y must resolve to **one** node, or the frontier grows a tree that never loops back — no shortcuts, no "this connects to where I started." Refinement needs a **dedup/identity step**: a new reference binds to an existing stub or creates one. This is also where loop-coherence lives (the market approached from two directions is one consistent place). Flagged, not solved.

## Open questions

- **L1 generation granularity:** per-stub (call explosion) vs **batch-per-region** (cheaper, more coherent — lean batch). Confirm.
- **Refinement trigger:** pure hop-distance (simple) vs perception/line-of-sight (a room you can see into through an open door is 1 hop but visible — L1.5?). Distance is the user's stated policy; perception is a possible refinement.
- **Testing bar for feature 1 (the atomic step):**
  - *Low* — "seed manifests": seed → one room reflecting it. Cannot fail interestingly; dodges retrieval.
  - *High/real* — "a few generated, persisted rooms stay mutually coherent in one session": room 4 must not contradict rooms 1–3 ⇒ hits retrieval/bounded-context immediately, still small (4 rooms, one session, no plot-evolution). **Leaning this is the real atomic step.**
- **Stub identity mechanism:** how references resolve to node IDs (name match? embedding? architect-assigned canonical IDs?).
- **Where facts live in SQLite:** pure event-log projection vs a materialized facts/world-state table (the event-sourcing snapshot question), given the bounded-context retrieval need.

## Naming (informal)

- **Bard / author** — produces facts, decides what becomes established.
- **Architect / designer** — reads facts, emits rooms; emitting a room writes new facts back.
