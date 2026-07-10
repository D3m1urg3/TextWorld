---
title: "Implementation notes: blocked-directions-exit-display"
date: 2026-07-10
status: complete
tags: [implementation, notes, exits, latent-exits, architect, render, mutations, seed]
source: .lore/work/plans/blocked-directions-exit-display.md
modules: [world-gen, architect, systems, render, mutations, world]
---

# Implementation notes: blocked-directions-exit-display

Orchestrated build of the "exits declared at birth" feature: latent (NULL-dest)
exit rows, architect-co-authored onward exits, truthful exit display. Source plan
has 8 steps; spec has 11 `EXITS` requirements.

## Orchestration decisions (deviations from the raw skill script, token-disciplined)

- **Skipped the cold `lore-researcher` dispatch.** The plan+spec were read in full;
  their `related:` frontmatter already enumerates all prior work (brainstorm,
  research/procedural-room-feel, story-seed-architect spec, ai-resolver plan). A cold
  researcher would re-derive context already in hand — pure token waste, against the
  standing [[verification-must-be-bounded]] / token-risk discipline.
- **Coarse phase batching.** Steps 1–6 all touch an overlapping file set
  (`architect.hpp/.cpp`, `mutations.cpp`, `systems.cpp`, `render.cpp`, `tests.cpp`),
  so parallel per-step agents would collide. Batched into sequential dispatches that
  respect the 2→3→4 hard chain, each honoring its own step's validation gate and
  running `cmake --build build` + `./build/tests` before reporting.
- Fallback agents used throughout (no `.lore/lore-agents.md`): `general-purpose` for
  implement/test/review roles.

## Progress tracker

- [x] Step 1 — create_room `exits` field + prompt flip (REQ-EXITS-5, -6)
- [x] Step 2 — RoomProposal.exits + gate sanitization (REQ-EXITS-7, -1)
- [x] Step 3 — writeGeneratedRoom realize-upsert + latent stubs (REQ-EXITS-8)
- [x] Step 4 — resolveGo three-case + three-state read (REQ-EXITS-2, -3, -10)
- [x] Step 5 — render filter + architectEnabled() (REQ-EXITS-4)
- [x] Step 6 — seed frontier + world.db refresh (REQ-EXITS-9)
- [x] Step 7 — gated live smoke code (REQ-EXITS-11)
- [x] Step 8 — final validation sweep vs spec AI-Validation 1–9

## Log

- Init: no task dir, no agent registry, no prior notes. `world.db` present + gitignored.
  Build dir present. Baseline assumed green (verified by first dispatch).
- **Steps 1+2 (dispatch 1):** DONE, build ok, `./build/tests` = 1873 checks / 0 failures,
  no network. architect.cpp (schema + prompt flip + normalizeDirection + lenient exit
  sanitization), architect.hpp (RoomProposal.exits, validateRoomProposal new sig +
  contract comments), tests.cpp (cannedCreateRoom exits arg; testArchitectRequestBody,
  testArchitectPrompt, testArchitectGate extended).
  - **Approved divergence:** the plan's Step-2 gate example (travel `"west"` →
    `{north,east,up}`) is internally inconsistent — `inverse("west")=="east"` would drop
    `east`. Agent kept the normalization demonstration by travelling `"in"` (inverse
    `"out"`, not in set) so capitalized `"East"`/spaced `" up "` all survive, and left the
    return-direction drop fully exercised by the `"north"`-travel sub-case (drops `south`).
    Behavior matches REQ-EXITS-7 exactly; only the gate's illustrative literal changed.
- **Steps 3+4 (dispatch 2):** DONE, build ok, `./build/tests` = 1904 checks / 0 failures,
  no network, no divergence. mutations.cpp writeGeneratedRoom (origin realize = upsert
  `ON CONFLICT(room,direction) DO UPDATE`, stderr diagnostic + log-and-proceed on absent
  latent row, latent-stub planting loop). systems.cpp (exitDest → `enum ExitState` +
  SQL `SELECT dest, dest IS NULL` three-state read, no db.hpp change; resolveGo rewritten
  to ordered a/b/c, free-generation + inverse guard removed). tests.cpp: testWriteGeneratedRoom,
  testResolveGoGenerate, testGeneratedEventInvisible rewritten; testArchitectGenerate left
  unmodified and still green (absent-row log-and-proceed branch). Both gates passed.
