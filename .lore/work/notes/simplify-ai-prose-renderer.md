---
title: "Simplify notes: prose.cpp + tests.cpp"
date: 2026-07-07
status: complete
tags: [simplify, cleanup, ai-integration, prose-renderer, tests]
source: .lore/work/notes/ai-prose-renderer.md
modules: [prose]
related: [.lore/work/specs/ai-prose-renderer.md]
---

# Simplify notes: prose.cpp + tests.cpp

`/simplify src/prose.cpp tests/tests.cpp` after the AI prose renderer implementation. Behavior preserved; cleanup only.

## Outcome

| File | Result |
|---|---|
| `src/prose.cpp` | No change — already clean. |
| `tests/tests.cpp` | One dead tautological assertion removed. |

## What changed

- **tests.cpp, testProseAiRender clause-c refusal block:** removed `const std::string shown = out ? *out : render(db,1); CHECK(shown == render(db,1));`. `!out.has_value()` was already asserted, so the ternary was dead and the CHECK was `render()==render()` — a tautology. Kept the real `CHECK(!out.has_value())` and left a truthful comment pointing at the AI-off runTurn byte-identity test, which actually covers the fallback==template property. This was the pre-existing nit flagged during step-7 review (the step-7 timeout block had already been cleaned the same way).

- **prose.cpp: no edits.** Simplifier and reviewer both confirmed it needed none — no dead code, no restating comments; the comments present are load-bearing contract docs (SELECT-only / network-egress banner, never-throw json parse rationale, character-identical append format). Not forcing a cosmetic change here was the right call given the never-throw and character-identical-append constraints.

## Verification

- Cleanup: code-simplifier (general-purpose).
- Test agent: build clean; `env -u ANTHROPIC_API_KEY ./build/tests` exit 0, **1455 checks** (one below 1456 — the removed tautology); hermetic with dummy key; mutation check bit and was hand-restored (git checkout avoided so the uncommitted simplify edit survived); SELECT-only grep empty.
- Review agent: clean — removal is genuinely dead code, no other assertion weakened, comment truthful, no lost coverage (fallback identity covered by the AI-off runTurn test asserting `output == render(db,5)`).

## Commit

`e12f125` — `refactor: drop tautological fallback assertion in refusal test` (tests/tests.cpp only).
