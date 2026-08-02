---
title: "Implementation notes: background-room-pregeneration"
date: 2026-08-02
status: complete
tags: [implementation, notes, pregeneration, prefetch, threading, latency, architect, libcurl, profiling]
source: .lore/work/plans/background-room-pregeneration.md
modules: [pregen, architect, aihttp, profile, systems, loop, main]
related: [.lore/work/specs/background-room-pregeneration.md, .lore/work/brainstorm/background-room-pregeneration.md, .lore/work/validation/background-room-pregeneration/findings.md]
---

# Implementation notes: background-room-pregeneration

Moving the architect's ~7.3 s generation call off the critical path by producing
room candidates on a background thread while the player reads. Source of truth:
**[.lore/work/specs/background-room-pregeneration.md]** (25 requirements, prefix
`PREGEN`), executed through
**[.lore/work/plans/background-room-pregeneration.md]** (11 steps).

No task files existed under `.lore/work/tasks/background-room-pregeneration/`,
so the plan's 11 steps became the phases directly.

## Progress

<div style="font-family: ui-monospace, monospace; line-height: 1.9; padding: 6px 0;">
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 1</span> profile: mutex, atomic turn, background flag, dwell &nbsp;
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 2</span> architect: Phase 2 extracted<br>
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 3</span> aihttp: performPost + worker client &nbsp;
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 4</span> pregen: store + queue, no thread<br>
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 5</span> pregen: the worker thread <i>(MED)</i> &nbsp;
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 6</span> pregen: the tick's wait + shutdown <i>(MED)</i><br>
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 7</span> systems: commit in resolveGo &nbsp;
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 8</span> architect: the scheduler<br>
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 9</span> main: guard, queue points, dwell &nbsp;
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 10</span> deterministic sweep<br>
<span style="background:#e6f4ea;color:#1e7e34;padding:2px 8px;border-radius:3px;">✓ 11</span> <b>one bounded live run</b> — 9.16 s → 4.35 s on the latent walk, $0.32 spent
</div>

- [x] **Step 1** — `profile`: serialized emission, `background` flag, dwell records
- [x] **Step 2** — `architect`: `architectCommitProposal` extracted from Phase 2
- [x] **Step 3** — `aihttp`: `performPost` + `AiHttpWorkerClient`
- [x] **Step 4** — `pregen.cpp`: job, store, queue — no thread
- [x] **Step 5** — `pregen`: the worker thread
- [x] **Step 6** — `pregen`: the tick's wait, and shutdown
- [x] **Step 7** — `systems`: commit a candidate inside `resolveGo`
- [x] **Step 8** — `architect`: `architectQueuePregen`
- [x] **Step 9** — `main`: `PregenGuard`, two queue points, the dwell timer
- [x] **Step 10** — deterministic validation sweep
- [x] **Step 11** — one bounded live run

## What was built

One new translation unit and additive edits to eight existing files.

| File | Change |
|---|---|
| `src/pregen.hpp` / `.cpp` | **new** — the job, the candidate store, the queue, the one worker thread, the gate. ~390 lines. No SQL, no `Db`. |
| `src/profile.hpp` / `.cpp` | mutex around `write()`, `g_turn` → `std::atomic`, `CallRecord::background`, `DwellRecord` + `ScopedDwell`, `PregenRecord` |
| `src/aihttp.hpp` / `.cpp` | `anthropicPost`'s body → file-static `performPost(handle, …)`; new `AiHttpWorkerClient` owning its own easy handle + abort callback |
| `src/architect.hpp` / `.cpp` | `architectCommitProposal` (Phase 2, extracted whole); `architectQueuePregen` (the scheduler) + its two reads and the occupancy set |
| `src/systems.cpp` | `resolveGo`'s latent branch consults the store, commits a candidate, emits one outcome record |
| `src/loop.hpp` / `.cpp` | `playerRoom(db)` |
| `src/main.cpp` | `PregenGuard`, the flush move, two queue points, `ScopedDwell` around `getline` |
| `CMakeLists.txt` | `src/pregen.cpp` added to `twcore` |

**Tests: 2616 → 2834 checks (+218), zero existing tests edited.** `git diff
tests/tests.cpp | grep "^-"` returns **0 lines** — the plan named this as its
main complexity control, and it held for all nine code steps. Every new test
uses a fake transport; the default suite still makes no network access.

## Log

### Step 1 — profile

Straightforward. Two details worth keeping:

