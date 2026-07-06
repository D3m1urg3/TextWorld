---
title: "Similar projects and approaches: LLM + deterministic engine text adventures"
date: 2026-07-05
status: active
tags: [prior-art, neuro-symbolic, llm-games, grounding, noun-coherence, memory-architecture, ecs, sqlite]
related: [.lore/work/brainstorm/engine-foundation-cpp-sqlite.md, .lore/brainstorm/ai-driven-text-adventure.md, .lore/vision.md]
---

# Similar projects and approaches: LLM + deterministic engine text adventures

Research round (2026-07-05) before building the two-room prototype. Question: who has built engine-plus-LLM text adventures, what did they learn, and what does it change for us?

## Key Findings

1. **The vision's core bet is independently validated.** Academic work (IVIE, PAYADOR, Story2Game, "Orchestrated Reality") converges on exactly our split: LLM owns creativity/prose, symbolic layer owns state and validation. Nobody credible argues for LLM-owns-everything anymore — that's the AI Dungeon failure mode, now well documented.
2. **Consistency, not creativity, is the known killer.** AI-DM studies report players hitting "cognitive friction" fast when the AI forgets established facts; commercial AI-GM products (Friends & Fables, AI Realm) still fight memory drift despite dedicated memory subsystems. Our store-first/canon design attacks precisely the weakness the field keeps shipping around, not through.
3. **Noun coherence has a named prior-art spectrum.** Story2Game *promotes* missing objects on demand (grounded via preconditions; only ~60% semantic success). IVIE *validates before accepting* (inconsistencies still occasionally slip through). PAYADOR sidesteps action libraries by having the LLM *predict outcomes* against a minimal world representation. Our open question #1 now has three named reference policies instead of zero.
4. **ECS-as-relational-database is established thinking** (Hexops devlog, ECS/SQL essays) — component table = SQL table, entity = primary key, query = SELECT. But no found project uses **SQLite as the live world store for an LLM game**. Our angle appears genuinely under-explored: the field builds memory *subsystems* (RAG, note files, embeddings) to approximate what a transactional store gives exactly.
5. **NPC memory has a canonical architecture:** Stanford Generative Agents' memory stream — append-only experience log + scored retrieval + periodic reflection. Our `events` table *is* a memory stream; NPC journals = filtered folds over it. The brainstorm's `journal.md` sketch matches the literature almost verbatim.
6. **Name collision:** Microsoft's **TextWorld** is a well-known RL research framework for text games (GitHub, arXiv 1806.11532). Project rename worth considering before anything goes public.

## What it changes for our open questions

### Noun coherence ("the AI lied about a bookshelf") — three reference policies

| Policy | Prior art | Mechanism | Cost |
|---|---|---|---|
| Promote on demand | Story2Game | Missing object instantiated when referenced; grounded via precondition checks | ~60% semantic success; random placement felt arbitrary |
| Validate before narrate | IVIE | Symbolic layer checks LLM output before it becomes canon | Validation gaps still leak; extra LLM round-trips |
| Constrain + predict outcomes | PAYADOR | LLM sees minimal world representation, predicts action outcomes rather than mapping to action library | Trades expressiveness for control |

Synthesis for us: a fourth option falls out of our architecture — **manifest-constrained narration + extraction pass**. Narrator prompt includes the room's entity manifest (cheap: one SELECT); a post-generation pass extracts nouns the prose introduced and writes them as inert `scenery` components — canon by extraction, not hallucination. Prose is then never "lying": anything it mentions gets a row. Untested hypothesis; fits store-first cleanly. Try in a later atomic step.

### NPC knowledge boundaries

Generative Agents answers this: NPCs know what their memory stream holds, period. Retrieval scoring (recency/importance/relevance) picks what enters context. "Sometimes-dumb NPC that says 'I don't know'" is the literature-backed choice — believability came from bounded memory, not omniscience.

### Action Resolver

Function-calling / canonical-action translation is standard practice now ("Labyrinth" AI-GM paper: function calling measurably improves state-update consistency). PAYADOR's warning applies: don't build a giant action library; keep canonical action set small, let outcome prediction handle the long tail. Reinforces "hardcoded parser is disposable scaffolding."

