---
title: "Implementation plan: npc-memory-store"
date: 2026-08-07
status: executed
tags: [plan, npc, memory, identity-profile, schema, mutations, append-only, major-npc, caps]
modules: [mutations, world, bard, main]
related: [.lore/work/specs/npc-memory-store.md, .lore/work/design/npc-memory-store.md, .lore/work/brainstorm/npcs.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/work/plans/bard-fact-store.md, .lore/work/specs/npc-conversation.md]
---

# Implementation plan: npc-memory-store

Brick 1 of the NPC feature: **schema, write helpers, read helpers, and the
hand-authored profile loader.** No AI call, no network, no new translation unit.
Source of truth: **[.lore/work/specs/npc-memory-store.md]** (38 requirements plus
-21a, -31a, -31b; prefix `NPCSTORE`). Every step is verifiable with SQL and the
compiled test binary — no API key, no fixture beyond seed SQL.

The work lands in six existing files (`src/world.hpp`, `src/world.cpp`,
`src/mutations.hpp`, `src/mutations.cpp`, `src/bard.cpp`, `src/main.cpp`) and
`tests/tests.cpp`. **No CMake change** — no new translation unit exists, matching
the spec's and design's `modules:` line, which names exactly `mutations`,
`world`, `bard`, `main`.

## Guiding constraints

- **Schema first, write helpers second, reads third, the loader fourth,
  invariants last.** Each step is independently compilable and testable.
- **Zero live-LLM verification.** The whole brick is deterministic — no step
  carries a HIGH token-risk badge, and [[verification-must-be-bounded]] has
  nothing to bite on. Size is the only real dial.
- **One helper / one behavior / one test per step**, matching the granularity of
  the existing `mutations.cpp` helpers.
- **The bump has a blast radius, and it is one line.** `SCHEMA_VERSION` 6 → 7
  breaks exactly one existing assertion: the verbatim table list in
  `testBandResistance` (`tests/tests.cpp:9932-9941`). Step 1 repairs it
  deliberately. That test is a *band* test, so REQ-NPCSTORE gate 36 — which
  names bard, catalog-selection, materialisation, and combat tests — is
  unaffected.

## Seams this touches (verified in tree)

| Seam | File:line | What this brick does with it |
|---|---|---|
| `SCHEMA_VERSION` | `src/world.hpp:12` | 6 → 7 — Step 1 |
| `SCHEMA_DDL` string | `src/world.cpp:14-95` | `catalog_profile` + `npc_memory` appended; `events.verb` comment gains `said`/`spoke`; `catalog.kind` comment gains `'major'` — Step 1 |
| no-write-verb contract | `src/mutations.hpp:9-10` | names **four** verbs today (`looked`, `waited`, `failed`, `examined`); gains `said` and `spoke` — Step 1 |
| verbatim table list | `tests/tests.cpp:9932-9941` | must gain `catalog_profile` + `npc_memory` — Step 1 |
| `utf8Truncate` | `src/term.hpp:142` | the code-point cut both new caps reuse — Steps 2, 3 |
| `kBardFocusMaxChars` | `src/mutations.hpp:247` | the shape the three new cap constants copy |
| `learnSpell` idempotence | `src/mutations.cpp:309` | the guard-in-SQL precedent `writeCatalogProfile` follows |
| `upsertMeta` | `src/mutations.cpp:78` | the `ON CONFLICT DO UPDATE` shape `writeNpcMemory` copies |
| `currentTurn` (anon ns) | `src/mutations.cpp:17` | reads `meta.turn` for `summary_turn` — Step 3 |
| `writeCatalogEntry` kind guard | `src/mutations.cpp:577` | accepts `'major'` — Step 6 |
| model-facing kind check | `src/bard.cpp:796` | does **not** accept `'major'`; divergence commented — Step 6 |
| `catalogRows` | `src/bard.cpp:91-115` | its `kind.empty()` → all-kinds branch is **deleted** — Step 7 |
| `offerable` | `src/bard.cpp:352` | takes a kind **list** — Step 7 |
| `eligibleCatalogForNewRoom` | `src/bard.cpp:952` | `/*kind=*/""` becomes `{"character","beat"}` — Step 7 |
| `initialize()` | `src/world.cpp:133-164` | writes the majors inside its existing transaction — Step 8 |
| `openWorld` signature | `src/world.hpp:54-56` | gains a defaulted `majors` vector — Step 8 |
| `main()`'s open call | `src/main.cpp:55` | reads `seed/majors/` and passes it — Step 9 |
| `buildOvertureContext` | `src/bard.cpp:521-528` | gains a third key; the "EXACTLY two keys" comment moves with it — Step 10 |
| `buildOvertureContext` contract | `src/bard.hpp:85-92` | same amendment in the header — Step 10 |
| `catalogEntryRefusal` | `src/bard.cpp:838` | **already** guards a pre-existing handle — Step 6 adds only the test |
| `readFileBytes` source guards | `tests/tests.cpp:1464-1477` | the precedent for encoding the spec's greps as tests — Step 11 |
| `main()` registration | `tests/tests.cpp:14025+` | one `testNpcStore*` call per new test |

## Micro-decisions pinned before drafting

Nine points the spec leaves open or states against the tree. All are decided here
so implementation does not re-litigate them; the first two were confirmed with the
author.

**1. Profile files arrive as data, not as a path (confirmed).** REQ-NPCSTORE-34
contradicts itself: it says profile files are read by `main.cpp` "exactly as
`setting.txt` already is," but `setting.txt` is read by **`world.cpp`**
(`readFileOrEmpty`, `world.cpp:125`, called inside `initialize()`) from a
defaulted path, and `main.cpp` passes nothing. The analogy is wrong; the two
normative clauses — *read by `main.cpp`*, *passed as data*, *`world.cpp` does no
file I/O of its own for these* — agree with each other and are what gets built:

```cpp
struct MajorProfileFile {
    std::string name;  // the file's name, for the failure message
    std::string text;  // its bytes, verbatim
};

OpenedWorld openWorld(const std::string& path,
                      const std::string& seedPath = "seed/base.sql",
                      const std::string& settingPath = "seed/setting.txt",
                      const std::vector<MajorProfileFile>& majors = {});
```

