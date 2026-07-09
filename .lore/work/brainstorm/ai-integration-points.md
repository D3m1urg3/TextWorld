---
title: AI integration points — where AI enters the engine
date: 2026-07-06
status: resolved
tags: [ai-integration, prose-renderer, nl-resolver, world-generation, story-seed, npc, architecture]
modules: [render, parser, mutations]
related: [.lore/brainstorm/ai-driven-text-adventure.md, .lore/vision.md]
---

# AI integration points — where AI enters the engine

## Context

Engine foundation prototype is done: tick loop, event log, SQLite canon store, disposable fixed-verb parser. Session goal: map where AI plugs in, pick the first step.

## The five integration points

User proposed 1–4; #0 surfaced from the vision's own example (engine resolves unlock, AI writes the click sentence) and was initially forgotten.

### 0. Prose renderer (chosen first step — deep dive below)

Event rows → prose instead of dumb templates. Seam: `render.cpp`.

### 1. NL action resolver

Natural language input → `action.hpp` structs. Replaces the disposable fixed-verb parser (vision anti-goal: command-parser input). Boundary: AI *translates*, never *decides* — engine still validates and can refuse. Open question: what happens when input maps to no verb ("smell the flowers")? Fail flat, flavor-only response, or escalate to generation. That failure mode defines game feel more than the happy path.

### 2. On-the-fly world generation

Trigger: unmapped exit. AI proposes room graph; engine enforces invariants (exit reciprocity, no orphans, valid items) before canon write. Hard parts: thematic coherence (why is a swamp next to a library?) and mid-turn latency — consider pre-generating one ring ahead. Fits "persisted content is canon."

### 3. Story seed + evolution

Starts from a user input file, evolves as the player progresses. Key claim from session: **not a feature parallel to the others — it's shared context that feeds 2 and 4.** Without story, generated rooms are theme-less noise and NPCs have no motive. So story lands *before* gen in dependency order. Tension with anti-goal "railroading narrator": story proposes beats, never forces them. Open questions: seed file contents (genre? premise? ending?), win condition vs endless, where story state lives (DB flags vs director-agent journal).

### 4. NPC life and interaction

Sharpest idea of the session: **NPCs are players.** NPC agent emits the same action structs, goes through the same tick, same event log — engine already treats world mutation uniformly. "Life" question: simulate offscreen NPCs every tick (expensive, mostly unobserved) or lazily reconstruct plausible history when the player meets them. Lazy is cheaper, player can't tell, and fake life once observed is still canon.

## Dependency order

0 renderer → 1 resolver → 3 story seed → 2 gen → 4 NPCs.

Story-before-gen is the contrarian bit. Renderer + resolver together make the game "conversational"; renderer alone (with fixed parser) already shows value.

## Deep dive: prose renderer

### Why #0

- Zero state risk: read-only by contract — worst case is bad prose, never a corrupted world.
- Templates stay as fallback; degradation story is free.
- No dependency on the resolver; works with the current fixed parser.
- The vision's own canonical example.

### Contract inherited

`render.hpp:7` already states it: "exactly the contract the future AI prose layer inherits."

- SELECTs only, never writes.
- Template rule "no output line without a sourcing event row" becomes **no claim without a sourcing fact** — hallucination is a contract violation.
- Narrow context (vision principle 4): this turn's event rows + current room slice (canon prose, exits, names). Tiny prompt.

### The hard problem: invented affordances

AI writes "sunlight streams through the window" — no window in the DB. Vision's tension table permits flavor without mechanical consequence, but the player *will* type "look through window." Options considered:

- Resolver shrugs → player feels gaslit.
- Renderer writes the window back to canon → contract break; renderer now mutates world.
- **Lean: nouns-must-exist rule** — renderer may decorate only nouns present in the DB; atmosphere is free ("cold air" yes, "a window" no).

Sharpest open question of the feature.

### Canon vs fresh split

Does per-turn flavor violate "persisted content is canon"? No — clean split:

- **Room descriptions** = canon, read verbatim from the `description` table, never re-prosed.
- **Connective tissue** (take/drop/move sentences) = ephemeral flavor, fresh each turn. Varied phrasing on repeat actions is fine, maybe good (narrator notices: "Again?").

The events table is already the permanent transcript; rendered prose need not be.

### Wrinkles

- **Latency + cost:** LLM call every turn. Possible tiering: templates for `inventory`, AI for movement/discovery. Or all-AI first, optimize later.
- **`renderError` path:** parse failures have no event row — no facts to ground on. Leave it template, or let AI be charming there precisely because nothing is at stake.
- **Voice drift:** narrator needs a style anchor in the prompt. Story seed (#3) later feeds tone; renderer works standalone but improves when story lands.
- **Testing:** determinism gone. Testable contract shrinks to: facts present, no forbidden nouns, fallback works.

## Decision

Start with the prose renderer. Natural first step; everything else layers on later.
