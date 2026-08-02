---
title: "Background room pregeneration: candidates for the latent exits"
date: 2026-08-01
status: draft
tags: [performance, latency, pregeneration, prefetch, threading, architect, profiling]
modules: [pregen, loop, architect, mutations, aihttp, profile]
related: [.lore/work/brainstorm/background-room-pregeneration.md, .lore/work/specs/turn-latency-polish.md, .lore/work/validation/turn-latency-polish/findings.md]
req-prefix: PREGEN
---

# Background room pregeneration: candidates for the latent exits

Walking into a room that does not exist yet is the game's worst turn: measured at
~10.8 s (resolve ~1.0 s + generate ~7.3 s + narrate ~2.5 s) against an ordinary
turn's ~4.7 s. This spec moves the generation call off the critical path by
producing room candidates in the background while the player reads, so a
movement turn onto a latent exit costs about what an ordinary turn costs.

The goal is **removing a variance spike, not lowering the mean.** Ordinary turns
are unchanged by this work. Success is that entering a new room stops being
distinguishable from any other move.

Two facts about the existing code shape the design:

- `architectGenerate` already brackets its DB write: Phase 1 (context → request →
  one transport call → validate) performs no write, Phase 2
  (`writeGeneratedRoom`) is the sole persistence path.
- Phase 1 is **not** database-free. It calls `eligibleEnemyBlurbs(db, room)`
  (`architect.cpp:393`) and `buildArchitectContext(db, room, direction)` (`:397`),
  both read-only SELECTs. A background worker therefore cannot run Phase 1 as
  written; this spec splits it (REQ-PREGEN-5).

## Requirements

### Gating and scope

**REQ-PREGEN-1** — Pregeneration is gated by `TEXTWORLD_PREGEN`, following the
`TEXTWORLD_AI` convention: **on by default**, disabled only when the variable is
set to exactly `"0"`. When disabled, no background thread is created, no job is
queued, and gameplay output, behavior, and control flow are byte-for-byte
identical to today.

**REQ-PREGEN-2** — Pregeneration additionally requires `architectEnabled()`. When
the architect is off there are no generatable exits, so no thread is created and
no job is queued. Pregeneration never changes what `architectEnabled()` returns
and never affects whether a latent exit is displayed or attemptable.

**REQ-PREGEN-3** — Depth 1 only. Jobs are queued for latent exits of the room the
player currently occupies. A candidate's own declared exits are never chased.

### What gets queued, and when

**REQ-PREGEN-4** — Jobs are queued at two moments: once at startup for the
starting room, and after any turn that leaves the player in a room whose latent
exits are not all accounted for. One job is queued per latent exit (`exits.dest
IS NULL`) of the player's current room that has no candidate, no in-flight job,
and no attempt already made during this occupancy. Queuing happens **after** the
tick's transaction has committed and after game output has been written, so it
never delays the turn the player is waiting on.

**REQ-PREGEN-5** — Every database read a job needs is performed on the **main
thread at queue time** and snapshotted into the job. A job carries exactly:
origin room id, direction, the context payload from `buildArchitectContext`, the
enemy blurb menu from `eligibleEnemyBlurbs`, and the turn number at which it was
snapshotted. The job holds **no `Db` reference and no pointer into world state**.

### The worker

**REQ-PREGEN-6** — Exactly one background worker thread exists, processing at most
one job at a time. Concurrent in-flight generation requests are explicitly **not**
introduced. (Rationale: coherent story generation, planned but out of scope, cannot
author rooms in ignorance of one another; a serial worker is what can later be
taught to consult shared story state without races.)

**REQ-PREGEN-7** — The worker performs **no database access of any kind**. Its
whole job is `buildArchitectRequestBody(contextPayload, enemyBlurbs)` → one
transport call → `validateRoomProposal(response, direction)`, yielding either a
`RoomProposal` or nothing. The worker, the job queue, and the candidate store
live in a **new translation unit** (`pregen.cpp` / `pregen.hpp`), not in
`architect.cpp` — which already contains six SELECTs alongside the pure helpers
the worker calls, and so could never satisfy the check below. `grep -En
"INSERT|UPDATE|DELETE|SELECT"` over `pregen.cpp` must be empty, and no function
in it may take a `Db&`.

**REQ-PREGEN-8** — The worker owns its **own libcurl easy handle**, honoring the
one-handle-per-thread contract recorded in `aihttp.hpp`. The main thread's
persistent handle is never touched from the worker, and no curl share handle is
introduced (the connection cache is not shared across threads). `aiHttpInit()`
continues to run before any thread is created.