The default is empty, so **every existing `openWorld` call site compiles and
behaves unchanged** — which is most of gate 36 for free. It also suits the test
suite, which today writes **no files at all**: a profile-file test builds the
vector in C++ rather than needing a temp-directory helper that does not exist.
`setting.txt`'s own path handling is **not** changed by this brick.

**2. Zero profile files ship (confirmed).** REQ-NPCSTORE-32 blesses zero
explicitly, and authoring a major character is content work this brick's spec
says nothing about. `seed/majors/` is not created; Step 9's reader must therefore
treat an absent directory as "no majors," which is a requirement either way.
Consequence to accept knowingly: the shipped game exercises the loader only in
its empty case until a cast is written.

**3. The read helpers live in `mutations.hpp` / `mutations.cpp`.** The design
lists them without naming a file, but both the spec and the design carry
`modules: [mutations, world, bard, main]` — no `npc` module, and therefore no new
translation unit and no CMake change in this brick. `mutations.hpp`'s header
comment ("the ONLY sanctioned write path") gains a sentence noting that the three
`npc*` reads are the file's first read helpers and are `SELECT`-only. The
conversation brick may still hoist them into an `npc` unit; nothing here depends
on their staying put.

**4. `npcLinesSince` probes one row past the cap.** REQ-NPCSTORE-21a needs to
know whether a leading `spoke`'s paired `said` was *cut* or simply predates
`summary_turn` — a leading `spoke` is legitimate in the second case. Selecting
`LIMIT kLineCap + 1` newest-first answers it exactly: if the extra row came back,
the cap really bit. Discard the probe row, then drop a leading `spoke`, yielding
`kLineCap - 1`. A guess based on "did we get exactly `kLineCap` rows" would
wrongly drop a legitimate leading `spoke`.

**5. `writeCatalogProfile` is one statement, both guards in SQL.**

```sql
INSERT OR IGNORE INTO catalog_profile(catalog, profile)
SELECT ?, ? WHERE EXISTS(SELECT 1 FROM catalog WHERE id = ?);
```

`OR IGNORE` is the write-once latch (REQ-NPCSTORE-11); the `EXISTS` clause is the
orphan refusal (REQ-NPCSTORE-14); `db.changes() != 0` is the return. No prior
read, and — the point — the source text contains no `UPDATE catalog_profile`, so
REQ-NPCSTORE-36 is true **by construction** rather than by discipline.

**6. `catalogRows`'s all-kinds branch is deleted, not worked around.**
REQ-NPCSTORE-25 asks that `eligibleCatalogForNewRoom` name its kinds explicitly.
Implementing that by calling `offerable` twice would leave the `kind.empty()`
branch (`bard.cpp:94-98`) alive for the next caller to reuse. Instead `catalogRows`
and `offerable` take a `std::vector<std::string>` of kinds, the SQL becomes
`AND c.kind IN (…)`, and the empty-kind path is removed. **`ORDER BY c.id` is
preserved**, so `testBardSelEligibleNewRoom`'s ordered assertion
(`tests.cpp:11993`) passes unmodified — which concatenating two calls would not
guarantee. Stronger than the requirement asks: no "every kind" path survives.

**7. REQ-NPCSTORE-31b needs no code.** The spec assumes an overture proposing a
handle a major already owns would throw on the `UNIQUE` constraint and roll back
the batch. It would not: `catalogEntryRefusal` already runs
`SELECT 1 FROM catalog WHERE handle = ?` (`bard.cpp:838`) and drops that entry
alone. The requirement reduces to a regression test, in Step 6.

**8. REQ-NPCSTORE-2's premise is one verb out of date.** `mutations.hpp:10` names
`'looked'`, `'waited'`, `'failed'`, **and `'examined'`** — the perception brick
added the fourth. The amendment takes it to six, and Step 11's substring test
asserts all six rather than only the two new ones.

**9. `buildOvertureContext` omits the key when there is no cast.** Validation 29
requires the no-majors payload be **byte-identical** to today's, so the third
element must be *absent*, not present-and-empty. `payload["majors"]` is assigned
only when the vector is non-empty.

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> schema + bump + verb comments + table-list repair<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>2</b> writeCatalogProfile ──┬─▶ <b>4</b> npcProfile / npcMemory<br>
&nbsp;&nbsp;├─▶ <b>3</b> writeNpcMemory ───────┘<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>5</b> npcLinesSince<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>6</b> kind = 'major' (helper yes, model no) ──▶ <b>7</b> explicit kind pair<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─────────┐<br>
&nbsp;&nbsp;└─▶ <b>8a</b> the parser (pure) ──▶ <b>8b</b> the loader ◀── (2 + 6)<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>8c</b> the six refusals<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>9</b> main.cpp reads seed/majors/<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>10</b> overture context<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>11</b> source-text invariants ──▶ <b>12</b> final validation<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified. **No step in this brick is higher.**

Step 1 gates everything. After it lands, the four branches (2/3→4, 5, 6→7, 8a)
are independent and can be reordered freely. Step 8b is the convergence point: it
needs Step 8a (the parser), Step 2 (the profile writer), and Step 6
(`kind = 'major'`), and Steps 8c, 9, and 10 all hang off it. Steps 11 and 12 are
terminal: Step 11 inspects the source lines written by Steps 2, 3, and 8b, so it
must come after them.

---

### Step 1 — Schema: two tables, the version bump, two verb comments, one test repair
**Requirements:** REQ-NPCSTORE-2, -3, -6, -7, -8, -9, -10. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In **`src/world.hpp:12`**, `SCHEMA_VERSION` 6 → 7. No migration is written — the
existing `openWorld` refusal path (`world.cpp:179-191`) already prints the
delete-the-world-file message, and REQ-NPCSTORE-10 asks for nothing more.

In **`src/world.cpp`**, append to `SCHEMA_DDL` after the `catalog` /
`motive_catalog` block, so the two character stores read next to the catalog they
key off:

```sql
-- The authored identity of a character: who they are, how they talk, what they
-- know, what they will not say. WRITE-ONCE — there is deliberately NO helper
-- that edits a profile, so a character's identity cannot drift
-- (REQ-NPCSTORE-11, -12). Asserted against the source text, not trusted.
--
-- Keyed by CATALOG id, not entity, because a hand-authored major character's
-- profile exists from world creation, long before any entity does.
CREATE TABLE catalog_profile(
  catalog INTEGER PRIMARY KEY,        -- catalog.id
  profile TEXT NOT NULL               -- the full character document, model-facing
);

-- What a character remembers, as it remembers it. Freely rewritten and CAPPED
-- (REQ-NPCSTORE-15, -17) — memory is a reconstruction, and a character
-- misremembering costs nothing mechanical. Keyed by ENTITY: memory exists only
-- once the character does. Rows are NOT pre-created at materialisation; the
-- write helper upserts, so there is no row to branch on (REQ-NPCSTORE-9).
CREATE TABLE npc_memory(
  entity       INTEGER PRIMARY KEY,
  summary      TEXT NOT NULL DEFAULT '',
  summary_turn INTEGER NOT NULL DEFAULT 0   -- the turn the summary last covered
);
```

Two comment amendments in the same file, both load-bearing as documentation:

- **`world.cpp:89-90`**, the `events.verb` vocabulary, gains
  `story: 'materialized'; speech: 'said','spoke' (REQ-NPCSTORE-1)`.
- **`world.cpp:62`**, `catalog.kind`, becomes
  `-- 'character' | 'beat' | 'major'` with a note that `'major'` is
  hand-authored and never model-proposed (REQ-NPCSTORE-23, -24).

In **`src/mutations.hpp:9-10`**, amend the no-write-verb contract. It names four
verbs today, not the three the spec expects (micro-decision 8):

```cpp
//   - `appendEvent` alone (no component write) is legal ONLY for the
//     no-write verbs: 'looked', 'waited', 'failed', 'examined', 'said',
//     'spoke'. Speech is a no-write verb because a conversation line is a
//     thing that HAPPENED, with no component to change (REQ-NPCSTORE-2).
```

No table is created for conversation lines (REQ-NPCSTORE-3) and no wake-predicate
change is made (REQ-NPCSTORE-4) — both are requirements to *not* build something,
verified in Step 12.

**Then repair the one existing assertion the bump breaks.** `tests/tests.cpp:9932-9941`
holds a verbatim table list, written so any shape a later feature added would show
up in it. Add `"catalog_profile"` and `"npc_memory"` in alphabetical position
(after `catalog`, and between `name` and `pending_strike`), and extend the comment
above it the way the bard fact store's two entries already are. Nothing else in
the suite pins the schema by name.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean; full suite green
(the repaired table list is the only existing failure expected — if anything else
fails, stop and read it). New <code>testNpcStoreSchema</code>, modeled on
<code>testBardStoreSchema</code> (tests.cpp:10151): <code>pragma_table_info('catalog_profile')</code>
has exactly 2 columns named <code>catalog</code>/<code>profile</code>;
<code>npc_memory</code> has exactly 3 named <code>entity</code>/<code>summary</code>/<code>summary_turn</code>,
and their <code>dflt_value</code>s are <code>''</code> and <code>0</code> (spec check 1 —
assert the defaults, not just the names). Plus spec check 2: write <code>6</code> into a
fresh world's <code>meta.schema_version</code>, reopen, assert <code>SchemaMismatch</code>
and byte-identity — the shape at tests.cpp:10208. Plus REQ-NPCSTORE-1, asserted the
way <code>testBardStoreSchema</code> already asserts <code>'materialized'</code>:
<code>src/world.cpp</code> contains <code>'said'</code> and <code>'spoke'</code> in the
verb vocabulary, and <code>src/mutations.hpp</code> names <b>both</b> in the no-write-verb
line — the two new verbs are documented, not merely used. Finally delete the untracked
dev <code>world.db</code> at the repo root; it is at version 6 and the game will now
refuse it.
</blockquote>

### Step 2 — `writeCatalogProfile`: write-once, guarded in SQL
**Requirements:** REQ-NPCSTORE-11, -12, -13, -14, -16, -17 (`kProfileCap`). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Declare in **`src/mutations.hpp`** with the design's comment block verbatim
(design lines 231-239) — including the sentence explaining why no edit helper
exists — beside a new cap constant in the `kBardFocusMaxChars` shape:

```cpp
// The cap on catalog_profile.profile, in CODE POINTS (REQ-NPCSTORE-17).
// Generous for a hand-written character (~700 words is well under) and a hard
// stop on a model-written one that runs away. This string is re-sent IN FULL on
// every conversation call, which is what the ceiling is protecting.
inline constexpr size_t kProfileCap = 4000;

bool writeCatalogProfile(Db& db, int64_t catalog, const std::string& profile);
```

Implement in **`src/mutations.cpp`** as the single statement of micro-decision 5:
truncate with `utf8Truncate(profile, kProfileCap)`, then one
`INSERT OR IGNORE … SELECT ?, ? WHERE EXISTS(…)`, then `return db.changes() != 0`.
No prior read, no event (REQ-NPCSTORE-13), no transaction of its own
(REQ-NPCSTORE-16).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreProfile</code> against
<code>tests/combat_fixture.sql</code> (spec checks 4-9): a first call on a fresh
catalog entry returns <b>true</b> and stores the text; a second call with
<b>different</b> text returns <b>false</b> and the stored value is
<b>byte-identical</b> to the first (assert the value, not just the return); a call
for a catalog id that does not exist returns false and adds <b>no</b> row; a
5000-code-point profile built from <b>multi-byte</b> characters stores at exactly
4000 code points (<code>SELECT length(profile)</code> counts characters — the right
unit) and is valid UTF-8, so a byte-truncating implementation fails; the
<code>events</code> count is <b>unchanged</b> across all of the above; and inside a
caller-owned <code>db.begin()</code> … <code>db.rollback()</code> nothing persists.
</blockquote>

### Step 3 — `writeNpcMemory`: free rewrite, stamped with the current turn
**Requirements:** REQ-NPCSTORE-15, -16, -17 (`kSummaryCap`), -18. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

```cpp
// The cap on npc_memory.summary, in CODE POINTS (REQ-NPCSTORE-17). Short enough
// that it cannot hold a personality essay. A BRAKE, NOT A GUARANTEE
// (REQ-NPCSTORE-18): the rule that matters — the summary carries facts, never
// voice — is a prompt rule and cannot be enforced on free text. The cap stops
// erosion compounding; it does not prevent it. Do not read it as enforcement.
inline constexpr size_t kSummaryCap = 800;

void writeNpcMemory(Db& db, int64_t entity, const std::string& summary);
```

