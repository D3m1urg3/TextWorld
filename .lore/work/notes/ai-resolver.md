---
title: "Implementation notes: ai-resolver"
date: 2026-07-08
status: in_progress
tags: [implementation, notes, ai-integration, nl-resolver, claude-api, tool-use]
source: .lore/work/plans/ai-resolver.md
modules: [parser, action, loop, nlresolve]
related: [.lore/work/specs/ai-resolver.md, .lore/work/notes/ai-prose-renderer.md]
---

# Implementation notes: ai-resolver

Lowers a raw input line into the engine's closed `Action` ISA via Claude tool-use,
with the fixed-verb parser as the permanent deterministic fallback. Mirrors the
shipped prose renderer (`src/prose.{cpp,hpp}`, `tests/tests.cpp`).

## Execution protocol (user-directed, overrides skill default)

The `/implement` skill defaults to fanning work out to sub-agents. The invoking
user explicitly directed the main thread to read the seams itself, work one step
at a time, one commit per step, build + `./build/tests` green after each, and
**check in after every commit** rather than run unattended. Work is therefore
driven in the main thread with tight per-step gates. No `lore-researcher` dispatch:
plan is approved and already cross-references all prior work; seams read directly.

Hard constraints held throughout:
- Deterministic skeleton first; Steps 1–8 fully unit-tested, NO network.
- Step 9 (live smoke) is the only high-token-risk step: gated behind
  `TEXTWORLD_AI_LIVE_TEST=1`, mechanical asserts only. Never a prompt-tune loop.
- Fixture shape pinned from Step 5's documented structure — no live probe.
- Resolver path read-only: no INSERT/UPDATE/DELETE in `nlresolve.cpp`, no ids on wire.

## Progress tracker

- [x] Step 1 — Hoist `lookupNoun` to `src/lookup.hpp` (S · low)
- [x] Step 2 — New TU + scope-context builder (M · low)
- [x] Step 3 — ISA system prompt (S · low)
- [x] Step 4 — Request body + `emit_action` tool schema (M · low)
- [x] Step 5 — Validation & mapping gate (M · low)
- [ ] Step 6 — `aiResolve` orchestration + production transport (M · low)
- [ ] Step 7 — Loop dispatch + parser promotion (S · low)
- [ ] Step 8 — Tier-b passthrough test (S · low)
- [ ] Step 9 — Live end-to-end smoke, gated (S code · HIGH token-risk)
- [ ] Step 10 — Final validation against spec checklist (S · low)

## Log

### Step 1 — Hoist `lookupNoun` (done)
- Created `src/lookup.hpp`: header-only `inline int64_t lookupNoun(Db&, const std::string&)`,
  exact body from `parser.cpp:34-39` (micro-decision #1, recommended path — no CMake
  change, no ODR concern since `inline`). Header comment states the recognition-not-
  applicability contract so the resolver gate reuses the same first-match rule.
- `parser.cpp`: `#include "lookup.hpp"`, removed the anon-namespace copy. `parse()`
  behavior unchanged.
- Gate: `cmake --build build` clean; `./build/tests` → 1455 checks, 0 failures;
  `testParser` (incl. `take lantern`→4, `take zeppelin`→nullopt) passes unchanged.
  No new test needed (behavior-preserving hoist).

### Step 2 — TU + scope-context builder (done)
- Created `src/nlresolve.{hpp,cpp}`; added `src/nlresolve.cpp` to twcore in
  `CMakeLists.txt:17`. Header includes `action.hpp` + `prose.hpp` (permanent TU
  contract; clangd flags them + `<optional>` unused for now — consumed in Steps 5–6),
  no parser types. Header comment states the read-only + network-egress contract.
- `buildResolveContext(Db&, line)` → `ResolveContext{payload}`. Payload JSON keys,
  **exactly 5**: `input` (raw line verbatim), `room` (name), `exits`, `items`,
  `inventory`. Resolves player itself via `SELECT entity FROM player LIMIT 1`; slices
  room/items/inventory relative to it. SELECT shapes copied from prose (`portableNamesIn`
  join char-identical). No ids in payload — room/actor ids used only to look up names.
- `testNlResolveContext`: asserts the exact 5-key set + values on fresh seed
  (`take the lantern` → stone hall / north / lantern / empty), then after
  take+go-north (garden / south / key / lantern-in-hand); reuses `checkPayloadHygiene`
  for the no-ids sweep; byte-identity purity check. Registered in `main()`.
