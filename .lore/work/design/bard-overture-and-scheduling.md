---
title: "The overture and wake scheduling"
date: 2026-08-03
status: implemented
tags: [bard, dungeon-master, cadence, scheduling, threading, worker, coalescing, overture, degradation, shutdown-ordering]
modules: [bard, pregen, main, loop, world]
related: [.lore/work/design/bard-fact-store.md, .lore/work/design/bard-catalog-selection.md, .lore/work/brainstorm/dungeon-master.md, .lore/work/specs/background-room-pregeneration.md]
---

# The overture and wake scheduling

## Scope

Design 3 of 4. Covers **when the bard runs**: the one cold call at world creation, the trigger predicate for subsequent wakes, coalescing, the rate ceiling, the worker thread, and where results are committed.

Depends on [bard-fact-store.md](bard-fact-store.md) (helpers) and [bard-catalog-selection.md](bard-catalog-selection.md) (tools, validation). Does not cover how catalog intent reaches room generation — design 4.

This is the design where *"the bard failing never makes the game worse than not having a bard"* stops being an assertion and has to be defended, because it is the only one with concurrency in it.

## Part 1 — The overture

### Decision 1.1 — Blocking, once per world file, before the loop starts

`openWorld` initializes only when the world file does not exist, so world creation is the natural once-ever hook. A resumed session never pays it.

**Blocking, not async.** The tempting alternative — start the game immediately and let the catalog land a few turns in — fails on a detail specific to this engine: **generated rooms are canon forever.** The first rooms a player walks are both the ones they remember most and, under an async overture, the only ones authored with no story behind them. Permanently. A one-time wait at world creation is cheap; a permanently story-less opening area is not.

The wait needs to be visible. A single line before the first room description, in the same register as the rest of the game — the player should understand the school is being written, not that the program has hung.

### Decision 1.2 — `openWorld` must report creation

Today it returns a `Db` whether it initialized or opened an existing file, so `main.cpp` cannot tell. Minimal change:

```cpp
struct OpenedWorld {
    Db db;
    bool created = false;   // true iff initialize() ran this call
};
OpenedWorld openWorld(const std::string& path, const std::string& seedPath,
                      const std::string& settingPath);
```

The overture belongs in `main.cpp`, **not** in `world.cpp`. That unit has no network and no AI, and `initialize()` runs inside a transaction — putting an HTTP call there would both break its shape and hold a write transaction open across a network round trip.

### Decision 1.3 — Call ordering in `main()`

```
AiHttpGuard   guard;          // curl_global_init — must precede any handle
auto world = openWorld(...);
if (world.created) bardOverture(world.db);   // main-thread handle, blocking
PregenGuard   pregen;         // worker thread starts
BardGuard     bard;           // bard worker thread starts (decision 2.4)
... game loop ...
```

The overture runs on the **main thread** using the shared persistent handle, before any worker exists. That sidesteps the threading contract entirely for this call — no second handle, no join ordering, no abort flag. It is the one bard call that needs none of that machinery, precisely because nothing else is running yet.

Placing it before `PregenGuard` is not functionally required — the pregen worker idles until a job is submitted, and the first submission happens after the first turn — but declaring it first makes the dependency order readable.

### Decision 1.4 — A longer timeout, deliberately

Every other AI call in this codebase uses an 8-second total timeout. The overture should not: it is a bulk generation of the entire catalog at high effort, and 8 s is likely to cut it off mid-write.

**60 seconds, for this call only.** Justified by everything that makes it different: it happens once per world, the player is already waiting and told so, nothing else is running, and there is no fallback that produces a *better* catalog — the alternative to waiting is an empty one.

This is a deliberate exception to a uniform rule and should be commented as such, or someone will "fix" it back to 8 s.

### Decision 1.5 — Failure is an empty catalog, and that is a supported state

No throw, no retry, no blocking the game. Timeout, transport error, malformed response, AI disabled, no API key — all produce the same outcome: zero catalog rows, the game proceeds, and the architect sees `meta.setting` exactly as it does today.

Design 2 already requires the empty catalog to be a normal answer everywhere. This is the path that makes that requirement load-bearing rather than theoretical.

## Part 2 — Wake scheduling

### Decision 2.1 — The trigger is a query, not a subscription

