---
title: "LLM-driven NPCs in 2026: dialogue, memory, grounding, cost"
date: 2026-08-06
status: active
tags: [npc, dialogue, memory-architecture, persona-drift, grounding, prompt-caching, latency, prior-art, social-engineering]
modules: [bard, architect, loop]
related: [.lore/work/brainstorm/npcs.md, .lore/work/research/similar-projects-and-approaches.md, .lore/work/research/drama-manager-prior-art.md, .lore/work/brainstorm/dungeon-master.md]
---

# LLM-driven NPCs in 2026: dialogue, memory, grounding, cost

Research round before designing the NPC bricks. Question: what does the field
know now that it did not when we last looked (2026-07-05), and what does it
change about the decisions in [npcs.md](../brainstorm/npcs.md)?

## Key Findings

1. **Players social-engineer NPCs, and it is now a named failure.** Give a
   player a free text box and they will talk a guard into handing over a
   quest-critical key. The consequence is that persuasion becomes decoupled from
   the game's own systems and progression stops meaning anything. **Our first
   version is immune by construction** — dialogue writes nothing to the world —
   and we picked that for a different reason. It is now the strongest reason.
2. **Persona drift is measured, and it is fast.** Persona self-consistency
   degrades by more than 30% after 8–12 dialogue turns, even with the context
   intact. The mechanism is dilution: a growing conversation buffer crowds out
   the character definition. Our design rebuilds the prompt each turn from a
   fixed profile plus a bounded summary, so it does not have the failure. **Do
   not "improve" it later by keeping a growing conversation buffer.**
3. **Splitting a fixed persona from swappable memory is now a published
   architecture**, not just our instinct. It is the same split as our
   identity-in-a-file / memory-in-the-database decision, arrived at
   independently.
4. **But generic summarisation erodes personality.** The NPC-specific memory
   work finds that episodic summaries strip out the character-defining details
   and the character goes flat over a long session. Our split protects us —
   personality lives in the immutable profile — **provided the rewritable
   summary is only ever allowed to carry facts, never voice.**
5. **The blackboard / "sandwich" pattern is the field's settled answer** and it
   is what we already built: symbolic layer checks prerequisites, the model
   generates flavour inside that verdict, output schema validation on the way
   back. No change needed, but it confirms we are not off on our own.
6. **The consistency-gate result quantifies what our architecture buys.** A
   trained world model predicting state deltas, with a gate that asks the model
   to revise when the two disagree, cuts hallucinated state from 17.6% to 3.5%
   at the cost of ~22% more model calls. We get zero by construction at zero
   overhead, because the engine is authoritative and dialogue changes nothing.
7. **Prompt caching got better and cheaper to use.** Cached prefixes bill at
   roughly a tenth, agentic systems measure 41–80% cost reduction and 13–31%
   better time-to-first-token, and Anthropic added automatic caching in early
   2026 — no explicit markers. This settles the "write generous profiles"
   question in favour of generous. It also imposes a **prompt ordering rule**
   (below).
8. **Hand-written character profiles are what the biggest shipped effort does
   too.** Ubisoft's NEO NPC personalities are written by a writer, then tweaked
   once the model starts improvising. Not machine-authored. That is our major-NPC
   decision, independently.
9. **Nobody has shipped this in a AAA game.** As of mid-2026 it is flagship
   demos and prototypes with caveats. There is no commercial answer to copy, so
   our architecture is not behind anything — it is in the same unsettled place
   as everyone else.
10. **The interactive-fiction community is more conservative than we are.** Their
    consensus is to use the model for mood, attitude, and persuasion checks
    rather than to generate dialogue prose at all, precisely because
    hallucinated dialogue introduces things the world does not model. Worth
    knowing that we are deliberately taking the more ambitious path, and that
    "nothing said is true until the bard promotes it" is what pays for it.

## What changes for our design

### A prompt ordering rule, from caching

Caching only pays if the stable part comes first and the volatile part last, and
naive "cache everything" can make latency *worse*. So the NPC call's message
must be ordered:

1. Engine-owned rules that apply to every character (never explain mechanics,
   never volunteer background, never name things that do not exist)
2. The character's profile — immutable, identical every call
3. The setting
4. The character's memory summary
5. Recent conversation lines
6. The player's line

Items 1–3 are byte-identical across every conversation with that character and
are the cache prefix. Items 4–6 change. This is a real constraint on how the
context builder is written, and it is cheap to get right now and annoying to
retrofit.

### The "never" rules belong in the engine prompt, and now there is a threat model

The brainstorm already moved the *never* block out of the character file and
into the engine-owned prompt. The social-engineering finding gives that a
sharper justification: the rules that matter are the ones a player will actively
attack, and a rule living in an editable character file is not a rule.

For the first version this is defence in depth rather than the real defence —
the real defence is that a conversation cannot write to the world. **The moment
an NPC can give the player an item, open a door, or reveal a mechanic, social
engineering becomes live and the engine must own the decision, not the
dialogue.** Record that as the condition for revisiting.

### The memory summary carries facts, never voice

From the personality-erosion finding. Our memory design is "a short summary the
character rewrites, plus recent raw entries." The rewrite is where erosion
happens in the literature. Because our personality lives in an immutable profile
that is re-sent every call, we are protected — but only if nothing tries to be
clever and let the summary describe how the character talks. Facts about what
happened and what the player did. Nothing else.

### Do not add a conversation buffer

The obvious later "improvement" — keep the last twenty exchanges in context — is
exactly the mechanism that produces measured persona drift. Bounded summary plus
a small number of recent lines, permanently.

### Latency: streaming is the industry's answer, and we already deferred it

