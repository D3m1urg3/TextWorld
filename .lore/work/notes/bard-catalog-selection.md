---
title: "Implementation notes: bard-catalog-selection"
date: 2026-08-03
status: complete
tags: [notes, bard, dungeon-master, eligibility, tool-use, validation-gate, wire-format]
modules: [bard, combat, mutations]
related: [.lore/work/plans/bard-catalog-selection.md, .lore/work/specs/bard-catalog-selection.md, .lore/work/notes/bard-fact-store.md]
---

# Implementation notes: bard-catalog-selection

Brick 2 of the bard feature, implemented from
[the plan](../plans/bard-catalog-selection.md) (15 steps) against
[the spec](../specs/bard-catalog-selection.md) (24 requirements, `BARD-SEL`).

**Orchestration note.** The `/implement` skill dispatches each phase to sub-agents.
This run is inline instead, per the standing preference recorded in memory
(`no-implement-subagents`): sub-agent dispatch for implementation work is a token
sink on this project. Phases, validation gates, and the notes discipline are
otherwise followed as written. No task files exist under
`.lore/work/tasks/bard-catalog-selection/`, so the plan's steps are the phases.

Baseline before Step 1: `cmake --build build` clean, `./build/tests` → **5506 checks,
0 failures**.

## Progress

- [x] 1 — unit skeleton, CMake wire-in, two combat exports
- [x] 2 — `eligibleCatalog`: gates (a), (b), (c), ordering
- [x] 3 — gate (d): knowledge beats need a live subject
- [x] 4 — `eligibleCatalogForNewRoom`
- [x] 5 — `catalogForHandle` / `catalogIdForHandle`
- [x] 6 — `buildOvertureContext`
- [x] 7 — `buildWakeContext`, event lines, `kBardWakeEventLimit`
- [x] 8 — the two system prompts
- [x] 9 — `buildOvertureRequestBody` + `write_catalog` schema
- [x] 10 — `buildWakeRequestBody` + the four wake tools
- [x] 11 — `validateOvertureResponse`
- [x] 12 — `validateWakeResponse`
- [x] 13 — admission (13a overture, 13b wake)
- [x] 14 — unit-contract guards as tests
- [x] 15 — final validation against the spec

## Log

### Step 1 — skeleton, CMake, combat exports ✅ (5518 checks, 0 failures)

`src/bard.hpp` + `src/bard.cpp` created, appended to `twcore`. `distanceFromSeed`
and `eligibleArchetypesForNewRoom` moved out of `combat.cpp`'s anonymous namespace
(declaration move only, bodies untouched) and declared in `combat.hpp`; the whole
existing suite stayed green, which is the plan's stated test that the move changed
no behavior. `testBardSelExports` pins distance 0/1/3 on the fixture, `INT64_MAX`
for an isolated room, and archetype↔blurb agreement with `eligibleEnemyBlurbs`.

**Discovery, acted on.** The unit-contract comment I first wrote in `bard.cpp`
spelled out "no INSERT, UPDATE, or DELETE" — which makes the spec's own mechanical
guard (`grep -En "INSERT|UPDATE|DELETE" src/bard.cpp` must be empty) fail on a
comment. `architect.cpp` avoids this by keeping the verbs in `architect.hpp` and
paraphrasing in the `.cpp`. Same fix applied: the full contract lives in
`bard.hpp`, and `bard.cpp`'s header says the verbs appear nowhere below, not even
in a comment. Grep is now empty (count 0).

### Steps 2–3 — `eligibleCatalog`, gates (a)–(d) ✅ (5550 checks, 0 failures)

One `LEFT JOIN` read (`catalogRows`) supplies gates (a) and (c) plus id ordering;
(b) and (d) are applied in code. Gate (b) guards the `INT64_MAX` sentinel *before*
comparing, per plan micro-decision 3 — otherwise an unreachable room would offer
every entry at every tier. Gate (d) unions `eligibleArchetypes` over the room and
**every** realized neighbor (micro-decision 4), computed lazily and at most once
per call, so a factless catalog pays nothing.

**Fixture constraint worth recording.** The truth gate requires a `resistance` row
for the fact's (archetype, element) pair, and `combat_fixture.sql` has resistance
rows for `rime_touched` only. So every *writable* knowledge beat in tests is about
`rime_touched` — and `rime_touched` is not in the bootstrap menu. `testBardSelEligibleFact`
therefore takes the world out of bootstrap (`architect_spawn_count = 1`) and teaches
the player `fire` before gate (d) can ever pass. It then adds room 21 one hop past
the outer hall and **rewrites room 15's exits so the empty neighbor is scanned
first** — that ordering is what makes the union assertion catch a
first-neighbor-only implementation.

