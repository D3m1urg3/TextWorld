---
title: "Implementation plan: bard-fact-store"
date: 2026-08-03
status: executed
tags: [plan, bard, dungeon-master, schema, catalog, mutations, append-only, motive, truth-gate]
modules: [world, mutations, combat, term, prose]
related: [.lore/work/specs/bard-fact-store.md, .lore/work/design/bard-fact-store.md, .lore/work/design/bard-architect-integration.md, .lore/work/specs/bard-architect-integration.md, .lore/work/brainstorm/dungeon-master.md]
---

# Implementation plan: bard-fact-store

Brick 1 of the bard feature: **schema, seed data, and mutation helpers only.** No AI
call, no network, no new translation unit. Source of truth:
**[.lore/work/specs/bard-fact-store.md]** (18 requirements, prefix `BARD-STORE`;
REQ-3 and REQ-11 deferred and not built). Every step below is verifiable with SQL
and the compiled test binary — no API key, no fixtures beyond seed SQL.

The work lands in four existing files (`src/world.hpp`, `src/world.cpp`,
`src/mutations.hpp`, `src/mutations.cpp`), two seed files, one small addition to
`src/term.*`, one line of `src/prose.cpp`, and `tests/tests.cpp`. **No CMake change** —
no new translation unit exists.

## Guiding constraints

- **Schema first, helpers second, invariants last.** Steps 1–2 move the schema; 3–8
  add one helper each; 10 is the mechanical append-only guard. Each step is
  independently compilable and testable.
- **Zero live-LLM verification.** The whole brick is deterministic — [[verification-must-be-bounded]]
  has nothing to bite on here, and no step carries a HIGH token-risk badge. This is
  the cheapest brick of the four; treat token-risk as uniformly LOW and size as the
  only real dial.
- **One helper / one behavior / one test per step**, matching the granularity of the
  existing `mutations.cpp` helpers rather than landing all six at once.
- **The bump has a blast radius.** `SCHEMA_VERSION` 5 → 6 breaks two *existing*
  assertions in `tests/tests.cpp` that were written to prove earlier features did
  **not** bump it. Step 1 repairs them deliberately; discovering them mid-step later
  is the main avoidable stumble in this brick.

## Seams this touches (verified in tree)

| Seam | File:line | What this brick does with it |
|---|---|---|
| `SCHEMA_VERSION` | `src/world.hpp:12` | 5 → 6 — Step 1 |
| `SCHEMA_DDL` string | `src/world.cpp:11-62` | `catalog` + `motive_catalog` appended; `events.verb` comment gains `materialized` — Step 1 |
| `initialize()` | `src/world.cpp:100-126` | three new `meta` rows, beside the existing `setting` INSERT — Step 1 |
| `meta.setting` precedent | `src/world.cpp:115-120` | the pattern the three bard rows copy (row, not shape) |
| seeded constants | `seed/base.sql:69-140` | eight `motive_catalog` rows beside `bestiary`/`spell_catalog` — Step 2 |
| combat test fixture | `tests/combat_fixture.sql:95-140` | same eight rows — the fixture every new test opens |
| `mintEntity` (anon ns) | `src/mutations.cpp:23` | reused by `placeCatalogEntry` — Step 6 |
| `placeEnemy` | `src/mutations.cpp:178-250` | the shape `placeCatalogEntry` mirrors (mint + component rows, event-free) |
| `learnSpell` | `src/mutations.cpp:252-260` | the idempotence precedent for the two latches |
| `appendEvent` | `src/mutations.cpp:32-45` | writes the `materialized` row — Step 5 |
| `utf8Length` | `src/term.hpp:140` | the code-point counter `utf8Truncate` is built beside — Step 8a |
| tag shield | `src/prose.cpp:314` | `materialized` added to `engineInternalTag` — Step 9 |
| `readFileBytes` source guards | `tests/tests.cpp:1320-1330` | the precedent for encoding the spec's greps as tests — Step 10 |
| **table-list assertion** | `tests/tests.cpp:7644-7650` | must gain `catalog` + `motive_catalog` — Step 1 |
| **`CHECK(SCHEMA_VERSION == 5)`** | `tests/tests.cpp:7632` | written to prove the *band* feature didn't bump; must change — Step 1 |
| `main()` registration | `tests/tests.cpp:7960+` | one `testBardStore*` call per new test |