- The `background` key is **omitted entirely** when false rather than emitted as
  `background=0`, so every pre-existing log line and every existing `formatCall`
  test is byte-identical. The test asserts this the strong way: the background
  line must equal the foreground line with exactly `" background=1"` spliced in
  after `status=`, which would fail if any other field moved.
- `ScopedDwell`'s correctness test is the one the live run **cannot** do. A
  piped script's true dwell is ~0, so an implementation that always emitted zero
  would pass a presence check; the unit test asserts a manufactured 60 ms delay
  shows up as ≥55 ms and <500 ms — a real dependence on the wait.

### Step 2 — architect

Pure refactor, zero behavior change. The proof is that
`testArchitectGenerate`, `testArchitectSpawn`, `testWriteGeneratedRoom`,
`testResolveGoGenerate` and `testGeneratedEventInvisible` all pass unedited.

The point of the extraction is structural: the pregen hit path and the
synchronous path are now literally the same code, so "indistinguishable canon"
(REQ-PREGEN-14) is not something anyone has to remember to keep true.

### Step 3 — aihttp

`performPost` takes the handle as a **parameter** specifically so it cannot
reach for the file-static shared one. `AiHttpWorkerClient` stores its handle as
`void*` in the header so `curl.h` stays out of a header every AI unit includes.

The abort path is the existing failure path: `CURLE_ABORTED_BY_CALLBACK` →
`transportError`, with no new branch downstream. Its **runtime** behavior needs
a real in-flight transfer and is therefore Step 11's business — the offline test
pins handle lifetime and thread ownership only, which the plan called for
explicitly rather than faking a check.

### Step 4 — pregen store

**Deviation from the plan's wording, deliberate.** The plan says the `Ready`
branch should "move the proposal out". It **copies** instead. Moving would leave
a slot in state `Ready` holding an empty `optional` — a shape no reader should
have to reason about, and one that reads oddly against REQ-PREGEN-11's "never
evicted for the lifetime of the process". The copy is a few hundred bytes, once
per room, on a path that is about to write to the database anyway. No gate or
requirement is affected; the store is strictly more inert than the plan
described.

`pregenSubmit` also refuses a key that is not `Absent`, even though the
scheduler already checks. Belt and braces on the requirement that matters most
for cost: one room is never paid for twice.

### Step 5 — the worker thread <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span>

The risk split worked exactly as the plan predicted: with the tick still never
waiting, the only failure mode available at this step was "a job that does not
run", and there was no hang to debug.

**One test failure, test-side.** The serial-execution test waited on the fake
transport's own `done == 3` counter, which counts transport *returns* — the
worker stores each result just after. The last store raced the assertions. Fixed
by waiting on the state the assertions actually read (`pregenStateOf(...) ==
Ready`) rather than on the fake's counter. The production code was not touched.

### Step 6 — the wait and shutdown <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span>

No hangs, first try. Two things came out of writing it:

- The `Running` wait **re-looks-up the slot by key on each wake** instead of
  capturing the iterator across the wait. The mutex is released while waiting,
  and a map probe costs nothing next to owing anyone an iterator-stability
  argument.
- **A latent flake found and fixed by inspection, not by a failure.** Two wait
  tests released the blocking transport immediately after spawning the tick
  thread — but the tick might not have reached the wait yet, in which case the
  job lands first and the outcome is a lucky `hit` instead of `waited`. Both
  tests passed repeatedly; the race was real anyway. Fixed with a new
  observability hook, `pregenWaitingCountForTest()`, so the release provably
  happens only once the tick is inside the wait. **This is an addition to
  `pregen.hpp` the plan did not call for** — a test-only counter incremented
  under the same mutex, with nothing in the production path branching on it.
  Suite is green over 8 consecutive runs.

### Step 7 — resolveGo

**A plan-internal inconsistency, resolved toward the step.** The plan's preamble
says "the only new SQL anywhere is two SELECTs in `architect.cpp`", but Step 7
itself instructs computing a hit's `age_turns` as
`currentMetaTurn - pre.snapshotTurn` inside `resolveGo` — which needs a
`meta.turn` read in `systems.cpp`. The step-level instruction wins: there is now
a `currentWorldTurn()` file-static in `systems.cpp`, matching the private
duplication `loop.cpp`, `combat.cpp` and `mutations.cpp` already use for this
one-row read. It is called **only when profiling is on and only on a hit**, so
an ordinary run never pays for it. No grep contract covers `systems.cpp`, so no
gate is affected — but the plan's blanket sentence is now inaccurate and is
recorded here rather than quietly ignored.