### Step 4 — `eligibleCatalogForNewRoom` ✅ (5557 checks, 0 failures)

Both kinds in one call (micro-decision 5), origin distance +1 with the overflow
guard copied from combat. The test seeds tiers 0/1/2 from the seed room, so the
origin's own menu stops one tier short of the prospective one — a menu built
against the wrong distance cannot pass.

### Step 5 — `catalogForHandle` / `catalogIdForHandle` ✅ (5569 checks, 0 failures)

The stale-snapshot case is driven for real: the menu is captured, the entry is
materialized, and the same handle then resolves to 0 — while `catalogIdForHandle`
still returns non-zero for it and for a tier-9 entry that is ineligible everywhere.

### Steps 6–7 — the two context builders ✅ (5596, then 5864 checks, 0 failures)

Overture: exactly `{setting, motives}`. Wake: exactly
`{setting, motives, journal, events, catalog}`, with the event lines passed
through the narrator's tag shield (subject name-resolved, `object` dropped
entirely, `detail` withheld on `burned`/`froze`/`materialized`) and the catalog
carried whole rather than as the eligible menu.

**Test failure worth keeping.** The first run failed two assertions — the
`materialized` event never reached the payload, and with it the subject name.
Cause: the wake carries events with `turn > meta.bard_last_wake_turn`, a fresh
fixture sits at turn **0**, and `materializeCatalogEntry` stamps its event with
`meta.turn`. So a turn-0 event can never qualify while `bard_last_wake_turn` is 0.
That is correct behavior, not a bug — the test now advances `meta.turn` first.
Anyone writing a Brick 3 test against a fresh world will hit this same edge.

The no-ids sweep uses ids well clear of the single-digit trap (room 4321, entity
5000) so a literal substring search means something; the structural half asserts
each catalog element has exactly its four model-facing keys and none of
`id`/`tier`/`seeded`/`entity`.

### Step 8 — the two prompts ✅ (5895 checks, 0 failures)

Both carry the situations-not-urgency clause verbatim, so one substring test
covers both. Written once and left alone, per the plan's MED token-risk note — no
rewrite-and-rerun loop.

**Plan gate amended, and why.** Step 8's gate asks that "neither contains the
words 'id' or 'tier' as a thing to emit", but Step 8's own body requires the
overture prompt to state what `tier` means ("how deep from the start a thing
belongs, not how dangerous it is") — and `tier` is a required field of the tool
schema, so the model must be told how to fill it. The two halves of the gate
cannot both hold. Implemented the body: the test asserts each prompt carries the
identifier prohibition ("Invent no numbers or identifiers"), and that `tier` is
described as depth ("Tier is distance, not danger" / "tier is DEPTH") rather than
danger. REQ-BARD-SEL-8 is about the WIRE, and it is asserted where it belongs —
on the payloads and bodies, which carry no tier value.

### Steps 9–10 — the request bodies ✅ (5928, then 5955 checks, 0 failures)

`motiveKeys(Db&)` + a vocabulary parameter (micro-decision 1) keeps both builders
pure string→string while the enum stays DB-driven; the ninth-motive test drives it
through the **emitted body**, not the reader. One file-local `catalogEntrySchema`
serves both `write_catalog.entries[]` and `append_catalog.entry`, and the test
compares the two serialized sub-objects for byte equality — the real anti-drift
fence. `max_tokens` 4096 for the overture, 1024 for the wake; `tool_choice` auto
on both.

### Steps 11–12 — the two pure gates ✅ (6054 checks, 0 failures)

Both share one exception-free parse (`contentArray`) and one per-entry reader
(`readEntry`), so leniency cannot diverge between the overture and a wake's
`append_catalog`. Every gate case in the tests is called with no exception
handler around it — that is the assertion that they never throw.

The wake's zero-tool case is tested with **stderr captured** (a `freopen`/`dup2`
helper, new — the suite had no such helper before) and asserted **empty**: a bard
declining to act must not even look like a failure in the log.

**One judgment call.** A response carrying *two* `write_catalog` blocks is not a
rejection — REQ-BARD-SEL-18 lists the only three clauses that reject in full, and
this is not among them. The first call is used, the extra is noted on stderr.
(The architect rejects on ≥2, but its tool is *required*; the bard's is `auto`,
and an overture that produced something is worth keeping.)