## Micro-decisions pinned before drafting

Four points the spec leaves open. All are decided here so implementation does not
re-litigate them; the first two were confirmed with the author.

**1. Truth-gate clause c requires a `resistance` row (confirmed).** REQ-BARD-STORE-10c
as written — *"a row exists … or no row exists and the entry is therefore asserting a
neutral matchup"* — covers every possible pair and can reject nothing. The design's
real intent (*"the blurb must not claim a weakness"*, design line 124) is unenforceable,
because the engine reads the pair, not the English. Clause c is therefore implemented
as **the pair must be materially non-neutral**: a `resistance` row must exist. A beat
on a neutral matchup teaches nothing, so refusing it is the honest form of the rule,
and it gives spec test 8's *"accepted iff the resistance table supports it"* a reject
case it otherwise could not have.

**2. The prose shield lands here, not downstream (confirmed).** `materializeCatalogEntry`
puts the catalog **handle** in `events.detail`, and `buildFacts` (`src/prose.cpp:287-315`)
forwards every non-`generated` detail to the narrator. That is the exact failure mode
`burned`/`froze` are shielded from, and `mutations.hpp:59` already warns *"Adding a
third tagged verb means updating that shield too."* No caller fires the event in this
brick, so the leak is latent — but the fix is one line and no sibling bard spec claims
it. Step 9.

**3. `kBardFocusMaxChars` is a header constant, and truncation counts code points.**
`inline constexpr size_t kBardFocusMaxChars = 300;` in `mutations.hpp` — "tunable"
(REQ-BARD-STORE-16a) means a one-line edit, not an env var; nothing in the engine reads
tuning from the environment except the AI gates. Truncation is by **code point**, not
byte, reusing the `utf8Length` discipline at `src/term.hpp:137-140` — a byte cut would
split a multi-byte character and store invalid UTF-8 into a string the architect pastes
into a JSON prompt on every generation. This costs one new pure function (Step 8a).

**4. Normalization collapses *runs* of line breaks to one space.** REQ-BARD-STORE-16a
says "every newline and carriage return is collapsed to a single space." Read literally
per character, `\r\n` becomes two spaces. Pinned reading: **a consecutive run of `\n`/`\r`
yields exactly one space**, then truncate. Other whitespace is untouched — the requirement
names line breaks only.

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> schema + bump + meta rows + test repairs<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>2</b> seed the eight motives ─┐<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>3</b> writeCatalogEntry + arg validation ──▶ <b>4</b> truth gate<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>7</b> markCatalogSeeded&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;├─▶ <b>5</b> materializeCatalogEntry ──▶ <b>6</b> placeCatalogEntry&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>10</b> append-only guards<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>9</b> prose shield&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>11</b> final validation<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;└─▶ <b>8a</b> utf8Truncate ──▶ <b>8b</b> writeBardJournal / writeBardFocus<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified. **No step in this brick is higher.**

Step 1 gates everything. After it lands, the three branches (2→3→4, 5→6, 8a→8b) are
independent of one another and can be reordered freely. Steps 10 and 11 are terminal:
Step 10 counts and inspects the source lines written by Steps 3, 5, 7, and 8b, so it
must come after all of them.

---

### Step 1 — Schema: two tables, three meta rows, the version bump, and the test repairs
**Requirements:** REQ-BARD-STORE-1, -2, -4, -6, -7. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In **`src/world.hpp:12`**, `SCHEMA_VERSION` 5 → 6. No migration is written — the
existing `openWorld` refusal path (`world.cpp:141-149`) already prints the
delete-the-world-file message, and REQ-BARD-STORE-1 asks for nothing more.