- **Steps 5+6 (dispatch 3):** DONE, build ok, `./build/tests` = 1912 checks / 0 failures,
  no network, no divergence. architect.hpp/.cpp: new `architectEnabled()` display gate
  (returns aiNarrationEnabled() today, documented as separable). render.cpp: `#include
  "architect.hpp"`, exits query `WHERE room=? AND (dest IS NOT NULL OR ?)` binding
  `architectEnabled()?1:0`; latent renders identical to realized. seed/base.sql: 2 latent
  frontier rows on corridor (room 2) — `north` (stair/landing) + `up` (floor above);
  realized cell↔corridor pair unchanged. Local world.db deleted (rebuilds from seed),
  stays untracked. tests.cpp: new testExitDisplayInvariant; testShippedSeedShape now
  COUNT==4 / 2 latent / realized-join stays 2. Both gates passed.
- **Step 7 (inline, no subagent):** DONE. Extended testArchitectLiveSmoke (still gated
  behind TEXTWORLD_AI_LIVE_TEST=1, registered first in main). Added MECHANICAL-only
  assertions: declared exits land as latent (dest-NULL) rows; anti-degeneration
  (`roomsWithOnward > 0` across the chain); displayed==walkable failure-tolerant — parses
  the rendered Exits line, asserts shown iff a row exists (architect enabled), and walks
  every UNSHOWN invertible direction to prove it walls with no move and no new row (free,
  no network — resolveGo case c returns before any architect call). Shown directions are
  deliberately NOT walked (the 3-room chain already exercises shown-latent→generates; a
  shown row that walls transiently keeping its row is conforming per REQ-EXITS-3). Added
  `#include <algorithm>`. NEVER run live. Default `./build/tests` = 1912 checks / 0
  failures, live smoke skipped (offline). No judge/LLM calls added.
- **Step 8 (inline validation sweep) — ALL 9 spec AI-Validation items PASS:**
  1. Build ok; `SCHEMA_VERSION == 1` (src/world.hpp:12); no ALTER/ADD COLUMN in src/;
     db.hpp Stmt API frozen (colInt/colText only — micro-decision #1 honored).
  2. Three-case movement — testResolveGoGenerate: realized→move/0 calls; latent+AI-on→
     generate/realize/move + persistence; undeclared no-row→wall/0 calls/no row created;
     latent+AI-off→wall/row survives; forced Phase-1 fail ×2→wall/row retryable.
  3. Display invariant — testExitDisplayInvariant: AI-on lists both, AI-off only realized,
     identical wording (no marker).
  4. Tool+prompt — testArchitectRequestBody (3 props, optional string-array exits,
     required==[name,description]) + testArchitectPrompt (requires declaring onward exits,
     excludes return, requires prose to describe).
  5. Gate leniency — testArchitectGate: junk/non-string/northeast/dup/return-dir dropped,
     room still created; blank name/description still fatal.
  6. Write helper — testWriteGeneratedRoom: origin realized via upsert (single row),
     return realized, declared exits latent NULL, one generated event, absent-row sub-case
     log-and-proceed.
  7. Seed frontier — testShippedSeedShape: exits COUNT==4, 2 latent, realized join==2.
  8. Regression + disabled parity — full suite green (1912/0); wall text byte-identical
     "You can't go that way."; latent hidden with key unset.
  9. Live smoke — code in place, gated, mechanical-only; run manually/rarely.

## Summary

Feature complete: room exits are now "declared at birth" — latent (NULL-dest) rows are
open directions that generate on walk, non-NULL rows move, absent rows are walls. The
architect co-authors onward `exits`; the engine owns invariants (return exit, direction
legality, dedup, lenient sanitization). Truthful `Exits:` display corrects itself off the
same table via a filter gated by the new `architectEnabled()` predicate.

- **8 plan steps** implemented across 6 source files + tests + seed.
- **Files:** src/architect.{hpp,cpp}, src/mutations.cpp, src/systems.cpp, src/render.cpp,
  seed/base.sql, tests/tests.cpp. Local world.db deleted (rebuilds from seed, stays
  untracked).
- **Tests:** `./build/tests` = 1912 checks / 0 failures, no network. Live smoke gated.
- **Constraints honored:** no SCHEMA_VERSION bump, no DDL/column change, no db.hpp API
  change, world.db not committed.
- **Divergences:** one (Step-2 gate example literal — see log above); behavior matches
  REQ-EXITS-7 exactly. No other divergences.
- **Orchestration note:** mid-run the user directed "stop spawning agents — token sink"
  ([[no-implement-subagents]]); Steps 7–8 done inline. Steps 1–6 were done via 3
  sequential batched subagent dispatches before that.
