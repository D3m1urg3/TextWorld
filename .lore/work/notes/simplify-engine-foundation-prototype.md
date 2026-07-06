---
title: "Simplify pass: engine-foundation-prototype"
date: 2026-07-06
status: complete
tags: [simplify, cleanup, engine, prototype]
source: .lore/work/notes/engine-foundation-prototype.md
modules: [engine]
related: [.lore/work/plans/engine-foundation-prototype.md]
---

# Simplify pass: engine-foundation-prototype

Behavior-preserving cleanup of the freshly implemented prototype. Scope: all project source (src/, tests/, CMakeLists.txt); vendor/ and seed/ excluded. No git repo, so scope = entire implemented slice. `code-simplifier:code-simplifier` agent unavailable this session — `general-purpose` used for all three roles.

## Changes

- **src/db.cpp** — removed unused `#include <utility>` (no std::move/exchange anywhere in src/; move ops do manual steal-and-null).
- **src/render.cpp** — extracted `portableNamesIn(db, holder)`; identical 7-line portable/location/name JOIN was duplicated in `roomBlock` and `inventoryBlock` (same query works for rooms and actors since carried items have `location.container = actor`). Still SELECT-only.
- **tests/tests.cpp** — `TempDbFile` RAII fixture (remove on construction = fresh-world guarantee; remove on destruction = cleanup) replaced repeated setup/teardown in all nine test functions, ~40 lines net removed. Every CHECK assertion byte-identical.

## Untouched (already clean or deliberately left)

db.hpp, world.{hpp,cpp}, action.hpp, parser.cpp, mutations.{hpp,cpp}, systems.{hpp,cpp}, render.hpp, loop.{hpp,cpp}, main.cpp, CMakeLists.txt.

Notable non-change: `roomOf` (systems/render) and `currentTurn` (mutations/loop) look duplicated but sit on opposite sides of the documented read/write module boundaries — merging would blur the seams. Left as-is.

## Verification

- Clean build from deleted build/, zero warnings; `./build/tests` exit 0, 324 checks (unchanged from pre-cleanup baseline).
- Independent test agent: scripted 7-tick session behaves identically (room blocks, take/drop, inventory, wait, wall bump); meta.turn = 7; no temp-db residue in $TMPDIR.
- Independent review: no findings. Fixture lifecycle audited for the tricky cases (persistence close/reopen spans one fixture; portability copy never deleted prematurely). Protected conventions intact: DISPOSABLE marker, write discipline, render SELECT-only, DDL byte-identical to design §3.

No failures, no escalations, single cleanup round.