In **`src/world.cpp`**, append to `SCHEMA_DDL` (after the `bestiary`/`drop_table`
block, before the events log, so the story catalog reads next to the combat one):

```sql
-- The story catalog: entries authored by the bard, materialized at most once.
-- Mirrors `bestiary` — a record the world is cast from — except that rows are
-- minted at RUNTIME by the bard rather than seeded, and each is cast ONCE.
-- APPEND-ONLY: no helper updates kind/handle/name/blurb/motive/tier/fact_*;
-- `entity` and `seeded` are one-way latches guarded in SQL (REQ-BARD-STORE-17).
CREATE TABLE catalog(
  id      INTEGER PRIMARY KEY,          -- engine-minted; NEVER on the wire
  kind    TEXT NOT NULL,                -- 'character' | 'beat'
  handle  TEXT NOT NULL UNIQUE,         -- the model-facing SELECTION token
  name    TEXT NOT NULL,                -- the in-world parser noun; becomes the
                                        -- minted entity's name row
  blurb   TEXT NOT NULL,                -- the ONLY prose the model sees to select
  motive  TEXT NOT NULL,                -- motive_catalog.motive (closed vocab,
                                        -- enforced in writeCatalogEntry)
  tier    INTEGER NOT NULL,             -- placement gate vs distanceFromSeed
  seeded  INTEGER NOT NULL DEFAULT 0,   -- 1 = hinted in prose, not yet materialized
  entity  INTEGER,                      -- NULL = latent; non-NULL = MATERIALIZED
  -- A KNOWLEDGE beat asserts something TRUE about combat. Both NULL on every
  -- other entry; both non-NULL together, never one. Validated at admission
  -- against bestiary/spell_catalog/resistance (REQ-BARD-STORE-10).
  fact_archetype TEXT,
  fact_element   TEXT
);

-- The closed motive vocabulary, authored for Thornmere. Engine-owned constants
-- like spell_catalog: seeded in base.sql, never written at runtime. The model
-- sees `blurb`, never the key.
CREATE TABLE motive_catalog(motive TEXT PRIMARY KEY, blurb TEXT);
```

`catalog_binding` is **not created** — REQ-BARD-STORE-3 defers it, and nothing in
any of the four bard specs writes to it.

Extend the `events.verb` comment at **`src/world.cpp:57`** with `materialized` (the
verb is written in Step 5; the vocabulary is documented here, REQ-BARD-STORE-7).

In **`initialize()`** (`src/world.cpp:117-120`), beside the existing `setting`
INSERT, add the three bard rows — rows, not shapes, exactly like `meta.setting`:

```cpp
// The bard's three meta rows (REQ-BARD-STORE-6): rows, not shapes. Written at
// init so every helper can UPDATE rather than branch on absence.
db.exec(
    "INSERT INTO meta(key, value) VALUES "
    "('bard_journal', ''), ('bard_focus', ''), ('bard_last_wake_turn', 0)");
```

**Then repair the two existing assertions the bump breaks** — both live in
`testBandResistance`, which was written to prove the *band* feature added no schema:

- `tests/tests.cpp:7632` — `CHECK(SCHEMA_VERSION == 5);` becomes a comment noting the
  bump came from the bard fact store, not from group G. Do **not** rewrite it to
  `== 6`: the assertion's purpose was "this feature didn't bump it," and pinning it to
  the current value would make every future bump edit a band test for no reason. The
  table-list check below carries the real guarantee.
- `tests/tests.cpp:7644-7650` — the verbatim expected table list gains `"catalog"` and
  `"motive_catalog"` in alphabetical position (after `bestiary`, and between `meta` and
  `name`).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean; full suite green
