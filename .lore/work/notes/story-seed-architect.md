---
title: "Implementation notes: story-seed + architect"
date: 2026-07-09
status: complete
tags: [implementation, notes, ai-integration, story-seed, world-generation, architect, claude-api, tool-use, mutations]
source: .lore/work/plans/story-seed-architect.md
modules: [world-gen, architect, systems, mutations, world, prose, render]
related: [.lore/work/specs/story-seed-architect.md, .lore/work/design/story-seed-architect.md, .lore/work/notes/ai-resolver.md]
---

# Implementation notes: story-seed + architect

Loads a hand-authored **setting** into canon at world init, then generates one new
room — coherent with the setting and the room being left — when the player walks an
**unmapped** exit, writing it to canon via Claude tool-use with the existing wall
(`"You can't go that way."`) as the permanent deterministic fallback. The **first
read-write AI unit**. Mirrors the shipped AI resolver (`src/nlresolve.cpp`,
`src/prose.hpp`, `tests/tests.cpp`).

## Execution protocol (user-directed, overrides skill default)

The `/implement` skill defaults to fanning work out to sub-agents. Per the established
protocol for this repo (see [[ai-resolver]] notes), the main thread reads the seams
itself and drives one step at a time with per-step build + `./build/tests` gates. This
invocation asks for autonomous execution of the 11 approved steps in order, stopping
only if the plan conflicts with what's found in the tree. No `lore-researcher` dispatch:
plan is approved and cross-references all prior work; seams read directly.

Hard constraints held throughout ([[token-risk-estimation]], [[verification-must-be-bounded]]):
- Deterministic skeleton first; Steps 1–9 + 11 fully unit-tested, NO network.
- Step 10 (live smoke) is the ONLY high-token-risk step: gated behind
  `TEXTWORLD_AI_LIVE_TEST=1`, mechanical asserts only, ONE bounded coherence judge call
  — never a prompt-tune loop.
- Fixture shape (`cannedCreateRoom`) pinned from Step 5's documented structure — no live probe.
- **Write discipline:** `architect.cpp` stays grep-clean of INSERT/UPDATE/DELETE; the
  only write is `writeGeneratedRoom` in `mutations.cpp` (REQ-ARCH-6/-9).
- **Two-phase catch boundary:** Phase 1 (network+validate) caught → wall; Phase 2 (the
  write) is OUTSIDE the catch so a DB fault propagates to runTurn's tick rollback
  (REQ-ARCH-4/-5).

## Progress tracker

- [x] Step 1 — Setting seed load (S · low)
- [x] Step 2 — New architect TU + context builder (M · low)
- [x] Step 3 — Architect system prompt (S · low)
- [x] Step 4 — Request body + `create_room` tool schema (M · low)
- [x] Step 5 — Validation gate + direction-invertibility table (M · low)
- [x] Step 6 — `writeGeneratedRoom` mutation helper (M · low)
- [x] Step 7 — `architectGenerate` orchestration + two-phase catch + transport (M · low)
- [x] Step 8 — The `resolveGo` seam (S · low)
- [x] Step 9 — `generated`-event invisibility (S · low)
- [x] Step 10 — Live end-to-end smoke + bounded coherence judge, gated (S code · HIGH) — written, NOT run live
- [x] Step 11 — Final validation against spec checklist (S · low)

## Log

Baseline before changes: `./build/tests` green at 1656 checks, 0 failures.

### Step 1 — Setting seed load (done)
- `world.hpp`/`world.cpp`: `openWorld` + `initialize` gain a `settingPath` param
  (default `"seed/setting.txt"`). New tolerant reader `readFileOrEmpty(path)` (returns
  `""` on open failure) — distinct from `readFile`, which still throws for the seed.
- `initialize` INSERTs `meta('setting', ?)` inside the existing transaction — written
  even when empty so the key is present. Zero DDL, no SCHEMA_VERSION bump.
- Committed a real `seed/setting.txt` (Elwyn Priory ruin) coherent with the base hall/garden.
- Test `testArchitectSettingLoad`: present-file → text in meta.setting, version unchanged;
  absent scratch path → init succeeds, meta.setting empty. Committed seed non-empty.
- Green at 1669 checks.

### Step 2 — Architect TU + context builder (done)
- New `src/architect.{hpp,cpp}`; added to twcore in `CMakeLists.txt` next to nlresolve.
- Header states the read-write-but-confined contract (no raw SQL in this TU; sole write
  is `writeGeneratedRoom` in mutations.cpp). Declares `RoomProposal{name, description}`.
