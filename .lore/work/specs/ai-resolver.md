---
title: AI resolver — requirements
date: 2026-07-08
status: draft
tags: [ai-integration, nl-resolver, claude-api, tool-use, action-isa, parser, fallback, requirements]
modules: [parser, action, loop, resolve]
related: [.lore/work/brainstorm/ai-resolver.md, .lore/work/brainstorm/ai-integration-points.md, .lore/work/specs/ai-prose-renderer.md, .lore/work/specs/engine-foundation-prototype.md]
req-prefix: RESOLVE
---

# AI resolver — requirements

## Context

Second AI feature in the engine (integration point #1). Replaces the disposable fixed-verb `parser.cpp` as the primary input path with a Claude-backed natural-language resolver that lowers a raw input line into the engine's closed `Action` set. The shape is settled in [.lore/work/brainstorm/ai-resolver.md]; this spec encodes it as verifiable requirements and mirrors the transport, fallback, and test seams established by the prose renderer ([.lore/work/specs/ai-prose-renderer.md]).

**Controlling frame:** the `Action` set is a closed ISA (REQ-PROTO-7's `enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit }`); the resolver is a compiler frontend that lowers NL source to at most one instruction; the engine (`resolve()`) is the machine that executes it and can fault (tier-b refusal). AI translates onto the ISA, never grows it.

**Boundary declaration (vision principle 1):** verb classification and noun-word selection are AI-driven (translation, no mechanical consequence at emit time). Entity-id assignment, scope/applicability, state mutation, and refusal remain deterministic (engine). The resolver emits an `Action`; it never writes. A resolver-produced `Action` is not a guarantee of success — a resolved `take` of an item that exists but is not in the room ticks and produces a tier-b `failed` event (REQ-PROTO-6b), never a retry or a resolver re-call. The engine remains the sole authority on applicability.

**Latency note (deferred, but stated):** in AI-enabled mode `aiResolve` is attempted on *every* line, including bare `look`/`wait`/`quit`, so every turn carries a minimum of one input round trip (two total with narration). This per-turn-minimum, not just aggregate cost, is the two-calls-per-turn deferral below.

## Requirements

### Dispatch & fallback

**REQ-RESOLVE-1** — When AI resolution is enabled, each input line is first lowered by `aiResolve(db, line)`. When it returns a value, that `Action` drives the turn. When it returns no value, the existing fixed-verb `parse(db, line)` is attempted. When that also returns no value, the turn is tier-a: `renderError("I don't understand that.")`, no tick. Input never fails to route because of the AI path.

**REQ-RESOLVE-2** — AI resolution is enabled under the *same* switch as AI narration (REQ-PROSE-2): `ANTHROPIC_API_KEY` set and non-empty AND `TEXTWORLD_AI` not exactly `0`. One flag governs both AI features; independent toggling of input vs output is out of scope. When disabled, the game never attempts a resolver call and input is handled exactly by the fixed-verb parser, as in the pre-AI build.

**REQ-RESOLVE-3** — Every failure of an attempted resolver call — HTTP error, timeout, malformed/unparseable response, no tool call, validation-gate failure — results in `aiResolve` returning no value, which falls through to the fixed-verb parser (REQ-RESOLVE-1). No failure crashes, blocks past the timeout, or leaks an error into player-visible output (a single non-prose diagnostic line on stderr is permitted). The no-key case is disabled mode per REQ-RESOLVE-2, not a call failure.

**REQ-RESOLVE-4** — `parser.cpp` is promoted from disposable throwaway to the **permanent deterministic fallback** for input, exactly as `render.cpp` templates are the permanent fallback for output. Its header comment is rewritten to state this role. Its behavior (REQ-PROTO-6a bare-verb `nullopt`, first-match `lookupNoun`, etc.) is unchanged.

### Contract preservation

**REQ-RESOLVE-5** — The resolver path performs only SELECT statements against the database (scope reads and the existing name lookup). No write. `grep -En "INSERT|UPDATE|DELETE" src/nlresolve.cpp` returns nothing. The db-byte-identical discipline (REQ-PROTO-9 / REQ-PROSE-5) extends to `aiResolve`: resolving a line mutates no database bytes.

**REQ-RESOLVE-6** — Nothing beyond the input line, the scope-context fields (REQ-RESOLVE-7), and the fixed system prompt leaves the process: no entity/row ids, no other tables, no file paths, no full event history, no schema. Entity ids are never sent to or received from the model (mirror REQ-PROSE-6).

### Scope-context payload

**REQ-RESOLVE-7** — A pure function of `(db, line)` builds the resolver context sent as the user message, containing exactly: the raw input line; the current room's name; its exit direction words; the names of items visible in the room; and the actor's inventory item names. Nothing else — no ids. The builder is callable and unit-testable without any network dependency.

### API request (tool-use / structured output)

**REQ-RESOLVE-8** — The resolver uses the Anthropic Messages API tool-use path, *not* free-text-then-parse. A single tool `emit_action` is defined with a **schema-enforced** `verb` enum (exactly the seven ISA verbs), an optional `subject` string (a noun word), and an optional `direction` string. The model calls `emit_action` at most once to lower the line to one instruction. `tool_choice` is `auto`: when the input maps to no single in-set action (unknown intent, or multi-intent), the model makes no tool call, which the resolver treats as no value (REQ-RESOLVE-3 → fallback).

**REQ-RESOLVE-9** — Request parameters mirror REQ-PROSE-8 except as noted: `POST /v1/messages`, `anthropic-version: 2023-06-01`, model `claude-opus-4-8` overridable via `TEXTWORLD_MODEL` (shared with the renderer), `max_tokens` 512 (resolver output is a single small tool call), no `thinking`, no streaming, no prompt caching. The `tools` array carries the REQ-RESOLVE-8 `emit_action` schema.

**REQ-RESOLVE-10** — The HTTP call has a total timeout of 8 seconds and performs no retries. Timeout counts as failure per REQ-RESOLVE-3.

**REQ-RESOLVE-11** — The resolver reuses the renderer's injectable `HttpTransport` seam (`prose.hpp`) so tests can substitute canned tool-use responses without network access. A test-visible `aiResolve(db, line, transport)` overload exists; the production two-arg form binds the libcurl transport. The transport is invoked at most once per call.

### The ISA spec (system prompt & grounding)

**REQ-RESOLVE-12** — The system prompt *is* the ISA spec. It defines each of the seven verbs crisply and non-overlappingly, and instructs the model: translate the input to exactly one action from the closed set; emit it via `emit_action`; `subject` must be one of the supplied in-scope noun names, verbatim, and no noun absent from the scope context may be introduced; `direction` is a movement/compass word for `go`; emit at most one action — multi-intent input gets no tool call; if the input maps to no single in-set action, make no tool call. No pronoun/anaphora resolution is attempted (v2).

### Validation & mapping gate (mechanical)

**REQ-RESOLVE-13** — The resolver accepts the model's tool call and lowers it to an `Action` only if all hold; otherwise `aiResolve` returns no value (→ fallback):
  a. HTTP 200 and the response contains **exactly one** `tool_use` block for `emit_action` (zero blocks, or two or more, is a gate failure — the latter enforces one-opcode-per-line);
  b. the `verb` argument is exactly one of the seven ISA verbs;
  c. for `take`/`drop`: `subject` is present and the shared name lookup (REQ-RESOLVE-14) resolves it to a real entity (non-zero id); the id is assigned mechanically by the engine, never taken from the model. This is a **world-wide** recognition check, at parity with the fixed-verb parser — *scope applicability* ("is it actually here?") is deliberately NOT checked here; it is the engine's job at tier-b (REQ-PROTO-6, recognition vs resolution). The REQ-RESOLVE-12 in-scope-noun rule is a prompt-side grounding instruction, not a second mechanical gate;
  d. for `go`: `direction` is present and non-empty;
  e. for `look`/`inventory`/`wait`/`quit`: no argument is required or consulted.
This gate is a pure function that never throws; on failure it may emit one stderr diagnostic line naming the first failed clause.

### Build

**REQ-RESOLVE-14** — No new build dependencies: the resolver reuses the already-vendored libcurl and nlohmann/json (REQ-PROSE-15). It lives in a new translation unit (`src/nlresolve.cpp` / `nlresolve.hpp`) — named to avoid colliding with the engine's existing mutating `resolve()` in `systems.cpp` — that must stay free of `parser.cpp` types, preserving the `action.hpp` seam. The noun→entity lookup currently private to `parser.cpp` (`lookupNoun`, anonymous namespace) is hoisted into a shared header that both `parser.cpp` and `nlresolve.cpp` include, so the REQ-RESOLVE-13c gate reuses one lookup rather than duplicating it.

### Testing

**REQ-RESOLVE-15** — Unit tests (no network, default `tests` target) cover: the scope-context builder fields (exactly the REQ-RESOLVE-7 set, nothing more — also covers REQ-RESOLVE-6); the REQ-RESOLVE-13 gate (each clause a–e, plus no-tool-call → no value); dispatch fallback (aiResolve no-value → parser → `renderError`); request-body assembly (the `emit_action` tool schema with the seven-verb enum, model `claude-opus-4-8` and the `TEXTWORLD_MODEL` override, `max_tokens` 512, no `thinking`/`stream` keys); db byte-identity across `aiResolve` with a fake transport (REQ-RESOLVE-5).

**REQ-RESOLVE-16** — A live end-to-end smoke test exists but runs only under the existing live gate (`TEXTWORLD_AI_LIVE_TEST=1`); skipped in the default run and excluded from any future CI. It drives real phrasings ("pick up the lantern", "grab lantern") through the production transport and asserts **mechanical invariants only** — a valid `Action` of the expected verb/subject, or a clean fallback — never model-specific wording, to stay non-flaky.

## Out of scope (explicit deferrals)

- **Good-compile-error refusals** — AI-generated in-fiction rejection prose on the no-action path. v1 routes no-value to the existing `renderError` template. This is the brainstorm's "good compile error"; deferred to v2.
- **Pronoun / reference resolution** — "take it", "the brass one", "the one on the table". v1 requires the noun word to appear in the input; no last-mentioned/anaphora tracking. v2.
- **Multi-action / batch instructions** — one opcode out per line; multi-intent is a no-action (genre-faithful, Zork-style). A batch instruction would be a REQ-PROTO-5 change; out of scope.
- **ISA extension** — new verbs are a developer-shipped "language version bump", never AI-invented mid-game.
- **Two-calls-per-turn latency/cost optimization** — resolve-in + narrate-out is two LLM round trips with a tick between; naming it, not optimizing it (async, caching, merging are v2 levers).
- **Independent input/output AI toggles** — one switch governs both features (REQ-RESOLVE-2).

## AI Validation

How the AI verifies completion, behaviorally. Each item names the requirements it verifies.

1. **Build check (REQ-RESOLVE-14):** `cmake -B build && cmake --build build` succeeds on macOS with no newly installed packages; `src/nlresolve.cpp`/`nlresolve.hpp` present and including no `parser.cpp` types; the hoisted `lookupNoun` lives in a shared header included by both `parser.cpp` and `nlresolve.cpp`.
2. **Contract grep (REQ-RESOLVE-5):** `grep -En "INSERT|UPDATE|DELETE" src/nlresolve.cpp` returns nothing.
3. **Prompt-content inspection (REQ-RESOLVE-12):** read the system-prompt constant in `src/nlresolve.cpp` and confirm it defines all seven verbs non-overlappingly and states each rule: one action only, subject must be a supplied in-scope noun, no new nouns, direction for `go`, no tool call on unknown/multi-intent, no pronoun resolution.
4. **Fallback run (REQ-RESOLVE-1, -2, -4):** with `ANTHROPIC_API_KEY` unset, run `./build/textworld` in a scratch dir; issue `look`, `take key`, `go north`, `inventory`, an unknown line ("smell the flowers"); input is handled by the fixed-verb parser identically to the pre-AI build, and the unknown line yields `renderError`. Also confirm `parser.cpp`'s header no longer says "DISPOSABLE" and states its permanent-fallback role (REQ-RESOLVE-4).
4b. **Tier-b passthrough (REQ-PROTO-6b via the boundary declaration):** with a fake transport that resolves `take <item>` for an item that exists but is not in the actor's room, confirm the turn ticks, emits a `failed` event, and neither retries nor re-calls the resolver.
5. **Kill-switch (REQ-RESOLVE-2):** with a key set but `TEXTWORLD_AI=0`, behavior matches check 4.
6. **Unit suite (REQ-RESOLVE-6, -7, -8, -11, -13, -15):** `./build/tests` passes, including new tests for: scope-context payload fields (exactly the REQ-RESOLVE-7 set); each REQ-RESOLVE-13 clause a–e and no-tool-call → no value; dispatch fallback to parser then `renderError`; request body containing the `emit_action` tool with the seven-verb enum, `claude-opus-4-8` (and `TEXTWORLD_MODEL` override), `max_tokens: 512`, no `thinking`/`stream`; db byte-identity across `aiResolve` with a fake transport.
7. **Timeout/failure behavior (REQ-RESOLVE-3, -10):** using the fake transport's timeout/malformed/throwing cases, confirm `aiResolve` returns no value, the line falls through to the parser within the timeout bound, no crash, nothing AI-flavored leaks.
8. **Live smoke (REQ-RESOLVE-16 — manual, optional):** with a real key and `TEXTWORLD_AI_LIVE_TEST=1`, resolve "pick up the lantern", "grab lantern", "head north"; observe each lowers to the expected verb/subject `Action` (or falls back cleanly), and a nonsense line ("smell the flowers") makes no tool call and falls through to `renderError`.