**REQ-PREGEN-9** — The transport call keeps every existing semantic: same request
body construction, the 8 s per-call timeout, `CURLOPT_NOSIGNAL`, at most one
attempt, no retries. A failed job (HTTP error, timeout, gate failure, exception)
produces no candidate and is otherwise silent to the player.

**REQ-PREGEN-10** — A failed job leaves its slot **empty**, not poisoned. It is not
retried during the current occupancy of the room, but becomes eligible for
re-queue when the player next enters that room (REQ-PREGEN-4). Retry attempts are
therefore bounded by room entries, not by time — there is no timer-driven retry
and no spin.

Accepted consequence: during a sustained API outage, a player pacing between two
rooms fires one failing background call per entry, because a failed slot never
populates and each entry is a fresh occupancy. Each entry costs a full turn
(seconds), so this cannot run away, and during such an outage `resolve` and
`narrate` are failing too — the session is already degraded. No entry-rate limiter
is required.

### The candidate store

**REQ-PREGEN-11** — Candidates live in an in-memory store keyed by `(room id,
direction)`. A candidate is **never evicted for the lifetime of the process**.
Re-entering a room whose neighbors were already generated therefore queues
nothing and costs nothing.

**REQ-PREGEN-12** — The store is accessed from both threads and must be safe under
that access. No partially-written candidate is ever observable, and a reader must
be able to distinguish four states for a key, with **queued** and **running**
kept distinct because the tick treats them differently (REQ-PREGEN-16):

| state | meaning |
|---|---|
| **absent** | no job ever queued for this key, or a job failed and the slot was cleared |
| **queued** | a job exists but the worker has not started its transport call |
| **running** | the worker is executing this job's transport call now |
| **ready** | a validated `RoomProposal` is stored and committable |

**REQ-PREGEN-13** — Each stored candidate records the turn number it was
snapshotted at (REQ-PREGEN-5). This value is **recorded and reported only** — it
does not invalidate anything in this spec. It exists as the hook a future story
system will need, and so that profiling can report how stale a committed
candidate was.

### Committing a candidate

**REQ-PREGEN-14** — When the player walks a latent exit and a **ready** candidate
exists for `(room, direction)`, the tick commits it by running **all of
`architectGenerate`'s Phase 2**, not `writeGeneratedRoom` alone. Phase 2 is:
`writeGeneratedRoom` (mint the id, realize the origin exit, plant the reciprocal
and declared latent stubs, append the `generated` event), **then**
`archetypeForEnemyBlurb` to re-check the proposal's enemy blurb, and on a
non-empty result `placeEnemy` plus `recordArchitectSpawn`
(`architect.cpp:413–426`). Omitting the enemy half would silently stop spawning.
The turn makes **no network call**, and the resulting canon is indistinguishable
from a room written by today's synchronous path.

**REQ-PREGEN-15** — On a **miss** (no candidate, or a job that failed), the turn
falls back to today's synchronous `architectGenerate` unchanged. A miss must be
indistinguishable from current behavior in output, canon, and failure handling.
Pregeneration is a pure optimization with a clean fallback.

**REQ-PREGEN-16** — When the player walks a latent exit whose job has not produced
a candidate yet, the tick's behavior depends on the job's state, and in no case
may it wait behind an **unrelated** job. With one serial worker (REQ-PREGEN-6),
naively waiting for "the queue to reach my job" would bound the wait at
(queue depth × 8 s), which is not acceptable:

- **running** — the tick waits for that job to finish, then proceeds as a hit or
  a miss according to its result. Bounded by the existing 8 s transport timeout.
  The request is never abandoned in favor of starting a second one for the same
  key: one room is never paid for twice.
- **queued** (not yet started) — the tick **removes the job from the queue and
  runs it synchronously on the main thread**, reusing the snapshot already taken
  (REQ-PREGEN-5). No wait behind the worker's current job, and no duplicate call.

Either way the tick waits at most one transport timeout.

**REQ-PREGEN-17** — The single-writer invariant is preserved exactly. All world
writes still happen on the main thread inside the turn's transaction. The worker
issues no SQL (REQ-PREGEN-7), so no second writer to `world.db` is introduced.

