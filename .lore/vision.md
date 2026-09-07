---
title: TextWorld Vision
date: 2026-04-18
status: current
tags: [vision]
---

# Vision

TextWorld is a single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. A hand-authored base area seeds play; the world grows as the player explores, and everything AI generates is written to canon and never regenerated. The project's defining design discipline is a clear, iterated boundary between what the engine owns (rules, state, consistency) and what AI owns (prose, character, emergent narrative).

# Principles

## 1. Name the boundary

Every feature must declare whether it's deterministic or AI-driven, and why.

**Looks like:** A combat damage calculation marked deterministic because fairness requires repeatability.
**Doesn't look like:** "The AI handles combat" — with no explicit line between engine arbitration and AI flavor.

## 2. Persisted content is canon

Once AI generates something, the engine stores it and re-reads it. AI never regenerates what the store already holds.

**Looks like:** A room described once on first visit; subsequent visits read the file.
**Doesn't look like:** Regenerating a room on each entry because "the prose will be richer this time."

## 3. AI extends classic mechanics; it doesn't replace them

The skeleton is a real text adventure (map, inventory, state, actions). AI adds prose, dialogue, and new territory on top.

**Looks like:** Engine resolves `unlock door with brass key` deterministically; AI writes the sentence that describes the click.
**Doesn't look like:** An LLM improvising whether the door opens.

## 4. Narrow agents over omniscient ones

Specialized agents with small, targeted context beat one agent that sees everything.

**Looks like:** An NPC voice agent loading only that NPC's identity and journal.
**Doesn't look like:** Stuffing the whole world into one system prompt.

## 5. Implementation is exploration; ship atomic steps

Agent count, roles, and boundaries will be discovered by running code, not settled in docs. Each step is the smallest testable increment.

**Looks like:** First build: hand-authored base area + deterministic engine + simplest resolver. No generation yet.
**Doesn't look like:** A six-agent architecture diagram before a single room loads.

# Anti-Goals

- **Multiplayer / MUD at this stage.** Shared-room coherence and shared NPC state distract from the core design question. Revisit later, if ever.
- **A single omniscient AI agent.** Expensive, incoherent, and hides architectural decisions.
- **Stochastic mechanics.** Combat outcomes, item stats, and economy must be predictable. If it can be exploited, it must be deterministic.
- **Command-parser input (`take lamp`).** That was a 1978 technical limit, not a design choice. Natural language is the baseline.
- **A railroading narrator.** AI must not override player agency to force plot beats.
- **Up-front architectural commitment.** No settled agent count, role list, or storage schema until running code demands it.

# Tension Resolution

| Tension | Default Winner | Exception |
|---|---|---|
| AI prose freedom vs engine consistency | Consistency | Flavor text with no mechanical consequence |
| Narrative progression vs player agency | Agency | Explicit opt-in "run me through a story" mode |
| Rich regeneration vs persistence | Persistence | Explicit player action that logically rewrites (e.g., "the inn burns down") |
| Clean architecture vs shipping the atomic step | Shipping | When the atomic step would create a trap that's expensive to unwind later |

# Current Constraints

- **Empty project.** No stack, no scaffolding. Language and persistence choice are open. *Review when the first atomic step is defined.*
- **Unsettled agent architecture.** Number and roles of agents are hypotheses. *Expire when running iterations yield stable patterns.*