Evaluated on the main thread after each turn, in the same place and under the same rule as `architectQueuePregen`: **after the tick's transaction has committed and after the player's text has been flushed**, so scheduling can never delay the turn they waited on.

```sql
SELECT 1 FROM events
 WHERE turn > (SELECT value FROM meta WHERE key = 'bard_last_wake_turn')
   AND verb IN ('generated','defeated','learned','materialized')
 LIMIT 1;
```

`meta.bard_last_wake_turn` is a new **row**, not a new shape — zero DDL, following the `meta.setting` precedent. It is stamped when a wake is *queued*, not when it completes, so a slow wake cannot re-trigger itself.

A query rather than a callback because the event log is already the engine's source of truth, and a subscription would be a second bookkeeping system that can drift from it — the same reason `discoveredResistances` derives from the transcript with "no cache and no shadow table."

### Decision 2.2 — The ceiling is part of the predicate

```
turn - bard_last_wake_turn >= kBardMinTurnGap    // engine-owned constant, start at 5
```

Checked *before* the event query, because it is cheaper and because it must hold regardless of how many triggers fired. This is the cost control identified in the brainstorm: at a 5-turn floor the bard cannot exceed 0.2 wakes/turn no matter how frantically the player generates irreversible events.

An engine-owned constant alongside the combat constants, not a prompt-level suggestion and not configurable by the model.

### Decision 2.3 — Coalesce, never queue

The bard is a **singleton** — unlike pregen, whose jobs are keyed by `(room, direction)`, there is only ever one bard and one journal. So there is no queue and no per-key state machine:

```
Idle ──trigger──▶ Running ──result──▶ Ready ──commit──▶ Idle
                     │
                     └── trigger while running ──▶ set dirty, change nothing
```

A trigger arriving mid-wake sets a flag; when the wake completes and commits, the flag causes one more evaluation. Clearing a room of three enemies is one fight, not three story beats, and this is what makes that structural.

Two consequences worth stating: there is never more than one bard call in flight, ever; and a wake's result is always the most recent one, because there is nothing behind it in a queue to become stale.

### Decision 2.4 — Its own thread, not pregen's worker

This is the decision I am least certain of, so both sides are recorded.

**The case for sharing pregen's worker:** one thread, one `AiHttpWorkerClient`, one join, one shutdown path. The threading contract in `aihttp.hpp` calls `AiHttpWorkerClient` *"the ONE sanctioned second handle"* — a third needs that comment revised. And `pregen.hpp:120` explicitly anticipates this: *"a serial worker is what can later be taught to consult shared story state without races."*

**The case against, which I find decisive:** pregen jobs are latency-critical in a way bard wakes are not. A pregen miss costs the player ~7.3 s at the frontier — the exact variance spike that unit exists to eliminate. If a bard wake occupies the single worker when a room job queues, the room job waits; if the player reaches that exit first, they take a `RanQueued` on the main thread and eat the spike anyway.

And the collision is not rare. **Both trigger on the same activity** — exploration generates rooms (pregen jobs) and `generated` events (bard triggers) simultaneously. Sharing a worker makes the bard degrade pregen precisely when both are busiest.

**Decision: a second worker thread**, structurally identical to pregen's — own `AiHttpWorkerClient`, own abort flag polled by the progress callback, own `BardGuard` declared *below* `PregenGuard` in `main()` so reverse destruction joins it before `curl_global_cleanup()`.

The `aihttp.hpp` threading comment must be updated in the same change, not later: it currently names exactly one sanctioned second handle, and shipping a third while the header says otherwise is how the next reader concludes the code is wrong.

*Fallback if a third thread proves objectionable:* a shared worker with **priority**, where room jobs always preempt bard jobs. Gets one thread and no pregen degradation, at the cost of queue complexity. Not chosen, but it is the escape hatch.

### Decision 2.5 — No `Db` in the bard worker either

The pregen invariant applies unchanged and for the same reason — the engine's single-writer rule is not negotiable:

```
grep -En "INSERT|UPDATE|DELETE|SELECT" src/bardworker.cpp   -> empty
grep -n  "Db" src/bardworker.{hpp,cpp}                      -> no parameter, no member
```