## Landscape snapshot

- **[World-Engine-LLM](https://github.com/hazlema/World-Engine-LLM)**, **[gpt-adventures](https://github.com/nferraz/gpt-adventures)** — hobby-scale, LLM-generates-world-per-turn; the anti-pattern our vision rejects. Useful as contrast, not blueprint.
- **["You See an LLM Here"](https://machinelearningmastery.com/you-see-an-llm-here-integrating-language-models-text-adventure-games/)** — practical tutorial matching our split exactly (JSON world = deterministic; LLM renders prose from meta-descriptions; cache generated prose = poor-man's canon). Validates prototype shape at tutorial scale.
- **[Friends & Fables](https://fables.gg/)** (ACE-1 "Agentic Campaign Engine") — commercial multi-agent AI GM; fine-tuned specialist models; memories snapshotted every 5 turns into retrieval context. Closest commercial cousin to our multi-agent sketch. Still reports memory drift in long campaigns → retrieval-approximation of state loses to exact state.
- **[IVIE](https://arxiv.org/abs/2606.13348)** (2026) — neuro-symbolic incremental IF world generation, four-stage validated pipeline. Closest academic cousin. Their residual failures (constraints bypassed, impossible goals) mark where our engine must be strict.
- **[Story2Game](https://arxiv.org/html/2505.03547v1)** — LLM preconditions/effects compiled to executable code; dynamic object promotion. Source of the promote-on-demand policy and its 60% ceiling.
- **[PAYADOR](https://arxiv.org/abs/2504.07304)** (ICCC 2024) — minimal world representation + outcome prediction instead of action mapping. Open source.
- **[Generative Agents](https://arxiv.org/abs/2304.03442)** (Stanford, 2023) — memory stream / retrieval / reflection. The NPC-memory blueprint.
- **[Microsoft TextWorld](https://github.com/microsoft/TextWorld)** — RL research framework, unrelated goals, owns the name.
- **ECS↔SQL:** [Hexops: ECS as database](https://devlog.hexops.org/2022/lets-build-ecs-part-2-databases/), [ECS and SQL](https://ogrady.github.io/jekyll/update/2021/12/17/entity-component-system.html), [HN: ECS outside game engines](https://news.ycombinator.com/item?id=21892126).

## Insights that feed the prototype

- Store-first design is the differentiator — everyone else approximates memory; we keep exact state. Don't compromise it for convenience later.
- Render-from-events habit is validated: function-calling-style structured contracts between engine and LLM measurably beat freeform.
- Cache-generated-prose-as-canon appears even in tutorials; our schema makes it structural. Right instinct.
- When AI arrives, feed the narrator an entity manifest and consider the extraction pass for new nouns.
- Consider renaming the project (Microsoft owns "TextWorld" mindshare).

## Sources

- https://arxiv.org/abs/2606.13348 (IVIE)
- https://arxiv.org/abs/2504.07304 (PAYADOR)
- https://arxiv.org/html/2505.03547v1 (Story2Game)
- https://arxiv.org/abs/2304.03442 (Generative Agents)
- https://arxiv.org/html/2409.06949v1 (AI GM function calling, "Thirteen Hours")
- https://arxiv.org/pdf/2606.16014 (Orchestrated Reality)
- https://machinelearningmastery.com/you-see-an-llm-here-integrating-language-models-text-adventure-games/
- https://fables.gg/blog/how-is-friends-and-fables-different-from-chatgpt-ai-dungeon-or-novelai
- https://github.com/hazlema/World-Engine-LLM · https://github.com/nferraz/gpt-adventures
- https://github.com/microsoft/TextWorld
- https://devlog.hexops.org/2022/lets-build-ecs-part-2-databases/
- https://ogrady.github.io/jekyll/update/2021/12/17/entity-component-system.html
- https://shiftmag.dev/dungeons-dragons-dnd-ai-game-engine-6240/ (AI DM consistency studies)
