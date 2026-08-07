---
title: "Implementation notes: examine — the perception verb"
date: 2026-08-07
status: complete
tags: [implementation, notes, examine, isa, perception, render, prose, resolver]
source: .lore/work/plans/examine-perception-verb.md
modules: [parser, action, systems, render, prose, nlresolve, mutations]
related: [.lore/work/specs/examine-perception-verb.md, .lore/work/design/examine-perception-verb.md]
---

# Implementation notes: examine — the perception verb

Source of truth: [the plan](../plans/examine-perception-verb.md) (8 steps) over
[the spec](../specs/examine-perception-verb.md) (29 requirements, 27 validation
items).

## Progress

<div style="font-family: ui-monospace, monospace; line-height: 1.8; padding: 8px 0;">
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">1 golden baseline</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">2 ISA + parser + resolveExamine</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">3 turn/bard/coverage</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">4 render branch</span><br>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">5 facts + prompt</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">6 clause f</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">7 resolver</span>
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 8px;border-radius:3px;">8 sweep</span>
</div>

**Suite: 8270 checks, 0 failures**, offline (`ANTHROPIC_API_KEY` unset, no
network). 9 source files changed, no new translation unit, no schema bump.

- [x] **Step 1** — golden-session baseline for every pre-existing verb
- [x] **Step 2** — the ISA verb, the parser, and `resolveExamine`
- [x] **Step 3** — turn cost, bard silence, description coverage guard
- [x] **Step 4** — the template render branch
- [x] **Step 5** — narrator facts: anchor, payload key, prompt rule
- [x] **Step 6** — clause f on the validation gate
- [x] **Step 7** — the resolver: twelfth verb, `things`, the prompt
- [x] **Step 8** — whole-feature validation sweep

## Log

### Step 1 — golden baseline (committed alone, `2f69f00`)

`testExamineGoldenSession` (`tests/tests.cpp`) runs a 17-line script over
`tests/combat_fixture.sql` through the template path and compares the whole
concatenated output to `kExamineGoldenSession`. Captured on unchanged engine
code — `git diff --stat src/` was empty at commit time — so it is a real
baseline rather than a photograph of the new behavior.

The script grew past the plan's list: after the second `attack` the goblin
falls and drops a grimoire, so two `read fire grimoire` lines were added to
cover the `learned` and `reread` branches too. `quit` stays out (no output).

Re-capture is env-gated: `TW_DUMP_GOLDEN=1 ./build/tests` prints the block to
paste back. That only ever runs when a verb's template output changes on
purpose.

### Step 2 — the ISA verb, the parser, `resolveExamine`

Four source files, one compile, exactly as planned: `Verb::Examine` in
`src/action.hpp`, the `examine` / `x` arm in `src/parser.cpp`, `resolveExamine`
+ its switch arm in `src/systems.cpp`, and the no-write-verb contract comment in
`src/mutations.hpp`.

Parser tests open a **second world from `seed/base.sql`** inside `testParser`,
because the spec's items name the candle in the dormitory cell.
`tests/fixture.sql` was left untouched — its entity and row counts are pinned
elsewhere.

Item 12 ("no component table row count changes") is asserted by reading the
table list out of `sqlite_master` and excluding only `events` / `meta`, rather
than naming component tables. A future component table is then counted without
anyone remembering to add it here.

The six-verb assertion in `testSystems` became a seven-verb assertion with
`'examined'` added, as the plan's gate calls for.

### Step 3 — cross-system guards (test-only, no source change)

`testExamineWorldGuards` holds items 14 and 10; item 15 extends the existing
quiet-verb loop in the bard trigger test with `"examined"`. `src/bard.cpp` is
untouched, which is the point of that assertion.

The coverage guard drives all five description writers in one world
(`dropGrimoire`, `placeEnemy`, `writeGeneratedRoom`, `writeCatalogEntry` +
`placeCatalogEntry`, plus the seed itself) and asserts every named entity has a
`description` row, excluding the player by `player` table membership. It also
asserts the exemption is real (the player genuinely has no description row), so
the filter can never pass vacuously.

### Step 4 — the render branch

One `examined` branch in `src/render.cpp`, a `SELECT` and nothing else, so the
unit's read-only contract holds.

Item 9's byte-equality is asserted against the seeded goblin **twice** — once at
full health, once after its health is dropped to 3 — so the output being the
description row exactly is shown to be independent of mechanical state.

Expected strings for items 5 and 6 are read out of the `description` table
rather than retyped, so the assertion is "verbatim" and not "equal to a string
someone copied correctly once".

**Coverage note (not a divergence).** Validation items 5, 6, 8 and 8a each have
two halves: scope (does the event fire?) and text (what does it say?). No text
exists until Step 4's branch, so the scope halves are asserted in `testSystems`
(Step 2) and the text halves in `testRender` (Step 4). Both halves are asserted;
they simply live in the step that can see them.

`queryText` was moved up beside `queryInt` at the top of the file, because
`testRender` now uses it and it had been defined after `testLoop`.

### Step 5 — narrator facts

`TurnFacts::examinedText` documents empty-means-inactive at the field. In
`buildFacts` the prose is attached as `e["description"]` on the `examined` event
object; the top-level payload keeps exactly its four REQ-PROSE-7 keys and the
`p.size() == 4` assertion is asserted again on an examine turn rather than
merely left alone.

