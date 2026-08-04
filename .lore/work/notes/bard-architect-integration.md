---
title: "Implementation notes: bard-architect-integration"
date: 2026-08-04
status: complete
tags: [implementation, notes, bard, architect, materialization, world-gen, nouns-must-exist, catalog, pregen, non-regression]
source: .lore/work/plans/bard-architect-integration.md
modules: [architect, bard, mutations, pregen]
related: [.lore/work/specs/bard-architect-integration.md, .lore/work/design/bard-architect-integration.md, .lore/work/plans/bard-catalog-selection.md, .lore/work/notes/bard-overture-and-scheduling.md]
---

# Implementation notes: bard-architect-integration

Brick 4 of 4. Source plan: [bard-architect-integration](../plans/bard-architect-integration.md), 10 steps.

**Baseline before any edit:** build clean; `./build/tests` → **6683 checks, 0 failures**.

**Method note.** The `/implement` skill prescribes sub-agent orchestration. Run
inline instead, per the standing `no-implement-subagents` preference. The plan's
per-step validation gates are the discipline in place of reviewer agents; the
final audit (Step 10) is the holistic check.

## Progress

- [x] 1 — `StoryProposal` on `RoomProposal` — 6683 checks
- [x] 2 — Context: `focus` + `story_options`, retired O(1) claim — 6739
- [x] 3 — The optional `story` tool field — 6767
- [x] 4 — The story clause in `kArchitectPrompt` — 6778
- [x] 5 — Lenient `story` extraction at the gate — 6846
- [x] 6 — Materialization in `architectCommitProposal` — 6948
- [x] 7 — Sync path: thread the menu through `architectGenerate` — 6992
- [x] 8 — Pregen path: snapshot the handles, hoist the menu — 7022
- [x] 9 — Nouns must exist, as provable today — 7038
- [x] 10 — Final validation & contract audit — all 4 mechanical checks pass

## Log

**Step 1.** `StoryProposal` + `#include "bard.hpp"` in `architect.hpp`. Build
clean, suite unchanged at 6683, no test file edited. clangd flagged `bard.hpp` as
an unused include for one step — expected, Step 2's signature consumes it.

**Step 2.** `bardFocusText()` beside `settingText`; `buildArchitectContext` gains
a defaulted 4th parameter; header doc comment rewritten.

- **Caught by the gate, not by review:** the first draft of the replacement doc
  comment quoted the retired claim verbatim (`The old "O(1) in world size" claim
  is retired`), so mechanical check 4's grep still matched — in the very comment
  announcing the retirement. Reworded to
  `constant-cost-in-world-size claim this comment used to make is RETIRED`, and
  the comment now says the phrase is gone *so a grep can prove it*. Worth
  recording because the check is a literal string search: any future comment
  that quotes the old wording re-breaks it.
- `testArchitectContext` passed unmodified — its `j.size() == 4` on a
  catalog-free, focus-free fixture **is** spec test 5's byte-identity proof.
  Note that `world.cpp:156` seeds `bard_focus` to `''`, so the omit-when-empty
  branch is the one every pre-bard world takes.

**Step 3.** `storyHandles` on `buildArchitectRequestBody`, mirroring the enemy
block. `testArchitectRequestBody` passed unmodified, `properties.size() == 3`
intact.

- **Scoping decision inside the new test.** Spec test 6 words the empty-menu
  claim as *substring absence of "story" in the body*. After Step 4 that is
  false of the whole body by construction — `kArchitectPrompt` is one
  unconditional constant and carries the story clause in `system` always. The
  test therefore asserts absence over `j["tools"]` and `j["messages"]` (the two
  things the menu actually gates) rather than over the raw body string, and says
  so in a comment. This is the plan's own Step-4 scoping, made concrete at the
  assertion; the alternative — asserting over the raw body — would have forced
  either a conditional prompt or a deleted assertion, both worse.

**Step 4.** One bullet appended to "Rules, absolute", in the enemy clause's
voice. `testArchitectPrompt` passed unmodified — every substring it asserts, and
its one negative assertion, survives an append untouched.

**Step 5.** Trim factoring (micro-decision 9) then the eight-condition lenient
story block. `testArchitectGate` passed unmodified, which is the fence the plan
asked for on the factoring — no revert needed.

**Step 6.** Story placement after enemy placement in `architectCommitProposal`.
Mechanical check 3 (`grep -En "INSERT|UPDATE|DELETE" src/architect.cpp`) still
empty — the write goes through `placeCatalogEntry`.

### DIVERGENCE (test mechanism only, requirement unchanged): the REQ-BARD-ARCH-13 fault injection

The plan specifies `db.exec("DROP TABLE location")` and argues it is surgical
because `writeGeneratedRoom` writes no location row. **The argument is about
writes and the problem is a read.** `systems.cpp:19` SELECTs the player's
container from `location` to find the current room *before* generation is
reached, so a dropped table faults the turn whether or not story placement sits
outside the Phase-1 catch. The arm would have gone green while proving nothing.

Replaced with a `BEFORE INSERT ON location` trigger raising ABORT, which is
surgical for the same intent:

- `moveEntity` is an UPDATE (`mutations.cpp:105`), so the player's move does not trip it;
- `writeGeneratedRoom` writes no location row at all;
- the proposal carries no enemy, so `placeEnemy`'s insert is not in play;
- `placeCatalogEntry` (`mutations.cpp:712`) is therefore the only INSERT the turn can make.

Two further adjustments fell out of it:

1. **The turn is driven through a pre-generated candidate**, not a fake
   transport. `runTurn` is the only entry point that owns the tick transaction
   whose rollback is under test, and it has **no transport seam**
   (`loop.hpp:28` takes `(Db&, const std::string&)` only). An injected candidate
   makes the turn commit-only and provably network-free. Side benefit: the arm
   now covers the pregen path's commit, which is where a stale story is most
   likely to appear.
2. **A control arm was added** — same world, same trigger, a candidate with no
   story — asserting the turn Ticks normally. That *proves* the trigger is
   surgical instead of asserting it in a comment.

**Both were verified by mutation, not by argument.** Wrapping the placement call
in `try { … } catch (...) {}` — the exact downgrade REQ-BARD-ARCH-13 forbids —
turns the suite red with **6 failures**; reverting returns it to 0. The arm
discriminates.

Also caught here: the instance-prose fixture originally read
`"the lectern's edge"`, and one assertion inlines that string into a SQL literal.
The apostrophe terminated the literal and the suite aborted with
`near "s": syntax error`. Prose reworded apostrophe-free, with a comment saying
why, since the next person to edit that fixture will not otherwise know.

**Step 7.** The menu read once inside the Phase-1 catch, spent twice — blurbs to
the context, handles to the schema. `architectProposeRoom` gained
`storyHandles` with **no default**, per the plan, so both call sites must be
explicit.

- **Step boundary blurred, deliberately.** Changing `architectProposeRoom`'s
  signature breaks `pregen.cpp:106` immediately, and Step 7's gate requires a
  clean build. So Step 8's two mechanical pieces — the `PregenJob::storyHandles`
  field and the worker's pass-through — landed inside Step 7. Step 8 kept its
  real content: the per-room hoist in `architectQueuePregen` and the tests.
  Noted because a reader diffing step-by-step will otherwise find Step 8's
  header edit under Step 7's commit.

**Step 8.** The hoist beside `enemyBlurbs`, per-room rather than per-direction.
`pregen.cpp` still contains no SQL (REQ-PREGEN-7 verified by grep).

- **`testArchitectStoryPregen` had to be relocated** ~700 lines further down the
  file: it uses `tickT` and `canonSnapshot`, both defined after
  `testArchitectQueuePregen` where the test naturally belonged. It now sits
  immediately before `testPregenCommit`, whose helpers it shares.
- **The path-equivalence proof excludes entity IDs, on purpose.** The two worlds
  mint in different orders, so identical ids were never the claim — identical
  *facts* are. The snapshot joins `catalog → name → description → location` and
  compares handle, noun, prose, and the containing room's NAME rather than its
  id; the event comparison is verb + handle detail. `canonSnapshot` is asserted
  equal on top of that, so nothing outside the story rows drifted either.

**Step 9.** Test-only, no production change. `tests.cpp` gained
`#include "lookup.hpp"`.

- **The lowercase defect is now executable, not just described.** The plan asked
  for the `catalog.name` case exposure to be recorded in a comment. It is — and
  a second fixture asserts it: a bard authoring `"Scorched Lectern"` mints an
  entity where `lookupNoun(db2, "scorched lectern") == 0` and only the
  exact-case string resolves. That assertion **passes today and is the bug**.
  When brick 1 lowercases `catalog.name`, that line is what fails, and the
  failure is the fix landing. A comment would have rotted silently; this cannot.

**Step 10.** All four of the spec's mechanical checks, plus the inherited guards:

| Check | Result |
|---|---|
| 1. `cmake --build build` | clean |
| 2. `./build/tests` | **7038 checks, 0 failures** |
| 2. `git diff tests/tests.cpp` | **additions only — zero deleted lines** |
| 3. `grep -En "INSERT\|UPDATE\|DELETE" src/architect.cpp` | empty |
| 4. `grep -n "O(1) in world size" src/architect.hpp` | empty; replacement states `O(eligible catalog)` |
| inherited: same grep on `src/bard.cpp` | empty |
| inherited: `grep -rn "place_catalog" src/` | empty |
| inherited: no SQL in `src/pregen.cpp` | empty |

## Result

Baseline 6683 → **7038 checks, 0 failures**. 355 new checks across 8 new test
functions; **no existing test edited**, which is what makes mechanical check 2
auditable rather than asserted.

Production diff: `architect.cpp` +204, `architect.hpp` +119, `pregen.hpp` +9,
`pregen.cpp` 1 line. No new translation unit, no `CMakeLists.txt` change, no DDL
— as the plan predicted.

All 17 `REQ-BARD-ARCH` requirements are pinned by a test that fails if the
behavior is removed. **Spec test 17 is partial by design** (see the plan's
"What this plan deliberately does not do"): the noun exists, resolves through
`lookupNoun`, and is named in the room's canon prose — Step 9 proves all three —
but it cannot be examined, because there is no examine verb and neither the
narrator's facts nor the resolver's scope enumerate non-portable entities.

## Carried forward, for the next spec

1. **Lowercase `catalog.name` in `writeCatalogEntry`** (`mutations.cpp:581`).
   A live defect that defeats the nouns-must-exist guarantee this brick exists
   to serve. Pinned by an executable assertion in `testArchitectStoryNoun`.
2. **Scenery in scope + an examine verb** — the five-subsystem follow-up that
   closes spec test 17.
3. **Measure `cache_read_input_tokens`** once this has run against real play:
   the menu is a large stable recurring prefix (design Decision 4). Recorded as
   a comment in `architect.hpp`, not as work.