(the two repaired assertions are the only existing failures expected — if anything
else fails, stop and read it). New <code>testBardStoreSchema</code>, modeled on
<code>testCombatSchema</code> (tests.cpp:301): <code>pragma_table_info('catalog')</code>
has exactly 11 columns with the expected names; <code>motive_catalog</code> has exactly 2;
<code>catalog_binding</code> does <b>not</b> exist (REQ-BARD-STORE-3 stays deferred);
<code>meta</code> contains <code>bard_journal</code>, <code>bard_focus</code>, and
<code>bard_last_wake_turn</code>. Plus REQ-BARD-STORE-1: write <code>5</code> into a
fresh world's <code>meta.schema_version</code>, reopen, assert <code>SchemaMismatch</code>
and byte-identity — the shape already at tests.cpp:191-206, with 5 instead of 999999.
Finally delete the untracked dev <code>world.db</code> at the repo root; it is at
version 5 and the game will now refuse it.
</blockquote>

### Step 2 — Seed the eight motives
**Requirements:** REQ-BARD-STORE-5. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Append the eight `motive_catalog` rows from REQ-BARD-STORE-5 to **`seed/base.sql`**,
beside `spell_catalog` and `bestiary`, with a comment marking the vocabulary as
**authored content, provisional pending author approval** — changing it is this one
edit plus a world-file delete.

Add the same eight rows to **`tests/combat_fixture.sql`**, which already duplicates
`bestiary`, `spell_catalog`, and `resistance` for the same reason. **Every new test in
this brick opens `tests/combat_fixture.sql`** — it is the only fixture carrying the
combat constants the truth gate reads. `tests/fixture.sql` is deliberately left alone
(it has no combat constants today); a future test that calls `writeCatalogEntry`
against it must seed motives itself.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardStoreSchema</code> gains: a fresh world has
exactly <b>eight</b> <code>motive_catalog</code> rows, every one with a non-empty
blurb, and the key set equals the eight named in REQ-BARD-STORE-5. Assert the same
count against <code>seed/base.sql</code> via <code>testShippedSeedShape</code>'s
world so a fixture/seed divergence is caught. Full suite still green.
</blockquote>

### Step 3 — `writeCatalogEntry`: mint + argument validation
**Requirements:** REQ-BARD-STORE-8, -9. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Declare in **`src/mutations.hpp`** with the design's comment block (design lines
199-219) — including the sentence explaining why there is deliberately no edit
helper — and implement in **`src/mutations.cpp`**. Signature per REQ-BARD-STORE-8:

```cpp
int64_t writeCatalogEntry(Db& db, const std::string& kind,
                          const std::string& handle, const std::string& name,
                          const std::string& blurb, const std::string& motive,
                          int64_t tier,
                          const std::string& factArchetype = "",
                          const std::string& factElement = "");
```

Validate **before** any write, so a refusal leaves the table untouched (the
`placeEnemy`/`moveEntity` discipline — throw before mutating):

- `kind` is exactly `character` or `beat`;
- `motive` has a `motive_catalog` row;
- `handle`, `name`, `blurb` are non-empty **after trim** (trim ASCII whitespace —
  there is no existing trim helper in the tree, so add a file-local one in the
  anonymous namespace beside `currentTurn`);
- `tier >= 0`;
- `factArchetype` and `factElement` are both empty or both non-empty.

Every failure is `std::runtime_error` with a message naming the helper and the
offending value, matching `placeEnemy`'s wording. These are engine faults: the
caller offers only valid values.

Then INSERT one row and return `last_insert_rowid()`. **No event** — a latent entry
has not happened (the `dropGrimoire`/`placeEnemy` precedent). Store the trimmed
strings, and store `fact_archetype`/`fact_element` as SQL NULL when empty, so the
"both NULL or both non-NULL" invariant in the DDL comment is literally true in the
data rather than a mix of `''` and NULL.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardStoreWrite</code> (spec tests 6, 7): a
valid entry returns a positive id, its row round-trips every column, and the
<code>events</code> count is <b>unchanged</b>; separate throw cases for unknown
<code>kind</code>, unknown <code>motive</code>, empty/whitespace-only
<code>handle</code>/<code>name</code>/<code>blurb</code>, negative <code>tier</code>,
<code>factArchetype</code> set with <code>factElement</code> empty, and the reverse.
After each throw, assert the catalog row count is unchanged.
</blockquote>

