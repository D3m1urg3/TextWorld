---
title: "Prior art for the bard: storylets, fronts, directors, and LLM drama managers"
date: 2026-08-03
status: active
tags: [dungeon-master, bard, storylets, quality-based-narrative, drama-manager, apocalypse-world, fronts, countdown-clocks, ai-director, pacing, llm-agents, narrative-planning, coherence, retcon, prior-art]
modules: [architect, world]
related: [.lore/work/brainstorm/dungeon-master.md, .lore/work/design/story-seed-architect.md, .lore/vision.md]
---

# Prior art for the bard: storylets, fronts, directors, and LLM drama managers

Research run against the design in [dungeon-master.md](../brainstorm/dungeon-master.md). Question asked: which existing systems already do "a story agent that selects from a catalog rather than inventing," what mechanisms can be stolen outright, what has failed and why, and does anything bear on our unresolved retcon boundary.

## Key findings

1. **Our catalog + eligible-menu design is a re-derivation of *storylets* / quality-based narrative**, a pattern with ~15 years of shipped practice (Fallen London, StoryNexus, Reigns, Hades). A storylet is formally *content + prerequisites + effects on world state* — which is exactly a catalog row + eligibility predicate + materialization. We are at a mapped point in a known design space, not inventing one.
2. **Apocalypse World "Fronts" is the macro catalog, already field-tested**, and it contains the piece our design is missing: the **countdown clock**, the mechanism by which threats advance *without* player input. Tabletop designers hit the purely-reactive problem and solved it with clocks.
3. **AW clocks independently arrive at our irreversibility ratchet**: past a threshold, a clock's consequences "cannot be reversed, only mitigated." That is the same claim as our L0→L2 materialization and immutable event rows, reached from a completely different direction.
4. **The strongest external validation of store-mediation (decision 2)** comes from 2026 LLM-GM research: *"Without a shared symbolic state to ground collaboration, instructions from high-level planners are often misinterpreted by downstream agents, leading to hallucinations that contradict established narrative facts."* Natural-language-only coordination between agents is a documented failure mode. Our SQLite canon **is** the shared symbolic state.
5. **StoryVerse (2024) is close to our exact architecture** — Act Director / Character Simulator / Game Environment — and contributes a mechanism we should steal: **placeholders** in abstract acts, bound to concrete entities at instantiation time.
6. **AI Dungeon's incoherence is misdiagnosed in popular accounts.** Research points at *strategy and error-accumulation*, not memory exhaustion, as the primary source of long-term derailment. That is directly relevant to our retcon boundary: the danger is compounding drift, not forgetting.
7. **Generative Agents cost thousands of dollars for 25 agents over 2 simulated days.** Hard empirical support for "never simulate NPCs every tick."
8. **Nobody has solved retcon.** In interactive-narrative vocabulary it is "Schrödinger's Question," and it is catalogued as a *trope*, not a mechanism. Our unresolved question is unresolved in the field.

## 1. Storylets and quality-based narrative

**What it is.** Emily Short's definition: a storylet has three parts — *content* ("a line or a whole section of dialogue... narration... an animation"), *prerequisites* "that determine when the content can play," and *effects on the world state that result after the content has played." Quality-based narrative (QBN) is Failbetter's term for narratives built from storylets unlocked by *qualities* — numeric variables covering inventory, skills, and story progress alike. Fallen London is the archetype; StoryNexus is the engine.

**Mapping to our design.**

| Storylet concept | Our equivalent |
|---|---|
| Storylet | Catalog entry (character, lock/key pair, beat) |
| Qualities | Component rows + `meta` + the event log |
| Prerequisites | The eligible-menu function (carried-forward question 4) |
| Effects on world state | Materialization — the L0→L2 transition |
| Salience-based selection | The bard picking from the engine-computed menu |

**Kreminski's four axes** (via Short's summary of the ICIDS 2018 survey) give a vocabulary for stating our design precisely: *precondition definition*, *repeatability*, *content type*, and *content selection architecture*. Ours is: engine-computed preconditions; non-repeatable (materialization is irreversible); LLM-generated content at placement time; model-selects-from-engine-menu architecture. Worth writing down in those terms in the design doc — it makes the design legible to anyone who knows the literature.

**The failure mode to avoid.** Short warns that "time cave" structures — where each storylet needs a *unique identifier flag* to gate it — eliminate "most of the value of using storylets." Applied to us: **if every catalog entry gets a bespoke unlock condition, we have hand-authored branching wearing a catalog costume.** Eligibility must come from *general* predicates (distance from seed, tier, does the player lack this key) the way `eligibleEnemyBlurbs` already does, not per-entry flags.

