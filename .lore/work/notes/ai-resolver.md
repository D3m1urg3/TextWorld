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
- [ ] Step 2 — New TU + scope-context builder (M · low)
- [ ] Step 3 — ISA system prompt (S · low)
- [ ] Step 4 — Request body + `emit_action` tool schema (M · low)
- [ ] Step 5 — Validation & mapping gate (M · low)
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
