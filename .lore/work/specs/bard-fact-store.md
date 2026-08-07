---
title: "Bard fact store: schema, helpers, and write rules"
date: 2026-08-03
status: implemented
tags: [bard, dungeon-master, schema, catalog, mutations, append-only, motive, truth-gate, migration]
modules: [world, mutations, combat]
related: [.lore/work/design/bard-fact-store.md, .lore/work/design/bard-architect-integration.md, .lore/work/brainstorm/dungeon-master.md]
req-prefix: BARD-STORE
---

# Bard fact store: schema, helpers, and write rules

Brick 1 of the bard feature. **Contains no AI call, no network, and no new translation unit** — it is schema, seed data, and mutation helpers only. Every requirement below is verifiable with SQL and a compiled test binary; nothing here needs a transport, a fake HTTP response, or an API key.

Source design: [bard-fact-store.md](../design/bard-fact-store.md), as amended by [bard-architect-integration.md](../design/bard-architect-integration.md).

## A. Schema

**REQ-BARD-STORE-1.** `SCHEMA_VERSION` is raised from 5 to 6. An existing world file at version 5 produces the current `SchemaMismatch` diagnostic and refuses to open; no migration is written.

**REQ-BARD-STORE-2.** The schema gains a `catalog` table:

```sql
CREATE TABLE catalog(
  id      INTEGER PRIMARY KEY,
  kind    TEXT NOT NULL,                -- 'character' | 'beat'
  handle  TEXT NOT NULL UNIQUE,         -- model-facing selection token
  name    TEXT NOT NULL,                -- in-world parser noun
  blurb   TEXT NOT NULL,                -- the ONLY prose the model sees to select
  motive  TEXT NOT NULL,                -- references motive_catalog.motive
  tier    INTEGER NOT NULL,             -- placement gate vs distanceFromSeed
  seeded  INTEGER NOT NULL DEFAULT 0,
  entity  INTEGER,                      -- NULL = latent; non-NULL = materialized
  fact_archetype TEXT,
  fact_element   TEXT
);
```

**REQ-BARD-STORE-3. — DEFERRED, NOT BUILT.** The design specifies a `catalog_binding` table for StoryVerse-style placeholders. **No spec in this feature ever writes to it**: nothing binds a placeholder, and materialization (REQ-BARD-ARCH-12) does not either. Shipping an inert table through a `SCHEMA_VERSION` bump buys nothing, so it is cut from this brick and arrives with the placeholder machinery that needs it — alongside the deferred `catalog.pending` flag. The id is retained rather than renumbered so design cross-references stay valid.

**REQ-BARD-STORE-4.** The schema gains a `motive_catalog` table: `(motive TEXT PRIMARY KEY, blurb TEXT)`. It is engine-owned constants, seeded in `base.sql` alongside `spell_catalog` and `bestiary`, never written at runtime.

**REQ-BARD-STORE-5.** `base.sql` seeds exactly eight motive rows. **The vocabulary is PROVISIONAL pending author approval** — it is authored content, and changing it is a one-line seed edit plus a world-file delete:

| motive | blurb |
|---|---|
| `curiosity` | wants to know something they have not been told |
| `secrecy` | has something to keep hidden, and is arranging for it to stay that way |
| `rivalry` | wants to be first, or to be seen to be first |
| `obligation` | is bound by a duty they did not choose |
| `grief` | is holding on to someone or something already gone |
| `appetite` | wants to take and carry off |
| `pride` | would rather be wrong than corrected |
| `homesickness` | does not belong here yet, and feels it |

**REQ-BARD-STORE-6.** Three new `meta` rows are written at `initialize()`: `bard_journal` (empty string), `bard_focus` (empty string), `bard_last_wake_turn` (0). These are rows, not shapes — they contribute no DDL beyond their INSERT.

**REQ-BARD-STORE-7.** The `events.verb` vocabulary gains `materialized`, documented in the schema comment beside the existing verbs.

## B. Write helpers

All helpers live in `mutations.cpp`, never begin/commit/rollback, and run inside the caller's ambient transaction — the existing convention in `mutations.hpp`.

**REQ-BARD-STORE-8.** `writeCatalogEntry(db, kind, handle, name, blurb, motive, tier, factArchetype = "", factElement = "") -> int64_t` mints one catalog row and returns its id. It emits **no event**: a latent entry has not happened. (Precedent: `dropGrimoire` and `placeEnemy` both mint entities and emit no event of their own.)

**REQ-BARD-STORE-9.** `writeCatalogEntry` throws `std::runtime_error` when `kind` is not `character` or `beat`, when `motive` has no `motive_catalog` row, when `handle`/`name`/`blurb` are empty after trim, or when `tier` is negative. These are engine faults — the caller offers only valid values.

**REQ-BARD-STORE-10 (the truth gate).** `factArchetype` and `factElement` are both empty or both non-empty; one alone throws. When both are set, `writeCatalogEntry` additionally throws unless:

- a. `factArchetype` has a `bestiary` row; **and**
- b. `factElement` appears as an `element` in `spell_catalog`; **and**
- c. the pair is materially consistent with `resistance` — a row exists for `(factArchetype, factElement)`, **or** no row exists and the entry is therefore asserting a neutral matchup.

A catalog entry may not promise a falsehood about the combat rules.

**REQ-BARD-STORE-11. — DEFERRED, NOT BUILT.** `bindCatalogPlaceholder` is cut with its table (REQ-BARD-STORE-3). Nothing in this feature calls it.