**It scales in production.** *Hades* runs thousands of guard-clause conditions — enemy kills, fish caught, upgrades purchased, plot beats completed — to decide which lines are eligible. Precondition-heavy design is shipped, mainstream practice, not a research curiosity.

**Kreminski's *Starfreighter*** binds resource requirements to storylets so that `sell [cargo] on [planet]` becomes eligible when matching resources exist and auto-instantiates with the specifics. That is the placeholder idea (see §4) arriving from the storylet side.

## 2. Apocalypse World Fronts — the macro, already designed

A **Front** is "a collection of threats and agendas, motivated by a Fundamental Scarcity, defined with specialty Moves, and tracked with Countdown Clocks." This is our overture's catalog, item for item.

Three things worth stealing:

**Fundamental Scarcity — a closed vocabulary.** Every front is motivated by exactly one of eight: *hunger, thirst, envy, ambition, fear, ignorance, decay, despair*. A closed enum of eight motives is exactly the kind of thing our engine can enforce as a schema-level constraint on catalog rows, and it prevents the bard from writing mush. Cheap, high-leverage.

**Countdown clocks — considered and REJECTED for this project (2026-08-03).** Kept here because the mechanism is real and the next person will otherwise rediscover it. `seed/setting.txt` makes tension **spatial, not temporal** — *"the further out, the safer and the stiller"* — and states outright that *"the invasion is a spreading front, **not a flood**."* A clock converts that spatial gradient into a temporal one, which destroys the safe-outer-edge structure the setting authored. The gradient is already implemented as `distanceFromSeed` gating enemy tier; **movement through space is the clock**. See brainstorm decision 16. The description below stands as a record of what was evaluated.

Quoting the source directly: *"A countdown clock is a reminder to you as MC that your threats have impulse, direction, plans, intentions, the will to sustain action and to respond coherently to others'."* This is precisely the deficiency identified at the end of the cadence discussion — a purely reactive bard means nothing can happen *to* the player. Tabletop's answer is not "wake more often"; it is a **per-threat counter that advances on defined events**, which the GM consults rather than recomputes.

That reframes our open design space nicely: a clock is *engine state*, not bard state. The bard authors clocks at the overture (what advances this, and what happens at each segment); the engine ticks them deterministically; the bard reads their positions on its next wake. No extra model calls, and the world gains the ability to move on its own **without** giving the bard a time-based trigger.

**Irreversibility past a threshold.** In AW, clock segments past 21:00 "cannot be reversed, it can only be mitigated." An independently-arrived-at version of our L2/immutable-event claim, and a useful precedent for the retcon question: the tabletop answer to "can the story be walked back" is *no, but it can be mitigated*, and that is a designed property rather than an accident.

## 3. Left 4 Dead's Director — reactive triggers are not enough on their own

The L4D Director tracks a per-survivor **emotional intensity** metric (0..1) and runs an explicit pacing state machine: **Build Up → Peak → Relax** (relax ≈ 30–45 s), spawning, withholding, and dropping supplies according to which phase it is in. Intensity rises when a survivor is attacked and when they kill nearby infected.

**What this exposes in our design.** Our irreversible-change trigger answers *when the bard wakes* but carries no notion of whether the player currently needs pressure or relief. Two different players can both trigger `defeated` while one is cruising and the other is nearly dead. L4D's answer is a cheap, deterministic, engine-computed scalar handed to the director as a fact.

We can compute an equivalent from the event log with no model involvement: damage taken over the last N turns, fights resolved, turns since last threat, distance from a safe room. That belongs alongside the catalog on the wire — an **engine-authored** number, exactly like the status band's cooldown figures are engine-authored rather than model-authored.

## 4. The academic drama-manager lineage

**Façade** (Mateas & Stern) is the canonical interactive drama. Authors write **beats**; a **drama manager** "globally sequences beats chosen from a large pool in order to make story tension rise and fall to match an Aristotelian arc." Select-from-a-pool, with a global tension target, has a twenty-year pedigree. The **Automated Story Director** is the same idea giving plot-based guidance to believable agents.

**StoryVerse** (2024) is the closest published analog to what we are building:

- **Act Director** — checks whether abstract acts meet prerequisites each timestep and plans concrete character actions.
- **Character Simulator** — autonomous character behavior when no act is active.
- **Game Environment** — holds world state, executes actions, receives player interventions.

An **abstract act** = *narrative goal* + *prerequisites* + **placeholders** — variables for unknown specifics resolved at instantiation, e.g. *"X — the character who got into the accident."* After an act runs, an LLM resolves each placeholder to a concrete entity and **stores the mapping for subsequent acts**.

**Steal the placeholder mechanism.** It resolves a tension in our catalog design: the overture cannot know who or where, but it should not therefore be vague. It can write *"X betrays the player over Y"* with typed placeholders, and the micro wake binds `X` and `Y` to real entities at materialization — after which the binding is canon and every later act must respect it. That is a catalog entry that is both concrete enough to be dramatic and abstract enough to be placeable.

