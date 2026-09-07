---
title: "Implementation plan: story-arc-store"
date: 2026-09-07
status: executed
tags: [plan, bard, story-arc, steps, advance-rule, schema, mutations, seed, golden-session]
modules: [world, mutations, systems, loop, bard, prose, seed]
related: [.lore/work/specs/story-arc-store.md, .lore/work/brainstorm/story-arc-and-evolving-setting.md, .lore/work/specs/bard-fact-store.md, .lore/work/plans/ai-resolver.md]
---

# Implementation plan: story-arc-store

Adds the arc, the ordered list of story steps, and the rule that walks it.
Source of truth: **[.lore/work/specs/story-arc-store.md]** (27 requirements,
prefix `ARC-STORE`, 18 validation items). This is brick 1 of three; bricks 2
(the overture writes the arc and the steps) and 3 (the architect, narrator and
bard consumers, and narrowing the wake trigger) are out of scope and the spec's
section I says so.

## Where the AI part went

There isn't one. This brick contains **no model call, no network, and no new
translation unit**. So the project's usual planning rule — isolate the live-LLM
steps and put them last — has nothing to bite on here, and this plan does not
invent a live step to satisfy it.

Every validation gate below is one of exactly two things: `sqlite3`-shaped SQL
run through the existing test binary, or a `grep`/build check. Nothing needs
`ANTHROPIC_API_KEY`, `TEXTWORLD_AI_LIVE_TEST`, or a fake transport. The whole
brick runs green offline, and that is deliberate: the story steps are
hand-written in `seed/base.sql` precisely so the advance rule is testable
without a model ([REQ-ARC-STORE-7](#the-seed)). Brick 2 replaces that content
without touching the schema.

The consequence for risk: **no step in this plan is MED or HIGH.** The
expensive thing in this project is live verification, and there is none. The
one place real risk lives is Step 11, and it is a risk of *regression*, not of
tokens — see the note there.

## Decisions taken as settled

These come from the spec and the brainstorm and are not reopened here:

- `SCHEMA_VERSION` goes **7 → 8 with no migration**; a version-7 world file
  refuses to open with the existing `SchemaMismatch` diagnostic. The
  `bard-fact-store` precedent (REQ-BARD-STORE-1).
- Which file each function goes in is fixed by
  [REQ-ARC-STORE-15a](#f-the-advance-rule):
  `advanceStoryStep` in `mutations.{hpp,cpp}`, `stepConditionMet` and
  `evaluateStoryAdvance` in `systems.{hpp,cpp}`, the call site in `loop.cpp`.
  No new translation unit, so **`CMakeLists.txt` is untouched**.
- The steps and their conditions are hand-authored in `seed/base.sql`.
- The bard's wake trigger gains `'advanced'` as a **fifth** verb. Narrowing it
  to `advanced` alone is brick 3 (REQ-ARC-STORE-23).

## What this changes, and where (checked against the tree)

| Thing | File:line | What changes |
|------|-----------|------------------------------|
| `SCHEMA_VERSION` | `src/world.hpp:13` | 7 → 8 — Step 2 |
| `SCHEMA_DDL` | `src/world.cpp:18` | two new `CREATE TABLE`s + the `events.verb` comment — Step 2 |
| the version refusal | `src/world.cpp:380` | unchanged; it is what Step 2's gate exercises |
| `upsertMeta` (anon ns) | `src/mutations.cpp` (used at `:737`) | `writeArc` is three calls to it — Step 4 |
| `trimAscii` (anon ns) | `src/mutations.cpp:35` | reused by `writeStoryStep`'s empty-prose check — Step 5 |
| `db.changes()` | `src/db.hpp:45` / `src/db.cpp:114` | how `advanceStoryStep` learns it latched exactly one row — Step 8 |
| `materializeCatalogEntry` | `src/mutations.cpp:657` | already does the same four things in the same order — UPDATE with the guard in the `WHERE`, check `changes()`, read the row, append the event. Copy it — Step 8 |
| `distanceFromSeed` | `src/combat.cpp:284` (public, `combat.hpp:104`) | the `reached_depth` condition calls it — Step 7 |
| the tick transaction | `src/loop.cpp` (`db.begin()` → `resolve` → `resolveCombat` → `db.commit()`) | `evaluateStoryAdvance` goes after `resolveCombat`, before `commit` — Step 11 |
| `buildFacts` current-turn query | `src/prose.cpp:303` | `verb <> 'generated'` → `verb NOT IN ('generated','advanced')` — Step 10 |
| `buildFacts` recent-events query | `src/prose.cpp:393` | the **second** exclusion site, same change — Step 10 |
| `render.cpp` verb chain | `src/render.cpp:117-227` | **untouched**; unrecognized verbs already render nothing (`:227`) |
| `hasTriggeringEvent` | `src/bard.cpp:1089` | four verbs → five — Step 10 |
| the two source-text pins of the four-verb list | `tests/tests.cpp:11944`, `tests/tests.cpp:13212` | both must be updated in the same step — Step 10 |
| `kExamineGoldenSession` / `TW_DUMP_GOLDEN` | `tests/tests.cpp:2125`, `:2264` | the pattern the new golden session copies — Step 1 |
| the test binary | `tests/tests.cpp` (~16.6k lines), `main()` at `:16371` | every new `testStory*` is added here and registered there |

## Three micro-decisions (flagged, not blocking)

**1. `condition_catalog` must be seeded into `tests/combat_fixture.sql` too.**
The spec names only `seed/base.sql` ([REQ-ARC-STORE-4](#b-schema)), but the tree
already establishes the rule for closed vocabularies: `motive_catalog` is
duplicated into the fixture (`tests/combat_fixture.sql:189-192`, with the reason
written in the comment) because the bard fact-store tests all open that fixture.
`writeStoryStep` validates `kind` against `condition_catalog`, so without the
duplicate every admission test would reject every kind. Seed the four rows in
both files.

The **five story steps go into `seed/base.sql` only.** That is not an oversight
— it leaves every fixture-based world with an empty `story_step` table, which
means the entire existing suite exercises
[REQ-ARC-STORE-19a](#f-the-advance-rule) (an empty step list is a silent no-op
every turn without anyone writing a test for it, and tests that need steps
write their own through
`writeStoryStep`.

**2. `advanced` is excluded from `buildFacts` at *both* sites, not one.**
[REQ-ARC-STORE-21](#g-the-event) says "excluded from `buildFacts` exactly as
`generated` is (`prose.cpp:303`)". `generated` is excluded in two places: the
current-turn `events` query at `:303` and the `recent_events` query at `:393`.
Doing only the first would leak an advance's prose into the narrator's context
for the following six turns and change AI-narrated output — which is the one
thing this brick promises not to do. Recommend matching `generated` exactly, at
both sites. Cost is one extra character-for-character identical edit.

**3. Validation item 15 asks for more than these tests will actually check.**
It says "assert a wake is queued". These tests do not queue a wake. `hasTriggeringEvent` is file-local in `bard.cpp`, and
`evaluateTrigger` requires `bardEnabled()` **and** `aiNarrationEnabled()` plus a
live transport. The existing suite already faced this and decided against it in
so many words — `tests/tests.cpp:11932-11937`: *"asserted as source text because
`hasTriggeringEvent` is file-local and testing it for real needs the bard
enabled and a transport — the wrong price for a one-line guarantee."* Step 10
follows that precedent: assert the **five**-verb predicate as source text (which
also proves the other four survived, item 15's second half), and assert
behaviorally that an `advanced` row lands in `events` at a turn **strictly
greater than** `meta.bard_last_wake_turn` — which is exactly the row the
predicate's query selects. What is not checked is the last link: that the query
then runs and a wake goes out.

---

## Step sequence

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> golden BASELINE — must land on a tree with no other part of this brick in it<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;▼<br>
<b>2</b> schema bump + the two tables<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>3</b> seed the condition vocabulary ─▶ <b>5</b> writeStoryStep + admission gate<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>6</b> seed the five steps ──┐<br>
&nbsp;&nbsp;├─▶ <b>4</b> the arc (seed rows + writeArc)&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>7</b> stepConditionMet ──┐&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>8</b> advanceStoryStep ─┴─▶ <b>9</b> evaluateStoryAdvance ─────────┤<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;└─▶ <b>10</b> renderer-invisible + 5th wake verb ──────────────┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>11</b> the call site in loop.cpp<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>12</b> final sweep vs the spec's 18 items<br>
</div>

Only three edges are real constraints. **1 before everything** (the baseline).
**3 before 5 and 6** (`writeStoryStep` and the seeded rows both need
`condition_catalog`). **6, 9 and 10 before 11** (the call site needs the rule,
the renderer exclusion, and steps to walk). Everything else is convenience
ordering — 4 and 7 and 8 and 10 hang off Step 2 independently and could be done
in any order, or at once.

Every step leaves the tree building and `./build/tests` green. Steps 2–10 leave
the *game* byte-identical to play, because nothing calls the rule until Step 11.

---

### Step 1 — Capture the golden-session baseline, before anything else
**Requirements:** [validation item 14](#ai-validation) (REQ-ARC-STORE-21).
**Size:** M

**This step must land on a tree with no other part of this brick in it.** That
is the reason it exists: the transcript it records is the "before" of a
before/after comparison, so it has to be recorded before the schema, the seed and the rule
exist. It is a step of its own for that reason and nothing else.

Add `kStoryGoldenSession` (a literal) and `testStoryGoldenSession` to
`tests/tests.cpp`, modeled exactly on `kExamineGoldenSession` /
`testExamineGoldenSession` (`tests/tests.cpp:2125`, `:2235`): a fixed script,
AI disabled, every turn's `runTurn(...).output` concatenated, compared to one
literal, with a `TW_DUMP_GOLDEN=1` early-return that prints the block for
re-capture. Register it in `main()`.

Two differences from the examine golden. Get either wrong and the test proves
nothing:

- It opens **`seed/base.sql`**, not `tests/combat_fixture.sql` — the steps are
  seeded in `base.sql`, and a fixture world would have an empty `story_step`
  table and never advance anything, which would make the gate vacuous.
- The script must reach a step advance **with AI off**. That is a real
  constraint, not a preference: with `aiNarrationEnabled()` false, a latent exit
  is a wall (`src/systems.cpp:100`), so no room is ever generated and neither
  `rooms_built` nor `reached_depth` can become true. **Only
  `enemies_defeated` and `spell_learned` are reachable offline**, which is what
  fixes step 1's seeded condition in Step 6.

Script (each line annotated with the event it produces):

```
"look",                // looked
"take wand",           // took
"go north",            // moved, into the corridor and the goblin
"attack",              // attacked + the enemy's turn
"attack",              // defeated → drops the fire grimoire   [step 1 will advance here]
"read fire grimoire",  // learned fire                          [step 2 will advance here]
"wait",                // waited — a quiet turn straight after an advance
"look",                // looked
"go up",               // failed: a latent exit is a wall with AI off
"go south",            // moved, back through a realized exit
```

The literal is **produced by running, never predicted** — the goblin's
telegraph/chip timing and the band's exact spacing are not worth deriving by
hand. Capture it with `TW_DUMP_GOLDEN=1 ./build/tests` and paste the printed
block in. Note that both golden tests dump under that variable now, so the new
one prints its own labeled header (`--- story golden session ---`) to keep the
two blocks apart.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<code>cmake --build build && ./build/tests</code> green, including the new
<code>testStoryGoldenSession</code>.
<code>git diff --stat</code> shows <b>only</b> <code>tests/tests.cpp</code> — no
<code>src/</code>, no <code>seed/</code>. Re-running
<code>TW_DUMP_GOLDEN=1 ./build/tests</code> reproduces the pasted block
character for character. Confirm the script actually defeats the goblin and
learns <code>fire</code> by checking the transcript contains
<code>"The goblin grunt falls."</code> and
<code>"and learn to cast fire"</code> — if it does not, the later advance can
never fire and the gate is worthless.
</blockquote>

### Step 2 — Schema: `SCHEMA_VERSION` 7 → 8, and the two tables
**Requirements:** REQ-ARC-STORE-1, -3, -4. **Size:** S

`src/world.hpp:13`: `SCHEMA_VERSION` 7 → 8. No migration is written; an
existing version-7 world file takes the existing `world.cpp:380` path, logs the
existing diagnostic, and throws `SchemaMismatch`.

`src/world.cpp` `SCHEMA_DDL`: add the two tables verbatim from
[REQ-ARC-STORE-3](#b-schema) and [REQ-ARC-STORE-4](#b-schema), with a comment
block, worded like the ones around it, saying why `condition_kind` and
`condition_arg` are two columns rather than one `kind:arg` token (admission
validation and evaluation are both plain SQL; the combined form is brick 2's
wire format, not a storage format).

Also extend the `events.verb` comment in the same DDL string to name
`'advanced'` alongside `'materialized'` — `testBardStoreSchema` already asserts
that comment mentions `'materialized'` (`tests/tests.cpp:10437`), so this keeps
the file's one machine-checked piece of documentation honest.

No seed content, no helpers, no call site. A world built at this step has two
empty new tables.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryStoreSchema</code>, modeled on
<code>testBardStoreSchema</code> (<code>tests.cpp:10397</code>): both tables
exist in <code>sqlite_master</code>;
<code>pragma_table_info('story_step')</code> has exactly <b>5</b> rows and the
name set <code>{n, condition_kind, condition_arg, prose, reached_turn}</code>;
<code>pragma_table_info('condition_catalog')</code> has exactly <b>3</b> and the
set <code>{kind, blurb, arg_kind}</code>; and
<code>readFileBytes("src/world.cpp")</code> contains <code>'advanced'</code>.
New <code>testStoryStoreVersionGate</code>, modeled on
<code>testBardStoreVersionGate</code> (<code>tests.cpp:10461</code>): build a
world, assert <code>schema_version == SCHEMA_VERSION</code>, stamp it to
<b>7</b>, snapshot the file bytes, assert reopening throws
<code>SchemaMismatch</code> and the file is <b>byte-identical</b> afterwards
(<b>validation item 1</b>). Whole suite green, <b>Step 1's golden literal
unchanged</b>.
</blockquote>

### Step 3 — Seed the condition vocabulary
**Requirements:** REQ-ARC-STORE-5. **Size:** S

Four `condition_catalog` rows, in **both** `seed/base.sql` and
`tests/combat_fixture.sql` (micro-decision 1), placed next to the
`motive_catalog` block in each and carrying the same "engine-owned constants,
never written at runtime, AUTHORED CONTENT PROVISIONAL pending author approval"
comment:

| kind | arg_kind | blurb (model-facing, brick 2 reads it) |
|---|---|---|
| `enemies_defeated` | `int` | when this many enemies have been put down |
| `rooms_built` | `int` | when this many new rooms have been discovered |
| `spell_learned` | `spell` | when the player has learned this spell |
| `reached_depth` | `int` | when the player has gone this many rooms deep from where they started |

No `story_step` rows yet, in either file.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryStoreConditionCatalog</code>, run
against <b>both</b> a <code>seed/base.sql</code> world and a
<code>tests/combat_fixture.sql</code> world (the second is what makes every
later admission test possible — the same divergence guard
<code>testBardStoreShippedSeedMotives</code> exists for):
<code>COUNT(*) == 4</code>; the key set is exactly the four kinds above;
<code>COUNT(*) WHERE blurb IS NULL OR blurb = ''</code> is 0;
<code>COUNT(*) WHERE arg_kind NOT IN ('int','spell')</code> is 0; and
<code>story_step</code> is empty in both. Whole suite green, golden literal
unchanged.
</blockquote>

### Step 4 — The arc: three seeded `meta` rows and `writeArc`
**Requirements:** REQ-ARC-STORE-2, -6, -9. **Size:** S

Rows, not a shape — zero DDL, the `meta.setting` precedent.

`seed/base.sql` gains one `INSERT INTO meta(key, value)` for
`arc_premise` / `arc_goal` / `arc_ending`, coherent with `seed/setting.txt`.
This is safe in the seed file because `initialize()` runs the seed SQL *before*
it inserts `schema_version` and `turn` (`world.cpp:250-256`). Draft content,
provisional pending author approval:

- **`arc_premise`** — A goblin warband has come up through a breach in
  Thornmere's foundations and is hunting the old grimoires shelved in the deep
  stacks. The school sleeps through it, and a first-night student is out of bed.
- **`arc_goal`** — The warband means to strip the deep stacks and carry the
  grimoires down through the breach before Thornmere wakes.
- **`arc_ending`** — It would be settled if the breach were sealed with the deep
  stacks still on their shelves, or if the school woke in time to seal it
  itself.

`mutations.{hpp,cpp}` gains `writeArc(db, premise, goal, ending)` — three calls
to the existing file-local `upsertMeta`, in that order. Free rewrite, like
`writeBardJournal`. **Event-free**: an arc is not something that happened.
Upserting rather than updating is what lets it work against a fixture world
where the seed never wrote the rows. Header comment in `mutations.hpp`'s
worded like the ones around it, saying that a second call replaces the values,
that it writes no event row, and `// Never begins/commits.`

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryStoreArc</code> — <b>validation
item 16</b>: against a fixture world, snapshot
<code>COUNT(*) FROM events</code>, call <code>writeArc</code>, assert three
<code>meta</code> rows exist and read back <b>byte-identical</b> to what was
passed, assert the event count is <b>unchanged</b>; call it again with different
values and assert the rows are <b>replaced</b>, not appended
(<code>COUNT(*) FROM meta WHERE key LIKE 'arc_%'</code> still 3) and the event
count is still unchanged. Plus the seed half of <b>validation item 2</b>: in a
<code>seed/base.sql</code> world, the three <code>arc_*</code> rows exist and
none is empty. Whole suite green, golden literal unchanged.
</blockquote>

### Step 5 — `writeStoryStep` and the admission gate
**Requirements:** REQ-ARC-STORE-10. **Size:** M

`mutations.{hpp,cpp}` gains
`writeStoryStep(db, n, kind, arg, prose)`. This is `writeCatalogEntry`'s motive
gate applied to conditions: **a step may not promise a condition the engine
cannot check.** Every check runs **before any write**, so a refusal leaves the
table untouched — the same ordering `writeCatalogEntry` already guarantees.

Throws `std::runtime_error` on each of:

1. `kind` has no `condition_catalog` row.
2. that row's `arg_kind` is `int` and `arg` is not a non-negative decimal
   integer (whole-string, so `"2 or 3"` is refused rather than read as 2 — the
   the `tier` check in `parseMajorProfile` at `world.cpp:329` already parses an
   integer this way — copy it).
3. that row's `arg_kind` is `spell` and `arg` has no `spell_catalog` row.
4. `prose` is empty after `trimAscii` (the existing anon-namespace helper at
   `mutations.cpp:35`).
5. `n` is already present in `story_step`.

Then one `INSERT`, storing the trimmed `prose`. **Event-free** — a step not yet
reached has not happened (the `writeCatalogEntry` / `dropGrimoire` precedent).
`reached_turn` is left NULL.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryStoreWrite</code> against a
<code>tests/combat_fixture.sql</code> world — <b>validation item 3</b>. Reusing
the existing <code>threwRuntimeError</code> helper
(<code>tests.cpp:10486</code>), assert a throw for each of: unknown kind
(<code>"phase_of_moon"</code>); <code>arg = "x"</code> on an <code>int</code>
kind; <code>arg = "-1"</code> and <code>arg = "2x"</code> on an
<code>int</code> kind; <code>arg = "levitate"</code> for
<code>spell_learned</code>; <code>prose = "   "</code>; and a duplicate
<code>n</code>. After every refusal assert
<code>COUNT(*) FROM story_step</code> is unchanged. Then one valid call: exactly
<b>one</b> new row, with <code>reached_turn IS NULL</code>, and
<code>COUNT(*) FROM events</code> <b>unchanged</b>. Whole suite green, golden
literal unchanged.
</blockquote>

### Step 6 — Seed the five story steps {#the-seed}
**Requirements:** REQ-ARC-STORE-7, -8. **Size:** S

Five rows in **`seed/base.sql` only** (micro-decision 1), hand-written, in
order, using four of the four condition kinds. Written as plain
`INSERT INTO story_step(n, condition_kind, condition_arg, prose)` — the seed is
SQL, not a helper caller, exactly as `motive_catalog` and `bestiary` are.
Authored content, provisional pending author approval.

| n | condition | prose |
|---|---|---|
| 1 | `enemies_defeated` : `1` | Word of the fight runs ahead of you. Below the stair, the warband knows the school is awake. |
| 2 | `spell_learned` : `fire` | They have set watchfires in the lower halls, and the deep stacks smell of smoke. |
| 3 | `rooms_built` : `4` | The Vigil Lamps no longer kindle in the inner corridors. Something has been at them. |
| 4 | `reached_depth` : `3` | They have found the index, and are reading it. The old grimoires are being counted. |
| 5 | `enemies_defeated` : `5` | The breach stands open to the lower halls, and the warband is carrying the deep stacks out through it. |

**Step 1's condition is `enemies_defeated:1` because of Step 1's constraint**,
not by taste: it is one of only two conditions reachable with AI disabled, and
the golden session has to reach at least one advance. Step 2's
`spell_learned:fire` is the other, and the golden script reaches it too — so
the transcript covers *two* advances and the one-per-turn rule between them.

No `reached_turn` values: [REQ-ARC-STORE-8](#c-seed-content) requires a fresh
world to be at step zero.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryStoreSeededSteps</code> against a
<code>seed/base.sql</code> world — the step half of <b>validation item 2</b>:
exactly <b>5</b> rows; <code>n</code> values exactly 1–5; at least <b>3</b>
distinct <code>condition_kind</code> values
(<code>COUNT(DISTINCT condition_kind) >= 3</code>); every
<code>condition_kind</code> has a <code>condition_catalog</code> row (a
<code>LEFT JOIN … WHERE c.kind IS NULL</code> count of 0); no empty
<code>prose</code>; and
<code>COUNT(*) WHERE reached_turn IS NOT NULL</code> is <b>0</b>. Assert
separately that a <code>tests/combat_fixture.sql</code> world still has
<b>zero</b> <code>story_step</code> rows, so the fixture stays the empty-list
world every other test runs in. Whole suite green — and the golden literal is
<b>still unchanged</b>, because nothing reads this table yet.
</blockquote>

### Step 7 — `stepConditionMet`
**Requirements:** REQ-ARC-STORE-12, -13, -14. **Size:** M

`systems.{hpp,cpp}` gains `stepConditionMet(db, kind, arg) -> bool`: a pure
read, no writes, **one query per kind**, implementing
[REQ-ARC-STORE-5](#b-schema) exactly.

- `enemies_defeated` — `SELECT COUNT(*) FROM events WHERE verb='defeated'` `>=`
  `arg`.
- `rooms_built` — the same over `verb='generated'`.
- `spell_learned` — a `known_spells` row exists for the player and `arg`.
- `reached_depth` — `distanceFromSeed(db, roomOf(db, player))` `>=` `arg`.
  Calls the public function at `combat.hpp:104`; **does not write a second
  BFS**, which is what REQ-BARD-SEL-3 forbids, and which keeps the threat
  growing with the same distance `tier` is already compared against.

An unrecognized `kind` **throws `std::runtime_error`**
([REQ-ARC-STORE-13](#e-the-condition-evaluator)), the way `moveEntity` throws
when an entity has no location row.
It cannot happen through a sanctioned path, so a silent `false` would hide an
engine bug. Header comment says exactly that.

**No condition reads `meta.turn` or any wall clock**
([REQ-ARC-STORE-14](#e-the-condition-evaluator)). This is the requirement that
keeps `bard-fact-store.md:108`'s no-clock decision intact, and it is checked
mechanically below.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryConditions</code> against a
<code>tests/combat_fixture.sql</code> world (which has rooms at depths 0, 1 and
2 — cell 1 at 0, corridor 2 and library 11 at 1, frost study 6 and armory 9 at
2). <b>Validation item 4</b>, all four kinds false-then-true via the
<b>sanctioned path</b>: <code>defeatEnemy</code> for
<code>enemies_defeated</code>; <code>writeGeneratedRoom</code> for
<code>rooms_built</code>; <code>learnSpell</code> for
<code>spell_learned</code>; <code>moveEntity</code> into room 6 for
<code>reached_depth:2</code>. <b>Validation item 5</b>: insert a
<code>story_step</code> row with a bogus kind by raw SQL, then assert
<code>stepConditionMet</code> <b>throws</b> rather than returning false
(<code>threwRuntimeError</code>). <b>Validation item 6, first half</b>, as a
function-scoped grep, not a file-wide one — a file-wide grep would fail on
correct code because <code>advanceStoryStep</code> legitimately reads
<code>meta.turn</code>:
<pre>sed -n '/^bool stepConditionMet/,/^}/p' src/systems.cpp | grep -c "meta.turn"</pre>
must print <code>0</code>, and the extracted range must be <b>non-empty</b> so
the check is not vacuous. Whole suite green, golden literal unchanged.
</blockquote>

### Step 8 — `advanceStoryStep`
**Requirements:** REQ-ARC-STORE-11, -11a, -19, -20. **Size:** M

`mutations.{hpp,cpp}` gains `advanceStoryStep(db, actor) -> bool`. The shape is
`materializeCatalogEntry`'s (`mutations.cpp:657`), copied deliberately:

1. The latch, and **the latch is the `WHERE` clause, never a prior read**:
   ```sql
   UPDATE story_step SET reached_turn = (SELECT value FROM meta WHERE key='turn')
    WHERE n = (SELECT MIN(n) FROM story_step WHERE reached_turn IS NULL)
   ```
2. `if (db.changes() == 0) return false;` — every step already reached, or the
   table is empty. Nothing is appended.
3. **Then** read the row that was just latched
   (`WHERE reached_turn = (SELECT value FROM meta WHERE key='turn')
   ORDER BY n DESC LIMIT 1`, or equivalently the `MAX(n)` non-NULL row), and
   append its event.

Point 3 is [REQ-ARC-STORE-11a](#d-write-helpers) and it is the one place a
plausible implementation goes wrong. A `SELECT` that *chooses* the row before
the `UPDATE` is forbidden: two callers inside one transaction could then latch
and describe different steps. Establish the latch first; read the latched row
after.

The event ([REQ-ARC-STORE-20](#g-the-event)): `actor` = the passed actor,
`verb` = `'advanced'`, `subject` = **0** (there is no entity — the zero-id rule
of REQ-PROSE-6), `object` = the step's `n`, `detail` = the step's `prose`.
`detail` here is a **model-facing fragment**, not an engine tag, so the row is
already the right shape for brick 3 and needs no shield in `prose.cpp`.

This helper is the **sole writer** of the `advanced` verb.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryAdvance</code> against a fixture
world with steps written through <code>writeStoryStep</code>.
<b>Validation item 7</b>: with one unreached step, call twice inside a
transaction — first returns <code>true</code> and writes exactly one
<code>advanced</code> row; second returns <code>false</code>, writes nothing,
and <code>reached_turn</code> still holds the <b>first</b> call's turn.
<b>Validation item 13</b>: that row's <code>actor</code> is the player,
<code>subject</code> is <b>0</b>, <code>object</code> is the step's
<code>n</code>, and <code>detail</code> is <b>byte-identical</b> to that step's
<code>prose</code> read back out of <code>story_step</code>.
<b>Validation item 8</b>: after every step is reached, ten further calls each
return <code>false</code>, append nothing, and <b>throw nothing</b>. Plus the
empty-table case: against a world with zero <code>story_step</code> rows, the
call returns <code>false</code> and throws nothing. Also assert the helper
appends its event only when it latched — snapshot
<code>COUNT(*) FROM events</code> across every false return. Whole suite green,
golden literal unchanged.
</blockquote>

### Step 9 — `evaluateStoryAdvance`
**Requirements:** REQ-ARC-STORE-15a, -16, -16a, -17, -19, -19a. **Size:** S

`systems.{hpp,cpp}` gains `evaluateStoryAdvance(db, actor)`, the rule that ties
the other two together. Three lines of logic:

```
read the LOWEST unreached step (ORDER BY n LIMIT 1) — its n, kind and arg
no row?  -> return   (every step reached, or the list is empty)
stepConditionMet(kind, arg) ? advanceStoryStep(db, actor) : return
```

`ORDER BY n LIMIT 1` is the requirement, not an optimization.
[REQ-ARC-STORE-16](#f-the-advance-rule): steps are an ordered list, not a set of
independent triggers, so step 3 cannot fire before step 2 even when its
condition holds. [REQ-ARC-STORE-16a](#f-the-advance-rule) makes "**at most one
condition evaluated per turn**" required rather than implied — `reached_depth`
runs a `distanceFromSeed` BFS, and scanning the unreached steps would put that
search on every turn of a growing world for a result REQ-ARC-STORE-16 would then
throw away. **The evaluator must not loop.**

The "no row" branch covers two distinct states with the same behavior:
every step reached ([REQ-ARC-STORE-19](#f-the-advance-rule)) and an
**empty** table ([REQ-ARC-STORE-19a](#f-the-advance-rule)) — the state brick 2
leaves behind when the overture fails, which is why it is required here rather
than discovered there. Neither writes anything and neither throws.

One clarification for the implementer, because the two requirements read as if
they conflict: `evaluateStoryAdvance` *does* read the lowest unreached step, and
`advanceStoryStep` then re-derives `MIN(n)` in its own `WHERE`. That is not a
duplicated read to collapse. REQ-ARC-STORE-11a is about `advanceStoryStep` being
self-contained — the row it latches and the row it describes are the same row,
whatever the caller thought.

**Nothing calls this yet.** The call site is Step 11.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryEvaluate</code>, driving
<code>evaluateStoryAdvance</code> <b>directly</b> inside a manual
<code>db.begin()</code> / <code>db.commit()</code> — no <code>runTurn</code>,
because there is no call site yet. <b>Validation item 10</b> in its direct
form: steps 1 (condition <b>false</b>) and 3 (condition <b>true</b>) present,
call ten times, assert <b>no</b> <code>advanced</code> row — an evaluator that
scanned would fire step 3. <b>Validation item 17</b> (REQ-ARC-STORE-19a), its
direct half: against a world with an empty <code>story_step</code> table, ten
calls write nothing and throw nothing — the item's "unchanged narrated output"
half needs a call site and is gated in Step 11. <b>Validation item 8</b>
(REQ-ARC-STORE-19), reached through this function rather than through
<code>advanceStoryStep</code>: same, with every step already reached. And the
happy path: with step 1's condition true, one call latches step 1 and writes one
event. Whole suite green, golden literal unchanged.
</blockquote>

### Step 10 — `advanced` is renderer-invisible, and the bard's fifth verb
**Requirements:** REQ-ARC-STORE-21, -22, -23. **Size:** S

This lands **before** the call site, so the verb can never reach a renderer or
a prompt on any turn, not even briefly.

`src/prose.cpp` — both `buildFacts` exclusion sites (micro-decision 2), each
changing `verb <> 'generated'` to `verb NOT IN ('generated','advanced')`:

- `:303`, the current-turn `events` payload key.
- `:393`, the `recent_events` payload key.

`src/render.cpp` — **no change**. The verb chain at `:117-227` falls through to
the unrecognized-verb default, which renders nothing, exactly as `generated`
does. **Brick 1 must not change one byte of narrated output**; how an advance is
told to the player is brick 3's decision, and making it here would smuggle a
design choice into a storage brick.

`src/bard.cpp:1089` `hasTriggeringEvent` — four verbs become **five**:
`verb IN ('generated','defeated','learned','materialized','advanced')`.
`kBardMinTurnGap` is unchanged. Narrowing to `advanced` alone is brick 3
([REQ-ARC-STORE-23](#h-the-bard-wake-trigger)); doing it here would stop the
cast growing between this brick and brick 3 for no gain.

**Two existing tests pin the four-verb string verbatim and will fail unless
they are updated in this same step** — `tests/tests.cpp:11944` (inside
`testNpcStoreInvariants`) and `tests/tests.cpp:13212`. Both assert
`contains(readFileBytes("src/bard.cpp"), "verb IN ('generated','defeated','learned','materialized')")`.
Update both to the five-verb string. Their surrounding assertions — that
`'said'` and `'spoke'` are absent from `bard.cpp` — stay as they are and stay
true.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testStoryRendererInvisible</code>, modeled
on the REQ-ARCH-10 test at <code>tests.cpp:8822</code> — <b>validation item
14's mechanism</b> and <b>REQ-ARC-STORE-21</b>: on a turn carrying an
<code>advanced</code> event, that verb is absent from <b>both</b>
<code>buildFacts</code> payload keys (<code>events</code> and
<code>recent_events</code>) while the turn's other verb is present, and
<code>render()</code> for that turn produces the same bytes as it does with the
<code>advanced</code> row deleted. New <code>testStoryWakeTrigger</code> —
<b>validation item 15</b>, per micro-decision 3:
<code>contains(readFileBytes("src/bard.cpp"), "verb IN ('generated','defeated','learned','materialized','advanced')")</code>,
which asserts the fifth verb <b>and</b> that the other four survived
(REQ-ARC-STORE-23); plus behaviorally, that
<code>advanceStoryStep</code> leaves an <code>advanced</code> row whose
<code>turn</code> is strictly greater than
<code>meta.bard_last_wake_turn</code> — the exact row the predicate's query
selects. The two updated pins at <code>tests.cpp:11944</code> and
<code>:13212</code> pass. Whole suite green, <b>golden literal still
unchanged</b> (nothing has advanced through the loop yet).
</blockquote>

### Step 11 — The call site in `loop.cpp`
**Requirements:** REQ-ARC-STORE-15, -15a, -16, -17, -18, -19a, -21. **Size:** S
(one line of production code, and every behavioral assertion in the brick)

One line in `runTurnCore`'s tick, after `resolveCombat(db, player, startRoom)`
and **before** `db.commit()`:

```cpp
evaluateStoryAdvance(db, player);
```

`systems.hpp` is already included. Placement is
[REQ-ARC-STORE-15](#f-the-advance-rule): inside the tick's transaction, after
systems resolve, before commit — so a step advance and the change that caused it
are **one atomic fact**, per `mutations.hpp`'s "both or neither". It runs
**once per turn**, and only on turns that actually tick: the tier-a
`renderError` path, `Verb::Spells`, and the cast-cooldown denial all return
before `db.begin()` and evaluate nothing.

The code change is one line. It is also the line that can break the golden
transcript, because this is the first time an `advanced` row exists on a turn
the player actually played. **If `kStoryGoldenSession` stops matching here,
something is printing the advance.** Find what. Do not re-capture the literal —
re-capturing it would hide exactly the bug the test exists to catch.

There is nothing to split here: one line of code, and a set of tests that mean
nothing until that line exists.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <b>Step 1's <code>testStoryGoldenSession</code>
passes byte-identically</b> — the single most important assertion in this plan
(<b>validation item 14</b>), and the run now genuinely advances two steps.
New <code>testStoryAdvanceRule</code>, driving whole turns through
<code>runTurn</code>:
<b>item 9</b> — steps 1, 2 and 3 all satisfied, one turn → <b>exactly one</b>
<code>advanced</code> row, with <code>object = 1</code>; a second qualifying
turn → <code>object = 2</code>, and still no <code>object = 3</code>.
<b>item 11</b> — from a world one event short of step 1's condition, run
<code>wait</code>, <code>look</code>, and a failing action; assert no
<code>advanced</code> row after any of them.
<b>item 6, second half</b> — 50 turns of <code>wait</code> alone leave
<code>reached_turn</code> NULL on <b>every</b> step.
<b>item 12 (atomicity)</b> — install a SQLite trigger
<code>CREATE TRIGGER … AFTER INSERT ON events WHEN NEW.verb='advanced' BEGIN SELECT RAISE(ABORT,'boom'); END;</code>
so the tick throws on the advance path (<code>Stmt::step</code> turns the
SQLite error into <code>std::runtime_error</code>, <code>db.cpp:56</code>);
assert <code>runTurn</code> returns <code>TurnOutcome::EngineError</code>,
<code>meta.turn</code> is unchanged, no <code>advanced</code> row survives,
<b>and</b> <code>reached_turn</code> is still NULL — the latch rolled back with
everything else.
<b>item 10 again, end to end</b> — the same no-scan-ahead world as Step 9
(step 1 false, step 3 true), but driven through ten real
<code>runTurn</code> calls rather than direct calls: still no
<code>advanced</code> row. The function does not change between Step 9 and
here, so this costs almost nothing; it means items 9, 10, 11 and 12 are all
checked by running turns, rather than three by running turns and one by calling
a function.
New <code>testStoryEmptyStepList</code> — <b>validation item 17</b>, its
"unchanged narrated output" half, which Step 9's direct test cannot reach.
Open a <code>seed/base.sql</code> world, <code>DELETE FROM story_step</code>,
run <b>Step 1's script verbatim</b>, and assert the transcript is
<b>byte-identical to <code>kStoryGoldenSession</code></b> — which is exactly
right, because that literal was captured in Step 1 on a tree where no step
existed at all. Assert also: no throw, and zero <code>advanced</code> rows. So
the same literal is now pinned from both directions — with the five seeded
steps present (two advances) and with the table emptied (none) — and that pair
is the strongest statement in the brick that an advance changes no narrated
byte. The pre-existing <code>testExamineGoldenSession</code> says the same
thing again at no extra cost, because it opens
<code>tests/combat_fixture.sql</code> and therefore runs against an empty
<code>story_step</code> table on every single test run.
<b>item 18</b> — <code>grep -rn "evaluateStoryAdvance" src/ | grep -v "systems\."</code>
yields <b>exactly one</b> line, in <code>loop.cpp</code>, and reading around it
confirms it sits between <code>db.begin()</code> and <code>db.commit()</code>.
Whole suite green.
</blockquote>

### Step 12 — Final validation sweep against the spec
**Requirements:** all (validation sweep). **Size:** S

Walk the spec's **AI Validation** section, items 1–18, and confirm each against
the built tree. Every item maps to a step above. If an item cannot be shown to
pass here, the implementation is missing something — the checklist is not
wrong.

| Item | Covered by | Command |
|---|---|---|
| 1 version refusal | Step 2 | `./build/tests` → `testStoryStoreVersionGate` |
| 2 what a fresh world contains | Steps 4, 6 (arc + steps), 3 (catalog) | `testStoryStoreArc`, `testStoryStoreSeededSteps`, `testStoryStoreConditionCatalog` |
| 3 what `writeStoryStep` refuses | Step 5 | `testStoryStoreWrite` |
| 4 each condition | Step 7 | `testStoryConditions` |
| 5 unknown kind throws | Step 7 | `testStoryConditions` |
| 6 no clock | Steps 7, 11 | the scoped `sed`+`grep`, and the 50-wait assertion |
| 7 latch one-way | Step 8 | `testStoryAdvance` |
| 8 exhaustion | Step 8 | `testStoryAdvance` |
| 9 one per turn | Step 11 | `testStoryAdvanceRule` |
| 10 does not scan ahead | Steps 9, 11 | `testStoryEvaluate` (direct), `testStoryAdvanceRule` (through ten real turns) |
| 11 quiet turns | Step 11 | `testStoryAdvanceRule` |
| 12 atomicity | Step 11 | `testStoryAdvanceRule` |
| 13 the event's columns | Step 8 | `testStoryAdvance` |
| 14 narrated output unchanged | Steps 1, 11 | `testStoryGoldenSession` |
| 15 wake trigger | Step 10 | `testStoryWakeTrigger` + the two updated pins — **weaker than the item asks, see micro-decision 3**: the five-verb list is checked as source text, and the `advanced` row is checked for real, but no wake is ever queued or observed |
| 16 the arc helper | Step 4 | `testStoryStoreArc` |
| 17 empty step list | Steps 9, 11 | `testStoryEvaluate` (no throw, no write), `testStoryEmptyStepList` (narrated output byte-identical to `kStoryGoldenSession`) |
| 18 one call site | Step 11 | the `grep` |

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<code>cmake -B build && cmake --build build</code> with <b>no new dependency
and no <code>CMakeLists.txt</code> change</b>;
<code>./build/tests</code> fully green;
<code>git diff --stat main</code> touches only
<code>src/world.hpp</code>, <code>src/world.cpp</code>,
<code>src/mutations.{hpp,cpp}</code>, <code>src/systems.{hpp,cpp}</code>,
<code>src/loop.cpp</code>, <code>src/prose.cpp</code>,
<code>src/bard.cpp</code>, <code>seed/base.sql</code>,
<code>tests/combat_fixture.sql</code> and <code>tests/tests.cpp</code> — no new
file anywhere in <code>src/</code>. Every one of the 18 items above passes and
names a requirement.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-ARC-STORE-1 (SCHEMA_VERSION 7→8, no migration) | 2 |
| REQ-ARC-STORE-2 (three `arc_*` meta rows) | 4 |
| REQ-ARC-STORE-3 (`story_step` table) | 2 |
| REQ-ARC-STORE-4 (`condition_catalog` table) | 2 |
| REQ-ARC-STORE-5 (the four condition kinds) | 3 (seeded), 7 (semantics) |
| REQ-ARC-STORE-6 (seeded arc for Thornmere) | 4 |
| REQ-ARC-STORE-7 (five seeded steps, ≥3 kinds) | 6 |
| REQ-ARC-STORE-8 (`reached_turn` NULL at creation) | 6 |
| REQ-ARC-STORE-9 (`writeArc`) | 4 |
| REQ-ARC-STORE-10 (`writeStoryStep` admission gate) | 5 |
| REQ-ARC-STORE-11 (`advanceStoryStep`, latch in `WHERE`) | 8 |
| REQ-ARC-STORE-11a (latch first, read after) | 8 |
| REQ-ARC-STORE-12 (`stepConditionMet`, pure read) | 7 |
| REQ-ARC-STORE-13 (unknown kind throws) | 7 |
| REQ-ARC-STORE-14 (no clock) | 7 (grep), 11 (50-wait behavior) |
| REQ-ARC-STORE-15 (once per turn, inside the tick) | 11 |
| REQ-ARC-STORE-15a (one named function, and which file each goes in) | 7, 8, 9, 11 |
| REQ-ARC-STORE-16 (lowest unreached step only) | 9 |
| REQ-ARC-STORE-16a (at most one condition evaluated) | 9 |
| REQ-ARC-STORE-17 (at most one advance per turn) | 9, 11 |
| REQ-ARC-STORE-18 (quiet turns never advance) | 11 |
| REQ-ARC-STORE-19 (exhaustion is a no-op) | 8, 9 |
| REQ-ARC-STORE-19a (empty step list is a no-op) | 9 (`testStoryEvaluate`), 11 (`testStoryEmptyStepList`) |
| REQ-ARC-STORE-20 (the event's shape) | 8 |
| REQ-ARC-STORE-21 (renderer-invisible, output unchanged) | 10 (`prose.cpp`), 1 + 11 (the golden gate) |
| REQ-ARC-STORE-22 (`advanced` as a fifth wake verb) | 10 |
| REQ-ARC-STORE-23 (narrowing is brick 3) | 10 (asserted as the five-verb list) |

All 27 requirements are covered.

**One field is deliberately written and read by nothing: `arc_ending`.** It is
stored by `writeArc` (Step 4) and seeded in `base.sql` (Step 4), and no code in
this brick consults it. That is intentional, not a gap — detecting an ending,
and doing anything about it, is named out of scope in the spec's section I, and
the question predates this feature (`ai-integration-points.md`, April). A
reviewer noticing that `arc_ending` is unused has read the code correctly.
There is nothing to fix.

**Nothing in this plan builds:** the overture writing the arc or the steps
(brick 2); the architect, narrator or bard consumers (brick 3); narrowing the
wake trigger to `advanced` alone (brick 3); ending detection; more than one list
of steps; regions; or any rewrite of `meta.setting` or a room description. If a
step starts producing work in any of those areas, it has drifted.