### Step 4 — The truth gate
**Requirements:** REQ-BARD-STORE-10. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 3. When both fact fields are set, `writeCatalogEntry` additionally
refuses unless all three hold:

- **a.** `factArchetype` has a `bestiary` row;
- **b.** `factElement` appears as an `element` in `spell_catalog`. Plain
  `WHERE element = ?` suffices — three-valued logic already excludes the NULL-element
  rows, so no explicit `IS NOT NULL` guard is needed. What matters is the resulting
  vocabulary: `ward`, `stun`, `dispel`, and `blast` all carry a NULL element
  (`seed/base.sql:115-121`), so the live element set is exactly `{fire, frost}` and a
  beat naming a *spell* rather than an element is refused;
- **c.** a `resistance` row exists for `(factArchetype, factElement)` — the
  non-neutral rule from micro-decision 1. Comment the *why* at the call site: the
  engine can check the pair but not the blurb's claim about it, so refusing neutral
  pairs is the enforceable form of "a catalog entry may not promise a falsehood."

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardStoreWrite</code> gains spec test 8, driven
from the seeded data so the test states the real matchup —
<code>('rime_touched','fire')</code> accepted (2x row), <code>('rime_touched','frost')</code>
accepted (1/2x row), <code>('goblin_grunt','fire')</code> <b>throws</b> (no row =
neutral, clause c), <code>('no_such_beast','fire')</code> throws (clause a),
<code>('rime_touched','acid')</code> throws (clause b), and
<code>('rime_touched','ward')</code> throws (clause b — a spell that is not an
element). Assert the catalog row count is unchanged after each refusal.
</blockquote>

### Step 5 — `materializeCatalogEntry`: the L0 → L2 latch and its event
**Requirements:** REQ-BARD-STORE-12, -13. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

```cpp
bool materializeCatalogEntry(Db& db, int64_t catalog, int64_t entity, int64_t actor);
```

In order, in one call: `UPDATE catalog SET entity = ? WHERE id = ? AND entity IS NULL`
— **on one source line**, see Step 10 — then, **only if `db.changes() != 0`**, read the
entry's `handle` and append one `materialized` event (actor; subject = `entity`;
object = `catalog`; detail = the handle). Return whether this call materialized it.

The latch is the `WHERE entity IS NULL` clause, never a prior read (REQ-BARD-STORE-13):
a second call changes nothing, appends nothing, returns false. Same shape as
`writeGeneratedRoom` writing its rows and its `generated` event together — the L0→L2
transition and the wake trigger are one fact, so they cannot drift.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardStoreMaterialize</code> (spec test 9):
first call returns true, sets <code>catalog.entity</code>, and appends <b>exactly one</b>
<code>materialized</code> event whose <code>subject</code> is the entity,
<code>object</code> is the catalog id, and <code>detail</code> is the handle. Second
call returns false, appends nothing, and leaves <code>entity</code> unchanged. A call
against a nonexistent catalog id returns false and appends nothing.
</blockquote>

### Step 6 — `placeCatalogEntry`
**Requirements:** REQ-BARD-STORE-14. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 5.

```cpp
int64_t placeCatalogEntry(Db& db, int64_t catalog, int64_t room,
                          const std::string& description, int64_t actor);
```

Mirrors `placeEnemy` (`mutations.cpp:178`): mint one entity via `mintEntity`, write
its `name` (**from `catalog.name`**), `description` (**from the `description`
argument — never the blurb**; the blurb is selection prose the model already saw),
and `location` (= `room`) rows, then call `materializeCatalogEntry`.