**Their stated limitations are our predicted ones**, which is reassuring rather than alarming: long-term dependency and coherence are hard; frequent LLM calls create latency; player modeling is underdeveloped. We have already priced the first two (cadence, off-critical-path) and deferred the third.

## 5. LLM-specific failure modes

**Shared symbolic state is not optional.** From 2026 work on LLM game-master architectures: role-playing paradigms with multiple specialized LLM personas "excel at stylistic diversity and dialogue richness," but their coordination "relies predominantly on unstructured natural language. Without a shared symbolic state to ground collaboration, instructions from high-level planners are often misinterpreted by downstream agents, leading to hallucinations that contradict established narrative facts."

This is decision 2 restated as an empirical finding. Note what it says about the *hierarchical* alternative we rejected: a mastermind issuing natural-language orders to executors is precisely the configuration that fails. Our store — typed rows, ids, engine-computed menus — is the symbolic grounding that makes the flat structure work.

**Schema-governed generation with an admission gate.** The G-KMS pipeline (Feb 2026) is: knowledge grounding → schema-governed generation → **normalization-based repair** → **engine-aligned knowledge admission** → application. Evaluated on a Unity RPG with structural/semantic analysis, playability probes, and human studies.

We already have four of five stages. The one we have only partially is **normalization-based repair** — repairing a near-miss rather than rejecting it. `validateRoomProposal` already does this for exits (a bad exit is dropped with a diagnostic and "NEVER rejects a room") but everything else is pass/fail with a fallback to the wall. Worth deciding deliberately for catalog admission rather than inheriting the stricter default by accident.

**Symbolic vs. LLM planning is a known trade, and hybrids win.** Symbolic narrative planning "guarantees the causal soundness of generated plots" but needs a hand-crafted formal knowledge base and yields limited complexity; LLM methods take flexible abstract specifications but lose soundness. The systems that work let writers author abstract high-level outlines that are automatically instantiated at play-time. That is the macro/micro split, arrived at independently.

**AI Dungeon.** Popular accounts blame context-window exhaustion — characters forgetting names, personality drift, contradictions, loop behavior, "identity collapse." But research on long-term derailment points to **strategy and error-accumulation, not memory exhaustion**, as the primary cause.

That matters for us. Our journal pattern solves the memory-exhaustion story (bounded context, flat cost regardless of session length) but does *nothing* about error accumulation — a bard that drifts one degree per wake is 50 degrees off after 50 wakes, with each step locally plausible. **The append-only-with-recorded-breakage design is exactly an error-accumulation brake**: an immutable record of what was promised means drift is detectable rather than absorbed.

**Generative Agents** (Park et al., UIST 2023 best paper): memory stream + retrieval scored on *relevance, recency, importance* + reflection into higher-level inferences + planning decomposed recursively. The memory-stream/reflection loop is our journal, validated. The cost figure — thousands of dollars for 25 agents over two simulated days — is the empirical form of the NPC argument already parked in the brainstorm.

## 6. On the retcon boundary — no one has an answer

The closest named concept is **"Schrödinger's Question"**: a player choice retroactively affecting aspects of the world that existed before the choice was made. It is catalogued as an interactive-storytelling *trope*, not as a solved mechanism.

The nearest thing to a working answer is the "justification over perfection" stance found in practitioner writing: rather than eliminating inconsistency, narratively justify it — unreliable narration, multiverse framing — so player expectations absorb procedural unpredictability. That is a legitimate design move but it is not available to us: our vision's persistence principle and the whole canon-vs-flavor split exist precisely to avoid needing it.

The two mechanisms that *do* exist and that we can lean on:

- **AW's ratchet** — past a segment, consequences cannot be reversed, only mitigated. Design irreversibility in, rather than trying to police revision.
- **Foreshadowing as measured probability** — practitioners recommend measuring which events will likely occur and referencing them *earlier*, so surprise survives while plausibility is established. This is the promise half of promise/redemption, and it suggests the bard's catalog wants a "seeded" flag: a catalog entry that has been hinted in prose but not yet materialized. A promise the player has *seen* is much more expensive to retcon than one they haven't.

Neither closes the question. The append-only arc that records its own breakage remains our own proposal, unsupported by prior art but not contradicted by it.

## What to steal, ranked

