---
title: "Implementation notes: engine-foundation-prototype"
date: 2026-07-05
status: complete
tags: [implementation, notes, engine, prototype, sqlite, cpp]
source: .lore/work/plans/engine-foundation-prototype.md
modules: [engine]
related: [.lore/work/specs/engine-foundation-prototype.md, .lore/work/design/engine-foundation.md]
---

# Implementation notes: engine-foundation-prototype

Orchestrated implementation of the two-room engine foundation prototype. Phases = plan steps 1–10. Each phase: implement → test → review via sub-agents.

## Progress

- [x] Phase 1 — Build scaffold (REQ-PROTO-1)
- [x] Phase 2 — Database wrapper
- [x] Phase 3 — Schema, world open/create, seed (REQ-PROTO-2, 3, 4)
- [x] Phase 4 — Action struct + disposable parser (REQ-PROTO-6a, 7)
- [x] Phase 5 — Mutation helpers (REQ-PROTO-8)
- [x] Phase 6 — Systems / action resolution (REQ-PROTO-6b, 7)
- [x] Phase 7 — Renderer (REQ-PROTO-9)
- [x] Phase 8 — Turn loop / REPL (REQ-PROTO-5, 6c, 10, 11)
- [x] Phase 9 — Test completeness pass (REQ-PROTO-12)
- [x] Phase 10 — Final validation against spec

## Log

### 2026-07-05 — Session start

- Researcher: greenfield, no prior notes/retros/learned/issues. Upstream chain (spec → design → brainstorm → research → vision) consistent, current.
- Cautions carried in: design wins over brainstorm on schema conflicts (`location.container`); do not preempt design's deferred-revisit list (no stmt cache, no FK pragma, no doctest, no migrations); parser disposable behind `Action` seam.
- No agent registry (`.lore/lore-agents.md` absent) — all roles use `general-purpose`.

### Phase 1 — Build scaffold — COMPLETE

- SQLite amalgamation 3.53.3 downloaded from sqlite.org, vendored. Static lib, compiled as C, `SQLITE_OMIT_LOAD_EXTENSION`.
- Gate 1 verified from clean by independent test agent: build clean, both exes exit 0, `otool -L` shows only libc++/libSystem.
- Review: no findings.
- **Deviation (minor, within plan wording):** Ninja not installed on machine; default Unix Makefiles generator used. Plan said "Ninja generator assumed but not required by the file."

### Phase 2 — Database wrapper — COMPLETE

