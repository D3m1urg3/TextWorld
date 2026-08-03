---
title: "Implementation notes: bard-fact-store"
date: 2026-08-03
status: complete
tags: [implementation, notes, bard, dungeon-master, schema, catalog, mutations, append-only, motive, truth-gate, utf8]
source: .lore/work/plans/bard-fact-store.md
modules: [world, mutations, term, prose]
related: [.lore/work/specs/bard-fact-store.md, .lore/work/design/bard-fact-store.md, .lore/work/brainstorm/dungeon-master.md]
---

# Implementation notes: bard-fact-store

Brick 1 of the bard feature, executed inline rather than via implement subagents
([[no-implement-subagents]]) — the full plan, spec, and design were already in
context, so dispatching would have paid to re-derive them.

Gate per step: `cmake --build build` clean + `./build/tests` green + that step's
named test. Zero live-LLM verification anywhere in this brick, as the plan
promised — every gate is SQL, source text, or the compiled binary.

## Progress

**Schema and seed**

- [x] Step 1 — `catalog` + `motive_catalog` DDL, `SCHEMA_VERSION` 5 → 6, three
      `meta` rows, `materialized` in the verb comment, and the two band-test
      repairs (`testBardStoreSchema`, `testBardStoreVersionGate`)
- [x] Step 2 — the eight motives in `seed/base.sql` **and**
      `tests/combat_fixture.sql` (`testBardStoreShippedSeedMotives`)

**Write helpers**

- [x] Step 3 — `writeCatalogEntry`: mint + argument validation (`testBardStoreWrite`)
- [x] Step 4 — the truth gate, clauses a/b/c (`testBardStoreWrite`)
- [x] Step 5 — `materializeCatalogEntry`: the L0 → L2 latch and its event
      (`testBardStoreMaterialize`)
- [x] Step 6 — `placeCatalogEntry` (`testBardStoreMaterialize`)
- [x] Step 7 — `markCatalogSeeded` (`testBardStoreWrite`)
- [x] Step 8a — `utf8Truncate` in `term` (`testTermTruncate`)
- [x] Step 8b — `writeBardJournal` / `writeBardFocus` + `kBardFocusMaxChars`
      (`testBardStoreMeta`)

**Invariants**

- [x] Step 9 — the `materialized` detail shielded from the narrator
      (`testBardStoreMaterialize`)
- [x] Step 10 — the append-only guards encoded as source-text assertions, plus
      the transaction/rollback test (`testBardStoreAppendOnly`)
- [x] Step 11 — final validation against the spec's AI Validation section

Final state: **5505 checks, 0 failures**, whole suite. No regressions in combat,
architect, pregen, or band. Eight new test functions, all registered in `main()`.

## What landed

Six files, no CMake change, no new translation unit — exactly the blast radius
the plan predicted.

| File | Change |
|---|---|
| `src/world.hpp` | `SCHEMA_VERSION` 5 → 6 |
| `src/world.cpp` | `catalog` + `motive_catalog` DDL; `materialized` in the `events.verb` comment; the three bard `meta` rows in `initialize()` |
| `src/mutations.hpp` | six helper declarations, `kBardFocusMaxChars`, and the updated `events.detail` contract note |
| `src/mutations.cpp` | the six helpers + three file-local helpers (`trimAscii`, `collapseLineBreaks`, `upsertMeta`) |
| `src/term.hpp` / `.cpp` | `utf8Truncate` |
| `src/prose.cpp` | `materialized` added to `engineInternalTag` |
| `seed/base.sql`, `tests/combat_fixture.sql` | the eight motive rows |
| `tests/tests.cpp` | eight new tests; two band-test repairs |

## Log

**The bump's blast radius was exactly what the plan said it was.** Both broken
assertions lived in `testBandResistance`, and nothing else. `CHECK(SCHEMA_VERSION
== 5)` became a comment rather than `== 6`, per the plan's reasoning: the check's
purpose was "group G bumped nothing", and pinning it to the current value would
make every future bump edit a band test. The verbatim table list carries the real
guarantee and gained `catalog` + `motive_catalog` in alphabetical position.

**One plan-level error, in a validation gate rather than in the design.** Step
8a's gate asked the test to assert that a truncated string's *last byte is not a
continuation byte*. That is wrong UTF-8 reasoning: a valid string ending in a
multi-byte character (`"a—"`) has a continuation byte last by construction. The
assertion failed four times against correct code. The correct check is on the
**cut boundary in the source** — if the result is shorter than the input, the
input byte at `result.size()` must not be a continuation byte. Fixed in the test,
with a comment explaining why the obvious formulation is wrong; `utf8Truncate`
itself needed no change.