- `buildArchitectContext(db, room, direction)` → JSON payload with EXACTLY 4 fields
  (setting, origin_name, origin_description, direction), no ids. Local reimplemented
  SELECTs (settingText/nameOf/canonProseOf), mirroring nlresolve's "reimplement, don't
  reach across TUs" stance.
- Test `testArchitectContext` + recursive `checkNoIdKeys` helper: exact key set, values
  from canon, no id-looking keys anywhere; empty-setting → thinner well-formed payload.
- Green at 1687 checks.

### Step 3 — Architect system prompt (done)
- `kArchitectPrompt` constant (git-versioned) declared `extern` in header, defined in TU:
  one room, coherent with setting + origin, via create_room name+description, forbids
  exits/directions, arrival narration, and ids.
- Test `testArchitectPrompt`: substring spot-check of each required phrase/prohibition.
- Green at 1697 checks.

### Step 4 — Request body + create_room tool schema (done)
- `buildArchitectRequestBody(payload)`: one `create_room` tool, input schema object with
  REQUIRED name+description strings and no other props; `tool_choice` = require the tool
  (`{"type":"tool","name":"create_room"}`); max_tokens 1024; model default +
  TEXTWORLD_MODEL override; system=kArchitectPrompt; one user message.
- Test `testArchitectRequestBody`: exact top-level key set == 6 (no thinking/stream/cache),
  tool schema props.size()==2, required==[name,description], model override cases.
- Green at 1720 checks.

### Step 5 — Validation gate + invertibility table (done)
- `inverseDirection(dir)` → optional (n↔s, e↔w, u↔d, in↔out; else nullopt).
- `validateRoomProposal(resp)` → optional<RoomProposal>, never throws, one stderr line per
  first failed clause (a: 200 + exactly one create_room block; b: name non-blank; c:
  description non-blank). Stray input keys (spurious id) ignored — only name+description
  reach the two-field struct.
- Test fixture `cannedCreateRoom(name, description)` (documented tool-use shape, no probe).
- Tests `testArchitectGate` (all clauses + 0/≥2-block + ids-not-from-model) and
  `testArchitectInvertible`. Green at 1746 checks.

### Step 6 — writeGeneratedRoom (done)
- `mutations.{hpp,cpp}`: forward-declared `struct RoomProposal;` in the header (full def
  pulled via `#include "architect.hpp"` in the .cpp) so mutations.hpp doesn't drag
  prose/architect into every includer. Returns the minted room id.
