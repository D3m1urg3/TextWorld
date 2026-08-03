---
title: "Bard overture and wake scheduling"
date: 2026-08-03
status: draft
tags: [bard, dungeon-master, overture, cadence, scheduling, threading, worker, coalescing, shutdown-ordering, degradation]
modules: [bard, world, main, pregen, aihttp]
related: [.lore/work/design/bard-overture-and-scheduling.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/bard-catalog-selection.md, .lore/work/specs/background-room-pregeneration.md]
req-prefix: BARD-WAKE
---

# Bard overture and wake scheduling

**When** the bard runs: one cold call at world creation, then wakes on irreversible change. This is the brick with concurrency in it, and the one where *"the bard failing never makes the game worse than not having a bard"* must be demonstrated rather than asserted.

Depends on [bard-fact-store](bard-fact-store.md) (helpers) and [bard-catalog-selection](bard-catalog-selection.md) (contexts, request bodies, gates).

Source design: [bard-overture-and-scheduling.md](../design/bard-overture-and-scheduling.md).

## A. The overture

**REQ-BARD-WAKE-1.** `openWorld` returns whether it initialized the world this call, via a struct carrying the `Db` and a `created` flag. Existing callers are updated; no other behavior changes.

**REQ-BARD-WAKE-2.** The overture runs from `main.cpp` immediately after `openWorld` returns, **only** when `created` is true, and **only** when AI is enabled. It never runs in `world.cpp` and never inside `initialize()`'s transaction.

**REQ-BARD-WAKE-3.** The overture runs on the **main thread**, using the shared persistent handle, before any worker thread is constructed. Guard ordering in `main()` is: `AiHttpGuard`, then `openWorld`, then the overture, then `PregenGuard`, then `BardGuard`.

**REQ-BARD-WAKE-4.** The overture blocks until it completes, fails, or times out. Before it begins, one line of player-facing text is printed indicating the world is being written; the game does not appear hung.

**REQ-BARD-WAKE-5.** The overture uses a **60-second** total transport timeout, not the 8 seconds every other AI call uses. The constant carries a comment stating it is a deliberate exception and why, so it is not normalized back.

**REQ-BARD-WAKE-6.** On any failure — non-200, transport error, timeout, unparseable body, no tool call — the overture writes nothing, throws nothing, and the game proceeds with an empty catalog. The turn loop begins normally.

AI-disabled and a missing API key are **not** in this list: they are handled by the pre-call guard in REQ-BARD-WAKE-2, so a disabled run constructs no transport and makes no call at all. The two must not both be true, and the guard is the one that is — matching how `runTurnCore` gates the resolver, where "a disabled run never constructs a transport."

**REQ-BARD-WAKE-7.** A successful overture writes its entries through `writeCatalogEntry` and its journal through `writeBardJournal`, inside one transaction opened by the overture's caller. A throw from any helper rolls back the whole overture, leaving zero catalog rows.

## B. Wake triggering

**REQ-BARD-WAKE-8.** After each turn, on the main thread, **after the tick's transaction has committed and after the player's text has been flushed**, the engine evaluates whether to queue a wake. Evaluation never delays the turn the player waited on.

**REQ-BARD-WAKE-9.** The ceiling is checked first: no wake is queued unless `meta.turn - meta.bard_last_wake_turn >= kBardMinTurnGap`. `kBardMinTurnGap` is an engine-owned constant, **default 5, tunable**, not model-visible and not configurable at runtime by the model.

**REQ-BARD-WAKE-10.** When the ceiling permits, a wake is queued iff at least one `events` row exists with `turn > meta.bard_last_wake_turn` and `verb IN ('generated','defeated','learned','materialized')`.

Three of those four verbs are **pre-existing** and unchanged by this feature: `generated` is written by `writeGeneratedRoom`, `defeated` by `defeatEnemy`, and `learned` alongside `learnSpell` (REQ-COMBAT-21). Only `materialized` is new (REQ-BARD-STORE-7). No existing verb's semantics are altered.

**REQ-BARD-WAKE-11.** `meta.bard_last_wake_turn` is stamped with the current turn when a wake is **queued**, not when it completes, so an in-flight wake cannot re-trigger itself.

**REQ-BARD-WAKE-12.** Trigger state derives from the `events` log on every evaluation. No cache, no shadow table, no subscription.

## C. Coalescing

**REQ-BARD-WAKE-13.** At most **one** bard call is in flight at any time, process-wide. The bard is a singleton; there is no queue and no per-key state.

**REQ-BARD-WAKE-14.** A trigger arriving while a wake is running sets a dirty flag and queues nothing. When the running wake finishes and commits, the flag causes exactly one further evaluation.

**REQ-BARD-WAKE-15.** Three `defeated` events in three consecutive ticks produce **one** wake, not three.

## D. The worker

**REQ-BARD-WAKE-16.** The bard runs on its **own** worker thread, distinct from pregen's, owning its own `AiHttpWorkerClient` and its own abort flag polled by a libcurl progress callback. Exactly one thread, processing at most one job at a time.

**REQ-BARD-WAKE-17.** The `aihttp.hpp` threading contract comment is updated in the same change to describe two sanctioned worker handles rather than one. Shipping a third handle while the header names one is a documentation defect, not a follow-up.

**REQ-BARD-WAKE-18.** `BardGuard` is declared in `main()` **below** `PregenGuard`, which is below `AiHttpGuard`. Reverse destruction therefore joins the bard worker before `curl_global_cleanup()` on every exit path — normal return, quit, EOF, `SchemaMismatch`, and every catch.