**Order matters for the already-materialized case.** REQ-BARD-STORE-14 requires that
a second call mint **no** entity, so the latch must be checked before minting: run
the `UPDATE … WHERE entity IS NULL` first and return 0 if it changed nothing. Two
readings of "mints … then calls materializeCatalogEntry" are possible; only this one
satisfies "no entity is minted." Concretely — pre-check `catalog.entity IS NULL`,
return 0 if not; otherwise mint, write the rows, and call `materializeCatalogEntry`,
which re-checks the latch authoritatively. The pre-check is an optimization for the
common case; the latch remains the guarantee.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardStoreMaterialize</code> gains spec test 10:
the minted entity's <code>name</code> equals <code>catalog.name</code>, its
<code>description</code> equals the <b>passed</b> description (explicitly assert it is
<b>not</b> the blurb), and its <code>location.container</code> is the given room. Called
twice for the same entry, the second call returns 0 and the <code>entities</code> row
count is <b>unchanged</b> — the assertion that catches a mint-then-check ordering bug.
</blockquote>

### Step 7 — `markCatalogSeeded`
**Requirements:** REQ-BARD-STORE-15. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`void markCatalogSeeded(Db& db, int64_t catalog);` — a single
`UPDATE catalog SET seeded = 1 WHERE id = ? AND seeded = 0`, **on one source line**
(Step 10). Event-free bookkeeping; the guard makes a second call a silent no-op by
construction, the `learnSpell` idempotence shape.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardStoreWrite</code> gains spec test 12: first
call sets <code>seeded = 1</code>; second call leaves the row and the
<code>events</code> count unchanged.
</blockquote>

### Step 8a — `utf8Truncate` in `term`
**Requirements:** REQ-BARD-STORE-16a (enabling). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add beside `utf8Length` (`src/term.hpp:137-140`), which already establishes the
code-point-counting discipline and the "no wcwidth, no CJK" note:

```cpp
// Truncate `s` to at most `maxChars` CODE POINTS, never splitting a multi-byte
// character. Same counting unit as utf8Length. Returns `s` unchanged when it is
// already short enough.
std::string utf8Truncate(std::string_view s, size_t maxChars);
```

Implement in `src/term.cpp` by advancing over lead bytes exactly as `utf8Length`
does and cutting at the byte offset where the count reaches `maxChars`. `term` stays
pure — no db include, per its header contract.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new checks in <code>testTermWrap</code> (or a sibling
<code>testTermTruncate</code>): ASCII shorter than the cap is returned unchanged;
ASCII longer is cut to exactly <code>maxChars</code> bytes; a string of multi-byte
characters (em-dash, accented vowels) cut mid-string yields
<code>utf8Length(result) == maxChars</code> and <b>valid UTF-8</b> — assert the result
is a byte-prefix of the input and that its last byte is not a continuation byte;
<code>maxChars == 0</code> yields empty.
</blockquote>

### Step 8b — `writeBardJournal` and `writeBardFocus`
**Requirements:** REQ-BARD-STORE-16, -16a. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 8a. Both upsert their `meta` row and are **free rewrite** — a second
call replaces the stored value entirely, never appends. Add
`inline constexpr size_t kBardFocusMaxChars = 300;` to `mutations.hpp` beside the
declarations, with the comment explaining the cap's purpose: this string is paid for
on **every** room generation, and an unbounded one would quietly become the largest
term in the architect's context.

`writeBardFocus` normalizes before writing (micro-decision 4): collapse each **run**
of `\n`/`\r` to a single space, then `utf8Truncate` to `kBardFocusMaxChars`. This is
what makes REQ-BARD-ARCH-1's "one short line" enforced rather than described —
a length cap alone would admit a multi-line focus that reads as prose in the
architect's context.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardStoreMeta</code> (spec tests 13, 13a):
input longer than the cap stores exactly <code>kBardFocusMaxChars</code> characters
(<code>SELECT length(value)</code> counts characters, not bytes — the right unit here);
input containing <code>\n</code> or <code>\r</code> stores neither, and
<code>"a\r\nb"</code> stores <code>"a b"</code> (one space, per micro-decision 4);
both helpers called twice with different text store <b>only the second</b>, asserted as
exact equality, never a concatenation; the journal is <b>not</b> truncated.
</blockquote>