- Gate: build clean; `./build/tests` → 1552 checks, 0 failures;
  `grep -En "INSERT|UPDATE|DELETE" src/nlresolve.cpp` empty (read-only ✓).

### Step 3 — ISA system prompt (done)
- Added `kResolveSystemPrompt` (raw-string constant) to `nlresolve.cpp` at file
  scope. Defines all seven verbs non-overlappingly and states every lowering rule:
  exactly one action / single `emit_action` call, subject ∈ supplied nouns copied
  verbatim, introduce no new noun, `direction` = movement/compass word for `go`,
  no tool call on unknown/multi-intent, no pronoun resolution, recognition-not-
  applicability boundary.
- **Divergence from prose (logged):** prose keeps its prompt private and tests it
  through `buildRequestBody`. That path (resolver's `buildResolveRequestBody`) is
  Step 4, so to give Step 3 its own gate I exposed the constant via
  `extern const char* const kResolveSystemPrompt` in `nlresolve.hpp`. Step 4 will
  embed this same constant in the request body's `system` field. Minor seam, not a
  behavioral change.
- `testNlResolvePrompt`: substring spot-check — seven verb names, `emit_action`,
  "exactly one action", "copied verbatim", "Introduce no noun", "compass word",
  "make no tool call", "pronoun", "recognition only". STRUCTURE only; quality → Step 9.
- Gate: build clean; `./build/tests` → 1569 checks, 0 failures.

### Step 4 — Request body + emit_action tool schema (done)
- Consulted the `claude-api` skill to pin the tool-use request shape (no live call,
  per constraint). Confirmed: tool defs use `input_schema` (type/properties/required);
  the verb enum lives at `input_schema.properties.verb.enum`; `tool_choice` is the
  **object** form `{"type":"auto"}`, not a bare string.
- `buildResolveRequestBody(contextPayload)` in `nlresolve.cpp`, mirroring
  `buildRequestBody`. Differences per spec: `max_tokens` **512**; one `emit_action`
  tool with a schema-enforced 7-verb enum + optional `subject`/`direction`, only
  `verb` required; `tool_choice` auto; `system` = `kResolveSystemPrompt`. Same model
  default + `TEXTWORLD_MODEL` override, no thinking/stream/cache keys. Top-level keys:
  exactly 6 (model, max_tokens, system, messages, tools, tool_choice).
- `testNlResolveRequestBody` (uses `ScopedModelEnv`): parses back and asserts the
  7-value enum, `tool_choice.type==auto`, `max_tokens==512`, model default + override
  + empty→default, and the exact 6-key top-level set (stray-key guard, mirroring
  prose's `j.size()==4`).
- Gate: build clean; `./build/tests` → 1592 checks, 0 failures.

### Step 5 — Validation & mapping gate (done)
- `validateAndLower(const HttpResponse&, Db&) → optional<Action>` in `nlresolve.cpp`,
  the analog of `validateAiResponse`. Never throws (exception-free `json::parse`,
  called outside try/catch in tests). Clauses:
  - **a** HTTP 200 + exactly one `emit_action` tool_use block. Navigates `content[]`
    counting `type=="tool_use" && name=="emit_action"`. **Design decision:** 0 blocks
    → nullopt WITHOUT a diagnostic (the model correctly declined — the REQ-RESOLVE-3
    no-action path; noisy "rejected" logs would mislead). ≥2 → clause-a failure +
    diagnostic. Matches the plan's "no tool call → nullopt, cleanly".
  - **b** verb ∈ seven ISA verbs (via `verbFromWord`).
  - **c** take/drop: subject present + `lookupNoun` (hoisted, Step 1) → non-zero id;
    id assigned mechanically, never from the model. Recognition, not applicability.
  - **d** go: direction present + non-empty.
  - **e** look/inventory/wait/quit: no argument consulted (stray args ignored).
- `failClause` helper mirrors prose's ("aiResolve: response rejected, clause X…").
- Test helper `cannedToolUse(verb, subject="", direction="")` emits the pinned
  fixture shape (`stop_reason:"tool_use"`, one emit_action block; subject/direction
  only when non-empty) — built from documented structure, NEVER a network probe.
- `testNlResolveGate` (seeded db, lantern=4/key=5): each clause a–e + no-tool-call,
  two-tool-call, out-of-set verb, unknown subject, non-200, malformed body, transport
  error → correct Action/nullopt.
- Included `lookup.hpp` in `nlresolve.cpp`; `<optional>` now used (clangd warning cleared).
- Gate: build clean; `./build/tests` → 1622 checks, 0 failures.
