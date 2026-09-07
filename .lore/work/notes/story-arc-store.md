---
title: "Implementation notes: story-arc-store"
date: 2026-09-07
status: complete
tags: [implementation, notes, bard, story-arc, advance-rule, schema, golden-session]
source: .lore/work/plans/story-arc-store.md
modules: [world, mutations, systems, loop, bard, prose, seed]
related: [.lore/work/specs/story-arc-store.md]
---

# Implementation notes: story-arc-store

Brick 1 of the story arc: the arc rows, the ordered `story_step` list, the closed
condition vocabulary, and the rule that walks it. No model call, no network, no
new translation unit.

Running the plan's twelve steps inline rather than through per-phase
implement/test/review subagents — the user's standing instruction for
`/implement` in this project.

## Progress

- [x] 1 — Golden-session baseline (must land on an otherwise untouched tree)
- [x] 2 — `SCHEMA_VERSION` 7 → 8, `story_step` + `condition_catalog`
- [x] 3 — Seed the condition vocabulary (both seed files)
- [x] 4 — The arc: three `meta` rows + `writeArc`
- [x] 5 — `writeStoryStep` and the admission gate
- [x] 6 — Seed the five story steps
- [x] 7 — `stepConditionMet`
- [x] 8 — `advanceStoryStep`
- [x] 9 — `evaluateStoryAdvance`
- [x] 10 — `advanced` renderer-invisible + the bard's fifth wake verb
- [x] 11 — The call site in `loop.cpp`
- [x] 12 — Final validation sweep against the spec's 18 items

## Log

**Initialize.** Prior work searched inline rather than via `lore-researcher`
(codebase-exploring agents stay inline here). The direct predecessors are
`.lore/work/specs/bard-fact-store.md` (the shape this brick copies: closed
vocabulary as a table, validation at admission, latch as a `WHERE` clause) and
`.lore/work/notes/bard-fact-store.md`. No task files exist under
`.lore/work/tasks/story-arc-store/`, so the plan's twelve steps are the phases.

Baseline build green before any change.

**Steps 1-6 complete.** Suite green at each gate; the golden literal has not
moved since it was captured.

- **Step 1.** `kStoryGoldenSession` + `testStoryGoldenSession`, captured with
  `TW_DUMP_GOLDEN=1` on a tree with no other part of the brick in it. The
  transcript reaches both offline-reachable events — the goblin falls, fire is
  learned — so the later advance gate is not vacuous.
- **Step 2.** `SCHEMA_VERSION` 7 → 8; `story_step` and `condition_catalog` in
  `SCHEMA_DDL`; `'advanced'` added to the `events.verb` comment.
  **Two pre-existing assertions had to move with the bump**, both direct
  consequences rather than divergences: the verbatim table list in
  `testResistanceDiscovery` (`tests.cpp:10184`) gained the two new tables, and
  `testNpcStoreSchema`'s `schema_version == 7` became `== SCHEMA_VERSION` — the
  form `testBardStoreVersionGate` already argues for in its own comment, having
  been edited by exactly this kind of bump once before.
- **Step 3.** The four condition kinds seeded into `seed/base.sql` **and**
  `tests/combat_fixture.sql` (plan micro-decision 1). Without the fixture copy
  every admission test would reject every kind.
- **Step 4.** Three `arc_*` `meta` rows in the seed; `writeArc` as three
  `upsertMeta` calls. Event-free, free rewrite.
- **Step 5.** `writeStoryStep` with all five refusals. **One choice not in the
  plan:** the `int` check is a whole-string digit test rather than a copy of
  `parseMajorProfile`'s `stoll` form. `stoll` would admit `" 2"` and `"+2"`,
  neither of which is a non-negative decimal integer, and the argument arrives
  from the model over the wire in brick 2. Same refusals for every case the
  spec names.
- **Step 6.** Five steps seeded in `seed/base.sql` only, using all four kinds.
  The fixture stays empty on purpose, so the whole existing suite exercises
  REQ-ARC-STORE-19a for free.

