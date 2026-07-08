---
title: "Simplify notes: ai-resolver"
date: 2026-07-08
status: complete
tags: [simplify, cleanup, nl-resolver, tests]
source: .lore/work/notes/ai-resolver.md
modules: [nlresolve, parser, loop]
---

# Simplify notes: ai-resolver

Behavior-preserving cleanup pass over the 6 files changed by the AI resolver
implementation. Run in the main thread (user's choice) rather than dispatched to
`code-simplifier:code-simplifier` — that agent is not available in this
environment, and no `.lore/lore-agents.md` registers a Code Quality substitute.

## Files reviewed

- `src/lookup.hpp` · `src/nlresolve.hpp` · `src/nlresolve.cpp` · `src/loop.cpp`
  · `src/parser.cpp` · `tests/tests.cpp`

## Outcome

**Production code (`nlresolve.{cpp,hpp}`, `lookup.hpp`, `loop.cpp`, `parser.cpp`):
reviewed, no changes.** It was written to mirror the shipped prose renderer's
structure and comment density, and is already at appropriate altitude:

- No dead code, no unused includes (early clangd unused-include flags on
  `<optional>` / `action.hpp` / `prose.hpp` all cleared once Steps 5–6 consumed them).
- The only duplication — the read-only SELECT lookups reimplemented from
  `prose.cpp` — is a deliberate, plan-documented "reimplement, don't reach across
  TUs" decision (micro-decision #2), not accidental complexity. Collapsing it would
  change the architecture, not simplify it, so it stays.
- Comments are dense but match the sibling `prose.cpp` / `prose.hpp` house style
  and encode requirement rationale; stripping them would make the resolver
  inconsistent with its sibling. Left as-is.

**Tests (`tests/tests.cpp`): one DRY extraction applied.** A "no emit_action tool
call" 200-response was hand-built inline in three places (`testNlResolveGate`,
`testNlResolveAiResolve`, `testNlResolveDispatch`). Extracted a `cannedNoToolUse()`
helper next to the existing `cannedToolUse`, collapsing the three sites. All three
cases resolve to zero `emit_action` blocks either way, so the assertions and their
outcomes are unchanged — a text-block body and an empty-array body are both 0-block
responses to the gate. Net ~10 fewer lines; clearer intent.

## Verification

- `cmake --build build` clean.
- `./build/tests` → **1656 checks, 0 failures** — identical count before and after,
  proving behavior preserved.
- Diff confined to `tests/tests.cpp` (+1 helper, 3 call sites collapsed). No
  production source touched. (`README.md` was already modified in the working tree
  before this session and is unrelated — left untouched.)