1. **Placeholders in catalog entries** (StoryVerse). Lets the overture be dramatic without knowing the map, with bindings frozen at materialization.
2. **An engine-computed intensity scalar** (L4D) — but **inverted** from its source use. Not "when to send a horde"; "when to leave the player alone." L4D's own Build Up → Peak → Relax says pressure should not be constant, and in this game exploration and conversation *are* the Relax phase, permanently. `distanceFromSeed` already supplies the spatial half; the player-state half (health, cooldowns, turns since last fight) is cheap and tells the bard when someone is limping back out to the quiet edges.
3. **A closed motive vocabulary** — the *idea* from AW's eight scarcities, not the vocabulary. Hunger/thirst/decay/despair is wrong for a school "gone soft and scholarly"; Thornmere's would be nearer curiosity, secrecy, rivalry, obligation, grief, and whatever the goblins want with the grimoires. Authored, not imported.
4. **Storylet vocabulary for the design doc** (Kreminski's four axes). Makes our design legible and forces the eligibility question into the open.
5. **A "seeded" flag on catalog entries** — hinted in prose but not yet materialized. Partially addresses retcon by making visible promises costlier to break than invisible ones.

**Rejected:** countdown clocks (see §2) — structurally incompatible with a spatial tension gradient.

## What to avoid

- **Per-entry unlock flags.** The time-cave failure. Eligibility must be general predicates.
- **Natural-language coordination between agents.** Documented hallucination source; keep everything on typed rows and ids.
- **Per-tick agent simulation.** Generative Agents' cost figure is the ceiling case.
- **Assuming the journal fixes coherence.** It fixes memory exhaustion. Error accumulation is a separate problem needing a separate brake.

## Sources

- [Storylets: You Want Them — Emily Short](https://emshort.blog/2019/11/29/storylets-you-want-them/)
- [Survey of Storylets-based Design (Kreminski) — Emily Short](https://emshort.blog/2019/01/06/kreminski-on-storylets/)
- [Beyond Branching: Quality-Based, Salience-Based, and Waypoint Narrative Structures — Emily Short](https://emshort.blog/2016/04/12/beyond-branching-quality-based-and-salience-based-narrative-structures/)
- [Quality-Based Narrative — SimpleQBN](https://videlais.github.io/simple-qbn/qbn.html)
- [Apocalypse World: Fronts — Black Armada](https://blackarmada.com/apocalypse-world-fronts/)
- [Locked, Clocked and Ready to Rock — Kevin Whitaker](https://kwhitaker81.medium.com/locked-clocked-and-ready-to-rock-c42ac20ffbd5)
- [Dungeon World Fronts in D&D — Sly Flourish](https://slyflourish.com/fronts_in_dnd.html)
- [The Director — Left 4 Dead Wiki](https://left4dead.fandom.com/wiki/The_Director)
- [AI Systems of L4D2 (Mike Booth)](https://www.scribd.com/doc/160700503/Ai-Systems-of-l4d2-by-Mike-Booth)
- [Structuring Content in the Façade Interactive Drama Architecture — Mateas & Stern](https://ojs.aaai.org/index.php/AIIDE/article/view/18722)
- [Integrating Plot, Character and Natural Language — Mateas](https://users.soe.ucsc.edu/~michaelm/publications/mateas-tidse2003.pdf)
- [StoryVerse: Co-authoring Dynamic Plot with LLM-based Character Simulation via Narrative Planning](https://arxiv.org/html/2405.13042v1)
- [Game Knowledge Management System: Schema-Governed LLM Pipeline for Executable Narrative Generation in RPGs](https://www.mdpi.com/2079-8954/14/2/175)
- [Generative Agents: Interactive Simulacra of Human Behavior — Park et al.](https://ar5iv.labs.arxiv.org/html/2304.03442)
- [Long-Term Coherence in LLMs — EmergentMind](https://www.emergentmind.com/topics/long-term-coherence-in-llms)
- [Negative Feedback on LLM-Powered Storytelling & Roleplay Apps](https://cuckoo.network/blog/2025/04/17/negative-feedback-on-llm-powered-storytelling-and-roleplay-apps)
- [Procedural Narrative and How to Make It Coherent — New to Narrative](https://newtonarrative.com/blog/procedural-narrative-and-how-to-keep-it-coherent/)
- [Interactive Storytelling Tropes — TV Tropes](https://tvtropes.org/pmwiki/pmwiki.php/Main/InteractiveStorytellingTropes)

## Caveats

- Kreminski's four axes and the storylet definitions are taken from Emily Short's summaries, not from the ICIDS paper directly. Worth verifying against the primary source before they become load-bearing.
- The MDPI G-KMS paper returned 403 to direct fetch; its description here comes from search-result summaries, so the pipeline stages are reliable but the quantitative results are not verified.
- Nothing here was found that runs an LLM drama manager against a *persistent, engine-authoritative* world store of the kind we have. The closest (StoryVerse) keeps world state in a simulator built for the experiment. That gap is either our advantage or an unexplored hazard.
