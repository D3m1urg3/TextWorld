---
title: "Engine foundation prototype implementation"
date: 2026-07-06
status: open
tags: [engine, prototype, cpp, sqlite, turn-loop, event-sourcing, orchestration, subagents, implement-skill, cmake, clangd, session-limit]
modules: [engine]
related: [.lore/work/plans/engine-foundation-prototype.md, .lore/work/specs/engine-foundation-prototype.md, .lore/work/notes/engine-foundation-prototype.md, .lore/work/notes/simplify-engine-foundation-prototype.md]
---

# Engine foundation prototype implementation

Two-session run (2026-07-05 and 2026-07-06) implementing the two-room engine prototype from `.lore/work/plans/engine-foundation-prototype.md`, followed by a simplify pass. Fully orchestrated: every implementation, test, and review action went through a sub-agent; the orchestrator only dispatched, routed findings, and kept notes. All 12 requirements passed final validation; spec marked `implemented`, plan `executed`.

## Plan versus reality

The plan held up almost verbatim. All ten steps executed in order, no reordering, no step skipped or split. Divergences, all minor and recorded in the notes file:

- Ninja not installed on the machine; Unix Makefiles used. The plan had pre-hedged this ("assumed but not required").
- The plan's own sanctioned elaborations (twcore static lib, systems.hpp/render.hpp/loop.{hpp,cpp}) were the only additions to the design's file list.
- One implementer judgment call ratified without escalation: a world file with a `meta` table but no `schema_version` row is treated as version −1 and refused rather than re-seeded.
- SQLite amalgamation download (the plan's flagged network-dependent moment) worked first try — 3.53.3 vendored.

Nothing in the plan turned out infeasible. No divergence rose to the escalation threshold; the user was never blocked on a decision.

## What the review cycle caught

Each phase ran implement → gate-test → review, with test and review agents dispatched in parallel. Five findings led to fixes:

1. Phase 2: `colText` returned empty string for both SQL NULL and OOM — OOM now throws.
2. Phase 2: `twcore PUBLIC sqlite3` leaked vendor include dirs to consumers, defeating db.hpp's forward declarations — made PRIVATE.
3. Phase 5: `moveEntity` on an entity with no location row was a silent no-op UPDATE that still appended an event — a false transcript entry. Both the gate-test agent and the review agent flagged it independently. Fixed with `Db::changes()` + throw before the event append.
4. Phase 8: `Db::rollback()` inside the catch path could itself throw when SQLite had auto-rolled-back (FULL/IOERR/NOMEM), killing the process instead of returning the tier-c EngineError. Fixed with an `sqlite3_get_autocommit` guard in the wrapper.
5. Phase 9: four test-coverage gaps (tier-b and unknown-noun legs not driven through `runTurn`, wall-bump and fault-injection assertions incomplete).

Phase 9's new persistence/portability tests exposed zero source bugs — the code passed on first run. Two review nits were recorded and deliberately not fixed: tier-b refusal message ordering leaks a property of an absent object, and the room block lists all portables (matters once NPCs exist).

## Trip-ups

- **Session limit mid-phase.** Both Phase 3 gate agents were killed mid-run by "You've hit your session limit · resets 3:50pm". After the reset, `SendMessage` resumed both from transcript with a short "here's where you were" note — no rework, both finished normally. A permission-classifier outage ("claude-opus-4-8[1m] is temporarily unavailable") also blocked one Bash call inside an agent during the same window.
- **clangd noise all session.** Every phase produced editor diagnostics ('db.hpp' file not found, unknown type Db, no member optional) that looked like build breaks but were clangd falling back to defaults with no compile_commands.json. Real builds were clean throughout. Fixed at Phase 4 by exporting compile_commands and symlinking it at repo root; residual noise afterward came from scratch probe files in the scratchpad and stale indexing.
- **Doc inconsistency surfaced by a gate test:** design §8 says "canon descriptions for everything" while §3, the spec, and the seed deliberately omit a player description. Spec treated as authoritative; design errata not yet written.

## Context worth keeping

- The tick-transaction shape lives in `runTurn` (loop.cpp): begin → single `UPDATE meta SET value=value+1` → resolve → commit; render reads committed state after. Tests exercise this production path directly, including tier-c fault injection via `DELETE FROM player`.
- The three failure tiers map to concrete test hooks: tier a = parse nullopt (no transaction), tier b = `failed` event (tick stands), tier c = throw → rollback (world as if prompt never happened).
- `roomOf`/`currentTurn` near-duplicates across modules are intentional — they sit on opposite sides of the read/write seam (render SELECT-only vs mutations sole write path). The simplify pass evaluated and kept them.
- Repo still not under git. Simplify-pass review had no baseline to diff against; behavior preservation was argued from the tests' exact-string pins instead.
- Project-name collision with Microsoft TextWorld remains open; binary/target names are still cheap to change.