- **Step 7.** `stepConditionMet` in `systems.cpp`. **Signature note:** it takes
  `player` as a fourth argument, which the spec's `stepConditionMet(db, kind,
  arg)` shorthand omits — two of the four conditions need the player entity, and
  `evaluateStoryAdvance(db, actor)` already has it. The no-clock check is
  function-scoped source text, not a file-wide grep, because `advanceStoryStep`
  legitimately reads `meta.turn`.
  *One test bug found and fixed here:* the `rooms_built` case first grew its
  room off the cell's `north` exit, which REPLACED the exit to the corridor and
  detached the graph, so every `reached_depth` assertion in that test was
  passing on an unreachable room. It now grows off the outer hall (15), into a
  direction that room has no exit for.
- **Step 8.** `advanceStoryStep`. **Chose `RETURNING` over `changes()` + a
  second read** — the plan allowed either. Nothing else in the tree uses
  `RETURNING` (SQLite 3.53 is vendored, so it is available), but it makes
  REQ-ARC-STORE-11a true by construction: the row the `UPDATE` latches is the
  row handed back, with no reasoning about whether reached steps always form a
  prefix. The `changes()` + `ORDER BY n DESC` form would have been correct only
  as long as nothing ever latched a step out of order.
- **Step 9.** `evaluateStoryAdvance`, three lines, no loop.
- **Step 10.** Both `buildFacts` exclusion sites (plan micro-decision 2), the
  fifth wake verb, and the two source-text pins at `tests.cpp:11952` and
  `:13220` updated together.
- **Step 11.** One line in `loop.cpp`. **`testStoryGoldenSession` passed
  byte-identically the moment the rule went live**, and the run now genuinely
  advances two steps — asserted, so the byte-identity cannot be satisfied by a
  rule that never fires. Two of the Step 10 tests had to be reordered: they
  wrote a step and then ran a `wait` turn, which the live rule now latches, so
  the step is written after the turn and latched by hand instead.
- **Step 12.** Clean rebuild from an empty `build/`; `./build/tests` green at
  **9604 checks, 0 failures**; `TW_DUMP_GOLDEN=1` reproduces the pasted literal
  byte for byte. `git diff --stat` touches exactly the eleven files the plan
  named plus `tests/tests.cpp` — no new file in `src/`, no `CMakeLists.txt`
  change.

## Divergences

None from the spec. Three choices the spec left open, recorded above: the
`player` argument on `stepConditionMet`, `RETURNING` in `advanceStoryStep`, and
the strict all-digits integer check in `writeStoryStep`.

## Known gap, carried from the plan

**Validation item 15 is checked in a weaker form than it asks for**, exactly as
plan micro-decision 3 says. It asks that a wake be *queued*; these tests do not
queue one. `hasTriggeringEvent` is file-local in `bard.cpp`, and reaching it for
real needs `bardEnabled()`, `aiNarrationEnabled()` and a live transport — the
price `tests/tests.cpp:11932-11937` already declined to pay for the same
guarantee. What is checked: the five-verb predicate as source text (which also
proves the other four survived, REQ-ARC-STORE-23), and behaviourally that an
`advanced` row lands at a turn strictly greater than `meta.bard_last_wake_turn`,
which is the exact row that predicate's query selects. The unchecked link is
that the query then runs and a wake goes out.

## Not built (out of scope, per the spec's section I)

The overture writing the arc and the steps; the architect, narrator and bard
consumers; narrowing the wake trigger to `advanced` alone; ending detection.
**`meta.arc_ending` is written by `writeArc`, seeded in `base.sql`, and read by
nothing** — intentional, not a gap.

## Validation coverage

All 18 of the spec's AI-Validation items pass, in the tests the plan's Step 12
table maps them to: `testStoryStoreSchema`, `testStoryStoreVersionGate`,
`testStoryStoreConditionCatalog`, `testStoryStoreArc`, `testStoryStoreWrite`,
`testStoryStoreSeededSteps`, `testStoryConditions`, `testStoryAdvance`,
`testStoryEvaluate`, `testStoryRendererInvisible`, `testStoryWakeTrigger`,
`testStoryAdvanceRule`, `testStoryEmptyStepList`, `testStoryOneCallSite`,
`testStoryGoldenSession`.

## Simplify pass

Four review angles (reuse, simplification, efficiency, altitude) over the diff.
Five findings applied, six skipped. Suite green after each: **9604 checks, 0
failures** on a clean rebuild.

**Applied**

- `knowsSpell` moved out of `combat.cpp`'s anonymous namespace and declared in
  `combat.hpp`; `stepConditionMet`'s `spell_learned` branch calls it instead of
  repeating its query. This is the same export `distanceFromSeed` already got,
  for the same reason — one reader, so combat and the story cannot disagree
  about what the player knows. Adds `src/combat.{cpp,hpp}` to the diff.
- `prose.cpp`: the renderer-invisible verb list is now one
  `kRendererInvisibleVerbs` constant interpolated into both `buildFacts`
  queries, instead of the same literal typed at two sites. The list already grew
  from one verb to two in this brick and a later brick will revisit it; editing
  one site and missing the other is the exact half-fix that would leak an
  advance's prose into the narrator's context for six turns.
- `stepConditionMet`: `enemies_defeated` and `rooms_built` are two plain
  branches over a shared lambda, so the kind is no longer compared twice.
- `advanceStoryStep`: the `int rows` counter became a `bool latched`. The loop
  stays — SQLite does not promise a RETURNING statement abandoned before
  `SQLITE_DONE` has applied all its changes, and that statement's change *is*
  the latch — but the comment now says that instead of implying the count
  matters.
- `tests.cpp`: one file-scoped `advancedCount(Db&)` replaces three local copies
  that had drifted into two different lambda shapes.

**Skipped, with reasons**

- *Gate the `reached_depth` BFS on the player having moved.* Real waste, but the
  fix adds a movement special case to a spec'd rule, and REQ-ARC-STORE-16a
  already bounds this to one BFS per turn deliberately. Micro-optimization
  against a turn that may make an HTTP call to a model.
- *Hoist the `db.prepare` out of `distanceFromSeed`'s BFS loop.* Legitimate, but
  pre-existing code outside this diff.
- *Index `events(verb)`.* Both counting conditions full-scan `events`. Fixing it
  is DDL, so another `SCHEMA_VERSION` bump — well outside brick 1. **Worth
  doing when the events log gets long enough to matter; recorded here rather
  than done.**
- *Bind `currentTurn(db)` instead of the inline `(SELECT value FROM meta …)`
  subquery in `advanceStoryStep`.* REQ-ARC-STORE-11 writes that statement
  verbatim, and the tree already carries the same turn-read SQL in `loop.cpp`,
  `mutations.cpp` and `world.cpp` — a fourth copy is the existing convention,
  not a new problem.
- *Use `rowExists` for the duplicate-`n` check.* `rowExists` is string-typed and
  `story_step.n` is `INTEGER PRIMARY KEY`; it would work through SQLite's type
  affinity, but the explicit five-line integer bind is plainer than a number
  passed as a string.
- *Drop the duplicate-`n` pre-check and catch the constraint violation instead.*
  REQ-ARC-STORE-10 frames the gate as every check running before any write, and
  exception-based control flow is not simpler than the check it replaces.