**REQ-BARD-STORE-12.** `materializeCatalogEntry(db, catalog, entity, actor) -> bool` performs, in this order and in one call: `UPDATE catalog SET entity = ? WHERE id = ? AND entity IS NULL`, and — **only if that update changed a row** — appends one `materialized` event (actor, subject = `entity`, object = `catalog`, detail = the entry's `handle`). Returns whether the entry was materialized by this call.

**REQ-BARD-STORE-13.** A second call to `materializeCatalogEntry` for an already-materialized entry changes nothing, appends no event, and returns false. The latch is enforced by the `WHERE entity IS NULL` clause, not by a prior read.

**REQ-BARD-STORE-14.** `placeCatalogEntry(db, catalog, room, description, actor) -> int64_t` mints one entity; writes its `name` (from `catalog.name`), `description` (from the `description` argument, **not** the catalog blurb), and `location` (= `room`) rows; then calls `materializeCatalogEntry`. Returns the minted entity id, or 0 if the entry was already materialized — in which case **no entity is minted**.

**REQ-BARD-STORE-15.** `markCatalogSeeded(db, catalog)` performs `UPDATE catalog SET seeded = 1 WHERE id = ? AND seeded = 0`. Event-free. A second call changes nothing.

**REQ-BARD-STORE-16.** `writeBardJournal(db, text)` and `writeBardFocus(db, text)` upsert `meta.bard_journal` and `meta.bard_focus` respectively. Both are **free rewrite**: a second call replaces the stored value entirely, never appends to it.

**REQ-BARD-STORE-16a.** `writeBardFocus` normalizes before writing: every newline and carriage return is collapsed to a single space, then the result is truncated to `kBardFocusMaxChars` (**default 300, tunable**). This is what makes "one short line" (REQ-BARD-ARCH-1) enforced rather than merely described — a length cap alone would admit a multi-line focus that reads as prose in the architect's context.

## C. Write rules — the append-only guarantee

**REQ-BARD-STORE-17.** **No helper exists that updates `catalog.kind`, `handle`, `name`, `blurb`, `motive`, `tier`, `fact_archetype`, or `fact_element`.** The only permitted mutations to an existing catalog row are the two one-way latches (`entity`, `seeded`). Correcting the bard's intent is expressed by appending a new entry, leaving the original visible and un-materialized.

**REQ-BARD-STORE-18.** No translation unit other than `mutations.cpp` issues `INSERT`, `UPDATE`, or `DELETE` against `catalog` or the three new `meta` rows.

## AI Validation

Everything here is offline and deterministic. The AI verifies completion by running the test binary and the greps below — no API key, no network, no fixtures beyond SQL.

**Mechanical checks (must all pass):**

1. `cmake --build build` clean, and the existing suite still green — no regressions in combat, architect, pregen, or band tests.
2. `grep -En "INSERT|UPDATE|DELETE" src/*.cpp | grep -E "catalog|bard_journal|bard_focus|bard_last_wake_turn"` returns **only** lines in `src/mutations.cpp` — verifies REQ-BARD-STORE-18.
3. `grep -En "UPDATE catalog SET" src/mutations.cpp` returns **exactly two** lines, one latching `entity` and one latching `seeded`, each carrying its guard clause — verifies REQ-BARD-STORE-17.
4. Opening a world file written at `SCHEMA_VERSION = 5` produces `SchemaMismatch` — verifies REQ-BARD-STORE-1.

**Behavioral tests (new, in `tests/`):**

5. A fresh world has exactly eight `motive_catalog` rows, and `meta` contains `bard_journal`, `bard_focus`, `bard_last_wake_turn`.
6. `writeCatalogEntry` with a valid entry returns a positive id and appends **zero** events (assert the `events` count is unchanged).
7. `writeCatalogEntry` throws on: unknown `kind`; unknown `motive`; empty `handle`, `name`, or `blurb`; negative `tier`; `factArchetype` set with `factElement` empty (and the reverse).
8. **Truth gate:** an entry naming `('rime_touched', 'fire')` is accepted iff the `resistance` table supports it; an entry naming an archetype absent from `bestiary`, or an element absent from `spell_catalog`, throws. Drive this from the seeded data so the test states the real matchup.
9. `materializeCatalogEntry` on a latent entry returns true, sets `entity`, and appends exactly one `materialized` event whose `object` is the catalog id and whose `detail` is the handle. A second call returns false, appends nothing, and leaves `entity` unchanged.
10. `placeCatalogEntry` mints an entity with `name` equal to `catalog.name` and `description` equal to the **passed** description (explicitly assert it is *not* the blurb), located in the given room. Called twice for the same entry, the second call returns 0 and mints **no** entity — assert the `entities` row count is unchanged.
11. *(deferred with REQ-BARD-STORE-3/11 — no binding table is built, so there is nothing to test.)*
12. `markCatalogSeeded` is idempotent.
13. `writeBardFocus` with input longer than `kBardFocusMaxChars` stores exactly `kBardFocusMaxChars` characters; input containing `\n` or `\r` stores neither — assert the stored value contains no line break.
13a. **Free rewrite, not append.** `writeBardJournal` called twice with different text stores only the second; likewise `writeBardFocus`. Assert the stored value equals the second input exactly, not a concatenation.
14. All helpers run correctly inside a caller-owned transaction and leave nothing committed when that transaction is rolled back — assert a rollback after `writeCatalogEntry` leaves zero catalog rows.

**Out of scope for this spec** — no requirement here references the bard's translation unit, the eligible-menu functions, any prompt, any transport, or `main.cpp`. If a test needs an `HttpResponse`, the brick has been scoped wrong.