**The plan's predicted `world.cpp` exception turned out to be unnecessary for the
grep, but was kept for the encoded test.** The plan expected `world.cpp`'s init
INSERT to trip spec grep #2 and require an exception. It does not: the statement
wraps, and the line carrying `INSERT` does not carry the `bard_*` keys — so
grep #2 returns **only** `src/mutations.cpp` lines, with no exception needed. The
exception survives in `testBardStoreAppendOnly` because it is line-based and a
future reflow of that INSERT would otherwise break the test for a correct write.

**Self-review caught a hole in the Step 10 guard.** The first draft enumerated
`src/resolver.cpp`, which does not exist (the file is `src/nlresolve.cpp`), and
omitted `aihttp`, `pregen`, `profile`, `systems`, and `nlresolve`. Because
`readFileBytes` returns `""` for a missing path, the typo passed silently — the
guard was checking nothing for that entry. The list is now every `src/*.cpp`
except `mutations.cpp`, and each file is asserted non-empty so a rename fails the
test loudly instead of quietly voiding its check. This is the failure mode the
`testCombatFinalSweep` precedent is exposed to as well.

**Substring matching on `catalog` is deliberately the spec's own semantics.** The
guard trips on `spell_catalog` and `motive_catalog` too, since both contain the
substring. Nothing in `src/*.cpp` writes to either today (they are seeded SQL), so
the check is clean — but a future runtime write to `spell_catalog` would fail this
test and need the predicate narrowed. Noted rather than pre-solved: the spec
states the rule as `grep -E "catalog"`, and matching it exactly is the point.

**Micro-decisions applied as pinned, all four.** Clause c requires a `resistance`
row (a neutral matchup is refused, which is what gives spec test 8 a reject case);
the prose shield landed here; `kBardFocusMaxChars` is a header constant and
truncation counts code points; a *run* of line breaks collapses to one space, so
`"a\r\nb"` stores `"a b"` rather than `"a  b"`.

**`writeCatalogEntry` mints, `placeCatalogEntry` pre-checks.** The latch is
pre-read before minting in `placeCatalogEntry` so a second call mints no entity
(REQ-BARD-STORE-14), with `materializeCatalogEntry`'s `WHERE entity IS NULL` still
the authoritative guarantee. The test asserts the `entities` row count is
unchanged across the second call — the assertion that catches a mint-then-check
ordering bug.

## Divergences from the plan

Two, both small, neither needing authorization — one is a bug in a validation
gate, the other is a comment made more precise:

1. **Step 8a's validation gate was wrong and was corrected** (see the log). The
   requirement it was testing is unchanged and is now correctly asserted.
2. **Step 9's comment overstated `render.cpp`.** The plan's framing implies
   `render.cpp` treats `materialized` like `burned`/`froze`. It does not — the
   verb has no branch in `render.cpp`'s if/else chain at all, so it is
   renderer-invisible the way `generated` is. The comment now says that.

Nothing in the spec was left unbuilt. REQ-BARD-STORE-3 and -11 remain **deferred
by the spec itself**, and `testBardStoreSchema` asserts `catalog_binding` does not
exist so the deferral stays real.

## Left as-is, deliberately

- **A duplicate `handle` throws from SQLite's UNIQUE constraint**, not from an
  explicit pre-check. The spec's refusal list (REQ-BARD-STORE-9) does not name
  duplicates, and the throw is still a `std::runtime_error` before any other
  helper runs, so the caller's rollback behaves identically. A named pre-check
  would produce a better message if the overture ever collides — worth revisiting
  when a real caller exists.
- **`placeCatalogEntry` does not verify `room` exists**, matching `placeEnemy`,
  which does not either.
- **`README.md` unchanged** — it documents the reset path but never enumerates
  the schema, and this brick adds no player-visible surface.
- **The repo-root `world.db` was deleted** after Step 1 and recreated at version 6
  during the Step 11 play-through.

## Verification record (spec AI Validation)

1. `cmake --build build` clean; **5505 checks, 0 failures**. ✅
2. Grep #2 returns only `src/mutations.cpp` lines (three: the INSERT and the two
   latches). ✅
3. Grep #3 returns **exactly two** lines, each carrying its guard clause on the
   same source line. ✅
4. A world file at `schema_version = 5` produces `SchemaMismatch` and is left
   byte-identical. ✅ (`testBardStoreVersionGate`)
5. Spec behavioral tests 5–10, 12, 13, 13a, 14 all encoded and passing; test 11
   is n/a by the spec's own deferral. ✅
6. The game launches against a fresh `world.db` and plays turns normally; the new
   world carries `schema_version = 6`, eight motives, and the three bard rows. ✅
</content>