Implement as an upsert in `upsertMeta`'s shape (`mutations.cpp:78`), stamping
`summary_turn` from the anonymous-namespace `currentTurn(db)` — read inside the
caller's ambient transaction, never passed in:

```sql
INSERT INTO npc_memory(entity, summary, summary_turn) VALUES (?, ?, ?)
ON CONFLICT(entity) DO UPDATE
  SET summary = excluded.summary, summary_turn = excluded.summary_turn;
```

Truncate with `utf8Truncate(summary, kSummaryCap)` before binding. No latch, no
append, no event.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreMemory</code> (spec checks 10-12),
asserted with raw SQL since the read helper lands in Step 4: a first call on an
entity with no row <b>creates</b> one; a second call <b>replaces</b> the summary
outright (exact equality, never a concatenation); <code>summary_turn</code> equals
<code>meta.turn</code> at the moment of each write, driven by advancing
<code>meta.turn</code> between two writes and asserting the value <b>changed</b>; a
900-code-point multi-byte summary stores at exactly 800 code points; the
<code>events</code> count is unchanged; and a rolled-back caller transaction leaves
no row.
</blockquote>

### Step 4 — `npcProfile` and `npcMemory`: the two scalar reads
**Requirements:** REQ-NPCSTORE-19, -20, -22. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 2 and 3. Add to `mutations.hpp`, with the design's comments
(design lines 260-270) and a note that these are the file's **first read
helpers** — `SELECT`-only, deterministic functions of the database
(REQ-NPCSTORE-22), added here rather than in a new unit per the spec's
`modules:` line (micro-decision 3):

```cpp
struct NpcMemory {
    std::string summary;
    int64_t summaryTurn = 0;
};

std::string npcProfile(Db& db, int64_t entity);
NpcMemory npcMemory(Db& db, int64_t entity);
```

`npcProfile` joins through the entity's catalog row —
`SELECT p.profile FROM catalog c JOIN catalog_profile p ON p.catalog = c.id WHERE c.entity = ?`
— and returns `""` when there is no match. **Empty is normal and common**
(REQ-NPCSTORE-19): it is the state of every minor character before its first
conversation, and it is never an error, never a throw, never a log line.
`npcMemory` returns `{"", 0}` for an entity with no row (REQ-NPCSTORE-20).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNpcStoreProfile</code> and
<code>testNpcStoreMemory</code> each gain their read half (spec check 13): a profile
written through <code>writeCatalogProfile</code> round-trips <b>byte-exact</b> through
<code>npcProfile</code> once the entry is materialized onto an entity; an entity that
is not a catalog character yields <code>""</code>; a catalog character with no profile
row yields <code>""</code>; a materialized entity whose profile was written
<b>before</b> materialisation still reads back (the catalog-keyed join is the point);
and <code>npcMemory</code> on an entity with no row returns <code>""</code> and
<code>0</code> <b>without throwing</b>.
</blockquote>

### Step 5 — `npcLinesSince`: the bounded, pair-safe raw-line read
**Requirements:** REQ-NPCSTORE-17 (`kLineCap`), -21, -21a, -22. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 1 only (it reads `npc_memory.summary_turn` and `events`).

```cpp
// The cap on rows returned by one raw-line read (REQ-NPCSTORE-17). The bounded
// read is what stops the recurring conversation prompt growing across a
// session — see the design's decision 4 and the persona-drift finding behind it.
inline constexpr int64_t kLineCap = 40;

struct SpeechLine {
    std::string verb;    // "said" or "spoke"
    std::string detail;  // the line, byte-exact as appended
};

std::vector<SpeechLine> npcLinesSince(Db& db, int64_t entity);
```

Read `summary_turn` (0 when no row), then:

```sql
SELECT verb, detail FROM events
 WHERE verb IN ('said','spoke')
   AND (actor = :npc OR subject = :npc)
   AND turn > :summary_turn
 ORDER BY id DESC
 LIMIT :cap_plus_one;
```

Newest-first with a `LIMIT` selects the recent **tail**, which is what
REQ-NPCSTORE-21 asks for — a character forgets the middle of a long conversation,
never the end of it. Then, in order:

1. If `kLineCap + 1` rows came back, **discard the probe row** (the oldest). Its
   presence is the proof that the cap actually cut something, which is what
   distinguishes REQ-NPCSTORE-21a's orphan from a legitimate leading `spoke`
   whose `said` simply predates `summary_turn` (micro-decision 4).
2. Reverse into oldest-first order.
3. If the probe row existed **and** the front row is now a `spoke`, drop it —
   the cap has handed back a reply without its question. The result is
   `kLineCap - 1` rows.
4. A **trailing** `said` with no `spoke` is kept untouched. That is what a failed
   reply looks like, and hiding it would make the character unaware it was
   spoken to.

`detail` is returned verbatim — no trimming, no normalisation, no shielding.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreLines</code> (spec checks 14-18c),
driven by <code>appendEvent</code> with <code>meta.turn</code> advanced between pairs:
six <code>said</code>/<code>spoke</code> rows with a summary written after the third
returns exactly the last three, <b>oldest first</b>; <code>kLineCap + 10</code>
qualifying rows return exactly <code>kLineCap</code> and the first returned row is
<b>not</b> the oldest qualifying row; lines from a conversation with a
<b>different</b> character are excluded; a <code>said</code> where the character is
the <code>subject</code> and a <code>spoke</code> where it is the <code>actor</code> are
both included; <code>looked</code>, <code>moved</code>, and <code>failed</code> rows in
the same turn range are excluded; every returned <code>detail</code> is
<b>byte-equal</b> to the text originally appended (18a — filtering and ordering are
worthless if the content is not what was stored); <code>kLineCap + 1</code> qualifying
rows whose oldest survivor would be a <code>spoke</code> return exactly
<code>kLineCap - 1</code> rows with a <code>said</code> first (18b); a trailing
<code>said</code> with no <code>spoke</code> is <b>present</b> (18c); and — the case
micro-decision 4 exists for — <b>fewer</b> than <code>kLineCap</code> qualifying rows
beginning with a <code>spoke</code> keeps that <code>spoke</code>.
</blockquote>