### Step 13 — admission ✅ (6115 checks, 0 failures)

The read-only pre-flight of micro-decision 6a, with no exception handler anywhere
in either admission function. The equivalence guard drives `catalogEntryRefusal`
and a direct `writeCatalogEntry` from one loop over twelve cases — every argument
refusal, all three truth-gate clauses, a duplicate handle, and one **valid** entry
(the other half of "iff") — asserting `refused == threw` each time.

Two entries sharing a handle inside one overture admit exactly one and do **not**
throw, which is the case a `catch` would have made indistinguishable from a disk
fault. `mark_seeded` on an entry that is ineligible everywhere still sets
`seeded = 1`, which is what catches `catalogForHandle` being wired in place of
`catalogIdForHandle`.

### Step 14 — contract guards ✅ (6166 checks, 0 failures)

**Two guards failed on their first run, both for the same reason as Step 1's:**
my own comments contained the tokens the guards grep for — `try/catch` in
`bard.cpp`, and `place_catalog` in `bard.hpp`'s header comment. Reworded both
("no exception handler", "no room-placement tool"). This is now the third
instance of the same trap, and it is worth stating plainly for Bricks 3–4:

> **A prose comment that names a forbidden token defeats the grep that forbids
> it.** When a requirement is enforced by a literal source-text search, the
> explanation of that requirement has to live in the *header* (which is not
> swept) or be paraphrased.

The guard also asserts the four write helpers *are* called from `bard.cpp`, so
the "no write verbs" check cannot pass vacuously.

### Step 15 — final validation ✅

1. `cmake --build build` clean; whole suite **6170 checks, 0 failures** (baseline
   5506 → +664). Zero regressions in combat, architect, pregen, band, or the
   Brick-1 `testBardStore*` block — the Step-1 combat export was the only
   plausible source of one.
2. All fourteen `testBardSel*` functions registered in `main()` beside the
   `testBardStore*` calls.
3. The spec's four mechanical greps run by hand and agree with the encoded
   guards: `INSERT|UPDATE|DELETE` in `src/bard.cpp` → empty; `place_catalog`
   under `src/` → empty; the urgency clause present in both prompts; build clean.
4. The game launches and plays several turns unchanged (`look`, `inventory`,
   `north`, `look`) — nothing bard-related is reachable at runtime, since nothing
   calls into `bard.cpp` yet. (The one `aiRender: HTTP status is not 200` line in
   that run is the sandbox having no network; it degrades to template mode, which
   is pre-existing behavior.)
5. `git diff --stat` touches only `CMakeLists.txt`, `src/combat.*`, and
   `tests/tests.cpp`, plus the new untracked `src/bard.hpp` / `src/bard.cpp`. The
   spec `.md` modification predates this work (it is micro-decision 6b's
   amendment, already applied).

**Spec AI-Validation walk.** All four mechanical checks and all fifteen
behavioral tests (5, 6, 7, 8, 9, 10, 11, 11a, 11b, 12, 13, 14, 15) have a passing
assertion. Coverage-map walk: every one of the 24 `BARD-SEL` requirements is
asserted somewhere, plus micro-decision 7's `kBardWakeEventLimit`.

## Divergences from the plan

Two, both minor, both recorded above in place:

1. **Step 8's validation gate is internally inconsistent** on `tier` — it asks
   that neither prompt contain "tier" as a thing to emit, while Step 8's own body
   requires the overture prompt to explain what `tier` means, and the tool schema
   makes it a required field. Implemented the body; the test asserts the
   identifier prohibition and that `tier` is described as depth rather than
   danger. REQ-BARD-SEL-8 is asserted where it actually applies — on the payloads
   and bodies, which carry no tier value.
2. **A second `write_catalog` block is not a rejection** (Step 11), for the
   reason given above.

Neither changes a requirement or the shape of any function the plan specified.

## Where this leaves Brick 3

Nothing here spawns a thread, constructs a transport, or touches `main.cpp` — the
brick is inert at runtime by construction. The seams Brick 3 picks up:
`buildOvertureContext` / `buildWakeContext` → `buildOvertureRequestBody` /
`buildWakeRequestBody` (both taking `motiveKeys(db)`) → transport →
`validateOvertureResponse` / `validateWakeResponse` → `admitOvertureProposal` /
`applyWakeProposal` inside a transaction Brick 3 opens and commits. The
transaction shape is already asserted here: a `db.rollback()` around an admission
leaves zero rows.
