---
title: "Implementation notes: mage-school seed world"
date: 2026-07-09
status: complete
tags: [implementation, notes, seed, setting, mage-school]
source: .lore/work/plans/mage-school-seed.md
modules: [seed, tests]
related: [.lore/work/specs/mage-school-seed.md, .lore/work/plans/mage-school-seed.md]
---

# Implementation notes: mage-school seed world

Orchestrated implementation of the approved plan. Per invocation constraints:
no lore-researcher, no re-research, no live-LLM checks; Step 1 gates all
content work; validation is Step 6's deterministic sweep only.

## Progress

- [x] Phase 1 — Test decoupling (tests/fixture.sql + ~34 redirects; gate: suite green, zero assertion edits)
- [x] Phase 2 — Rewrite seed/setting.txt (Thornmere Hall; add light-related proper noun)
- [x] Phase 3 — Rewrite seed/base.sql (id ledger 1–6, north/south pair, five descriptions)
- [x] Phase 4 — Structural test testShippedSeedShape()
- [x] Phase 5 — Doc touch-ups (README ×4 hunks, story-seed-architect spec parenthetical)
- [x] Phase 6 — Deterministic validation sweep (spec AI Validation items 1–7)

## Log

- 2026-07-09: Session start. No task files, no .lore/lore-agents.md. Build via
  `cmake --build build`, suite via `./build/tests` from repo root.
- **Process change (user directive)**: no sub-agent dispatch — all
  implementation, testing, and review done inline in the main thread to avoid
  each agent re-reading context. Saved to memory as `no-subagent-spawning`.
- **Phase 1**: `tests/fixture.sql` created via `cp` (verified `cmp`
  byte-identical to the pre-change seed). tests.cpp: exactly 34 −/+ pure
  path-string swaps `"seed/base.sql"` → `"tests/fixture.sql"` (a first
  dispatch attempt was interrupted mid-run after it had already applied these
  swaps; the diff was verified line-by-line instead of redone — every changed
  line is a path swap, zero assertion edits). Gate: build clean, 1837 checks /
  0 failures; only the architect live smoke kept `"seed/base.sql"`.
- **Phase 2**: `seed/setting.txt` replaced with the approved Thornmere Hall
  draft. Draft-time item resolved: added **"the Vigil Lamps"** (brass fixtures
  that kindle as you pass) as the recurring light-related proper noun, used
  twice. All REQ-MAGE-1 elements present.
- **Phase 3**: `seed/base.sql` rewritten on the same skeleton: ids 1–6
  (cell, corridor, player, candle, key, wand), exits (1,'north',2)/(2,'south',1),
  candle+wand in room 1, key in room 2, player in room 1 with no description,
  ledger comments kept. Note: the current seed file has **no meta tail** —
  `meta.turn`/schema rows are written by `openWorld`, not the seed; "identical
  in structure" was satisfied by keeping the file skeleton exactly. Suite
  re-run green (1837/0) after the swap, proving the decoupling worked.
- **Phase 4**: `testShippedSeedShape()` added after `testWorld()` and
  registered in main(); purely structural REQ-PROTO-2 checks (2 rooms,
  mutual-inverse exit pair from the invertible set, player in room 1 with no
  description row, ≥2 portables with one in each room, non-empty
  names/descriptions for rooms+portables, meta.turn=0, meta.setting
  non-empty). Suite: 1861 checks / 0 failures (24 new).
- **Phase 5**: README hunks ×4 (line ~19 lantern→candle, ~44 first-launch
  description, ~60 lantern→candle, ~86 shipped-setting paragraph) and
  story-seed-architect.md REQ-ARCH-1 parenthetical → "(the dormitory cell /
  corridor)". Prompt examples in src/prose.cpp / src/architect.cpp left
  untouched per plan.
- **Phase 6 sweep — all 7 items pass**:
  1. Build clean, 1861 checks / 0 failures.
  2. `grep '"seed/base.sql"' tests/tests.cpp` → exactly testShippedSeedShape
     (l.205) + architect live smoke (l.2138).
  3. testShippedSeedShape passes against the new seed (in suite run).
  4. Content checklist: all REQ-MAGE-1 elements grep-confirmed ("Nothing
     threatens" spans a line wrap — present); REQ-MAGE-2 ledger/exits/
     placement confirmed in base.sql.
  5. Playthrough (TEXTWORLD_AI=0, fresh world.db): cell prose on look, takes
     succeed, corridor prose + key after `go north`, inventory = candle, key,
     wand, `go south` returns to cell; relaunch resumes with same room and
     inventory. Local world.db was deleted per plan (worlds disposable).
  6. `grep -in 'priory\|stone hall\|lantern' README.md` → no matches.
  7. Human read-through of the five descriptions vs REQ-MAGE-3: standalone
     night-school tone, no people/NPC promises, wand inert — pass.
- Architect live smoke NOT run (user-triggered only, outside done-criteria).
- No divergences from the plan; no assertion was edited at any point.