### Step 6 — `kind = 'major'`: the helper accepts it, the model still cannot
**Requirements:** REQ-NPCSTORE-23, -24, -31b. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

At **`src/mutations.cpp:577`**, the helper's guard admits the third kind:

```cpp
// 'major' is hand-authored and loaded at world creation; it never arrives from
// the model. The MODEL-FACING check at bard.cpp:796 deliberately does NOT
// admit it, and the divergence is the point (REQ-NPCSTORE-24) — this helper is
// the engine's write path, that one is the wire's admission gate.
if (kind != "character" && kind != "beat" && kind != "major") {
    refuse("kind must be 'character', 'beat', or 'major', got '" + kind + "'");
}
```

At **`src/bard.cpp:796`**, leave the predicate **unchanged** and add the mirrored
comment: the bard authors characters and beats, so a `major` arriving from the
model stays a refusal, and the two checks diverge on purpose. This is the one
place in the tree where the comment at `bard.cpp:793` ("The order below mirrors
`writeCatalogEntry`'s… what matters is the SET of predicates") stops being true,
so say so there rather than leaving a reader to discover it.

REQ-NPCSTORE-31b needs **no code** (micro-decision 7): `catalogEntryRefusal`
already rejects a handle that exists in `catalog` (`bard.cpp:838`), and
`admitOvertureProposal` already continues to the next sibling on a refusal. This
step adds only the test that keeps it true.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreMajorKind</code> (spec checks 3, 22,
30b): <code>writeCatalogEntry(db, "major", …)</code> returns a positive id and stores
<code>kind = 'major'</code>, while <code>"wanderer"</code> still <b>throws</b>; a model
response proposing <code>kind: "major"</code> is refused by
<code>validateOvertureResponse</code>/<code>admitOvertureProposal</code> with the
<b>existing</b> wording (<code>"kind must be 'character' or 'beat'"</code>, asserted as
a substring so a reworded refusal is caught) and its <b>siblings in the same batch
still admit</b>; and an overture batch proposing a handle a pre-existing
<code>'major'</code> row already owns drops <b>that entry only</b>, leaves the major's
row untouched, admits the siblings, and <b>throws nothing</b>.
</blockquote>

### Step 7 — The room generator's menu becomes a stated list
**Requirements:** REQ-NPCSTORE-25, -26. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 6 — the regression test needs a `'major'` row to exist.

This is the trap the design names: `eligibleCatalogForNewRoom` calls
`offerable(db, /*kind=*/"", …)` (`bard.cpp:952`), an empty kind means *every*
kind, and adding `'major'` would silently start offering major characters to the
room generator — the exact arrival mechanism this design avoids.

Change `catalogRows` (`bard.cpp:91`) and `offerable` (`bard.cpp:352`) to take
`const std::vector<std::string>& kinds`, build the SQL's `AND c.kind IN (?,?,…)`
from its size, and **delete the `kind.empty()` all-kinds branch** entirely
(micro-decision 6). `eligibleCatalog` passes `{kind}`; `eligibleCatalogForNewRoom`
passes `{"character", "beat"}` with the design's reasoning as a comment. Keep
`ORDER BY c.id` exactly as it is — the existing ordered assertion at
`tests.cpp:11993` depends on it and must pass unmodified.

Nothing else loses a capability by this: `offerable` has exactly two call sites
(`bard.cpp:457`, `bard.cpp:952`) and `catalogRows` exactly one, and the
empty-kind branch has exactly **one** user — the call REQ-NPCSTORE-25 is about.
Deleting it removes a path with no remaining caller rather than narrowing a
contract someone else relies on.

REQ-NPCSTORE-26 needs no change: `wakeCatalog` (`bard.cpp:431`) selects the full
`catalog` table with no kind filter, so majors are already visible to the bard.
Step 7 adds the assertion that pins it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreGeneratorMenu</code> — <b>the
regression that matters</b> (spec check 19): a world with one <code>'major'</code>
entry and one <code>'character'</code> entry, <b>both eligible by tier</b>, and
<code>eligibleCatalogForNewRoom</code> returns the character and <b>not</b> the major.
Written to fail if the explicit pair is ever reverted to an empty kind. Plus spec
check 21: <code>buildWakeContext</code> on the same world <b>contains</b> the major's
handle. Spec check 20 is carried by the existing catalog-selection tests
(<code>testBardSelEligible</code>, <code>testBardSelEligibleFact</code>,
<code>testBardSelEligibleNewRoom</code>, <code>testBardSelHandle</code>) passing
<b>unmodified</b> — confirm that, and confirm no <code>src/*.cpp</code> still calls
<code>offerable</code> or <code>catalogRows</code> with an empty kind list.
</blockquote>

### Step 8a — The profile-file parser, pure and directly testable
**Requirements:** REQ-NPCSTORE-27, -30, -31a (syntax half). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 1 only. The parser touches no database, so it is split from the
loader and tested on its own — four of the six failure modes in Step 8c are
parser failures and cost a string each here instead of a world creation there.

In **`src/world.hpp`**, beside the existing `OpenedWorld`:

```cpp
// One hand-authored major-character file, as main.cpp read it off disk. `name`
// is carried only so a malformed file can be NAMED in the failure
// (REQ-NPCSTORE-31); nothing in the world file records it.
struct MajorProfileFile {
    std::string name;  // the file's name, for the failure message
    std::string text;  // its bytes, verbatim
};

// The parsed form: the header supplies the catalog columns, the body supplies
// catalog_profile.profile and is MODEL-FACING ONLY (REQ-NPCSTORE-28). The
// engine-owned rules — never explain a mechanic, never volunteer background,
// never name what does not exist — are deliberately NOT here. They live in the
// prompt the engine controls, because a rule a player will actively attack
// cannot live in a file an author can edit or forget.
struct MajorProfile {
    std::string handle, name, motive, profile;
    int64_t tier = 0;
};

// Parse one profile file. Returns "" on success, else the reason — WITHOUT the
// file name, which the caller prefixes. Pure: no database, no I/O, no logging.
std::string parseMajorProfile(const std::string& text, MajorProfile& out);
```

Implement in **`src/world.cpp`**. Split at the **first blank line**: everything
before is `key: value` header lines, everything after is the body **verbatim**.
The header ends there and **never resumes** (REQ-NPCSTORE-27), so a `key: value`
line inside the body is body text. Recognised keys are exactly `handle`, `name`,
`motive`, `tier`; all four are required, and an **unrecognised** key is a
failure, not a silent skip (REQ-NPCSTORE-31a) — the concrete case being an author
writing `goal:`, a field this spec deliberately does not carry
(REQ-NPCSTORE-30) and would otherwise let quietly do nothing. `tier` must parse
as an integer, whole-string.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreProfileParse</code>, calling
<code>parseMajorProfile</code> directly with no world at all (spec check 24, and the
syntax half of 25 and 30a): a well-formed file yields all four header values and a
body <b>byte-exact</b> from the character after the blank line to the last byte; a
body containing a <code>key: value</code> line <b>after</b> the blank line keeps it in
the body and does <b>not</b> parse it as a header; a body opening with its own blank
line survives intact; a file with <b>no</b> blank line, a missing
<code>motive:</code>, a <code>goal: deeper</code> line, a <code>tier: soon</code>, and a
header line with no colon each return a <b>non-empty</b> reason naming the offending
key or line; and every reason is non-empty <b>without</b> containing a file name —
the caller owns that.
</blockquote>

### Step 8b — The loader: catalog rows, profiles, and `initialize()`
**Requirements:** REQ-NPCSTORE-28, -29, -31b, -32, -33, -34 (callee half). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 2, 6, and 8a.

Add the fourth `openWorld` parameter of micro-decision 1, with a comment stating
that the caller reads the files and this function only stores them, and that an
empty vector is the normal case (REQ-NPCSTORE-32) — not a fault, not a
diagnostic:

```cpp
OpenedWorld openWorld(const std::string& path,
                      const std::string& seedPath = "seed/base.sql",
                      const std::string& settingPath = "seed/setting.txt",
                      const std::vector<MajorProfileFile>& majors = {});
```

In **`src/world.cpp`**'s anonymous namespace, `writeMajors(db, files)` — for each
file in the order given: `parseMajorProfile`, prefixing the file's name onto any
reason and throwing; check the handle against the handles of **earlier files in
this batch** (REQ-NPCSTORE-31b scopes the duplicate check to the files, because
the catalog is empty at this point); then
`writeCatalogEntry(db, "major", handle, name, blurb, motive, tier)` and
`writeCatalogProfile(db, id, body)`. The **blurb is the body's first non-empty
line** (REQ-NPCSTORE-29) — `writeCatalogEntry` requires one and a major is never
selected from a menu, so no second authoring surface is invented for a field
nothing reads. The unknown-motive refusal comes free from `writeCatalogEntry`'s
existing `motive_catalog` check; catch it and re-throw with the file name
attached.

Call `writeMajors(db, majors)` from **`initialize()`** inside its **existing**
transaction, after the seed and the `meta` rows, before `db.commit()` — so major
rows exist when `openWorld` returns and before any overture call is possible
(REQ-NPCSTORE-33), and so a failure rolls the whole world back rather than
leaving it half-seeded.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreMajorFiles</code> (spec checks 23,
26, 27, 28): a well-formed file yields <b>one</b> catalog row carrying the header's
handle, name, motive, and tier with <code>kind = 'major'</code>, <b>one</b>
<code>catalog_profile</code> row whose text is the body <b>byte-exact</b> (which is
also REQ-NPCSTORE-28 — nothing the engine owns is injected into the stored text),
and a blurb equal to the body's <b>first non-empty</b> line, driven with a body that
opens with a blank line; a 5000-code-point body stores truncated to 4000; two
majors in one call both land, in file order, with ascending catalog ids; the major
rows are present the instant <code>openWorld</code> returns, before any overture call
is possible; and <b>zero</b> files creates a world successfully with no major rows
and <b>no diagnostic at any log level</b> — asserted against a captured log sink,
not by eye.
</blockquote>

### Step 8c — The six ways a profile file fails world creation loudly
**Requirements:** REQ-NPCSTORE-31, -31a. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 8b. These are hand-authored seed files like `base.sql`, and a
silently dropped major character is a world missing its most expensive content
with nothing to show for it — so every one of these throws out of `initialize()`,
rolls the transaction back, and carries **the file's name** in the message.

The six: a missing required header key; an **unrecognised** header key; a motive
absent from `motive_catalog`; a non-integer `tier`; a handle duplicating another
**profile file's** handle; and a header line that does not parse. Four are
Step 8a's reasons with a file name prefixed; the motive and duplicate-handle
cases are the loader's own.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testNpcStoreMajorFiles</code> gains spec checks 25,
30, 30a: each of the six malformed inputs makes <code>openWorld</code> throw
<code>std::runtime_error</code> whose <code>what()</code> <b>contains the file name</b>
(assert the name as a substring, once per case, so a generic message fails); the
<code>goal: deeper</code> case is present by name, since it is the concrete reason
REQ-NPCSTORE-31a exists; and — spec check 30 — after any of these failures the
world file has <b>no rows at all</b>: reopen it raw with <code>Db</code> and assert
<code>sqlite_master</code> holds <b>no</b> <code>meta</code> table, proving the whole
<code>initialize()</code> transaction rolled back rather than leaving a half-seeded
world.
</blockquote>

### Step 9 — `main.cpp` reads `seed/majors/`
**Requirements:** REQ-NPCSTORE-34 (caller half), -32. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 8b. A file-local reader in `main.cpp`'s anonymous namespace,
using `std::filesystem` (already the log module's dependency, `log.hpp:28`):
enumerate `seed/majors/`, take regular files only, **sort by filename** so catalog
ids and the duplicate-handle check are deterministic across platforms, read each
one's bytes, and return the vector. An **absent or unreadable directory yields an
empty vector, silently** — no majors is a valid world (REQ-NPCSTORE-32), and
`seed/majors/` does not exist in this brick (micro-decision 2).

Then the one call site changes:

```cpp
auto world = openWorld("world.db", "seed/base.sql", "seed/setting.txt",
                       readMajorProfiles("seed/majors"));
```

The directory is read on every launch, including resumed ones where
`initialize()` never runs. That is a few small file reads at startup and buys a
call site with no branch in it; note it rather than optimising it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean; the game launches
against a freshly created <code>world.db</code>, plays a few turns, and quits — the
bump's real-world check, and the only proof that the new parameter is wired at the
one call site that matters. Assert in <code>testNpcStoreMajorFiles</code> that the
shipped tree has <b>no</b> <code>seed/majors/</code> directory and that a default
<code>openWorld(path, "seed/base.sql")</code> therefore creates <b>zero</b> major rows.
</blockquote>

### Step 10 — `buildOvertureContext` gains the cast
**Requirements:** REQ-NPCSTORE-35. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 8b — the test needs majors in a world.

At **`src/bard.cpp:521-528`**, add a third key: the major cast as **name plus
profile**, ordered by `catalog.id`, read with a `SELECT` join over `catalog` and
`catalog_profile` where `kind = 'major'`. The key is assigned **only when the
cast is non-empty** (micro-decision 9), so a world with no majors produces a
byte-identical payload to today's.

The comment at `bard.cpp:522` says "EXACTLY two keys and no ids ever" and the
header contract at `bard.hpp:85-92` says "carrying EXACTLY two things". Both are
documented contracts and **both move with the code** — three keys, still no ids.
Add the design's reasoning: the overture still cannot place anyone, because the
map does not exist yet; it reads the cast so the story is authored around them.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardSelContext</code> gains spec check 29: on a
world with two majors the payload contains <b>both names and both profiles</b> and
still contains <b>no ids</b> (assert the catalog ids and entity ids do not appear as
substrings); on a world with none the payload is <b>byte-identical</b> to the
two-key payload — the third key <b>absent</b>, not present-and-empty. Every existing
assertion in <code>testBardSelContext</code> and
<code>testBardSelRequestBody</code> passes unmodified.
</blockquote>

### Step 11 — The invariants, encoded as tests
**Requirements:** REQ-NPCSTORE-4, -22, -36, -37, -38. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 2, 3, 4, 5, and 8b — this step inspects the SQL they write and
calls the helpers they add.

The spec states these as greps. Encode them as source-text assertions in the test
binary — the `testCombatFinalSweep` precedent (`tests.cpp:1464-1477`) and the
fact store's two append-only guards — so they survive as regression guards rather
than being run once by hand:

- **REQ-NPCSTORE-36:** no line of any `src/*.cpp` contains
  `UPDATE catalog_profile`. Write-once is a property of the source text, not a
  convention. Micro-decision 5 makes this true by construction, and this is what
  keeps it true.
- **REQ-NPCSTORE-37:** for every `src/*.cpp` other than `mutations.cpp`, no line
  containing `INSERT`/`UPDATE`/`DELETE` also mentions `catalog_profile` or
  `npc_memory`. Enumerate the sources explicitly, the way the no-RNG guard
  enumerates its files. **`world.cpp` needs no exception** — its DDL is
  `CREATE TABLE`, and Step 8b's loader calls the mutation helpers rather than
  writing SQL. If an exception turns out to be needed, the loader has been
  written wrong.
- **REQ-NPCSTORE-2's amendment (spec check 31a):** `mutations.hpp`'s no-write-verb
  comment names all **six** verbs — `looked`, `waited`, `failed`, `examined`,
  `said`, `spoke` — asserted by substring. That line is the thing a future reader
  checks a new verb against, so it is verified like the other source-text
  invariants rather than trusted.
- **REQ-NPCSTORE-4:** the wake predicate at `bard.cpp:1043` still reads
  `verb IN ('generated','defeated','learned','materialized')` **verbatim**, and
  `src/bard.cpp` contains neither `'said'` nor `'spoke'`. Speech is not
  irreversible and must not wake the bard. Asserted as source text because
  `hasTriggeringEvent` is file-local and the behavioural surface
  (`bardAfterTurn`) needs the bard enabled and a transport — the wrong price for
  a one-line guarantee.
- **REQ-NPCSTORE-22:** the three read helpers write **nothing**. Behavioural, not
  textual, because they live in `mutations.cpp` and the grep above deliberately
  exempts that file: snapshot every table's row count, call `npcProfile`,
  `npcMemory`, and `npcLinesSince` — each on a populated entity and on an entity
  with no rows at all — and assert every count is unchanged, including `events`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testNpcStoreInvariants</code> holds all five and
passes. Then spec check 33, <b>by mutation</b>, run by hand once: in a scratch copy
add a profile-editing helper (<code>UPDATE catalog_profile SET profile = ?</code>) to
<code>mutations.cpp</code> and confirm the suite turns <b>red</b>; add an
<code>INSERT INTO npc_memory</code> to <code>src/systems.cpp</code> and confirm the
suite turns <b>red</b>; add <code>'said'</code> to the wake predicate and confirm the
suite turns <b>red</b>. Revert all three. A guard that has never been seen to fail is
not a guard.
</blockquote>

### Step 12 — Final validation against the spec
**Requirements:** all. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section top to bottom and confirm each item,
then walk the coverage map below and confirm each requirement has a passing
assertion.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> (1) <code>cmake --build build</code> clean and the
<b>whole</b> suite green with <b>no network and no <code>ANTHROPIC_API_KEY</code></b>
(gates 34, 35) — no regressions in combat, architect, pregen, band, or bard;
(2) the spec's grep <code>grep -En "UPDATE catalog_profile" src/*.cpp</code> returns
<b>nothing</b>; (3) <code>grep -En "INSERT|UPDATE|DELETE" src/*.cpp | grep -E
"catalog_profile|npc_memory"</code> returns only <code>src/mutations.cpp</code> lines;
(4) <code>git diff</code> touches <b>no existing test body</b> except the table list in
<code>testBandResistance</code> and the additive halves of
<code>testBardSelContext</code> — gate 36 checked as a property of the diff;
(5) the two "build nothing" requirements hold, and both are already
suite-asserted rather than eyeballed — no conversation-line table
(REQ-NPCSTORE-3, carried by Step 1's verbatim table list) and no speech verb in
the wake predicate (REQ-NPCSTORE-4, carried by Step 11); (6) a version-6 world file
produces <code>SchemaMismatch</code>; (7) every new <code>testNpcStore*</code> is
registered in <code>main()</code> (<code>tests.cpp:14025+</code>) next to the other
schema/mutation tests, not appended at the end.
</blockquote>

---

## Coverage map

| Requirement | Step | Spec check |
|---|---|---|
| REQ-NPCSTORE-1 — `said`/`spoke` event verbs | 1 | — (step-1 substring) |
| REQ-NPCSTORE-2 — no-write-verb contract amended | 1, 11 | 31a |
| REQ-NPCSTORE-3 — no table for conversation lines | 1 | — (verbatim table list) |
| REQ-NPCSTORE-4 — not in the wake predicate | 11 (no code change) | — (source text) |
| REQ-NPCSTORE-5 — `said` holds the player's typed input | 5 | 18a |
| REQ-NPCSTORE-6 — `catalog_profile` table | 1 | 1 |
| REQ-NPCSTORE-7 — `npc_memory` table | 1 | 1 |
| REQ-NPCSTORE-8 — `summary_turn` watermark | 1, 3 | 11 |
| REQ-NPCSTORE-9 — rows not pre-created | 3 | 10 |
| REQ-NPCSTORE-10 — `SCHEMA_VERSION` 6 → 7 | 1 | 2 |
| REQ-NPCSTORE-11 — `writeCatalogProfile`, guarded in SQL | 2 | 4, 5 |
| REQ-NPCSTORE-12 — no edit path exists | 2, 11 | 31, 33 |
| REQ-NPCSTORE-13 — event-free | 2 | 8 |
| REQ-NPCSTORE-14 — orphan profile refused | 2 | 6 |
| REQ-NPCSTORE-15 — `writeNpcMemory` upserts + stamps | 3 | 10, 11 |
| REQ-NPCSTORE-16 — ambient transaction only | 2, 3 | 9 |
| REQ-NPCSTORE-17 — three code-point caps | 2, 3, 5 | 7, 12, 15 |
| REQ-NPCSTORE-18 — the cap is a brake, not a guarantee | 3 | — (comment) |
| REQ-NPCSTORE-19 — `npcProfile`, empty is normal | 4 | 13 |
| REQ-NPCSTORE-20 — `npcMemory`, no row is not an error | 4 | 13 |
| REQ-NPCSTORE-21 — `npcLinesSince`, oldest first, recent tail | 5 | 14, 15 |
| REQ-NPCSTORE-21a — no reply without its question | 5 | 18b, 18c |
| REQ-NPCSTORE-22 — reads are `SELECT`-only and deterministic | 4, 5, 11 | 16, 17, 18 |
| REQ-NPCSTORE-23 — `kind = 'major'` accepted by the helper | 6 | 3 |
| REQ-NPCSTORE-24 — the model-facing check still refuses it | 6 | 22 |
| REQ-NPCSTORE-25 — the generator's menu is a stated list | 7 | 19, 20 |
| REQ-NPCSTORE-26 — majors visible to the bard | 7 | 21 |
| REQ-NPCSTORE-27 — header + blank line + verbatim body | 8a | 24 |
| REQ-NPCSTORE-28 — engine rules live in the prompt, not the file | 8a, 8b | 23 (body byte-exact) |
| REQ-NPCSTORE-29 — blurb = first non-empty body line | 8b | 23 |
| REQ-NPCSTORE-30 — no goal field | 8a, 8c | 30a |
| REQ-NPCSTORE-31 — malformed fails loudly, naming the file | 8c | 25 |
| REQ-NPCSTORE-31a — unrecognised key is a failure | 8a, 8c | 30a |
| REQ-NPCSTORE-31b — duplicate check scoped to the files | 6, 8b | 30b |
| REQ-NPCSTORE-32 — zero files is valid and silent | 8b, 9 | 26 |
| REQ-NPCSTORE-33 — written inside `initialize()`'s transaction | 8b | 28, 30 |
| REQ-NPCSTORE-34 — `main.cpp` reads, `openWorld` stores | 8b, 9 | 28 |
| REQ-NPCSTORE-35 — the overture context's third element | 10 | 29 |
| REQ-NPCSTORE-36 — no `UPDATE catalog_profile` in the source | 2, 11 | 31 |
| REQ-NPCSTORE-37 — `mutations.cpp` is the only writer | 11 | 32 |
| REQ-NPCSTORE-38 — both asserted by the suite, checked by mutation | 11 | 33 |

## Explicitly not built here

- **The `say` action, the conversation prompt, and the model call** — owned by
  [specs/npc-conversation.md](../specs/npc-conversation.md). If implementation
  reaches for an `HttpResponse`, the brick has been scoped wrong. Nothing in this
  brick ever writes a `said` or `spoke` row; the verbs exist, the read that folds
  them exists, and the writer arrives with the conversation.
- **The narrator's view of `said` / `spoke`.** The spec puts it out of scope
  explicitly. Worth knowing for whoever takes the conversation brick:
  `buildFacts` (`src/prose.cpp:287-340`) forwards every non-shielded `detail` to
  the narrator, and a `said` detail is the player's raw typed input — so the
  shield decision lands there, not here. It is latent until something writes the
  first row.
- **Movement, goals, and major-character arrival.** Third brick. `kind = 'major'`
  and the one-way materialisation latch are what it will build on.
- **Any actual major character.** Zero profile files ship (micro-decision 2);
  `seed/majors/` is not created. The first authored major is content work and the
  loader's first real end-to-end exercise.
- **Regenerating a minor character's profile.** Write-once here, so a bland first
  profile is permanent — the same bet this project already makes about room
  descriptions, and the first thing to revisit if characters come out flat.
- **Promoting anything a character said into the catalog.** The bard sees speech
  in its wake context for free; nothing here acts on it.

## Notes for whoever implements this

- **Delete the repo-root `world.db` after Step 1.** It is gitignored dev state at
  version 6; the game will refuse it, which is correct behavior, not a bug.
- **No CMake change.** Everything lands in files already in `twcore`. If a new
  `.cpp` starts to feel necessary, re-read micro-decision 3 first — the spec's
  `modules:` line is the constraint.
- **`README.md` needs no change.** It documents the reset path ("delete
  `world.db` and relaunch") but never enumerates the schema, and this brick adds
  no player-visible surface.
- **New tests open `tests/combat_fixture.sql`**, the fixture carrying the combat
  constants `writeCatalogEntry`'s truth gate reads — the same rule the bard fact
  store's tests follow.
- Register every new test in `main()` (`tests/tests.cpp:14025+`) beside the other
  `testBardStore*` entries, not appended at the end of the list.
