---
title: "Implementation notes: ai-resolver"
date: 2026-07-08
status: complete
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
- [x] Step 6 — `aiResolve` orchestration + production transport (M · low)
  - [x] 6a orchestrator + tests · [x] 6b libcurl transport
- [x] Step 7 — Loop dispatch + parser promotion (S · low)
- [x] Step 8 — Tier-b passthrough test (S · low)
- [x] Step 9 — Live end-to-end smoke, gated (S code · HIGH token-risk) — written, NOT run live
- [x] Step 10 — Final validation against spec checklist (S · low)

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

### Step 6 — aiResolve orchestration + production transport (done, split 6a/6b)
- **6a** `aiResolve(db, line, transport)`: context → body → ONE transport call →
  `validateAndLower`, whole body in try/catch → nullopt + one stderr line on any
  failure (REQ-RESOLVE-3), transport at most once (REQ-RESOLVE-10). Mirrors
  `aiRender`. Enable check NOT here — like `aiRender`, it's at the dispatch point
  (Step 7's loop gate), faithful mirror. `testNlResolveAiResolve` with fake
  transports: canned→Action + call-count==1, transportError/malformed/**throwing**
  →nullopt, no-tool-call→clean nullopt, world-file byte-identity (REQ-RESOLVE-5).
  1632 checks; read-only grep clean.
- **6b** `curlTransport` (anon-ns private) + production `aiResolve(db, line)` binding
  it. Deliberately reimplements prose's transport (only the `HttpTransport` type is
  shared, micro-decision #2); same URL / x-api-key+version+content-type headers /
  **8 s** timeout, no retries, key only in header. No direct unit test — exercised
  only by Step 9's gated live smoke, so isolated in its own commit for review. Test
  count unchanged (1632); twcore already links CURL::libcurl.

### Step 7 — Loop dispatch + parser promotion (done)
- `resolveOrParse(db, line[, transport])` in `nlresolve`: aiResolve → parse (the
  permanent fallback); first Action wins, else nullopt. Production overload binds
  curlTransport; injected-transport overload makes the chain unit-testable offline.
- `runTurn` (`loop.cpp`): `aiNarrationEnabled() ? resolveOrParse(db,line) : parse(db,line)`
  — disabled mode never constructs a transport. Order aiResolve→parse→renderError.
  `#include "nlresolve.hpp"`. Everything downstream (Quit-before-tick, tick txn,
  resolve, narration dispatch) untouched.
- `parser.cpp` header rewritten: no longer "DISPOSABLE / deleted without ceremony" —
  now the permanent deterministic fallback, input-side analog of render.cpp's
  template renderer (REQ-RESOLVE-4). `parse()` behavior unchanged.
- `testNlResolveDispatch`: fake no-tool-call transport → resolver declines →
  parser yields Take (subject 4); "smell the flowers" → both decline → nullopt;
  fake resolving transport → resolver's Go wins ("head north" isn't a fixed verb).
  Existing `testLoop` passes unchanged (no key → disabled → parser path identical).
- Gate: build clean; `./build/tests` → 1639 checks, 0 failures; `grep DISPOSABLE
  src/parser.cpp` empty.

### Step 8 — Tier-b passthrough test (done)
- `testNlResolveTierBPassthrough`: `take key` where key(5) exists world-wide but is
  in the garden (2), not the player's room (1). aiResolve (fake transport) →
  valid `Take` subject 5, calls==1 (clause c recognition only). Driving `*action`
  through `tick()` → turn +1, one new `failed` event with detail
  "You don't see that here.", calls still 1 (resolve never calls the resolver),
  key unmoved (still container 2). Drives aiResolve + resolve directly, not runTurn.
- Gate: build clean; `./build/tests` → 1656 checks, 0 failures.

### Step 9 — Live end-to-end smoke, gated (written, deliberately NOT run live)
- `testNlResolveLiveSmoke`, structured exactly like `testProseLiveSmoke`: no-op
  unless `TEXTWORLD_AI_LIVE_TEST=1`; reads env without mutating; called FIRST in
  `main()` (right after `testProseLiveSmoke`, before the hermetic unset).
- Drives real phrasings through the PRODUCTION 2-arg `aiResolve` (libcurl):
  "pick up the lantern" / "grab lantern" → IF resolved, Take/subject 4 (else clean
  nullopt); "head north" → IF resolved, Go with non-empty direction; nonsense
  "smell the flowers" through `runTurn` → NoTick (renderError). **Mechanical asserts
  only** — conditional on resolution, never on model wording, never "must resolve".
  This is not a prompt-tune loop.
- **Per the [[verification-must-be-bounded]] constraint I did NOT execute it live.**
  Default `./build/tests` skips it (test count unchanged 1656; zero "RESOLVER LIVE
  SMOKE" lines → no network). Run rarely, manually, under the gate.
- Gate: default run skips it, no network access.

### Step 10 — Final validation against spec checklist (done)
Walked the spec's AI Validation items 1–8; every one passes:
1. **Build (REQ-RESOLVE-14):** `cmake --build build` clean, no new packages (only
   the pre-existing `find_package(CURL)`). `nlresolve.{cpp,hpp}` present; `lookup.hpp`
   included by both `parser.cpp` and `nlresolve.cpp`; nlresolve free of parser.cpp
   types (the only `parse()` use is `resolveOrParse` calling the `action.hpp` seam).
2. **Contract grep (REQ-RESOLVE-5):** `grep -En "INSERT|UPDATE|DELETE" src/nlresolve.cpp`
   empty.
3. **Prompt (REQ-RESOLVE-12):** grep confirms all seven verb definitions + every rule
   (one action, verbatim in-scope noun, no new nouns, compass direction, no tool call,
   no pronoun, recognition-only).
4/4b. **Fallback + tier-b run:** `./build/textworld` with key unset → "AI narration
   off — template mode"; `look`/`take key`/`go north`/`inventory` handled by the
   parser; `take key` (out of room) → "You don't see that here."; `smell the flowers`
   → "I don't understand that." parser.cpp header no longer says "DISPOSABLE".
5. **Kill-switch (REQ-RESOLVE-2):** key set + `TEXTWORLD_AI=0` → byte-identical to
   item 4, no network.
6. **Unit suite (REQ-RESOLVE-6/7/8/11/13/15):** `./build/tests` → 1656 checks, 0
   failures, all six `testNlResolve*` green.
7. **Timeout/failure (REQ-RESOLVE-3/10):** fake transportError/malformed/throwing →
   nullopt, no crash (testNlResolveAiResolve).
8. **Live smoke (REQ-RESOLVE-16):** written + gated (Step 9); run manually/rarely.

## Summary

Built the AI action resolver in 10 plan steps / 11 commits (Step 6 split 6a/6b),
all on `main`, mirroring the shipped prose renderer's seams. Deterministic skeleton
(Steps 1–8) fully unit-tested with no network (+201 checks, 1455 → 1656); the single
live-LLM step (9) is isolated, gated behind `TEXTWORLD_AI_LIVE_TEST=1`, mechanical-
only, and was deliberately NOT executed. Resolver path is read-only (no writes, no
ids on the wire), reuses `aiNarrationEnabled()` as the one shared switch, and defers
all applicability to the engine. No divergences from the plan required user
authorization; two small in-step implementation choices logged (Step 3 extern prompt
accessor; Step 5 clean no-diagnostic 0-block path).

Suggested next: run `/simplify` on the changed files (src/nlresolve.{cpp,hpp},
src/lookup.hpp, src/loop.cpp, src/parser.cpp, tests/tests.cpp) to clean up for clarity.
