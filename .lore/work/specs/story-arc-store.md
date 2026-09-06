---
title: "Story arc store: the arc, the steps, and the advance rule"
date: 2026-09-06
status: draft
tags: [bard, story-arc, steps, advance-rule, schema, mutations, append-only, ledger, condition-vocabulary, evolving-setting]
modules: [world, mutations, loop, bard, seed]
related: [.lore/work/brainstorm/story-arc-and-evolving-setting.md, .lore/work/design/bard-fact-store.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/bard-overture-and-scheduling.md, .lore/work/brainstorm/dungeon-master.md, .lore/vision.md]
req-prefix: ARC-STORE
---

# Story arc store: the arc, the steps, and the advance rule

Brick 1 of the story arc feature. **Contains no AI call, no network, and no new
translation unit** — it is schema, seed content, mutation helpers, and one
evaluator. Every requirement below is verifiable with SQL and the existing test
binary; nothing here needs a transport, a canned response, or an API key.

This is the brick that fills the hole found in
[the brainstorm](../brainstorm/story-arc-and-evolving-setting.md): the arc, the
antagonist force, and the ending were designed in `dungeon-master.md` §13 and
never built, because the first bard design was scoped as a schema and an arc is
not a row shape.

It follows the method of [bard-fact-store](bard-fact-store.md) deliberately, and
reuses three of its patterns without change: a closed vocabulary seeded as a
table, admission validation in the write helper, and a one-way latch expressed as
a `WHERE` clause rather than a prior read.

## A. Context

The bard authors what could happen; the engine decides when it has. Today the
first half is a cast list and the second half does not exist.

A **step** is one entry in a short ordered list of how the threat gets closer.
Each carries a **condition** the engine can check and a line of **prose**
describing the world once that step is reached. The bard writes them; the engine
walks them.

**Boundary declaration (vision principle 1).** The prose of each step and the
choice of which condition guards it are AI-driven — flavor and selection from a
closed set, with no mechanical consequence at write time. Which step is current,
whether a condition holds, when an advance fires, and the one-way latch are
deterministic (engine). The bard never advances anything and is never told to.