- `src/db.{hpp,cpp}` (~158 lines): Db + move-only Stmt, throw-on-error with errmsg. Added `twcore` static lib to CMake (not in plan's file list — packaging elaboration so both exes share db.cpp).
- Gate 2 verified independently: clean build, 14 checks pass; harness failure-detection proven via out-of-tree broken copy (exit 1).
- Review findings (both fixed, re-verified): (1) `colText` swallowed OOM as empty string — now throws on SQLITE_NOMEM; (2) `twcore PUBLIC sqlite3` leaked vendor includes — now PRIVATE, `textworld` links sqlite3 explicitly (direct API user in main stub).
- Implementer added rollback-discards-work test beyond gate letter — kept (API surface coverage).

### Phase 3 — Schema, world open/create, seed — COMPLETE

- `src/world.{hpp,cpp}` + `seed/base.sql`. DDL verbatim per design §3 (ten tables, no inventory, no FK pragma). `SCHEMA_VERSION = 1`. Mismatch → `SchemaMismatch` exception after stderr refusal; zero writes, byte-identity pinned by test.
- Create path: seed read before BEGIN; DDL+seed+meta in one transaction, rollback on failure — failed create leaves no `meta` table, next open rebuilds.
- Gate 3 verified independently incl. sqlite3 CLI probe of generated world (counts, exits, descriptions=4, turn=0). 43 checks pass.
- Review: no findings. C++20 applied to all targets; editor inline-variable warning is clangd noise (no compile_commands.json).
- **Decision (implementer, ratified):** meta present but schema_version row missing → treated as version −1 → refusal (fail-safe over re-seed).
- **Doc inconsistency noted (not a defect):** design §8 "canon descriptions for everything" vs §3/spec/seed (no player description). Spec wins; consider design errata later.
- Session limit interrupted both gate agents mid-run 2026-07-05 ~13:04; resumed after reset, no rework needed.

### Phase 4 — Action struct + disposable parser — COMPLETE

- `src/action.hpp` (Verb enum, Action struct, parse decl — parser-free seam) + `src/parser.cpp` (DISPOSABLE-marked). 61 checks pass.
- Noun recognition world-wide via `SELECT entity FROM name WHERE value = ? LIMIT 1` — single query instead of iterating rows; semantically identical, first match = SQLite row order. Parser contains zero writes/transactions.
- look/inventory/wait/quit ignore trailing arguments (documented simplification).
- Gate 4 verified independently; review: no findings.
- Tooling: `compile_commands.json` now exported + symlinked at repo root — kills recurring clangd false-error noise in editor.

### Phase 5 — Mutation helpers — COMPLETE

- `src/mutations.{hpp,cpp}`: appendEvent (turn from meta.turn inside ambient txn; NULL detail via unbound param — safe, no stmt cache) + moveEntity (one UPDATE + one event). No begin/commit inside helpers. Convention documented: appendEvent alone only for looked/waited/failed.
- Review finding (fixed): moveEntity didn't check UPDATE row count — entity without location row = silent no-op + false event. Now `Db::changes()` (sqlite3_changes64) added; moveEntity throws before appendEvent on 0 rows (tier-c surface). Test: moveEntity(999) throws, rollback leaves tables untouched.
- 100 checks pass. Grep confirms zero raw SQL writes outside mutations.cpp/world.cpp.

### Phase 6 — Systems / action resolution — COMPLETE

- `src/systems.{hpp,cpp}`: fixed dispatch, SELECT-only reads (roomOf/containerOf/exitDest/isPortable), all writes via helpers. Six event verbs only; inventory = `looked` + detail `'inventory'`. Distinct tier-b refusal texts for take (carried/non-portable/elsewhere).
- **Decision (implementer):** Quit reaching resolve throws `std::logic_error` (surfaces loop bug; rollback = no tick recorded).
- Gate 6 verified independently incl. scratch-program probe: go north/take key/go south/drop key → correct component state, events moved@1 took@2 moved@3 dropped@4, meta.turn=4.
- Review finding (fixed): cross-room drop unpinned by tests — added take-in-1/go-north/drop-in-2 scenario. 169 checks pass.
- Review nit (skipped, recorded): non-portable + elsewhere reports "You can't take that" before presence check — spec doesn't order tier-b refusals; revisit only if in-world info leak matters later.

### Phase 7 — Renderer — COMPLETE

- `src/render.{hpp,cpp}`: events WHERE turn ORDER BY id → dumb templates; SELECTs only (grep-verified, purity pinned byte-for-byte on world file, valid under DELETE journal). `renderError` sole non-event path (tier a).
- looked/NULL reads actor's current room at render time — conforms (design §6 permits read-only fleshing-out; event stores no room).
- Missing rows degrade gracefully: name → "something", missing description skipped, player-without-location throws (engine-error convention).
- Gate 7 pass incl. probe with rendered samples (garden prose + "Exits: south." + "You see: key."; wall bump renders detail verbatim). Review: no findings. 189 checks.
- Watch item for multi-actor phases: room block lists ALL portables in room — fine now (player not portable), revisit when NPCs exist.

### Phase 8 — Turn loop / REPL — COMPLETE

- `src/loop.{hpp,cpp}` (runTurn: TurnOutcome NoTick/Ticked/EngineError/Quit; shared TU so tests hit production path) + `src/main.cpp` (world.db in cwd, SchemaMismatch → exit 1, non-tick startup render, getline loop, EOF = quit). `renderRoomOf` exported from render for startup block (elaboration per plan).
- Gate 8 pass: spec validation script #5 through binary → meta.turn=8, events 1..8 complete, all verbs per REQ-PROTO-7; relaunch shows persisted garden, startup consumes no turn; tier-a probes through binary leave turn/events untouched; EOF exits 0.
- Review finding (fixed): `Db::rollback()` could throw in catch path when SQLite auto-rolled-back (FULL/IOERR/NOMEM) — process died instead of tier-c EngineError. Now checks `sqlite3_get_autocommit`, no-ops when no txn. Tier-c production-path test added (DELETE FROM player → runTurn wait → EngineError, turn/events unchanged, connection clean after). 230 checks.
- Review findings deferred to Phase 9 (its scope anyway): persistence-after-play reopen test, file-copy portability test.

### Phase 9 — Test completeness pass — COMPLETE

- Added testPersistence (close/reopen: turn, positions, transcript 1..N complete, playable to N+1) and testPortability (copy_file, divergent play on copy, original byte-identical, both playable, divergence asserted).
- Gate 9 break/restore verified twice (implementer + independent test agent, sha256-pinned restore).
- Review findings (all fixed): tier-b take-key and unknown-noun now driven through runTurn; wall bump pins events +1 exactly + item containers unchanged; fault injection moved to mutating verb (go north) with location assertions for entities 3/4/5 post-rollback.
- Final: 324 checks, 0 failures. No src changes needed — no bugs exposed by new tests.

### Phase 10 — Final validation — COMPLETE (2026-07-06)

- Spec's AI Validation items 1–11 executed end-to-end from clean build by independent agent. **12/12 requirements PASS.** No divergences beyond pre-accepted (Unix Makefiles; twcore lib + systems/render/loop split).
- Evidence highlights: version-gate tamper refused with byte-identical file; transcript gap check via recursive CTE (0 missing turns 1..11); render.cpp SELECT-only; file-copy divergence proven both directions; tests break/restore exit-code check with sha256-pinned restore.

## Final summary

Two-room engine foundation prototype fully implemented: 10 phases, all gates passed. C++20 + vendored SQLite 3.53.3, ~9 source files + seed + 324-check test suite. Event-sourced turn loop with three failure tiers enforced and tested through the production path. Five review findings fixed across phases (colText OOM, include leak, moveEntity silent no-op, rollback-throw tier-c bug, coverage gaps); two nits recorded as watch items (tier-b refusal ordering, room block vs future NPCs). Statuses updated: spec → implemented, plan → executed, notes → complete.