### Step 9 — Shield the `materialized` detail from the narrator
**Requirements:** none in this spec — micro-decision 2. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 5 (the verb must exist to test). At **`src/prose.cpp:314`**:

```cpp
const bool engineInternalTag =
    verb == "burned" || verb == "froze" || verb == "materialized";
```

Extend the surrounding comment, and update the contract note at **`mutations.hpp:50-59`**
so the "TWO kinds of value" paragraph names the handle as the third tagged form.

This is the one step outside the spec's stated boundary, taken deliberately: the
handle is a machine token (`scorched_lectern`), `buildFacts` forwards every
non-`generated` detail to the narrator, and the failure mode is the same
REQ-PROSE-11 noun-echo that `burned`/`froze` are already shielded from.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new check in <code>testBardStoreMaterialize</code>,
modeled on <code>testGeneratedEventInvisible</code> (tests.cpp:6342): after a
materialization, <code>buildFacts</code> for that turn parses to a payload whose
<code>events</code> array contains the <code>materialized</code> verb but <b>no</b>
<code>detail</code> key on it, and the handle string appears <b>nowhere</b> in the
payload. Existing prose tests stay green.
</blockquote>

### Step 10 — The append-only guards, encoded as tests
**Requirements:** REQ-BARD-STORE-17, -18. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 3, 5, 7, and 8b — this step counts and inspects the SQL those steps
write, and its rollback test calls their helpers.

The spec states these as greps. Encode them as source-text assertions in the test
binary — the `testCombatFinalSweep` precedent (tests.cpp:1320-1330, the no-RNG
guard) — so they survive as regression guards rather than being run once by hand:

- **REQ-BARD-STORE-18:** for every `src/*.cpp` other than `mutations.cpp`, no line
  containing `INSERT`/`UPDATE`/`DELETE` also mentions `catalog`, `bard_journal`,
  `bard_focus`, or `bard_last_wake_turn`. Enumerate the sources explicitly, the way
  the no-RNG guard enumerates its files. Note that `world.cpp`'s DDL is `CREATE TABLE`
  and its three `meta` INSERTs name the keys — so **`world.cpp` must be excepted for
  the `bard_*` keys** (that INSERT is REQ-BARD-STORE-6's init write and is correct);
  the `catalog` half of the check applies to it with no exception.
- **REQ-BARD-STORE-17:** `src/mutations.cpp` contains **exactly two** occurrences of
  `UPDATE catalog SET`, one latching `entity` with `AND entity IS NULL` and one
  latching `seeded` with `AND seeded = 0`, each **on a single source line** — assert
  the guard clause is present on the same line as the `SET`. This is why Steps 5 and 7
  say "on one source line": a wrapped SQL string would pass the count and lose the
  guarantee. Also assert `mutations.cpp` contains no `UPDATE catalog SET kind`,
  `handle`, `name`, `blurb`, `motive`, `tier`, `fact_archetype`, or `fact_element`.

Add the transaction test here too (spec test 14): every helper runs correctly inside
a caller-owned transaction and leaves nothing behind when it is rolled back.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardStoreAppendOnly</code> holds all of the
above and passes. Spec test 14: <code>db.begin()</code>, <code>writeCatalogEntry</code>,
assert the row is visible, <code>db.rollback()</code>, assert <b>zero</b> catalog rows;
repeat for a <code>materializeCatalogEntry</code> + its event, and for
<code>writeBardFocus</code>. Then run the spec's two greps by hand once and confirm
they agree with the encoded tests.
</blockquote>

### Step 11 — Final validation against the spec
**Requirements:** all. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section top to bottom and confirm each item, then
walk the coverage map below and confirm each requirement has a passing assertion.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> (1) <code>cmake --build build</code> clean and the <b>whole</b>
suite green — no regressions in combat, architect, pregen, or band; (2) the spec's
grep #2 returns only <code>src/mutations.cpp</code> lines; (3) the spec's grep #3
returns exactly two lines, each carrying its guard clause; (4) a version-5 world file
produces <code>SchemaMismatch</code>; (5) every new <code>testBardStore*</code> is
registered in <code>main()</code>; (6) the game still launches and plays a few turns
against a freshly created <code>world.db</code> — the bump's real-world check.
</blockquote>

---

## Coverage map

| Requirement | Step | Spec test |
|---|---|---|
| REQ-BARD-STORE-1 — `SCHEMA_VERSION` 5 → 6, no migration | 1 | mech. 4 |
| REQ-BARD-STORE-2 — `catalog` table | 1 | — (schema test) |
| REQ-BARD-STORE-3 — `catalog_binding` **deferred** | — | 11 (n/a) |
| REQ-BARD-STORE-4 — `motive_catalog` table | 1 | 5 |
| REQ-BARD-STORE-5 — eight motive rows | 2 | 5 |
| REQ-BARD-STORE-6 — three `meta` rows | 1 | 5 |
| REQ-BARD-STORE-7 — `materialized` in the verb comment | 1 | — |
| REQ-BARD-STORE-8 — `writeCatalogEntry`, event-free | 3 | 6 |
| REQ-BARD-STORE-9 — argument validation throws | 3 | 7 |
| REQ-BARD-STORE-10 — both-or-neither fact pairing | 3 | 7 |
| REQ-BARD-STORE-10 — the truth gate (clauses a/b/c) | 4 | 8 |
| REQ-BARD-STORE-11 — `bindCatalogPlaceholder` **deferred** | — | 11 (n/a) |
| REQ-BARD-STORE-12 — `materializeCatalogEntry` | 5 | 9 |
| REQ-BARD-STORE-13 — latch idempotence | 5 | 9 |
| REQ-BARD-STORE-14 — `placeCatalogEntry` | 6 | 10 |
| REQ-BARD-STORE-15 — `markCatalogSeeded` | 7 | 12 |
| REQ-BARD-STORE-16 — journal/focus free rewrite | 8b | 13a |
| REQ-BARD-STORE-16a — focus normalize + truncate | 8a, 8b | 13 |
| REQ-BARD-STORE-17 — no edit path exists | 10 | mech. 3 |
| REQ-BARD-STORE-18 — `mutations.cpp` is the only writer | 10 | mech. 2 |
| *(micro-decision 2)* — prose shield | 9 | — (new) |

## Explicitly not built here

- **`catalog_binding` and `bindCatalogPlaceholder`** (REQ-BARD-STORE-3, -11) — deferred
  with the placeholder machinery that would write to them.
- **`catalog.pending`** — deferred alongside the above.
- **The L1 `sketch` lane** — the catalog goes L0 → L2 in one step (design decision 3).
- **`eligibleCatalog` / `catalogForHandle`** — read side, owned by
  [specs/bard-catalog-selection.md](../specs/bard-catalog-selection.md).
- **`buildArchitectContext` carrying `bard_focus`** — owned by
  [specs/bard-architect-integration.md](../specs/bard-architect-integration.md).
  This brick only guarantees the stored value is one short line.
- **The overture, the wake hook, `meta.bard_last_wake_turn` updates** — owned by
  [specs/bard-overture-and-scheduling.md](../specs/bard-overture-and-scheduling.md).
  This brick creates the row at 0 and never advances it.
- **`main.cpp`, any prompt, any transport** — if implementation reaches for an
  `HttpResponse`, the brick has been scoped wrong.

## Notes for whoever implements this

- **Delete the repo-root `world.db` after Step 1.** It is gitignored dev state at
  version 5; the game will refuse it, which is correct behavior, not a bug.
- **`README.md` needs no change.** It documents the reset path ("delete `world.db`
  and relaunch", line 157) but never enumerates the schema, and this brick adds no
  player-visible surface.
- **No CMake change.** Everything lands in files already in `twcore`.
- Register every new test in `main()` (`tests/tests.cpp:7960+`) next to the other
  schema/mutation tests, not at the end of the list.