**No clock, still.** `bard-fact-store.md:108` killed the clock deliberately —
*"there is no clock column and no urgency column, and that absence is
deliberate"* — because a timer converts Thornmere's spatial escalation into the
flood `seed/setting.txt` explicitly rejects. Nothing here is time-based. No
condition reads `meta.turn`, and waiting cannot advance a step
([REQ-ARC-STORE-18](#the-advance-rule)).

**A refinement of the brainstorm's phrasing, stated openly.** The brainstorm said
steps advance on *irreversible player events*. Two of the four conditions —
`reached_depth` most obviously — can be momentarily true and then false again if
the player walks back. What makes an advance irreversible is not the condition but
**the latch**: `reached_turn` is set once and never cleared, so the step means
*"this was true at least once"*. That is a strictly simpler rule than gating
evaluation on a verb list, and it keeps the no-clock property intact.

## B. Schema

**REQ-ARC-STORE-1.** `SCHEMA_VERSION` is raised from 7 to 8. An existing world
file at version 7 produces the current `SchemaMismatch` diagnostic and refuses to
open; no migration is written. (The `bard-fact-store` precedent, REQ-BARD-STORE-1.)

**REQ-ARC-STORE-2.** The arc is three new rows in `meta` — `arc_premise`,
`arc_goal`, `arc_ending` — each a freeform string. New **rows**, not a new
**shape**: zero DDL, following the `meta.setting` precedent. They are separate
rows rather than one blob so that a later consumer can send the architect the
premise without the ending leaking into every room build.

**REQ-ARC-STORE-3.** A new table holds the ordered list:

```sql
CREATE TABLE story_step(
  n              INTEGER PRIMARY KEY,  -- 1-based; the list is walked in this order
  condition_kind TEXT NOT NULL,        -- condition_catalog.kind (closed vocabulary)
  condition_arg  TEXT NOT NULL,        -- shape governed by condition_catalog.arg_kind
  prose          TEXT NOT NULL,        -- what the world looks like once reached
  reached_turn   INTEGER               -- NULL = not yet reached; set once, never cleared
);
```

`condition_kind` and `condition_arg` are separate columns rather than one
`kind:arg` token, so admission validation and evaluation are both plain SQL. The
combined `kind:arg` form is a wire format for brick 2, not a storage format.

**REQ-ARC-STORE-4.** A new table holds the closed condition vocabulary,
engine-owned constants seeded exactly like `motive_catalog` and never written at
runtime:

```sql
CREATE TABLE condition_catalog(
  kind     TEXT PRIMARY KEY,
  blurb    TEXT NOT NULL,   -- the model-facing description; brick 2 reads this
  arg_kind TEXT NOT NULL    -- 'int' | 'spell'
);
```

**REQ-ARC-STORE-5.** `condition_catalog` is seeded with exactly four kinds:

| kind | arg_kind | Holds when |
|---|---|---|
| `enemies_defeated` | `int` | the count of `defeated` events is >= the argument |
| `rooms_built` | `int` | the count of `generated` events is >= the argument |
| `spell_learned` | `spell` | the player has a `known_spells` row for that spell |
| `reached_depth` | `int` | `distanceFromSeed` of the player's room is >= the argument |

`reached_depth` reuses the spatial metric `tier` already gates on
(`combat.cpp:284`), so escalation stays spatial rather than inventing a second
measure of progress.

## C. Seed content

**REQ-ARC-STORE-6.** `seed/base.sql` seeds the three arc rows for Thornmere Hall,
coherent with `seed/setting.txt`. Authored content, provisional pending author
approval — the `motive_catalog` convention.

**REQ-ARC-STORE-7.** `seed/base.sql` seeds a list of **five** story steps, in
order, hand-written, using at least three of the four condition kinds. Hand
authorship is the point of this brick: it makes the whole advance rule testable
with SQL and no API key, and brick 2 replaces the content without changing the
schema.

**REQ-ARC-STORE-8.** Every seeded step has `reached_turn` NULL at world creation.
A freshly initialized world is at step zero — nothing reached.

## D. Write helpers

All three live in `mutations.{hpp,cpp}` and follow that file's stated contract:
they never begin, commit, or roll back, and systems code never writes these
tables directly.

**REQ-ARC-STORE-9.** `writeArc(db, premise, goal, ending)` upserts the three
`meta` rows. Free rewrite, like `writeBardJournal`. Event-free — an arc is not
something that happened.

**REQ-ARC-STORE-10.** `writeStoryStep(db, n, kind, arg, prose)` inserts one row
and **validates at admission**, throwing `std::runtime_error` on any of:

- `kind` absent from `condition_catalog`
- `arg_kind` is `int` and `arg` is not a non-negative decimal integer
- `arg_kind` is `spell` and `arg` is absent from `spell_catalog`
- `prose` empty after trimming
- `n` already present

This is `writeCatalogEntry`'s motive gate applied to conditions: **a step may not
promise a condition the engine cannot check.** Event-free — a step not yet
reached has not happened.

**REQ-ARC-STORE-11.** `advanceStoryStep(db, actor)` → `bool` latches the lowest
unreached step and appends its event, both in the caller's ambient transaction:

```sql
UPDATE story_step SET reached_turn = (SELECT value FROM meta WHERE key='turn')
 WHERE n = (SELECT MIN(n) FROM story_step WHERE reached_turn IS NULL)
```

The latch is the `WHERE` clause, never a prior read (the
`materializeCatalogEntry` shape, REQ-BARD-STORE-13). It returns `false` and
appends nothing when every step is already reached. It is the **sole writer** of
the `advanced` verb.

**REQ-ARC-STORE-11a.** The helper returns `true` if and only if it latched
exactly one row, and the event it appends describes **that same row** — the `n`
and `prose` of the step just latched, never of a row read beforehand. The
implementation therefore establishes the latch first and reads the latched row
afterwards (`sqlite3_changes()` plus a read, or `RETURNING`); a `SELECT` that
chooses the row before the `UPDATE` is what
[REQ-ARC-STORE-11](#d-write-helpers) forbids, because two callers in the same
transaction could then latch and describe different steps.

## E. The condition evaluator

**REQ-ARC-STORE-12.** `stepConditionMet(db, kind, arg)` → `bool` is a pure read —
no writes, no mutation, one query per kind — implementing the four semantics in
[REQ-ARC-STORE-5](#b-schema) exactly.

**REQ-ARC-STORE-13.** An unrecognized `kind` throws `std::runtime_error`. It
cannot occur through any sanctioned path — [REQ-ARC-STORE-10](#d-write-helpers)
rejects it at admission and [REQ-ARC-STORE-1](#b-schema) refuses world files from
an earlier vocabulary — so reaching it is an engine error, and a silent `false`
would hide a real bug. The `moveEntity` discipline.

**REQ-ARC-STORE-14.** No condition reads `meta.turn` or any wall clock. This is
mechanically checkable and is the requirement that keeps
`bard-fact-store.md:108`'s decision intact.

## F. The advance rule

**REQ-ARC-STORE-15.** The rule runs **once per turn**, inside the tick's
transaction, after systems resolve and before commit — so a step advance and the
change that caused it are one atomic fact, per `mutations.hpp`'s "both or
neither".

**REQ-ARC-STORE-15a.** The rule is one named function, `evaluateStoryAdvance(db,
actor)`, called once per tick from `loop.cpp`. It reads the lowest unreached
step, calls `stepConditionMet`, and calls `advanceStoryStep` when that holds.

File discipline, so no new translation unit is needed and each piece sits with
its own kind of work:

| Piece | Home | Why |
|---|---|---|
| `advanceStoryStep` | `mutations.{hpp,cpp}` | it writes, and that file is the only sanctioned write path |
| `stepConditionMet` | `systems.{hpp,cpp}` | a pure rule over world state |
| `evaluateStoryAdvance` | `systems.{hpp,cpp}` | the rule that ties them together |
| the call site | `loop.cpp` | the tick owns the transaction |

**REQ-ARC-STORE-16.** It evaluates **only the lowest-numbered unreached step**.
Steps are an ordered list, not a set of independent triggers; step 3 cannot fire
before step 2 even if its condition holds.

**REQ-ARC-STORE-16a.** It follows that **at most one condition is evaluated per
turn**, and this is required rather than merely implied. The evaluator must not
scan the unreached steps looking for a satisfied one. `reached_depth` runs a
`distanceFromSeed` breadth-first search (`combat.cpp:284`); evaluating every
unreached step would put that search on every turn of a growing world, for a
result that [REQ-ARC-STORE-16](#f-the-advance-rule) would then discard.

**REQ-ARC-STORE-17.** **At most one step advances per turn.** When a step
advances and the next step's condition is already true, the next advance waits
for a later turn. This bounds the rule to one event in, at most one event out.

**REQ-ARC-STORE-18.** A turn whose only event is `waited`, `looked`, or `failed`
never advances a step. This follows from the four conditions rather than from a
verb filter — none of them can change on such a turn — and is asserted as
behavior, not as an implementation detail.

**REQ-ARC-STORE-19.** When every step is reached the rule is a no-op each turn.
Exhaustion is a terminal state and nothing extra is written. **Detecting an
ending, and doing anything about it, is out of scope** (see §I).

**REQ-ARC-STORE-19a.** A world whose `story_step` table is **empty** behaves the
same way: `evaluateStoryAdvance` is a no-op every turn, writes nothing, and
throws nothing. This is a distinct state from "every step reached", and it is the
state brick 2 leaves behind when the overture fails — so it is required here
rather than discovered there.

## G. The event

**REQ-ARC-STORE-20.** The `advanced` verb is written with `actor` = the player,
`subject` = 0 (there is no entity — the zero-id rule of REQ-PROSE-6), `object` =
the step's `n`, and `detail` = the step's `prose`. `prose` is a model-facing
fragment, not an engine tag, so the row is already the right shape for brick 3.

**REQ-ARC-STORE-21.** `advanced` is **renderer-invisible in this brick**. It is
excluded from `buildFacts` exactly as `generated` is (`prose.cpp:303`,
REQ-ARCH-10), and `render.cpp` gains no branch for it. **Brick 1 must not change
one byte of narrated output** — how an advance is told to the player is brick 3's
decision, and making it here would smuggle a design choice into a storage brick.

## H. The bard wake trigger

**REQ-ARC-STORE-22.** `hasTriggeringEvent` (`bard.cpp:1089`) gains `'advanced'`,
giving five verbs: `generated`, `defeated`, `learned`, `materialized`,
`advanced`. `kBardMinTurnGap` is unchanged.

**REQ-ARC-STORE-23.** Narrowing the trigger to `advanced` alone — the end state
decided in the brainstorm — is **brick 3**, deliberately. Doing it here would stop
the cast growing between this brick and brick 3, a regression lived with for the
length of the build for no gain.

## I. Out of scope (explicit deferrals)

- **The overture writing the arc and the steps.** Brick 2. Everything here is
  hand-authored in `seed/base.sql`.
- **Consumers.** The architect seeing the current step, the narrator telling an
  advance, the bard reading its own history on wake. Brick 3.
- **Narrowing the wake trigger** to `advanced` alone. Brick 3
  ([REQ-ARC-STORE-23](#h-the-bard-wake-trigger)).
- **Detecting or acting on an ending.** `arc_ending` is stored and read by
  nothing. Whether the engine can recognize a finished story is genuinely
  unanswered and predates this feature (`ai-integration-points.md`, April).
- **More than one list of steps.** The emergence argument in the brainstorm needs
  several running at once. The schema does not forbid it — `story_step` could
  gain a list id later — but one list is what this brick builds and the honest
  expectation is that it feels like a fuse, not a world.
- **Regions.** A world unfolding through space that does not exist yet. Parked in
  the brainstorm; the step's own prose carries the geography instead.
- **Rewriting anything.** `meta.setting` is untouched, and no room description
  changes. The evolving setting arrives in brick 3 as *newly built rooms knowing
  the current step*.

## AI Validation

How the AI verifies completion, behaviorally. Each item names the requirements it
covers. All of it runs against `./build/tests` and `sqlite3`; none of it needs an
API key.

1. **Version refusal** (1). Open a world file stamped `schema_version = 7`;
   assert the `SchemaMismatch` diagnostic and that the process refuses to open it.
2. **Fresh world shape** (2, 3, 4, 6, 7, 8). Create a world; assert via SQL that
   the three `arc_*` meta rows are non-empty, `condition_catalog` holds exactly
   four kinds, `story_step` holds five rows numbered 1–5 using at least three
   distinct kinds, and `SELECT COUNT(*) FROM story_step WHERE reached_turn IS NOT
   NULL` is 0.
3. **Admission gate** (10). Assert `writeStoryStep` throws on each of: an unknown
   kind, `arg = "x"` for an `int` kind, `arg = "levitate"` for `spell_learned`,
   empty prose, a duplicate `n`. Assert a valid call inserts exactly one row and
   appends **no** event row.
4. **Each condition** (5, 12). For all four kinds, build a world where the
   condition is false, assert `stepConditionMet` is false; make it true by the
   sanctioned path (defeat an enemy, generate a room, `learnSpell`, move the
   player to a room at the required depth) and assert it flips to true.
5. **Unknown kind throws** (13). Insert a `story_step` row with a bogus kind by
   raw SQL, then assert `stepConditionMet` throws rather than returning false.
6. **No clock** (14). `grep` **`stepConditionMet` alone** for `meta.turn` and
   assert no match — scoped to that function, because `advanceStoryStep`
   legitimately reads `meta.turn` to stamp `reached_turn`, and a file-wide grep
   would fail on correct code. Then assert that advancing 50 turns with `wait`
   alone leaves `reached_turn` NULL on every step.
7. **Latch is one-way and fires once** (11, 11a). Call `advanceStoryStep` twice against
   a world with one unreached step; assert the first returns true and writes one
   `advanced` row, the second returns false and writes nothing, and
   `reached_turn` still holds the first call's turn.
8. **Exhaustion** (19). Advance past the last step; assert `advanceStoryStep`
   returns false, appends nothing, and throws nothing on the following ten turns.
9. **One per turn** (16, 17). Construct a world where steps 1, 2 and 3 all have
   satisfied conditions; run one turn; assert **exactly one** `advanced` row
   exists and it carries `object = 1`. Run a second qualifying turn; assert
   `object = 2`.
10. **Does not scan ahead** (16, 16a). Construct a world where step 1's condition
    is **false** and step 3's is **true**; run ten turns; assert no `advanced` row
    was written. This is the behavioral form of "at most one condition evaluated":
    an evaluator that scanned would fire step 3.
11. **Quiet turns** (18). From a world one event short of a step's condition, run
    `wait`, `look`, and an action that fails; assert no `advanced` row after any
    of them.
12. **Atomicity** (15). Force a throw after the advance but before the tick
    commits; assert the transaction rolled back and no `advanced` row survives.
13. **Event shape** (20). Assert the row's `actor` is the player, `subject` is 0,
    `object` is the step number, and `detail` is byte-identical to that step's
    `prose`.
14. **Narrated output is unchanged** (21). Record a golden session against the
    seeded world *before* the change, following the `kExamineGoldenSession`
    pattern (`tests/tests.cpp:2125`, `TW_DUMP_GOLDEN=1`), driving it far enough
    that at least one step advances. After the change, assert the transcript is
    **byte-identical**. This is the brick's non-regression gate and the single
    most important item in this list.
15. **The wake trigger fires** (22, 23). With a world whose bard is idle, cause one
    step to advance and assert a wake is queued — and assert the other four verbs
    still trigger, so nothing was narrowed early (23).
16. **The arc helper** (9). Assert `writeArc` writes all three `meta` rows; that a
    second call **replaces** rather than appends; that it appends no event row;
    and that reading back returns the three values byte-identical to what was
    written. Distinct from item 2, which tests `seed/base.sql` rather than the
    helper.
17. **Empty step list** (19a). Delete every `story_step` row from a live world;
    run ten turns of mixed actions; assert no throw, no `advanced` row, and
    unchanged narrated output.
18. **The rule has one call site** (15a). Assert `evaluateStoryAdvance` is called
    exactly once per tick — `grep` for its call sites and assert a single one, in
    `loop.cpp`, inside the tick's transaction.