Cloud responses of two to three sentences run 0.8–2.5 s, which matches our
measurements. The three mitigations in use are local small models, speculative
pre-generation, and streaming the response so generation time is masked.
Streaming is the only one that fits us, and it is already on the deferred list
from the latency work. Dialogue is the strongest argument yet for doing it:
waiting four seconds for a *room description* is tolerable, waiting four seconds
for a person to answer you is not.

### Nothing forces a change to the memory schema

The agent-memory field in 2026 is converging on multi-signal retrieval,
async writes, and benchmarks at 1M–10M tokens. Async writes match what we
already do with the bard's worker thread. The retrieval sophistication is
solving a problem we do not have: per-character memory here is tiny and scoped
to one conversation partner. A summary plus recent entries is not a naive
version of the state of the art; it is the right size for the problem.

## Landscape

| Work | What it is | What we take |
|---|---|---|
| [Fixed-Persona SLMs with Modular Memory](https://arxiv.org/pdf/2511.10277) | Small model with fixed persona plus runtime-swappable memory modules, for local multi-character deployment | Independent arrival at our profile/memory split |
| [Hierarchical Memory Consolidation for NPCs](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6536138) | Three-tier STM/episodic/core memory with graph retrieval; names personality erosion from generic summarisation | The warning that summaries must not carry voice |
| [GILP: Grounded Iterative Language Planning](https://arxiv.org/abs/2606.27806v1) | Parameterised world model plus consistency gate; hallucinated state 17.6% → 3.5% at ~22% more calls | The price of getting grounding from a model instead of an engine |
| [Neuro-symbolic game AI / blackboard](https://veriprajna.com/blog/neuro-symbolic-game-ai-npc-design) | Shared world-state blackboard the model reads but cannot contradict; constrained decoding; the symbolic/neural/validation sandwich | The social-engineering threat model; confirmation of our layering |
| [Ubisoft NEO NPC](https://inworld.ai/blog/ubisoft-neo-npc-prototype) | Largest shipped-adjacent effort; Inworld-backed; writer-authored personalities | Hand-written major-character profiles; the "predictable enough to play their part" problem |
| [State-Inference-Based Prompting](https://arxiv.org/pdf/2507.07203) | Structured game state injected into NPC prompts with explicit valid-action guidelines, for trading NPCs | Confirmation that structured state injection beats freeform context |
| [Persona drift literature](https://www.emergentmind.com/topics/persona-drift) | >30% self-consistency degradation after 8–12 turns; activation-space measures | The reason not to keep a growing conversation buffer |
| [Mem0 state of agent memory 2026](https://mem0.ai/blog/state-of-ai-agent-memory-2026) | Ten memory approaches benchmarked; async writes settled; open problems at 1M+ tokens | Confirmation that our memory size is not the field's problem |
| [MemFail](https://arxiv.org/pdf/2605.26667) | Stress-tests memory failure modes: temporal degradation, retrieval failure, scaling | Vocabulary for what to test |
| [IF forum: LLM NPCs in parser games](https://intfiction.org/t/plug-in-an-llm-to-text-adventure-to-make-a-smarter-npc/75236) | Practitioner discussion; consensus is to use the model for attitude and persuasion checks, not prose | The dissenting position, worth holding in mind |
| [AI-powered NPCs: hype or hallucination](https://medium.com/curiouserinstitute/ai-powered-npcs-hype-or-hallucination-11ddfc530e33) | Critical account: NPCs invent locations, quests, and impossible commitments | The concrete failure catalogue our rules have to prevent |
| [Prompt caching in 2026](https://www.digitalapplied.com/blog/prompt-caching-2026-cut-llm-costs-engineering-guide) · [Don't Break the Cache](https://arxiv.org/pdf/2601.06007) | ~10× cheaper cached prefixes, automatic caching, and the ordering discipline that makes it work | The prompt ordering rule |

## What did not change

- The vision's split is still the field's answer. Nothing found argues for
  letting the model own state.
- Generative Agents is still the memory blueprint, and its cost lesson still
  holds: do not simulate characters that nobody is looking at.
- Our earlier conclusion that retrieval-approximation of state loses to exact
  state is, if anything, stronger — the 2026 memory benchmarks are all measuring
  how badly retrieval degrades at scale.

## Sources

- https://arxiv.org/pdf/2511.10277 — Fixed-Persona SLMs with Modular Memory
- https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6536138 — Hierarchical memory for NPC personality (abstract only; full text paywalled)
- https://arxiv.org/abs/2606.27806v1 — GILP, consistency gate
- https://arxiv.org/pdf/2507.07203 — State-inference-based prompting for NPC trading
- https://arxiv.org/pdf/2605.26667 — MemFail
- https://arxiv.org/pdf/2601.06007 — Don't Break the Cache
- https://veriprajna.com/blog/neuro-symbolic-game-ai-npc-design
- https://inworld.ai/blog/ubisoft-neo-npc-prototype · https://news.ubisoft.com/en-us/article/5qXdxhshJBXoanFZApdG3L/how-ubisofts-new-generative-ai-prototype-changes-the-narrative-for-npcs
- https://medium.com/curiouserinstitute/ai-powered-npcs-hype-or-hallucination-11ddfc530e33
- https://intfiction.org/t/plug-in-an-llm-to-text-adventure-to-make-a-smarter-npc/75236
- https://www.emergentmind.com/topics/persona-drift · https://www.emergentmind.com/topics/persona-collapse
- https://mem0.ai/blog/state-of-ai-agent-memory-2026
- https://www.digitalapplied.com/blog/prompt-caching-2026-cut-llm-costs-engineering-guide
- https://www.solidaitech.com/2026/06/ai-video-games.html — 2026 landscape, latency figures
- https://memedadacoin.com/blog/ai-npcs-nvidia-ace-inworld — shipping status
