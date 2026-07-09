---
title: AI resolver — NL input as a compiler frontend
date: 2026-07-08
status: resolved
tags: [ai-integration, nl-resolver, compiler-frontend, action-isa, tool-use, usability]
modules: [parser, action, loop]
related: [.lore/work/brainstorm/ai-integration-points.md]
---

# AI resolver — NL resolver as a compiler frontend

## Context

Prose renderer (integration point #0) is shipped: read-only, network-egress
seam, mechanical validation gate, template fallback. This session picks up
integration point #1 — the NL action resolver — and lands its shape. The
resolver replaces the disposable fixed-verb `parser.cpp`, translating natural
language input into the engine's closed `Action` set.

## Core framing: resolver = compiler frontend

The controlling analogy, and it holds all the way down:

- **Action set = an ISA.** A fixed, uniquely-defined instruction set
  (`enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit }` today).
  Think bytecode / opcodes, not a synonym menu.
- **Resolver = the frontend.** Lowers NL source → a single lowered
  instruction (`Action`).
- **Engine (`resolve()`) = the machine.** Executes the opcode and can *fault*
  (refuse) — this is already how the engine behaves.
- **nullopt = a compile error.**

```
input line ──▶ [RESOLVER] ──▶ Action ──▶ engine resolves/writes ──▶ events ──▶ [RENDERER] ──▶ prose
              AI translates                 mechanical truth                    AI translates
```

Resolver and renderer are mirror images across the tick. Both read-only by
contract: the resolver emits an `Action`, the engine does the mutation — the
resolver never writes. It reuses the renderer's network-egress seam and the
"AI translates, never decides" boundary.

## Stakes invert vs the renderer

Same seam shape, opposite failure cost:

- **Renderer failure** = ugly sentence → template fallback → world untouched.
  Hallucination caught by "no claim without a sourcing fact."
- **Resolver failure** = *wrong opcode* → engine mutates the wrong thing.
  A hallucinated Action has mechanical teeth.

The safety net already exists: the engine validates Actions regardless
(unknown nouns → nullopt in the current parser; `resolve()` can refuse). So the
AI only has to be **safe** (valid enum verb, real in-scope entity id), not
**correct** — correctness the engine re-checks anyway. That is the resolver's
"no claim without a sourcing fact": *subject must be a real in-scope entity.*

## Decisions

### No synonym table — the LLM does all translation

Rejected the `unordered_map` synonym approach ("get|grab|pick → Take"). The
whole point is that the ISA is uniquely defined and the LLM lowers arbitrary
phrasing onto it — including the tail no finite table reaches: unbounded
phrasing ("I'll take that lantern", "the lantern, please") and, the strongest
case for AI, reference/pronoun resolution ("take it", "grab the brass one",
"the one on the table"), which needs scope context, not a word map.

### Tool-use / structured output, not free-text-then-parse

The resolver emits a *machine instruction*, so use the Anthropic
tool-calling / structured-output path: one `emit_action` tool (or one tool per
opcode) with a `verb` enum + `subject` + `direction`. The model calls the
opcode with typed arguments. The verb enum is **schema-enforced** — the model
cannot emit an out-of-set verb, killing a class of hallucination for free.
Subject-is-real-entity is still checked mechanically after. This is a real
architectural fork from the renderer's free-text approach.

### The prompt *is* the ISA spec — this is the actual work

A probabilistic translator only lands deterministically if each opcode is
crisp and non-overlapping. The deliverable is less code than a tight
instruction-set spec: Take vs Drop vs Go with zero semantic overlap, or
translations drift on ambiguous input. This is the resolver's analog of the
narrator system prompt, but *stricter* — prose slightly off is fine, an opcode
slightly off executes the wrong thing.

### nullopt = a good compile error, delivered as flavor prose

The compiler lens picks the answer to the prior session's sharpest open
question (fail-flat vs flavor-only vs escalate-to-gen):

- A *bad* compiler says `syntax error` and dies — the parser's current
  fail-flat behavior.
- A *good* compiler emits a real diagnostic: "nothing here responds to smell."
  That diagnostic is a grounded, flavor-only response — generated, no state
  touched.

So: **not fail-flat, not escalate-to-gen — a good compile error that happens
to be flavor prose.** Extending the ISA (a new verb) is a *language version
bump* the developer ships; never something the AI does mid-game. Boundary
stays crisp: AI translates to the ISA, never grows it.

### One opcode out — multi-intent is a compile error (deferred to game design)

Real compilers lower one statement to many instructions, but the engine is
one-prompt-one-tick (REQ-PROTO-5). So the resolver emits **at most one opcode
per line**. "take the lantern and the key" (two Takes), "put the lantern on the
table" (compound), "take all" (no opcode) → compile error. This is
genre-faithful: Zork is one command per line. The multi-opcode question is a
game-design bucket item, not a resolver question, and is deferred. No
REQ-PROTO-5 change forced.

## The two-axis product frame

The project is a Zork-like game enhanced by AI along two axes, which map onto
the five integration points:

- **Usability axis** — resolver (#1) + renderer (#0). Input NL + output NL =
  the "conversational wrapper" the prior brainstorm named.
- **Replayability axis** — story (#3), worldgen (#2), NPCs (#4). The
  generative engine underneath.

### The counterintuitive consequence

The two axes have **opposite failure tolerances**, which inverts which AI is
the hard engineering:

- **Usability fails functionally.** Wrong opcode → wrong mutation → broken
  game. Zero tolerance. Hence the strict ISA spec, tool-use output, and
  entity-scope gate.
- **Replayability fails aesthetically.** A swamp next to a library is
  disappointing, not broken. Higher mechanical tolerance; the hard part is
  coherence, and engine invariants (exit reciprocity, no orphans) already
  catch the *unsafe* generation.

So the naive intuition — "generative AI is the hard part, the interface is
plumbing" — is backwards for this engine's contracts. The resolver, though
"just usability," is the **highest-rigor AI application in the codebase**: the
one place an AI mistake corrupts state instead of merely looking bad. That
justifies front-loading design effort here rather than treating it as a thin
wrapper.

## Open / carried forward

- **Parser's fate.** Renderer kept templates as a permanent fallback; does the
  disposable `parser.cpp` get deleted or promoted to the resolver's
  deterministic fallback? Its header says "DISPOSABLE, deleted without
  ceremony," which the renderer's fallback pattern argues against. Undecided.
- **Two LLM calls per turn.** Resolve in, narrate out, with a mechanical tick
  between. Cannot merge (world changes between; seam must stay clean).
  Latency/cost the renderer flagged just doubled. Named, not solved.
- **Multi-opcode / batch instruction** — deferred to game design.

## Landed shape

- Resolver = compiler frontend; Action set = closed ISA; engine = machine that
  can fault.
- Strictly one opcode out; multi-intent = compile error (genre-faithful).
- Tool-use / structured output; verb enum schema-enforced.
- Mechanical gate reuses the prose pattern: subject must be a real in-scope
  entity.
- nullopt = a good compile error, delivered as grounded flavor prose.
- The prompt *is* the ISA spec — crisp, non-overlapping opcode definitions are
  the real work.
