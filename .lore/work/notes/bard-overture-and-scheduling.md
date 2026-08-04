---
title: "Implementation notes: bard-overture-and-scheduling"
date: 2026-08-04
status: complete
tags: [implementation, notes, bard, overture, cadence, scheduling, threading, worker]
source: .lore/work/plans/bard-overture-and-scheduling.md
modules: [bard, bardworker, world, main, aihttp, mutations, pregen]
related: [.lore/work/specs/bard-overture-and-scheduling.md, .lore/work/design/bard-overture-and-scheduling.md, .lore/work/plans/bard-catalog-selection.md]
---

# Implementation notes: bard-overture-and-scheduling

Brick 3 of 4. Source plan: [bard-overture-and-scheduling](../plans/bard-overture-and-scheduling.md), 11 steps.

**Baseline before any edit:** build clean; `./build/tests` → **6170 checks, 0 failures**. `grep -c "openWorld(" tests/tests.cpp` = 145.

## Progress

- [x] 1 — `OpenedWorld` + 145 call sites
- [x] 2 — `AiRole::Bard`, per-call timeout, threading contract
- [x] 3 — `writeBardWakeTurn`
- [x] 4 — `bardworker` skeleton
- [x] 5 — the job, state machine, coalescing, worker body
- [x] 6 — the overture + `main()` guard ordering
- [x] 7 — trigger evaluation
- [x] 8 — committing a ready result
- [x] 9 — coalescing end to end
- [x] 10 — the degradation claim, seven arms
- [x] 11 — docs + the full mechanical sweep

## Log

**Session start.** No task files under `.lore/work/tasks/bard-overture-and-scheduling/`, so phases are the plan's steps directly. No `.lore/lore-agents.md`; work is being done inline per standing project preference rather than dispatched to sub-agents.

**Step 1 — `OpenedWorld`.** Done as planned. The scripted `perl` rewrite hit all 145 sites on the first pass; both greps read 145 afterwards and the suite ran at the *identical* 6170 checks / 0 failures it did before the edit, which is the real proof the mechanical rewrite changed nothing. `src/world.cpp` needed `<utility>` added for `std::move` in the two braced returns. Gate: passed (6172 after the two `created` assertions).

**Step 2 — `AiRole::Bard`, per-call timeout, threading contract.** Done as planned. `performPost` gained `long timeoutSeconds` ahead of `background`; `AiHttpWorkerClient::post` passes `kAiHttpTimeoutSeconds` explicitly, so the ordinary budget for wake calls is stated at the call site rather than inherited by default (micro-decision 12). Both `modelForRole(AiRole::Generate)` sites in `bard.cpp` switched to `AiRole::Bard` in this step, per micro-decision 14 — brick 2's request-body tests passed unedited, confirming the plan's expectation that they assert the model string and not the role. Added `testAiHttpThreadingContract`, a source-text test over `src/aihttp.hpp`: the old singular claim is gone and both guards plus "BELOW AiHttpGuard" are named. Gate: passed (6181, `grep AiRole::Generate src/bard.cpp` empty, `grep "ONE sanctioned second handle"` empty).

**Step 3 — `writeBardWakeTurn`.** Done as planned, with one deliberate departure: the helper binds the turn as an **integer** through its own `INSERT … ON CONFLICT` rather than going through `upsertMeta`, whose signature is `(Db&, const char*, const std::string&)`. `world.cpp` seeds this row as the integer `0` and every reader treats it as a number, so routing it through the string path would have left the column's storage class dependent on who wrote it last. The test asserts `typeof(value) == 'integer'` to pin that. Gate: passed (6193; brick 1's mechanical check 2 still returns only `src/mutations.cpp`).

**Step 4 — the `bardworker` skeleton.** Done as planned. The header was written to the step's surface only (env gate, start/stop, running hook, transport injection, reset, guard) and step 5's declarations added afterwards, so step 4's gate was answered by a worker that genuinely did nothing. Added `TEXTWORLD_BARD="00"` and `="1"` to the matrix beyond the plan's list: the convention is "off only when exactly `0`", and a truthiness reading of the variable would pass every other row. Gate: passed (6214); both REQ-BARD-WAKE-19 greps clean — the only `Db` hits in either file are comment prose, exactly as pregen's are.

**Step 5 — the job, the state machine, the worker body.** Done as planned; the state machine is the design's `Idle → Running → Ready → Idle` plus the flag, with no `Queued`. Two things worth recording:

- `BlockingTransport` (`tests/tests.cpp:~4950`) gained an optional `canned` response. The bard's worker consumes wake responses and the helper returned only room proposals, but the *timing* machinery is exactly what was needed — so the payload is now overridable and the blocking behavior is shared rather than duplicated.
- **Test (f) asserts less than its name suggests, deliberately.** REQ-BARD-WAKE-20's abort path is libcurl's: `g_stopping` is handed to `AiHttpWorkerClient` as its abort flag and polled from a progress callback. A hermetic suite has no libcurl to abort, so what the test pins is the half that is ours — `bardStop()` *completes* rather than deadlocking against a worker that needs the mutex to finish its iteration. That is the failure `stopWorker`'s no-lock-around-the-join comment exists to prevent, and this test fails if that changes. The comment in the test says so, so nobody later reads it as proof of the abort itself.

Beyond the plan's list, one further arm: a response calling **no tool at all** reaches `Ready` with an empty proposal. "The bard declined to act is a success" is the single most likely misreading of `validateWakeResponse`, and nothing else in this step's suite would have caught a worker that treated it as a failure. Gate: passed (6258).

**Step 6 — the overture and `main()`'s guard ordering.** Done as planned, including the proposed player-facing line verbatim ("The school is being written…"). One consequence the plan did not name:

> **Brick 2's `CHECK(!contains(bard, "catch"))` had to be narrowed.** That assertion (`testBardSelContract`) encoded plan micro-decision 6a — *admission* catches nothing — as a whole-file grep, which was exact while `bard.cpp` was inert. Micro-decision 15 requires both new entry points to catch everything, so the whole-file form now forbids the degradation guarantee. It is now scoped to the two function bodies it was always about (`admitOvertureProposal`, `applyWakeProposal`), with a comment recording why widening it back would be wrong. This is the only brick-2 test this brick changed.

Admission is made to throw mid-way by a SQLite `BEFORE INSERT … RAISE(ABORT)` trigger keyed on one entry's handle — a *genuine engine fault*, which is the only kind that can reach past `catalogEntryRefusal`'s pre-flight, and therefore the only kind that tests REQ-BARD-WAKE-7 rather than testing the pre-flight. Gate: passed (6342).

**Step 7 — trigger evaluation.** Done as planned; the evaluation order is the plan's code verbatim. The overture's stdout line now appears in the test run's output, which is expected — it is production behavior on a created world, and the arms that induce it are the ones that call it.

One plan arm was rewritten. The plan proposed inducing an evaluation throw by *deleting the `bard_last_wake_turn` meta row*, but that path does not throw: `bardMetaInt` reads an absent row as 0 by design. The throw is induced where it can actually happen — `DROP TABLE events`, which makes the trigger query itself fail. Same requirement, real failure. A first attempt also called `render()` after the drop, which throws in the *test* (render reads `events` too) and aborted the suite; the arm now asserts on `meta.turn` and a live statement instead, with a comment saying why render is not used there.

**Step 8 — committing a ready result.** Done as planned; the code shipped with step 7 and this step is its gate. Spec test 17's retry half needed restructuring: `bardSetWorkerTransportForTest` cannot change a *running* worker's transport — `workerMain` copies it at thread start — so the retry now removes the induced fault from the **world** (drops the trigger) and re-queues, rather than swapping the response. That is a better test of REQ-BARD-WAKE-23 anyway: the same wake, against a world that is no longer broken. Gate: passed (6453).

**Step 9 — coalescing end to end.** Done as planned, and as the plan predicted it required **no production code at all** — steps 5, 7 and 8 had already built every piece, and the three sub-cases passed on the first run. The third sub-case (the ceiling masking the flag) is the one worth keeping: it is the *common* path, and it is the only test of micro-decision 9's claim that the flag is belt to the query's braces. Gate: passed (6485).

**Step 10 — the degradation claim.** Done as planned, seven arms, all byte-identical. Three departures worth recording:

- **Arm 7 could not be induced the way the plan proposed.** The plan said to delete `meta.bard_last_wake_turn` so the trigger path throws; it does not — `bardMetaInt` reads an absent row as `0` by design. The arm now drops `motive_catalog`, which makes the snapshot builders throw during evaluation. Same requirement, a real failure, and it does not perturb player-facing bytes (nothing in the render path reads that table).
- **An anti-vacuity check was added.** Seven byte-identical sessions is *also* exactly what a bard that never ran would produce, so the test now asserts the arms really tried: overture-transport invocations `== 5` and wake invocations `> 0`. Writing that check found a real fact — only five of the seven arms reach the overture's transport, because arm 7's dropped `motive_catalog` makes the builders throw *before* the call. That is itself REQ-BARD-WAKE-6 (a builder fault costs the call, not the session), so the count is asserted at 5 with the reason written down.
- Beyond the bytes, each arm's `meta.turn` and full `events` log are compared to the baseline's as canonical strings, per the plan's gate.

Gate: passed (6683).

**Step 11 — docs and the mechanical sweep.** README gained the third bard paragraph, an extended test-coverage paragraph, and a rewritten `src/` inventory line naming `bardworker.cpp` and dropping "not yet wired to anything". Every mechanical check in the spec's list passes, plus the inherited guards from bricks 1 and 2. `src/pregen.cpp` is untouched and `src/pregen.hpp`'s diff is **comment-only** (verified by filtering the diff to non-comment lines — it is empty).

One wording adjustment at the end: several comments said the two guards' order *relative to each other* carries nothing. That is true of the undefined-behavior question but sits awkwardly against REQ-BARD-WAKE-18, which fixes `BardGuard` below `PregenGuard` in as many words. The comments in `main.cpp` and `bardworker.hpp` now say both things — the spec fixes the order and a test pins it; the "below `AiHttpGuard`" half is what separates defined from undefined behavior.

**End-to-end smoke.** `build/textworld` was run against a fresh world in a scratch directory with no API key (deliberately *not* the repo's `world.db`, which is a real save): startup, two turns, and `quit` all behave, exit 0, no overture line (AI off, as required), and no hang at shutdown with both guards in place.

**A counting note, recorded so the next session does not re-derive it.** Check counts do not move by the number of `CHECK(` lines added: `queryInt` (`tests/tests.cpp:132`) and `queryText` (`:1865`) each contain a `CHECK` of their own, so every helper call inflates the total. Step 3 added 6 `CHECK` sites and 12 checks. Total so far: 6170 → 6193 = 2 + 4 + 5 + 12, fully reconciled.

## Outcome

All 11 steps complete. **6170 → 6683 checks, 0 failures**, build clean with no warnings.

Every REQ-BARD-WAKE-1..-26 is implemented and pinned by a test that fails if the behavior is removed; all 21 spec tests and all 5 mechanical checks are covered. Two spec-test wordings were satisfied through the plan's own stated substitutions rather than literally: test 7's `runTurn` is driven as `tickT` + `render` (micro-decision 16 — `runTurn` with narration enabled reaches production transports, which the suite forbids), and test 15's "once the tick has provably reached its wait" is served by `bardWaitForIdleForTest` (micro-decision 3 — the bard's tick never blocks, so there is no production waiter to count).

### Divergences from the plan

| # | Plan said | What shipped | Why |
|---|---|---|---|
| 1 | `writeBardWakeTurn` beside the other `meta` writers | Its own `INSERT … ON CONFLICT`, binding an integer | `upsertMeta` takes a `std::string`; the row is seeded as an integer and read as a number |
| 2 | — (unstated) | Brick 2's whole-file `CHECK(!contains(bard, "catch"))` narrowed to the two admission functions | Micro-decision 15 requires both new entry points to catch everything; the whole-file form now forbids the degradation guarantee |
| 3 | Evaluation-throw induced by deleting `meta.bard_last_wake_turn` | Induced by `DROP TABLE events` (step 7) and `DROP TABLE motive_catalog` (step 10 arm 7) | Deleting the row does not throw — `bardMetaInt` reads an absent row as 0 by design |
| 4 | Spec test 17's retry swaps in a new wake transport | Removes the induced fault from the world and re-queues | `bardSetWorkerTransportForTest` cannot change a *running* worker's transport — `workerMain` copies it at thread start |

None of these changed a requirement; each is recorded above with its reasoning.

### Follow-ups worth raising, not done here

- The plan's own "deliberately does not do" list stands unchanged: no streaming overture progress, no wake count in the profile record, no `downed` trigger, no cross-session persistence of a pending result, no `catalogForHandle` call site (brick 4).
- `kBardMinTurnGap = 5` is still a guess. The design wants it tuned against a real session's trigger distribution, which needs the wake count in the profile record first.
- The overture's player-facing line ships as the plan proposed it — "The school is being written…" — and is the one piece of authored content here that may simply want rewording.
