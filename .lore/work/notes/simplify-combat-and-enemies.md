---
title: "Simplify notes: combat-and-enemies"
date: 2026-07-12
status: complete
tags: [simplify, cleanup, combat, mutations]
source: .lore/work/notes/combat-and-enemies.md
modules: [combat, mutations]
related: [.lore/work/plans/combat-and-enemies.md]
---

# Simplify notes: combat-and-enemies

Cleanup pass over the combat surface on branch `feat/combat-and-enemies`
(`src/combat.cpp`, `combat.hpp`, `mutations.cpp`, `architect.cpp`, `systems.cpp`,
`render.cpp`, and the combat tests). Run **inline** (no subagents, per standing
rules [[no-implement-subagents]] / [[verification-must-be-bounded]]). Behavior
preserved throughout — the full suite stayed at **2475 checks, 0 failures** after
each change, and `combat.cpp` + `architect.cpp` remained raw-write-free.

## Changes made

1. **`mintEntity(db)` helper (mutations.cpp).** The "mint one entity" pattern —
   `INSERT INTO entities DEFAULT VALUES` + read `last_insert_rowid()` + throw on
   failure — was duplicated verbatim in three helpers (`dropGrimoire`,
   `placeEnemy`, `writeGeneratedRoom`). Extracted to one anon-namespace helper;
   the three sites now read `const int64_t x = mintEntity(db);`. Net −~18 lines.
   The only behavioral delta is the throw's diagnostic string (now uniformly
   `"mintEntity: rowid read failed"`), which no test asserts.

2. **`knowsSpell(db, player, spell)` helper (combat.cpp).** The exact-spell
   `known_spells` lookup was duplicated in `resolveRead` (the already-known /
   re-read test, REQ-COMBAT-21) and `castDenialReason` (the learned gate,
   REQ-COMBAT-7). Extracted to one anon-namespace helper; both sites now call it.
   Identical behavior (same `s.step()` result).

## Deliberately left alone

- **Cross-TU read-helper duplication** (e.g. `currentTurn`/`playerEntity` also
  living in prose/render/architect). The codebase explicitly accepts this — the
  small duplication is the stated cost of keeping each AI TU self-contained; not a
  simplification target.
- **`render.cpp`'s combat-verb if/else chain.** Idiomatic here — it mirrors the
  base verbs, and each branch has distinct template text + subject/object reading.
  A table-driven rewrite would restructure without reducing real complexity.
- **Merging `knowsSpellWithEffect` / `knowsSpellOfElement`.** They differ only in
  the catalog column (`effect` vs `element`); merging would interpolate a column
  name into SQL — a clarity/safety regression for ~6 saved lines. Both are clear,
  self-documenting, single-purpose.

## Verification

- `cmake --build build` clean after each change.
- `./build/tests` → 2475 checks, 0 failures (unchanged from pre-simplify).
- `grep -En "INSERT|UPDATE|DELETE" src/combat.cpp src/architect.cpp` → empty (the
  write discipline held; the extracted `mintEntity` lives in mutations.cpp).

The combat code was already written in tight, single-purpose helpers across the
23-step implementation, so the pass found only these two verbatim duplications —
both real, both safe. No test or review failures; no escalation.