One narrator prompt rule was added beside the canon-verbatim rule, and the
prompt substring test extended for it.

The clause-f line in `prose.hpp`'s validation-gate contract comment landed here,
with the field, rather than in Step 6 with the clause itself — the two steps run
back to back and the comment documents the field's purpose.

### Step 6 — clause f

One clause after clause e, plus the `a..e` → `a..f` correction in the two places
that name the order. Clauses a–e keep their behavior, order, and diagnostic
wording; item 19 is asserted by the fact that every existing canned response
above the new block is unmodified.

The clause-f diagnostic is captured through `logSetSink` at debug level, so
"the diagnostic names clause f" is an assertion rather than a claim.

**A wrong assertion, corrected.** Item 20 was first written as
`contains(runTurn(...).output, lanternProse)`. It failed — `runTurn` wraps prose
to terminal width, so a description longer than the width is broken across
lines. The pipeline is right and the assertion was wrong; it now reads
`output == wrapProse(render(...), width) + composeBand(...)`, the form the rest
of the file already uses. Worth knowing: **the display layer rewraps canon
prose**, for examine exactly as it already does for room descriptions.
`render()` itself still emits the row verbatim, which is what REQ-EXAMINE-10
and REQ-EXAMINE-23 constrain.

### Step 7 — the resolver

`verbFromWord`, the tool enum, the `things` payload key with its
`namedEntitiesIn` helper, and four prompt edits (scope-facts sentence, verb
count, the `- examine:` line, the subject-source rule).

**One thing the plan did not name.** `validateAndLower`'s verb switch in
`nlresolve.cpp` is exhaustive with no `default`, so `Verb::Examine` had to join
the take/drop/read arm — examine carries a subject and must be lowered by
`lookupNoun` like they are. Without it the verb would have reached the engine
with `subject = 0`. This is the plan's own rule ("a subject may be drawn from
`items`, `inventory`, or `things`") applied at the gate, so it was implemented
rather than escalated; recorded here because the plan's seam table did not list
that switch. Its diagnostic string widened from `take/drop/read has no subject`
to `take/drop/read/examine has no subject`; no test pinned it.

`things` excludes the room itself, because rooms have no `location` row —
which is also the mechanical reason `examine <room name>` refuses, as the spec's
out-of-scope list says it should.

Two counts in `nlresolve.hpp`/`.cpp` still say "seven ISA verbs" in comments
and one diagnostic. That drift predates this feature (the set was already
eleven) and was left alone rather than folded into this change.

### Step 8 — whole-feature sweep

All five whole-feature gates pass:

| Gate | Result |
|------|--------|
| Item 25 — no raw writes | `git diff main -- src/ \| grep -En "^\+.*(INSERT\|UPDATE\|DELETE)"` is empty |
| Item 26 — offline | 8270 checks, 0 failures with `ANTHROPIC_API_KEY` unset and no network |
| Item 27 — no schema movement | `src/world.cpp` untouched; a pre-change `world.db` (turn 15, schema_version 6) opens and plays |
| REQ-EXAMINE-29 — band | `src/band.cpp` / `src/band.hpp` untouched |
| REQ-EXAMINE-12 — `AiRole` | `src/aihttp.hpp` untouched |

Also untouched, and asserted so: `src/bard.cpp`, `src/combat.cpp`,
`src/loop.cpp`, `CMakeLists.txt`, and all three fixture/seed SQL files.

**A mistake worth recording.** The plan says of the pre-change world file:
"copy it, don't play the original." I copied it — and then played the original
anyway, because `src/main.cpp` **ignores `argv`** and always opens `./world.db`
relative to the working directory. Passing a path does nothing. The repo's
`world.db` advanced from turn 15 to 21 before I noticed; it was restored byte
for byte from the copy taken beforehand (turn 15, zero `examined` rows, same
trailing events), and the check was redone in an isolated directory holding its
own `world.db` and a symlink to `seed/`. Anyone repeating this check must `cd`
into a scratch directory, not pass a path.

The payoff is visible in that saved world: the materialized story entity
`wandering stair` — the one the README called "present and inert" — now answers
`examine`, and a character in another room correctly answers
`You don't see that here.`

## Validation-item coverage, as built

| Items | Where |
|-------|-------|
| 1–4 parser | `testParser`, second world from `seed/base.sql` |
| 5, 6, 8, 8a scope | `testSystems` (event + turn) and `testRender` (text) |
| 7 refusal + turn | `testSystems` |
| 9 byte-equality | `testRender`, asserted twice (full health and damaged) |
| 10 coverage guard | `testExamineWorldGuards` |
| 11, 12, 13 event/turn | `testSystems` |
| 14 enemy acts | `testExamineWorldGuards` |
| 15 bard quiet | the bard trigger test's quiet-verb loop |
| 16 template byte-exact | `testRender` |
| 17 other verbs byte-identical | `testExamineGoldenSession`, literal unedited since `2f69f00` |
| 18, 18a, 19 clauses | `testProseValidation` |
| 20 fallback prints | `testProseAiRender` |
| 21 enum equality | `testSpellsVerb`, twelve words |
| 22, 23 payload | `testNlResolveContext` (+ `checkPayloadHygiene`) |
| 24 regression | existing resolver tests, unmodified, still green |
| 25, 26, 27 | the Step 8 table above |
| — gate lowering | `testNlResolveGate` (new `Verb::Examine` arm) |