Everything the wake needs is snapshotted on the main thread at queue time: the journal text, the events since the last wake (already rendered to strings), and the eligible menu. The worker produces **text**; every allocation of a scarce or unique thing still happens on the main thread at commit.

Note this means the eligible menu is snapshotted and may be stale by commit time — which is exactly why design 2 requires the live `catalogForHandle` re-check rather than trusting the snapshot.

### Decision 2.6 — Results commit in their own transaction

A ready result is applied on the main thread after a turn, alongside the trigger evaluation. It gets **its own transaction**, not the next tick's:

- the bard's writes are not part of the player's action, and folding them in would make a rollback of one roll back the other;
- `meta.turn` must not move — a wake is not a turn;
- a commit failure must leave the catalog unchanged and nothing else.

`appendEvent` stamps with the current `meta.turn`, so a `materialized` event committed here is correctly attributed to the turn during which it landed.

On any exception: roll back, log one diagnostic, leave `bard_last_wake_turn` where it is. The next irreversible event triggers a fresh wake.

## Part 3 — Defending the degradation claim

With threads involved, *"the bard failing never makes the game worse"* needs enumerating rather than asserting.

| Failure | Effect on the player |
|---|---|
| Overture times out or errors | empty catalog; today's game |
| Bard worker thread never starts (AI off) | no wakes; today's game |
| Worker throws mid-wake | thread survives, wake abandoned, next trigger retries |
| Worker hangs | bounded by the transport timeout; abort flag tears it down at shutdown |
| Commit transaction throws | rollback; catalog unchanged; retried on next trigger |
| Every wake fails all session | today's game plus an unused overture catalog |
| Player quits mid-wake | `BardGuard` sets abort, joins; libcurl abandons at its next progress callback |

**The one genuine hazard is shutdown ordering.** A live easy handle outliving `curl_global_cleanup()` is undefined behavior, and it is the one failure here that is *worse* than not having a bard. It is handled the same way pregen handles it — by RAII guard ordering in `main()`, so every exit path including the error ones gets it from reverse destruction, with no explicit call to forget.

`aihttp.hpp` already states the rule: every `AiHttpWorkerClient` must be destroyed and its thread joined **before** `aiHttpShutdown()`. Two workers means two guards, both below `AiHttpGuard`.

## Open questions

- **`kBardMinTurnGap = 5` is a guess.** It should be tuned against a real session's trigger distribution, and the profile record should carry the wake count so the distribution is observable rather than estimated.
- **Does the overture deserve a progress indication beyond one line?** A 60 s wait with a single static line is a long time to look at a terminal. Streaming is available and would turn the wait into something to read — but it is meaningful extra machinery for a once-per-world event.
- **Should a `downed` event trigger a wake?** It is a significant story beat (the player was beaten and woke elsewhere) but it is *not* irreversible — health is restored and the fight resets. Under decision 11's rule it is excluded, and I think that is right, but it is the closest call in the verb list.
- **What happens to a ready result if the player quits before it commits?** Currently: discarded. The alternative — persisting the proposal to commit next session — adds a serialization format and a staleness problem for a rare case. Discarding seems clearly right, but it is worth being deliberate about.

## Decision summary

1. **Overture is blocking, once per world file**, on the main thread before any worker exists — because generated rooms are canon forever and a story-less opening area is permanent.
2. **`openWorld` returns whether it created the world**; the overture lives in `main.cpp`, never in `world.cpp`.
3. **60 s timeout for the overture only**, a deliberate exception to the uniform 8 s, commented as such.
4. **Overture failure yields an empty catalog** and today's game.
5. **The trigger is a query over the event log** for `generated`/`defeated`/`learned`/`materialized` since `meta.bard_last_wake_turn`, evaluated post-commit and post-flush.
6. **The ceiling `kBardMinTurnGap` (start at 5) is checked first**, engine-owned, not model-visible.
7. **Coalesce, never queue** — one wake in flight ever; a trigger during a wake sets a dirty flag.
8. **A second worker thread**, not pregen's, because sharing would make the bard degrade pregen exactly when both are busiest. `aihttp.hpp`'s "one sanctioned second handle" comment must be updated in the same change.
9. **No `Db` in the bard worker**, enforced by the same greps pregen uses; the snapshotted menu is re-checked live at commit.
10. **Results commit in their own transaction**, never the tick's; `meta.turn` does not move.