**REQ-BARD-WAKE-19.** The bard worker performs **no database access of any kind**. Everything a wake needs — journal text, rendered event lines, the catalog rendering — is snapshotted on the main thread at queue time and carried in the job struct. The job carries no `Db` and no pointer into world state.

**REQ-BARD-WAKE-20.** The worker starts only when both AI and the bard are enabled; otherwise no thread is created. The bard follows the `TEXTWORLD_PREGEN` convention: on by default, off only when its environment variable is set to exactly `"0"`.

## E. Committing results

**REQ-BARD-WAKE-21.** A ready result is applied on the main thread after a turn, in **its own transaction**, never the tick's.

**REQ-BARD-WAKE-22.** Committing a wake does not advance `meta.turn`. A wake is not a turn.

**REQ-BARD-WAKE-23.** Any throw during commit rolls back the whole result, leaves the catalog and both `meta` rows unchanged, emits one diagnostic, and does not advance `meta.bard_last_wake_turn` beyond its queue-time stamp. The next irreversible event triggers a fresh wake.

**REQ-BARD-WAKE-24.** Selections carried by a result are re-checked live at commit (REQ-BARD-SEL-6), because a snapshot may be many turns old by the time it commits.

**REQ-BARD-WAKE-25.** A result still pending when the process exits is discarded. Nothing is persisted for a later session.

## F. Test hooks

**REQ-BARD-WAKE-26.** The bard unit exposes test-only hooks mirroring pregen's, each named for what it is and called by no production code: install a fake worker transport; report whether the worker thread is running; reset all bard state; and report how many threads are currently blocked waiting on a wake.

## AI Validation

Threading is the risk here, so the tests must prove ordering and singleness, not merely absence of crashes. Every test below runs with a **fake transport and no network**.

**Mechanical checks:**

1. `cmake --build build` clean; existing suite green, including all pregen tests.
2. `grep -En "INSERT|UPDATE|DELETE|SELECT" src/bardworker.cpp` is empty — verifies REQ-BARD-WAKE-19.
3. `grep -n "Db" src/bardworker.hpp src/bardworker.cpp` shows no parameter and no member — verifies REQ-BARD-WAKE-19.
4. `aihttp.hpp` no longer claims a single sanctioned second handle — substring check, verifies REQ-BARD-WAKE-17.
5. The 60-second overture timeout constant carries its justifying comment — substring check, verifies REQ-BARD-WAKE-5.

**Overture tests:**

6. Opening a **new** world with a canned overture response writes the expected catalog rows and journal; opening an **existing** world runs no overture at all — assert the transport is invoked zero times on the second open.
7. Each failure mode (non-200, transport throw, unparseable body, no tool call) leaves zero catalog rows, prints no stack, and permits `runTurn` to execute normally afterwards. AI-disabled is covered separately by test 12b, since it must never reach the transport at all.
8. A helper throwing mid-overture leaves **zero** catalog rows, not a partial set — verifies REQ-BARD-WAKE-7's single transaction.

**Trigger tests:**

9. A turn producing only `moved` / `took` / `looked` / `waited` / `failed` queues no wake.
10. A turn producing `generated`, `defeated`, `learned`, or `materialized` queues a wake — one test per verb.
11. With `meta.turn - bard_last_wake_turn < kBardMinTurnGap`, a qualifying event queues **nothing**; advancing past the gap then queues on the next qualifying event.
12. `bard_last_wake_turn` is stamped at queue time — assert it advances before the fake transport returns.
12a. **Evaluation never delays the turn (REQ-BARD-WAKE-8).** With a fake transport that blocks until released, run a turn that fires a trigger and assert the player-facing text for that turn was **already produced and flushed** before the transport was entered. Without this, "after the text has been flushed" is a comment rather than a requirement.
12b. **Disabled means no call.** With AI disabled, opening a new world invokes the transport **zero** times — verifies the REQ-BARD-WAKE-2 pre-call guard rather than a swallowed in-function failure.

**Coalescing tests:**

13. Three `defeated` events across three consecutive ticks, with the worker held mid-call by a blocking fake transport, produce exactly **one** transport invocation.
14. A trigger arriving during a running wake causes exactly one further evaluation after that wake commits — not zero, not two.
15. Use the waiting-count hook to release the fake transport only once the tick has provably reached its wait, so the test cannot pass by accident of timing.

**Commit tests:**

16. Committing a wake leaves `meta.turn` unchanged — verifies REQ-BARD-WAKE-22.
17. A helper throwing during commit leaves the catalog, `bard_journal`, and `bard_focus` byte-identical to their pre-commit values.
18. A result whose selection became ineligible between snapshot and commit applies the rest of the wake and drops that selection.

**Lifecycle tests:**

19. With the bard disabled by environment variable, no thread is created — assert via the worker-running hook, not by the absence of log lines.
20. `BardGuard` destruction joins the worker; constructing and destroying guards in the documented order completes without hanging when a fake transport is mid-call and the abort flag is set.

**The degradation claim, tested directly:**

21. For each of: overture failed, worker never started, worker threw, commit threw, and every wake failed — assert that a scripted sequence of turns produces **byte-identical player-facing output** to the same sequence run with the bard disabled entirely. This is the requirement that the bard can never make the game worse, and it is the single most important test in this spec.

**Out of scope** — the architect's `story` field and materialization at room generation are [bard-architect-integration](bard-architect-integration.md).
