---
title: "Implementation notes: npc-memory-store"
date: 2026-08-07
status: complete
tags: [implementation, notes, npc, memory, identity-profile, schema, mutations, major-npc, caps]
source: .lore/work/plans/npc-memory-store.md
modules: [mutations, world, bard, main]
related: [.lore/work/specs/npc-memory-store.md, .lore/work/design/npc-memory-store.md]
---

# Implementation notes: npc-memory-store

Source of truth: [the plan](../plans/npc-memory-store.md) (12 steps) over
[the spec](../specs/npc-memory-store.md) (38 requirements, 40 validation items).

No task files exist under `.lore/work/tasks/npc-memory-store/`, so the plan's
steps are the phases directly.

## Progress

- [x] 1 — schema, `SCHEMA_VERSION` 6 → 7, verb comments, table-list repair
- [x] 2 — `writeCatalogProfile`
- [x] 3 — `writeNpcMemory`
- [x] 4 — `npcProfile` / `npcMemory`
- [x] 5 — `npcLinesSince`
- [x] 6 — `kind = 'major'`
- [x] 7 — explicit kind list for the room generator
- [x] 8a — `parseMajorProfile`
- [x] 8b — the loader in `initialize()`
- [x] 8c — the six loud failures
- [x] 9 — `main.cpp` reads `seed/majors/`
- [x] 10 — `buildOvertureContext` gains the cast
- [x] 11 — source-text invariants as tests
- [x] 12 — final validation against the spec

Final state: clean full rebuild, **8708 checks, 0 failures**, with no network and
no `ANTHROPIC_API_KEY`. No CMake change, no new translation unit.

## Log

Session opened. Plan and spec read in full; the tree's seams verified against
the plan's table before starting.

### Divergence 1 — the bump broke a second assertion, not one

The plan's guiding constraints say `SCHEMA_VERSION` 6 → 7 "breaks exactly one
existing assertion: the verbatim table list in `testBandResistance`." It breaks
**two**. `testBardStoreVersionGate` (`tests.cpp:10218`) also pins the literal
`6` — it opens a fresh world, asserts `schema_version == 6`, then writes `5` and
checks the refusal.

Repaired by pinning `SCHEMA_VERSION` instead of the literal, so the next brick
that bumps the schema does not have to edit a bard test a third time. The
literal `5` in the refusal half stays a literal deliberately: it means "some
version that is not this one," which is what that half is about.

This is a real edit to an existing bard test body, so spec gate 36 ("every
existing bard test passes unmodified") is now true of every bard test **except
this one**. Recorded rather than hidden — the alternative was leaving the suite
red.

### Steps 1–7

All green, 8400 checks, 0 failures. Notes worth keeping:

- The repo-root dev `world.db` was deleted after Step 1, as the plan instructs.
  It was at version 6 and the game now refuses it.
- Step 5's probe row works as micro-decision 4 predicted. Both the orphan case
  (`kLineCap + 1` rows, leading `spoke` dropped, `kLineCap - 1` returned) and
  the legitimate case (a leading `spoke` whose `said` predates `summary_turn`,
  kept) are asserted, and they differ only by whether the probe row came back.
- Step 6 needed a local `npcProposal` helper in the tests: `bardProposal` is
  defined at `tests.cpp:11874`, well after the NPC block, and hoisting it would
  have reordered a section that is not this brick's.
- Step 7's `catalogRows` now builds its `IN (?,?,…)` from the kind list rather
  than concatenating one query per kind, which is what preserves `ORDER BY c.id`
  across the pair. `testBardSelEligibleNewRoom`'s ordered assertion passes
  unmodified, as the plan required.

### Divergence 2 — a second existing test pinned a literal the brick changed

`testBardOvertureContract` (`tests.cpp:14653`) is a source-ORDER test, and it
found the open call by the exact string `openWorld("world.db")`. Adding the
fourth `majors` parameter broke it.

Repaired by dropping the closing paren from the needle — `openWorld("world.db"`.
The test's guarantee is the ORDER of `logInit` → `AiHttpGuard` → `openWorld` →
`bardOverture` → the two worker guards, and that is untouched. The argument list
was never what it was asserting.

So the full list of existing test bodies this brick edited is four, not the
plan's two: `testBandResistance` (the table list, authorized), `testBardSelContext`
(additive, authorized), and the two above.

### Step 8a — one parser bug the tests caught

The first version looped `while (pos <= text.size())`, which read the phantom
empty line after a file's trailing newline as the header/body separator. A file
with a header and no blank line at all therefore parsed as "header, separator,
empty body" instead of failing for what it was. Fixed to `pos < text.size()`;
the "no blank line" case in `testNpcStoreProfileParse` is what found it.

### Extension beyond the spec — a duplicate header key is a failure

REQ-NPCSTORE-31 lists five malformed cases and does not name a repeated key
(`handle:` twice in one file). The parser refuses it anyway, for REQ-NPCSTORE-31a's
stated reason: an unrecognised key is a failure because nothing in a
hand-authored seed file should quietly do nothing, and under last-wins the FIRST
`handle:` is exactly what quietly does nothing. Refusing it is one branch per
key and no new concept.

### Step 11 — the mutation check, run and reverted

Spec check 33, all three by hand, each confirmed to turn the suite red and then
reverted:

| Mutation | Result |
|---|---|
| `UPDATE catalog_profile SET profile = ?` helper added to `mutations.cpp` | red at `tests.cpp:11594` |
| `INSERT INTO npc_memory` added to `src/systems.cpp` | red at `tests.cpp:11608` |
| `'said'` added to the wake predicate in `bard.cpp` | red at `tests.cpp:11626`, `:11627` |

Tree restored green after each. A guard that has never been seen to fail is not
a guard.

### Step 12 — the spec's gates, checked

1. Clean full rebuild (`--clean-first`), whole suite green, **no network and no
   `ANTHROPIC_API_KEY`**. No regressions in combat, architect, pregen, band, or
   bard.
2. `grep -En "UPDATE catalog_profile" src/*.cpp` — empty.
3. `grep -En "INSERT|UPDATE|DELETE" src/*.cpp | grep -E "catalog_profile|npc_memory"`
   — two lines, both `src/mutations.cpp`.
4. Diff property: four existing test bodies touched (two authorized, two
   recorded above). The four catalog-selection tests named by spec check 20 have
   **no diff hunks at all**.
5. Both "build nothing" requirements hold and are suite-asserted, not eyeballed:
   no conversation-line table (carried by the verbatim table list) and no speech
   verb in the wake predicate (carried by `testNpcStoreInvariants`).
6. A version-6 world file produces `SchemaMismatch` and writes nothing.
7. All nine `testNpcStore*` are registered in `main()` beside the `testBardStore*`
   entries, not appended at the end.
8. No CMake change, no new translation unit, no network include added to any
   source.

### Also worth knowing

- The game was launched against a freshly created `world.db`, played several
  turns, and quit cleanly. The new world is at `schema_version` 7 with zero
  major rows and zero profile rows, which is the shipped configuration
  (micro-decision 2 — no profile files ship, and `seed/majors/` is not created).
- `world.cpp` now includes `mutations.hpp`. The loader writes majors through
  `writeCatalogEntry` / `writeCatalogProfile` rather than raw SQL, which is what
  lets REQ-NPCSTORE-37's guard cover `world.cpp` with no exception carved out
  for it.
- The consequence to keep in view: the loader's non-empty path is exercised only
  by tests until a cast is written. The first authored major is content work and
  the loader's first real end-to-end run.