The canon-equality assertion is the one worth having: the hit path and the
synchronous path each build a world from the same fixture and the same proposal,
and `canonSnapshot()` — the whole exit graph, names, prose, event verbs, and row
counts, ids included — must be **string-equal**. Not compared by eye, as the
plan insisted.

The `generate` stage keeps its single meaning and accompanies a `miss` **only**
(plan decision D5). There is a direct test for that, because reusing the label
on a `ran_queued` path would give one stage name two different spans and quietly
skew exactly the miss-vs-ran_queued comparison this feature exists to be judged
on.

### Step 8 — the scheduler

**One test failure, test-side.** The snapshot-turn assertion read
`PregenResult::snapshotTurn` off a `RanQueued` result, but that field is
Hit-only by contract — `age_turns` is reported for a hit alone (REQ-PREGEN-13,
-23). The test now reads it back the way production does: acquire once to run
the queued job, then again for the `Hit` that carries the stamp. Production code
unchanged; the test was asserting against a field the header already documented
as Hit-only.

**One addition the plan did not call for:**
`architectResetPregenOccupancyForTest()`. The occupancy set is a file-static, so
without a reset one test block's attempted directions leak into the next
(fresh databases, same room ids). Mirrors `pregenResetForTest()` in naming and
intent.

### Step 9 — main

The `fflush` move is the small thing that makes REQ-PREGEN-4 literally true
rather than nearly true: without it the player's text is still in the buffer
while the scheduler runs its reads. Same bytes in the same order — only the
timing of the write syscall changes, which the byte-identical diff confirms.

`Quit` never queues: the `break` precedes both the flush and the queue point.

### Step 10 — sweep

Every check run and its literal output recorded in
[the findings file](../validation/background-room-pregeneration/findings.md).
All pass.