- Mints via `INSERT INTO entities DEFAULT VALUES` + `SELECT last_insert_rowid()` (first
  runtime mint); writes room/name/description rows (NO location row), both reciprocal
  exits, and the `generated` event (subject=new, object=origin, detail=direction).
  Defensive throw if direction non-invertible (caller guarantees it isn't).
- Test `testWriteGeneratedRoom` (east from hall). Green at 1770. Write-boundary grep clean.

### Step 7 — architectGenerate + two-phase catch + transport (done)
- Phase 1 (context→body→ONE transport call→validate) wholly in try/catch (+ catch-all) →
  false on any failure. Phase 2 (`writeGeneratedRoom`) OUTSIDE the catch. Production
  overload binds a local `curlTransport` (8 s, no retry — copy-adjacent from nlresolve).
- Test `testArchitectGenerate`: success creates room+exits+event, calls==1; every Phase-1
  failure (transportError/malformed/throwing/no-tool-call) → false with NO orphan row
  (table counts unchanged). Green at 1799. Write-boundary grep still clean.

### Step 8 — resolveGo seam (done)
- Threaded a test-injectable `HttpTransport` through `resolve` (new overload) → an internal
  `resolveImpl(…, const HttpTransport*)` → `resolveGo(…, transport)`. Production path
  passes nullptr (architectGenerate binds curl). 3-way branch in order: (a) exitDest→move;
  (b) enabled && invertible && architectGenerate→move through new exit; (c) wall.
- `systems.hpp` now includes `prose.hpp` for the HttpTransport type.
- Test `testResolveGoGenerate` via a `tickT` helper + dummy key: (a) generate+move on east;
  (b) persistence — west then east re-crosses via branch a, calls stay 1; (c) non-invertible
  northeast → wall, no call; (d) key unset → wall, no call. Green at 1827.

### Step 9 — generated-event invisibility (done)
- `prose.cpp`: added `AND verb <> 'generated'` to BOTH payload SELECTs (current-turn
  `WHERE turn = ?` at ~:321 and recent-events `WHERE turn < ?` at ~:381). Left
  `deterministicAppends`'s third `WHERE turn = ?` untouched (branches on moved/looked only).
- `render.cpp`: replaced the stale "fixed six" comment — template already emits nothing for
  unrecognized verbs like 'generated'.
- Test `testGeneratedEventInvisible`: generation turn's `events` key excludes 'generated'
  but keeps 'moved'; next turn's `recent_events` excludes it; render shows the moved room
  block. Green at 1837.

### Step 10 — Live smoke + bounded coherence judge (done — code only, NOT run live)
- `testArchitectLiveSmoke`: no-op unless `TEXTWORLD_AI_LIVE_TEST=1`; skips loudly if no
  key. Registered in `main()` FIRST, alongside the other live smokes, BEFORE the hermetic
  `ANTHROPIC_API_KEY` unset (the load-bearing ordering gotcha).
- Drives a 3-room chain (east, north, east) through the PRODUCTION tick, asserting
  MECHANICAL invariants only (minted id > 5, non-empty name+desc, both reciprocal exits,
  a real move). A clean fallback stops the chain rather than failing.
- ONE bounded coherence-judge call via a test-local curl transport (`liveCoherenceJudge`):
  setting + descriptions → one plain Messages call, "coherent? yes/no + one line", printed
  to stderr for a human to read. No loop. `tests` target now links `CURL::libcurl`.
- Verified: default offline run no-ops it (1837 checks); `TEXTWORLD_AI_LIVE_TEST=1` with no
  key prints the SKIPPED line for all three smokes and still passes. NOT run against a real
  key (bounded-verification discipline — that live run is the user's to trigger).

### Step 11 — Final validation vs spec checklist (done)
Walked all 8 AI-Validation items:
1. Build clean; `src/architect.{cpp,hpp}` + `seed/setting.txt` present; no new package
   (CURL was already the sole `find_package`). architect reuses prose.hpp's transport seam.
2. `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` → empty; the only write is
   `writeGeneratedRoom` (mutations.cpp).
3. Setting load: present/absent scratch cases, SCHEMA_VERSION unchanged, real setting.txt
   committed (`testArchitectSettingLoad`).
4. Prompt: one room, coherent w/ setting+origin, create_room name+desc, forbids
   exits/ids/arrival (`testArchitectPrompt`).
5. Disabled-mode wall: **drove the actual binary** with the key unset — `go east` →
   byte-identical `"You can't go that way."`, `go north` still moves. Existing tests
   unchanged.
6. Unit suite green: 1837 checks, 0 failures, incl. every `testArchitect*` and
   generated-event invisibility against BOTH payload keys.
7. Failure behavior: fake timeout/malformed/throwing → false, wall, no crash, no orphan
   (`testArchitectGenerate`).
8. Live smoke + bounded judge: code present + gated (manual/optional; not run).

## Summary

All 11 steps executed in order, each with its validation gate met before proceeding. The
architect is the engine's first read-WRITE AI unit, and the two load-bearing new
disciplines held throughout:
- **Write discipline (REQ-ARCH-6/-9):** `architect.cpp` is grep-clean of raw SQL; the sole
  write is `writeGeneratedRoom` in `mutations.cpp` (which also performs the first runtime
  entity mint).
- **Two-phase catch boundary (REQ-ARCH-4/-5):** Phase 1 (network+validate) fully caught →
  wall; Phase 2 (the write) OUTSIDE the catch → a DB fault propagates to runTurn's tick
  rollback, never downgraded to a wall.

Files touched: `src/world.{hpp,cpp}`, `seed/setting.txt` (new), `src/architect.{hpp,cpp}`
(new), `src/mutations.{hpp,cpp}`, `src/systems.{hpp,cpp}`, `src/prose.cpp`, `src/render.cpp`,
`CMakeLists.txt`, `tests/tests.cpp`. Net test growth: 1656 → 1837 checks, 0 failures.

**Divergences from the plan:** none material. Two mechanical choices worth noting:
- `writeGeneratedRoom` returns the minted room id (plan left the return type open) — handy
  for tests and callers; harmless.
- The Step-10 coherence judge uses a **test-local** curl transport (the `tests` target now
  links `CURL::libcurl`) rather than exposing the architect's private `curlTransport` —
  keeps production surface unchanged and matches the codebase's per-unit-transport stance.

Next: `Run /simplify on the changed files to clean up for clarity.` The one live step
(Step 10) can be exercised on demand with `TEXTWORLD_AI_LIVE_TEST=1` and a real key.