**REQ-PREGEN-18** — Enemy selection is unchanged by this spec. The eligible menu is
snapshotted at queue time (REQ-PREGEN-5) and the model selects from it as it does
today. The engine's existing re-check at commit — `archetypeForEnemyBlurb` against
the **live** menu — is preserved verbatim (REQ-PREGEN-14), so a candidate whose
enemy is no longer eligible resolves to `""` and simply spawns nothing, exactly as
a hallucinated blurb does today. The room is still made.

The only residual artifact is a candidate's prose describing a creature that the
commit-time re-check then declines to place. This is **accepted** and deliberately
not chased: enemy spawning moves to the planned game-master AI, and building
invalidation for it now would be work thrown away.

### Lifecycle

**REQ-PREGEN-19** — The worker thread is **joined before `aiHttpShutdown()`**. No
in-flight transfer and no live easy handle may outlive `curl_global_cleanup()`.
This ordering is not optional; violating it is undefined behavior.

**REQ-PREGEN-20** — Quitting must not make the player wait on a background call.
Shutdown signals the worker to stop; an in-flight transfer aborts promptly via a
libcurl progress callback that returns non-zero once the stop flag is set. The
join completes in well under a second in the normal case, rather than blocking
for the remainder of an 8 s timeout.

**REQ-PREGEN-21** — Memory is bounded in practice by never storing more than one
candidate per `(room, direction)`. A candidate is a few hundred words; a long
session holding hundreds of them costs single-digit megabytes. No eviction policy
is required (REQ-PREGEN-11).

### Observability

**REQ-PREGEN-22** — All profiling remains behind `TEXTWORLD_PROFILE` and off by
default. Profile records are emitted from **two threads**; emission must be
serialized so that no record line is ever interleaved or truncated. One record is
still one line on stderr, and game output on stdout stays clean.

**REQ-PREGEN-23** — Each latent-exit walk emits one record naming the outcome, using
the four states of REQ-PREGEN-12: `hit` (ready), `waited` (running — plus how long
the tick waited), `ran_queued` (dequeued and run synchronously), or `miss`
(absent). For a `hit`, the record also carries how many turns old the candidate was
(REQ-PREGEN-13). This is the record that makes the feature's hit rate measurable
rather than assumed.

**REQ-PREGEN-24** — Background generation calls emit the same `kind=call` records
as foreground ones (role, model, curl timings, token counts), distinguishable as
background so a reader can tell which calls were on the critical path and which
were not. Background calls do not appear inside any turn's stage timings, since
they are not part of a turn.

**REQ-PREGEN-25** — Player dwell time is instrumented: the gap between the moment a
turn's output is flushed and the moment the next input line is read, emitted as
one record per turn. This is the number that determines whether a single serial
worker can keep up with a player, and it is currently unmeasured.

## AI Validation

Deterministic checks first; live checks bounded to one short scripted session per
configuration.

**Build and offline regression**

- `cmake --build build` succeeds and the `tests` binary passes its existing suite
  with no live API.
- With `TEXTWORLD_PREGEN=0`, a scripted AI-off session's stdout is byte-identical
  to the same script run on `main`. Note this alone does **not** exercise the
  `TEXTWORLD_PREGEN` gate: AI-off already forces `architectEnabled() == false`,
  which suppresses pregen via REQ-PREGEN-2 regardless. Isolating REQ-PREGEN-1
  requires the **AI-on** comparison below. (REQ-PREGEN-1)
- With the architect disabled, no worker thread is created — assert via a test
  hook that reports whether the thread exists. Observing "no pregen records in the
  log" is **not** an acceptable substitute: with no latent exits queued, the log
  is empty whether the thread was never created or was created and left idle.
  (REQ-PREGEN-2)
- `grep -En "INSERT|UPDATE|DELETE|SELECT"` over the worker's translation unit is
  empty, and the worker's job struct contains no `Db` member or reference.
  (REQ-PREGEN-5, REQ-PREGEN-7)
- Code inspection confirms the worker creates and destroys its own easy handle,
  never referencing the shared one, and that the join precedes `aiHttpShutdown()`
  on every exit path including error exits. (REQ-PREGEN-8, REQ-PREGEN-19)

**Offline unit tests with a fake transport** (no network, deterministic)

- **Hit** — pre-populate a candidate for `(room, direction)`, walk it, assert the
  room is written to canon, the transport was invoked **zero** times during the
  turn, and the resulting exits/reciprocal/event rows match those produced by the
  synchronous path for the same proposal. (REQ-PREGEN-14)