**One recorded deviation:** the byte-identical script diff uses **`e0ac408`**
(this branch's HEAD before this work) as the baseline, not `main`. `main` is at
`b598fcb`, which predates the entire turn-latency-polish round already committed
on this branch — diffing against it would show that round's changes and prove
nothing about pregeneration. Result: stdout **and** stderr identical, both with
`TEXTWORLD_PREGEN` unset and with it set to `0`.

## Requirement coverage

All 25 requirements have a deterministic owner. Live checks corroborate; none
stands alone.

| Req | Where it is satisfied | Deterministic proof |
|---|---|---|
| -1 gate | `pregenEnabled`, early-outs in `pregenStart`/`Submit`/`Acquire`/`QueuePregen` | `testPregenStore` (a), `testPregenWorker` (a), `testArchitectQueuePregen` (f) |
| -2 needs architect | `pregenStart`, `architectQueuePregen` | `testPregenWorker` (a) via `pregenWorkerRunning()` |
| -3 depth 1 | `architectQueuePregen` scans only the current room | `testArchitectQueuePregen` (a) |
| -4 what/when queued | `main.cpp` startup + per-turn, after flush | (a)(b); `main` inspection |
| -5 main-thread snapshot | `PregenJob`; `architectQueuePregen` | `testArchitectQueuePregen` (c)(d); greps |
| -6 one serial worker | `workerMain`, single thread | `testPregenWorker` (c) |
| -7 no DB in worker | `pregen.cpp` | greps, both empty |
| -8 own easy handle | `AiHttpWorkerClient` built inside `workerMain` | `testAiHttpWorkerClient`; inspection table |
| -9 transport semantics | shared `performPost` | `testPregenStore` (e) |
| -10 failure clears, re-queues | `storeResultLocked`; occupancy set | `testPregenStore` (e), `testArchitectQueuePregen` (e) |
| -11 keyed, never evicted | slot map; `Ready` copies | `testPregenStore` (c)(f) |
| -12 four states | `PregenState` | `testPregenStore` (b), `testPregenWorker` (b), `testPregenWait` (b) |
| -13 staleness stamp | `snapshotTurn` → `age_turns` | `testArchitectQueuePregen` (c), `testPregenOutcomeRecords` (a) |
| -14 commit = all of Phase 2 | `architectCommitProposal` | `testPregenCommit` (a) canon equality, (d1) |
| -15 miss = today | the `else` branch is the original code | **existing tests, unedited** |
| -16 running waits / queued runs | `pregenAcquire` | `testPregenWait` (a)(b) |
| -17 single-writer | no SQL in `pregen.cpp`; writes stay in the tick | greps + `testPregenCommit` |
| -18 enemy unchanged | live re-check inside `architectCommitProposal` | `testPregenCommit` (d1)(d2), `testArchitectQueuePregen` (d) |
| -19 join before shutdown | `PregenGuard` below `AiHttpGuard` | `testPregenWait` (c)(d); `main.cpp:21,28` |
| -20 prompt quit | abort callback; stop flag | `testPregenWait` (c); **runtime is Step 11** |
| -21 bounded memory | one slot per key | `testPregenStore` (f) |
| -22 serialized emission | mutex in `write()` | `testProfileBackgroundAndDwell` (d), 2×200 records |
| -23 one record per walk | `PregenRecord` in `resolveGo` | `testPregenOutcomeRecords` (a)–(d) |
| -24 background marked | `CallRecord::background` | `testProfileBackgroundAndDwell` (a) |
| -25 dwell | `ScopedDwell` + `main` call site | `testProfileBackgroundAndDwell` (c) — **correctness** |

**"No partially-written candidate is ever observable" (REQ-PREGEN-12) is
structural, not tested directly.** The slot's `state` and its `proposal` are
always written in the same critical section under the one mutex, so a reader
either sees `Ready` with a complete proposal or does not see `Ready` at all.
There is no interleaving for a test to catch — which is the right reason not to
have one, and is recorded here so nobody later reads its absence as an oversight.

## Divergences from the plan

Four, all small, none changing behavior or a validation gate.

1. **`Ready` copies the proposal instead of moving it** (Step 4). Keeps
   REQ-PREGEN-11 literally true and avoids a `Ready`-with-nothing-in-it state.
2. **A `meta.turn` read now exists in `systems.cpp`** (Step 7), required by the
   plan's own Step 7 instruction but contradicting its preamble sentence about
   "the only new SQL anywhere". Gated behind profiling and a hit.
3. **Two test-only hooks the plan did not list:**
   `pregenWaitingCountForTest()` (Step 6, to kill a real ordering flake) and
   `architectResetPregenOccupancyForTest()` (Step 8, to stop occupancy leaking
   between test blocks).
4. **The byte-identical baseline is `e0ac408`, not `main`** (Steps 9–10), with
   the reasoning recorded above and in the findings.

### Step 11 — the live run

Run and complete. Full measurements in
[the findings file](../validation/background-room-pregeneration/findings.md).
**The feature does what it was built to do:** the same script, same seed, same
latent exit went from **9160 ms** (`TEXTWORLD_PREGEN=0`, a 5222 ms `generate`
stage inside the tick) to **4345 ms** with a **2.147 ms** tick — the worst turn
is now inside the ordinary-turn band. Cost: **$0.32** across all sessions.

Three things came out of it that the offline suite could not have found:

1. **The shipped seed can't run the plan's script 1.** A goblin is hand-placed
   in the corridor — the only room with latent exits — and the flee guard
   refuses a latent walk while a hostile is present. The first attempt ended
   with the player downed, so its final `north` was a *realized* move and
   emitted no pregen record at all. The replacement script kills the goblin
   first, which turns out to be the **better** test: the two jobs were queued on
   entry and ran *during the fight*, so the candidate was ready when it ended.
   That is plan decision **D8** confirmed live.
2. **A `kind=pregen` record is still emitted with `TEXTWORLD_PREGEN=0`**
   (`outcome=miss`), where the plan's §3 expected none. The isolation check
   itself passed on its substantive half — `background=1` count 0 vs 2 — which
   is what actually separates REQ-PREGEN-1 from -2. Divergence recorded; keeping
   or suppressing the record is an open call.
3. **Half the background generations produced no candidate** (4 of 8 returned
   200). Two were quit-aborts by design; two were full 8 s timeouts that never
   connected. REQ-PREGEN-8 deliberately denies the worker a shared connection
   cache, so every background call pays a cold handshake against the same 8 s
   budget the foreground path spends from a warm socket — and successful
   generations already take 5.2–5.9 s. n=2 is too small to diagnose and big
   enough to flag.

**The cost trade, now measured rather than assumed:** entering a room
speculatively queues one generation per latent exit, so the corridor's two
exits cost two generations for at most one walk. $0.077 of background spend
bought 2 hits (~$0.038 each) — roughly 2× generation spend to remove the
latency spike, bounded per room because candidates are never evicted.

## What is left

Nothing blocking. The plan is fully executed and every requirement has a
deterministic owner plus live corroboration. Two open calls for the user, both
recorded above: whether to suppress the `outcome=miss` record when pregen is
disabled, and whether the background path's 8 s timeout deserves its own
constant now that it is known to start from a cold socket.