- **Miss** — no candidate present; walk the exit and assert behavior matches the
  current synchronous path exactly, including the wall on a declining transport.
  (REQ-PREGEN-15)
- **Running** — a fake transport that blocks until released; walk the exit while
  the job is executing and assert the tick waits, then commits that job's result,
  and that the transport was invoked exactly **once** in total. (REQ-PREGEN-16)
- **Queued** — with the worker occupied by a *different* blocking job, walk an exit
  whose job is still queued. Assert the tick does **not** wait for the unrelated
  job, runs its own synchronously, and that the transport was invoked exactly once
  for that key. (REQ-PREGEN-12, REQ-PREGEN-16)
- **Enemy placement on a hit** — a candidate whose proposal carries an eligible
  enemy blurb, committed via the hit path, places the enemy and records the spawn;
  a candidate whose blurb is no longer eligible places nothing and still creates
  the room. (REQ-PREGEN-14, REQ-PREGEN-18)
- **Dwell timing** — a unit test with a manufactured delay between output flush and
  the next input read asserts the recorded dwell value tracks the delay. The live
  script below can only confirm the record's presence and format, not its
  correctness. (REQ-PREGEN-25)
- **Failure** — a throwing or non-200 transport leaves the slot empty, the turn
  falls back to a wall, and re-entering the room re-queues the job. (REQ-PREGEN-9,
  REQ-PREGEN-10)
- **Store states** — absent / in-flight / ready are distinguishable, and a
  concurrent read during a write never observes a partial candidate.
  (REQ-PREGEN-12)

**Bounded live run** (one scripted session, `TEXTWORLD_PROFILE=1`, AI on)

- A script that enters a room, spends several non-movement turns there, then walks
  a latent exit. Observe a `hit` record and a total turn time in the neighborhood
  of an ordinary turn (~3–4 s) rather than ~10.8 s. (REQ-PREGEN-14, REQ-PREGEN-23)
- A script that walks a latent exit immediately on entering a room, giving the
  worker no time. Observe `miss` or `inflight`, and a completed turn either way.
  (REQ-PREGEN-15, REQ-PREGEN-16)
- **The `TEXTWORLD_PREGEN` gate, isolated:** the same AI-on script run with
  `TEXTWORLD_PREGEN=0` and again with the default. The first shows no pregen
  records and no background `kind=call` records; the second does. This is the only
  check that separates REQ-PREGEN-1 from REQ-PREGEN-2. (REQ-PREGEN-1)
- The log shows background `kind=call` records marked as background, not folded
  into any turn's stage timings, with no interleaved or truncated lines across the
  two threads. (REQ-PREGEN-22, REQ-PREGEN-24)
- Dwell-time records appear, one per turn, in the expected format. Correctness of
  the value is established by the unit test above, not here — a piped script's true
  dwell time is near zero, so an implementation that always emitted zero would pass
  this check. (REQ-PREGEN-25)
- `quit` typed while a background generation is in flight exits promptly (well
  under a second), with no crash, no hang, and no libcurl warning. Run under a
  leak/UB check if one is already available. (REQ-PREGEN-19, REQ-PREGEN-20)
- Induced failure: an invalid `ANTHROPIC_API_KEY` for a session — background jobs
  all fail silently, the player sees the ordinary fallback behavior, and no retry
  storm appears in the log. (REQ-PREGEN-9, REQ-PREGEN-10)

## Out of Scope

- **Parallel prefetch.** One worker, one job at a time (REQ-PREGEN-6). Revisit only
  if measured dwell time (REQ-PREGEN-25) shows a serial worker cannot keep up.
- **Depth 2 / momentum-prioritized prefetch.** Chasing a candidate's own neighbors
  is deliberately excluded; decide it with hit-rate data from REQ-PREGEN-23.
- **Candidate invalidation.** The staleness stamp is recorded, never acted on
  (REQ-PREGEN-13, REQ-PREGEN-18).
- **Coherent story generation** — NPCs, objects, monsters, puzzles as a connected
  whole. Its own future work. This spec's contribution is the boundary it draws:
  the worker produces text, and every allocation of a scarce or unique thing
  happens on the main thread at commit.
- **Parser-first resolution** (skipping the resolver call when the fixed-verb
  parser already handles the input, ~1 s/turn) and **streaming narration** (the
  perceived-latency lever on every turn). Both are separate work identified in the
  brainstorm as the next two levers after this one.
