---
title: "Implementation plan: background-room-pregeneration"
date: 2026-08-01
status: executed
tags: [plan, performance, latency, pregeneration, prefetch, threading, architect, libcurl, profiling]
modules: [pregen, loop, systems, architect, aihttp, profile, main]
related: [.lore/work/specs/background-room-pregeneration.md, .lore/work/brainstorm/background-room-pregeneration.md, .lore/work/specs/turn-latency-polish.md, .lore/work/plans/turn-latency-polish.md, .lore/work/validation/turn-latency-polish/findings.md]
---

# Implementation plan: background-room-pregeneration

Move the architect's ~7.3 s generation call off the critical path by producing
room candidates on a background thread while the player reads. Source of truth:
**[.lore/work/specs/background-room-pregeneration.md]** (25 requirements, prefix
`PREGEN`). Supporting context: the
[brainstorm](../brainstorm/background-room-pregeneration.md) and the
[prior latency work](turn-latency-polish.md), which deliberately left the
forward-compat hooks this plan consumes.

The goal is **removing a variance spike, not lowering the mean.** An ordinary
turn is untouched by every step below.

## Guiding constraints (from memory + spec)

- **Deterministic skeleton first, live LLM last.** Steps 1–10 are fully
  offline-verifiable with fake transports. The single live run is Step 11,
  isolated and gated ([[verification-must-be-bounded]]). Mechanical observations
  only — never a tune-retry loop.
- **The transport seam does not change.** `HttpResponse` / `HttpTransport`
  (`src/prose.hpp:48-53`) stay exactly as they are, so every existing
  fake-transport test keeps compiling and passing untouched. This is the same
  discipline the last plan held and it is what makes the concurrency testable
  offline.
- **Off means off, twice.** `TEXTWORLD_PREGEN=0` or `!architectEnabled()` → no
  thread, no job, no behavioral delta (REQ-PREGEN-1, -2).
- **A miss is today's code.** Every step preserves the property that an empty
  candidate store makes `resolveGo` behave byte-for-byte as it does on `main`.
  This is what lets the whole existing suite keep passing at every step.
- **One seam / one file / one testable behavior per step.**

## Complexity & token-risk assessment

Estimated **before** sequencing, per [[token-risk-estimation]], and it changed
the plan: the concurrency work was one step and is now two (5 and 6), because
the thread's *existence* and the tick's *wait on it* fail in different ways and
debugging them together is exactly the compounding-iteration trap.

| Step | Scope | Size | Token-risk | Why that risk |
|---|---|---|---|---|
| 1 | profile: mutex, atomic turn, 2 record shapes | S–M | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | additive; existing formats byte-identical |
| 2 | architect: extract Phase 2 | S | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | pure refactor, proven by unedited tests |
| 3 | aihttp: thread-owned easy handle | M | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | mechanical extraction; abort path defers to 11 |
| 4 | pregen: store + queue, no thread | M | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | single-threaded state machine, fully unit-testable |
| 5 | pregen: the worker thread | M | <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span> | first real concurrency; deterministic but iteration-prone |
| 6 | pregen: the tick's wait + shutdown | M | <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span> | condvar + join ordering; a hang here burns cycles fast |
| 7 | systems: commit in `resolveGo` | M | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | one branch; existing tests are the regression proof |
| 8 | architect: the scheduler | M | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | two SELECTs + a set; no concurrency |
| 9 | main: guard, queue points, dwell | S | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | wiring, verified by a byte-identical diff |
| 10 | deterministic sweep | S | <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> | runs commands, writes down output |
| 11 | one bounded live run | S code / M run | <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span> | the only step that spends API calls |

**Overall: ~1 new TU (`pregen.cpp`/`.hpp`, est. 250–350 lines) plus additive
edits to 7 existing files.** No file is rewritten and no existing test is
edited — that last property is the plan's main complexity control, and if a step
finds itself editing an existing test, that is the signal to stop and re-check
the miss path rather than to change the test.

**Two containment rules for the MED and HIGH steps:**

- **Steps 5–6 (concurrency).** Every test uses an explicit `std::promise` /
  mutex gate the test releases — **no `sleep`-based sequencing**, which is what
  turns a concurrency bug into an unbounded retry loop. If a test hangs, it is a
  design bug to be read out of the code, not a timing knob to be tuned.
- **Step 11 (live).** One scripted session per configuration, mechanical
  observations only, findings written down whether or not they match the
  expectation. No prompt is tuned in this step ([[verification-must-be-bounded]]).

## Seams this touches (verified in tree)

| Seam | File:line | What this plan does with it |
|------|-----------|------------------------------|
| `write()` profile sink | `src/profile.cpp:31-37` | serialize under a mutex — Step 1 |
| `g_turn` | `src/profile.cpp:23` | make `std::atomic<int64_t>` — Step 1 |
| `CallRecord` / `formatCall` | `src/profile.hpp:52`, `src/profile.cpp:72` | add `background` flag — Step 1 |
| `architectGenerate` Phase 2 | `src/architect.cpp:413-426` | extract to `architectCommitProposal` — Step 2 |
| `architectGenerate` Phase 1 | `src/architect.cpp:393-408` | unchanged; its parts are already the worker's job — Step 4 |
| `anthropicPost` | `src/aihttp.cpp:136-208` | body extracted to a handle-parameterized helper — Step 3 |
| threading contract | `src/aihttp.hpp:56-68` | the header already promised this; honored — Step 3 |
| `resolveGo` latent branch | `src/systems.cpp:97-108` | consult the candidate store before generating — Step 7 |
| `buildArchitectContext` | `src/architect.cpp:138` | called at **queue** time on the main thread — Step 8 |
| `eligibleEnemyBlurbs` | `src/combat.hpp:121` | called at **queue** time on the main thread — Step 8 |
| `main()` REPL loop | `src/main.cpp:32-41` | guard, startup queue, per-turn queue, dwell timer — Step 9 |
| `playerId` / `roomOf` (static, `loop.cpp`) | `src/loop.cpp:21-39` | one of them exposed as `playerRoom()` so `main` can name a room — Step 9 |
| twcore sources | `CMakeLists.txt:17` | add `src/pregen.cpp` — Step 4 |
| tests `main()` | `tests/tests.cpp` (tail) | register the new tests — Steps 1–9 |

**The only new SQL anywhere is two SELECTs in `architect.cpp`** (Step 8: the
latent-exit scan and the `meta.turn` read), which keeps its
`grep -En "INSERT|UPDATE|DELETE"` contract (`architect.hpp:9`) intact.
`pregen.cpp` carries **no SQL at all** (REQ-PREGEN-7).

## Decisions the spec left open

These are judgment calls made here, not in the spec. Each is cheap to reverse if
you disagree — flag them at review rather than during implementation.

| # | Decision | Why |
|---|----------|-----|
| **D1** | The **scheduler** (latent-exit scan + snapshot + submit) lives in `architect.cpp` as `architectQueuePregen(Db&, int64_t room)`, not in `pregen.cpp` | REQ-PREGEN-7 forbids any `pregen.cpp` function from taking a `Db&`. `architect.cpp` already owns `buildArchitectContext` and calls `eligibleEnemyBlurbs`, and is contractually SELECT-only. One new TU, not two. |
| **D2** | The snapshot turn (REQ-PREGEN-5/-13) is **`meta.turn`**, the world turn — not `profileCurrentTurn()` | "how many turns old" is a gameplay fact. `meta.turn` is already being read on the main thread at queue time; the process-local profile counter would drift from it on `NoTick` turns. |
| **D3** | The scheduler runs after **every** non-`Quit` `runTurn`, whatever the outcome, plus once at startup | REQ-PREGEN-4's condition is a property of the room, not of the turn. Uniform is cheaper to reason about and idempotent: a room with nothing to queue queues nothing. |
| **D4** | "This occupancy" (REQ-PREGEN-4, -10) = *the room the scheduler was last called with*. A change of room clears the per-key attempted set | Gives exactly the spec's stated behavior: standing still never re-queues a failed slot; leaving and returning does. |
| **D5** | The `generate` **stage** keeps exactly one meaning — "the synchronous `architectGenerate` ran" — so it appears on a **miss only**. `ran_queued` and `waited` instead carry their cost on the pregen record itself, as `run_ms` and `wait_ms`. Both still emit a normal, **non**-background `kind=call` record when they make a call | Today's `generate` stage spans Phase 1 *and* Phase 2 (`architect.cpp:388`). A `ran_queued` path can only wrap Phase 1 — Phase 2 happens later in `resolveGo` — so reusing the label would give one stage name two spans and quietly skew any miss-vs-ran_queued comparison. Better to leave the stage alone and let the pregen record carry the new numbers. |
| **D6** | The dwell record carries the turn **just completed** | It is emitted after that turn's output and before the next `profileNextTurn()`. |
| **D7** | REQ-PREGEN-23's four names (`hit`/`waited`/`ran_queued`/`miss`) are canonical | The spec's live-run bullet says `inflight` once; that is a stray, not a fifth state. |
| **D8** | The scheduler does **not** skip queuing when a hostile is in the room | `resolveGo`'s flee guard (`systems.cpp:86`) blocks the *walk*, not the prefetch. Prefetching during combat is free and the candidate is waiting when the fight ends. |
| **D9** | The abort callback's granularity is libcurl's progress-callback cadence (≈1 s ceiling while idle-waiting on TTFB), not instantaneous | REQ-PREGEN-20 asks for "well under a second in the normal case". Step 11 measures it; if a real `quit` regularly costs ~1 s, that is the finding, not a silent pass. |

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> profile: thread-safe + 2 records ─┐<br>
<b>3</b> aihttp: worker handle ────────────┼─▶ <b>4</b> pregen store + queue <i>(no thread)</i><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─────────────────▶&nbsp;<b>5</b> worker thread <span style="background:#fef3c7;color:#92400e;padding:0 5px;border-radius:3px;">MED</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>6</b> tick wait + shutdown <span style="background:#fef3c7;color:#92400e;padding:0 5px;border-radius:3px;">MED</span><br>
<b>2</b> architect: extract Phase 2 ────────────────────────────────────────────────────┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>7</b> commit in resolveGo<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>8</b> scheduler<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>9</b> main wiring<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>10</b> deterministic sweep<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>11</b> live run <span style="background:#fce8e6;color:#b00;padding:0 5px;border-radius:3px;">HIGH — isolated</span><br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span> deterministic but concurrency-shaped — iteration-prone · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification.

Steps 1, 2 and 3 are mutually independent and each independently shippable —
none of them changes behavior. The game is playable and the suite green after
every step.

---

### Step 1 — `profile`: thread-safe emission, background calls, dwell records
**Requirements:** REQ-PREGEN-22, -24 (format half), -25 (correctness half). **Size:** S–M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/profile.hpp`, `src/profile.cpp`, `tests/tests.cpp`

Nothing here knows about pregen; it is pure instrumentation groundwork.

1. **Serialize the sink.** Wrap the body of `write()` (`profile.cpp:31`) in a
   `std::lock_guard` over a file-static `std::mutex`. One record is one
   `write()`; the mutex makes it one *atomic* line (REQ-PREGEN-22). Keep the
   default sink's single `fprintf` — do not split it into two writes.
2. **`g_turn` → `std::atomic<int64_t>`** (`profile.cpp:23`). The worker reads it
   via `profileCurrentTurn()` while the main thread increments it; today that is
   a plain data race. `profileNextTurn()` becomes `++g_turn` on the atomic.
   Header comment updated to say the counter is safe to read from any thread.
3. **`CallRecord` gains `bool background = false`** (`profile.hpp:52`).
   `formatCall` appends ` background=1` when set, and **omits the key entirely**
   when false — so every existing `formatCall` test and every existing log line
   is byte-identical (REQ-PREGEN-24). Place it after `status=`, before the
   timing keys.
4. **New `DwellRecord { int64_t turn; double ms; }`** plus `formatDwell` and a
   `profileEmit(DwellRecord)` overload, emitting
   `twprof kind=dwell turn=N ms=1234.567`.
5. **New `ScopedDwell` RAII**, mirroring `ScopedStage`: constructor reads the
   monotonic clock, destructor emits a `DwellRecord` at `profileCurrentTurn()`.
   This is the unit that makes REQ-PREGEN-25 testable without a REPL (D6).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li><code>formatCall</code> with <code>background=false</code> is byte-identical to the current expectation; with <code>background=true</code> it gains exactly <code> background=1</code>. <code>formatDwell</code> matches the documented shape.</li>
<li><code>ScopedDwell</code> around a manufactured <code>sleep_for(60ms)</code> emits one record whose <code>ms</code> is ≥ 55 and &lt; 500 — a real dependence on the delay, not a constant. <i>(REQ-PREGEN-25, the correctness half the live run cannot establish.)</i></li>
<li><b>Concurrency:</b> two <code>std::thread</code>s each emit 200 records into a capturing sink; assert 400 captured, every one a complete well-formed line, none empty, none containing an embedded <code>twprof </code> past position 0. <i>(REQ-PREGEN-22.)</i></li>
<li>Full existing suite passes with <b>no test edits</b>.</li>
</ul>
</blockquote>

### Step 2 — `architect`: extract Phase 2 into a callable commit
**Requirements:** REQ-PREGEN-14 (the mechanism), -18. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/architect.hpp`, `src/architect.cpp`, `tests/tests.cpp`

A pure refactor with zero behavior change, done before anything can call it.

1. Add to `architect.hpp`:
   ```cpp
   // Phase 2 of architectGenerate, verbatim and whole (REQ-PREGEN-14): mint the
   // room, realize the origin exit, plant the reciprocal + declared latent stubs
   // and the 'generated' event; THEN re-check the proposal's enemy blurb against
   // the LIVE eligible menu and, when it still resolves, placeEnemy +
   // recordArchitectSpawn. Runs inside the caller's tick transaction. Outside any
   // catch: a genuine DB fault propagates to runTurn's rollback (REQ-ARCH-5).
   // Returns the minted room id.
   int64_t architectCommitProposal(Db& db, int64_t originRoom,
                                   const std::string& direction,
                                   const RoomProposal& proposal, int64_t actor);
   ```
2. Move `architect.cpp:413-426` into it **unchanged**, comments included.
3. `architectGenerate`'s tail becomes
   `architectCommitProposal(db, room, direction, *proposal, actor); return true;`.

The whole point is that the pregen hit path and the synchronous path are then
*the same code*, so "indistinguishable canon" (REQ-PREGEN-14) is structural
rather than asserted.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li><code>testArchitectGenerate</code>, <code>testArchitectSpawn</code>, <code>testWriteGeneratedRoom</code>, <code>testResolveGoGenerate</code>, <code>testGeneratedEventInvisible</code> pass <b>with no edits</b>. That is the refactor's proof.</li>
<li>One new direct test: call <code>architectCommitProposal</code> on a hand-built <code>RoomProposal</code> inside a transaction and assert the same rows <code>testArchitectGenerate</code> asserts (exit realized, reciprocal planted, declared latents present, one <code>generated</code> event).</li>
<li><code>grep -En "INSERT|UPDATE|DELETE" src/architect.cpp</code> still empty.</li>
</ul>
</blockquote>

### Step 3 — `aihttp`: a second easy handle, owned by whoever holds it
**Requirements:** REQ-PREGEN-8, -9, -20 (transport half), -24. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/aihttp.hpp`, `src/aihttp.cpp`, `tests/tests.cpp`

The header already wrote this contract down (`aihttp.hpp:56-68`); this step
cashes it. No pregen types appear here.

1. **Extract the body of `anthropicPost`** (`aihttp.cpp:136-208`) into a
   file-static
   ```cpp
   HttpResponse performPost(CURL* handle, const std::string& body, AiRole role,
                            bool background, const std::atomic<bool>* abort);
   ```
   Everything moves verbatim: reset, per-call header list, the identical option
   set, the 8 s `CURLOPT_TIMEOUT`, `CURLOPT_NOSIGNAL`, `CURLOPT_TCP_KEEPALIVE`,
   one `curl_easy_perform`, no retries (REQ-PREGEN-9). Two additions, both inside
   the per-call block because `curl_easy_reset` wipes them:
   - when `abort != nullptr`: `CURLOPT_NOPROGRESS 0` +
     `CURLOPT_XFERINFOFUNCTION` returning `1` once `abort->load()` is true
     (REQ-PREGEN-20). A caller-abort shows up as `CURLE_ABORTED_BY_CALLBACK` →
     `transportError`, i.e. the existing failure path, no new branch downstream.
   - `record.background = background` on the emitted `CallRecord`.
2. `anthropicPost(body, role)` becomes
   `performPost(sharedHandle(), body, role, /*background=*/false, nullptr)` —
   identical behavior, identical records.
3. **New `AiHttpWorkerClient`** in `aihttp.hpp`:
   ```cpp
   // ONE easy handle, owned by the thread that constructs it (REQ-PREGEN-8).
   // NEVER touches the shared main-thread handle; no curl share handle exists,
   // so the connection cache is deliberately not shared. Construct and destroy
   // it on the SAME thread, and only between aiHttpInit() and aiHttpShutdown().
   class AiHttpWorkerClient {
     public:
       explicit AiHttpWorkerClient(const std::atomic<bool>* abort);
       ~AiHttpWorkerClient();                        // curl_easy_cleanup
       AiHttpWorkerClient(const AiHttpWorkerClient&) = delete;
       AiHttpWorkerClient& operator=(const AiHttpWorkerClient&) = delete;
       HttpResponse post(const std::string& body, AiRole role);  // background=true
   };
   ```
4. Extend the threading-contract comment block to name the new class as the
   sanctioned second handle, and to state that its destruction must precede
   `aiHttpShutdown()`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li>Existing <code>testProseTransport</code>, <code>testProfileRecords</code>, <code>testAiRoleModel</code> pass unedited: the shared path is untouched.</li>
<li><code>formatCall</code> for a background record carries <code> background=1</code>; for a foreground record it does not. <i>(REQ-PREGEN-24.)</i></li>
<li>Construct and destroy an <code>AiHttpWorkerClient</code> on a <code>std::thread</code> between <code>aiHttpInit()</code>/<code>aiHttpShutdown()</code> <b>without calling <code>post</code></b> — no network, no crash. Proves handle lifetime and thread ownership mechanically.</li>
<li>Code inspection recorded in the notes: <code>performPost</code> takes the handle as a parameter, the worker client never names the file-static shared handle, and the abort option pair sits in the per-call block below <code>curl_easy_reset</code>. <i>(REQ-PREGEN-8.)</i> The abort's <i>runtime</i> behavior is Step 11's business — it cannot be shown without a real in-flight transfer, and the plan says so rather than faking a check.</li>
</ul>
</blockquote>

### Step 4 — `pregen.cpp`: the job, the store, the queue — **no thread yet**
**Requirements:** REQ-PREGEN-1, -7, -9, -10, -11, -12 (3 of 4 states), -13, -16 (queued half), -21. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/pregen.hpp` (new), `src/pregen.cpp` (new), `CMakeLists.txt`, `tests/tests.cpp`

Building the store before the thread is the deliberate risk split: this whole
step is a **single-threaded** state machine and is unit-testable as one.

1. **`src/pregen.hpp`** — the whole surface, `Db`-free by construction
   (REQ-PREGEN-7):
   ```cpp
   struct PregenJob {           // NO Db, NO pointer into world state
       int64_t room = 0;
       std::string direction;
       std::string contextPayload;            // buildArchitectContext, snapshotted
       std::vector<std::string> enemyBlurbs;  // eligibleEnemyBlurbs, snapshotted
       int64_t snapshotTurn = 0;              // meta.turn at queue time (D2)
   };

   enum class PregenState { Absent, Queued, Running, Ready };

   enum class PregenOutcome { Hit, Waited, RanQueued, Miss };

   struct PregenResult {
       PregenOutcome outcome = PregenOutcome::Miss;
       std::optional<RoomProposal> proposal;   // present only on a success
       double waitMs = 0.0;                    // Waited only
       double runMs = 0.0;                     // RanQueued only
       int64_t snapshotTurn = 0;               // Hit only, for the age report
   };

   bool pregenEnabled();                       // TEXTWORLD_PREGEN, "0" means off
   void pregenSubmit(PregenJob job);
   PregenState pregenStateOf(int64_t room, const std::string& direction);

   // The ONE call the tick makes. `transport` overrides the production
   // main-thread transport on the synchronous paths (tests inject here, exactly
   // as resolve() already threads one through).
   PregenResult pregenAcquire(int64_t room, const std::string& direction,
                              const HttpTransport* transport);
   ```
   Test-only hooks, named as such: `pregenResetForTest()`,
   `pregenInjectReadyForTest(room, dir, proposal, snapshotTurn)`,
   `pregenPendingCountForTest()`.
2. **`pregenEnabled()`** — `TEXTWORLD_PREGEN` read once into a file-static bool,
   following `profilingEnabled()`'s shape exactly: unset → **on**, set to exactly
   `"0"` → off (REQ-PREGEN-1). Plus `pregenRefreshEnabledForTest()`, mirroring
   `profileRefreshEnabled()`.
3. **The store**: one `std::mutex`, a `std::map<std::pair<int64_t,std::string>, Slot>`
   where `Slot { PregenState state; std::optional<RoomProposal> proposal;
   int64_t snapshotTurn; }`, and a `std::deque<PregenJob>` of pending work. A
   candidate is **never erased** (REQ-PREGEN-11, -21); a failure sets the slot
   back to `Absent` (REQ-PREGEN-10). The deque is unkeyed, so the `Queued` branch
   below **linear-scans it for the matching `(room, direction)` and erases that
   element** under the lock — depths are single-digit (1–3 latent exits per room),
   so a scan is the right structure.
4. **`runJob(const PregenJob&, const HttpTransport&)`** — the whole worker body,
   already testable synchronously:
   `buildArchitectRequestBody(job.contextPayload, job.enemyBlurbs)` → one
   transport call → `validateRoomProposal(resp, job.direction)`, wrapped in the
   same `try`/`catch(...)` pair `architectGenerate` uses. No DB, no retries.
5. **`pregenAcquire`**, three of four branches (Running arrives in Step 6):
   - `!pregenEnabled()` → `Miss`, immediately, touching nothing.
   - `Ready` → move the proposal out, `Hit`, carry `snapshotTurn`.
   - `Queued` → **scan the deque for that key and erase it**, mark the slot
     `Running`, **release the mutex**, `runJob` on the main thread, re-take the
     mutex, store the result or reset to `Absent`, → `RanQueued` with the elapsed
     `runMs`. No `generate` stage is emitted here (D5).
   - `Absent` → `Miss`.
   The mutex is never held across a transport call.
6. `CMakeLists.txt:17` — add `src/pregen.cpp` to `twcore`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li><code>grep -En "INSERT|UPDATE|DELETE|SELECT" src/pregen.cpp</code> is <b>empty</b>, and <code>grep -n "Db" src/pregen.hpp src/pregen.cpp</code> shows no <code>Db</code> parameter or member. <i>(REQ-PREGEN-5, -7 — run these as commands and paste the output into the notes.)</i></li>
<li><b>Gate:</b> with <code>TEXTWORLD_PREGEN=0</code>, submit + acquire on the same key yields <code>Miss</code> and the store stays empty. Unset → the job is stored. <i>(REQ-PREGEN-1.)</i></li>
<li><b>States:</b> fresh key → <code>Absent</code>; after submit → <code>Queued</code>; after inject → <code>Ready</code>. <i>(REQ-PREGEN-12, three of four.)</i></li>
<li><b>Hit:</b> inject a ready candidate; acquire returns <code>Hit</code> with that proposal and its <code>snapshotTurn</code>, transport invoked <b>zero</b> times.</li>
<li><b>RanQueued:</b> submit with a counting fake; acquire returns <code>RanQueued</code> with the validated proposal, transport invoked <b>exactly once</b>, pending count back to 0.</li>
<li><b>Failure:</b> a throwing fake and a non-200 fake each leave the slot <code>Absent</code> and return no proposal; transport invoked once, not twice. <i>(REQ-PREGEN-9, -10.)</i></li>
<li>Full suite green — nothing calls into pregen yet, so nothing else moves.</li>
</ul>
</blockquote>

### Step 5 — `pregen.cpp`: the worker thread
**Requirements:** REQ-PREGEN-2 (thread-existence hook), -6, -8, -12 (Running). **Size:** M · **Token-risk:** <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span>
**Files:** `src/pregen.hpp`, `src/pregen.cpp`, `tests/tests.cpp`

> ⚠️ **First of the two concurrency steps.** It adds the thread and the
> `Running` state and *nothing else* — the tick still never waits, so a bug here
> can only show up as a job that does not run, never as a hang. That separation
> is the point of splitting this from Step 6.

1. **New surface:**
   ```cpp
   void pregenStart();              // no-op unless pregenEnabled() && architectEnabled()
   bool pregenWorkerRunning();      // TEST HOOK for REQ-PREGEN-2's validation
   void pregenSetWorkerTransportForTest(HttpTransport);  // instead of a real handle
   ```
2. **Exactly one thread** (REQ-PREGEN-6), one job at a time, drained from the
   front of the deque. Its loop: `std::condition_variable` wait on
   `pending non-empty || stopping`; pop; flip the slot to `Running`; release the
   mutex; `runJob`; re-take; store `Ready` or reset to `Absent`.
3. It constructs its `AiHttpWorkerClient` **once, inside the thread body**, bound
   to the file-static `std::atomic<bool> g_stopping`, and destroys it on the way
   out — so the handle is created and destroyed on the worker thread
   (REQ-PREGEN-8). If a test transport is set, no client is constructed at all
   and no curl call happens.

<blockquote style="border-left:4px solid #92400e;padding-left:12px;margin-left:0;">
<b>✅ Validation gate</b> — fake transports only, and every test sequences with an explicit <code>std::promise</code>/mutex gate the test releases. <b>No <code>sleep</code>-based sequencing.</b>
<ul>
<li><b>Thread existence:</b> pregen + architect enabled → <code>pregenWorkerRunning() == true</code>; <code>TEXTWORLD_PREGEN=0</code> → <code>false</code>; architect disabled → <code>false</code>. <i>(REQ-PREGEN-1, -2 — the hook the spec explicitly demands instead of "no records in the log".)</i></li>
<li><b>Running is observable:</b> submit a blocking job; the test sees <code>pregenStateOf(...) == Running</code> before releasing it, and <code>Ready</code> after. <i>(REQ-PREGEN-12, the fourth state.)</i></li>
<li><b>Serial:</b> submit three jobs against a fake that asserts it is never re-entered concurrently; all three complete, in order. <i>(REQ-PREGEN-6.)</i></li>
<li>Full suite green.</li>
</ul>
</blockquote>

### Step 6 — `pregen.cpp`: the tick's wait, and shutdown
**Requirements:** REQ-PREGEN-16 (running half), -19, -20. **Size:** M · **Token-risk:** <span style="background:#fef3c7;color:#92400e;padding:1px 6px;border-radius:3px;">MED</span>
**Files:** `src/pregen.hpp`, `src/pregen.cpp`, `tests/tests.cpp`

> ⚠️ **The step that can hang.** Everything that blocks a thread lives here and
> nowhere else, so a hang has exactly one place to be.

1. **`pregenAcquire`'s fourth branch — `Running`:** record a `steady_clock`
   start, wait on the condvar until the slot leaves `Running`, then report
   `Waited` with the elapsed `waitMs` and the resulting proposal, or none. The
   tick **never** starts a second call for the same key (REQ-PREGEN-16), and
   never waits behind an unrelated job — the `Queued` branch from Step 4 is what
   guarantees that. The worker `notify_all`s after every slot transition.
2. **`void pregenStop()`** — set `g_stopping`, `notify_all`, join. Idempotent,
   and safe when no thread was ever started. The worker client's abort callback
   (Step 3) tears down an in-flight transfer (REQ-PREGEN-20).

<blockquote style="border-left:4px solid #92400e;padding-left:12px;margin-left:0;">
<b>✅ Validation gate</b> — same discipline: explicit gates, no sleeps.
<ul>
<li><b>Running / waited:</b> submit job A with a blocking fake; spin until <code>pregenStateOf(A) == Running</code>; call <code>pregenAcquire(A)</code>; release the fake; assert outcome <code>Waited</code>, the proposal is that job's result, <code>waitMs > 0</code>, and the transport was invoked <b>exactly once in total</b>. <i>(REQ-PREGEN-16 first bullet.)</i></li>
<li><b>Queued behind an unrelated job:</b> submit blocking job A <b>and</b> job B; with the worker stuck inside A, <code>pregenAcquire(B)</code> returns <code>RanQueued</code> promptly — assert it completed <i>before</i> A was released, and that B's key saw exactly one transport call. <i>(REQ-PREGEN-12, -16 second bullet — the queue-depth × 8 s failure this design exists to avoid.)</i></li>
<li><b>Stop while blocked:</b> start, submit a blocking job, call <code>pregenStop()</code>, release the fake; the join completes and the process does not hang. Then <code>pregenStop()</code> again — idempotent, no crash. <i>(REQ-PREGEN-19, -20.)</i></li>
<li>Full suite green, and the suite's total wall-clock has not visibly grown — a test that got slower is a test that is sleeping somewhere it shouldn't.</li>
</ul>
</blockquote>

### Step 7 — `systems`: commit a candidate inside `resolveGo`
**Requirements:** REQ-PREGEN-14, -15, -17, -18, -23. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/systems.cpp`, `src/profile.hpp`/`.cpp` (one record shape), `tests/tests.cpp`

The latent branch at `systems.cpp:97-108` becomes:

```cpp
if (state == ExitState::Latent && aiNarrationEnabled()) {
    const PregenResult pre = pregenAcquire(room, action.direction, transport);
    bool generated = false;
    if (pre.proposal) {
        // Hit / Waited / RanQueued that produced a candidate: NO network on this
        // turn's commit path — Phase 2 only, the same Phase 2 the sync path runs.
        architectCommitProposal(db, room, action.direction, *pre.proposal, player);
        generated = true;
    } else {
        // Miss, or a job that failed: today's synchronous path, byte-identical.
        generated = transport != nullptr
            ? architectGenerate(db, room, action.direction, player, *transport)
            : architectGenerate(db, room, action.direction, player);
    }
    if (generated) { /* realize + move, unchanged */ }
}
```

Everything before this branch — case (a) realized, the flee guard — is untouched,
so a realized exit still never consults pregen (REQ-PREGEN-15, -17).

**The outcome record** (REQ-PREGEN-23): add `PregenRecord` and `formatPregen` to
`profile.cpp`, emitting one line per latent-exit walk:

```
twprof kind=pregen turn=7 outcome=hit age_turns=3
twprof kind=pregen turn=7 outcome=waited wait_ms=2140.118
twprof kind=pregen turn=7 outcome=ran_queued run_ms=6912.004
twprof kind=pregen turn=7 outcome=miss
```

`resolveGo` emits exactly one, computing `ageTurns` for a `hit` as
`currentMetaTurn - pre.snapshotTurn` (D2). Each optional key appears on its own
outcome and nowhere else — `miss` carries none, and the `generate` stage still
accompanies a `miss` alone (D5).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate</b> — driven through <code>tickT</code> with fake transports, as <code>testResolveGoGenerate</code> already does.
<ul>
<li><b>Hit:</b> inject a ready candidate for the fixture's latent <code>(1, 'east')</code>, walk it; assert the room is written, the player moved, transport invoked <b>zero</b> times, and the exits / reciprocal / declared-latent / <code>generated</code>-event rows <b>equal</b> those <code>testResolveGoGenerate</code> case (b) produces for the same proposal — compare row-for-row, not by eye. <i>(REQ-PREGEN-14.)</i></li>
<li><b>Miss:</b> empty store; the assertions of <code>testResolveGoGenerate</code> cases (b) and (e) hold verbatim, including the wall and the surviving latent row on a declining transport. <i>(REQ-PREGEN-15.)</i></li>
<li><b>Enemy on a hit, both ways:</b> a ready candidate whose <code>enemyBlurb</code> is currently eligible places the enemy and records the spawn; the same candidate with an ineligible/stale blurb places nothing and <b>still creates the room</b>. <i>(REQ-PREGEN-14, -18.)</i></li>
<li>One <code>kind=pregen</code> record per latent walk with the right <code>outcome</code> for each of the four states: <code>age_turns</code> on <code>hit</code> only, <code>wait_ms</code> on <code>waited</code> only, <code>run_ms</code> on <code>ran_queued</code> only, and a <code>stage=generate</code> line accompanying <code>miss</code> <b>only</b> (D5). <i>(REQ-PREGEN-23.)</i></li>
<li>Existing <code>testResolveGoGenerate</code>, <code>testProfileGenerateStage</code>, <code>testCombatFlee</code>, <code>testGeneratedEventInvisible</code> pass <b>unedited</b> — the store is empty in all of them, so they take the miss path. <b>This is the regression proof for REQ-PREGEN-15.</b></li>
</ul>
</blockquote>

### Step 8 — `architect`: the scheduler
**Requirements:** REQ-PREGEN-3, -4, -5, -10 (re-queue half), -13, -18. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/architect.hpp`, `src/architect.cpp`, `tests/tests.cpp`

```cpp
// Snapshot and submit one pregen job per latent exit of `room` that has no
// candidate, no in-flight job, and no attempt already made during this
// occupancy (REQ-PREGEN-4). Depth 1: a candidate's own declared exits are never
// chased (REQ-PREGEN-3). READ-ONLY — SELECTs only, on the MAIN thread, so the
// job it hands off carries no Db and no world pointer (REQ-PREGEN-5). No-op
// when pregen or the architect is off. Never begins/commits; call it AFTER the
// tick's transaction has committed.
void architectQueuePregen(Db& db, int64_t room);
```

1. Early-out on `!pregenEnabled() || !architectEnabled()` (REQ-PREGEN-1, -2).
2. **Occupancy (D4):** a file-static `int64_t g_occupancyRoom` and a
   `std::set<std::string>` of directions attempted during it. `room !=
   g_occupancyRoom` → clear the set, adopt the new room.
3. **Two new SELECTs, the only SQL this plan adds anywhere:**
   `SELECT direction FROM exits WHERE room = ? AND dest IS NULL` (the latent
   scan) and `SELECT value FROM meta WHERE key = 'turn'` (the snapshot stamp,
   D2). The `meta.turn` read is privately duplicated in `loop.cpp:27`,
   `combat.cpp`, and `mutations.cpp` already — reimplement it as a file-static
   here, matching the codebase's standing convention rather than introducing a
   shared accessor for it.
4. Per direction: skip if attempted this occupancy, or if `pregenStateOf(...)` is
   not `Absent`. Otherwise snapshot — `buildArchitectContext(db, room, dir)`,
   `eligibleEnemyBlurbs(db, room)` and the turn read (both hoisted out of the
   loop, once per room) — mark the direction attempted, and `pregenSubmit`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li><code>grep -En "INSERT|UPDATE|DELETE" src/architect.cpp</code> still <b>empty</b>.</li>
<li>A fixture room with two latent exits and one realized exit queues <b>exactly two</b> jobs, one per latent direction, never one for the realized exit. <i>(REQ-PREGEN-3, -4.)</i></li>
<li>Calling it <b>twice for the same room</b> queues nothing the second time. <i>(REQ-PREGEN-4, idempotence — D3's uniform-call decision rests entirely on this.)</i></li>
<li>A queued job's <code>contextPayload</code> equals <code>buildArchitectContext(db, room, dir)</code> called directly, its <code>enemyBlurbs</code> equals <code>eligibleEnemyBlurbs(db, room)</code>, and its <code>snapshotTurn</code> equals <code>meta.turn</code> — advance the turn counter between two scheduler calls and assert the stamp moves with it, so the second SELECT is genuinely exercised. <i>(REQ-PREGEN-5, -13.)</i></li>
<li><b>Occupancy:</b> force a job to fail (slot → <code>Absent</code>); call again for the <b>same</b> room → nothing re-queued. Call for a different room, then back → the failed key <b>is</b> re-queued. <i>(REQ-PREGEN-10.)</i></li>
<li>With <code>TEXTWORLD_PREGEN=0</code>, and separately with the architect disabled, the call queues nothing. <i>(REQ-PREGEN-1, -2.)</i></li>
</ul>
</blockquote>

### Step 9 — `main`: the guard, the two queue points, the dwell timer
**Requirements:** REQ-PREGEN-4 (call sites), -19 (ordering), -25 (call site). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** `src/main.cpp`, `src/loop.hpp`, `src/loop.cpp`, `src/pregen.hpp`

0. **Give `main` a way to name a room.** It has none today: `playerId` and
   `roomOf` are file-static in `loop.cpp:21-39`, `renderStartup` computes the
   room and discards it, and `TurnResult` carries only `{outcome, output}`.
   Expose one accessor in `loop.hpp`, implemented over the two helpers already
   there:
   ```cpp
   // The room the player currently occupies. Read-only — no tick, no
   // transaction. Exists so main() can name the room the pregen scheduler
   // should look at (REQ-PREGEN-4) without duplicating the two lookups.
   int64_t playerRoom(Db& db);
   ```
   Deliberately **not** a widened `TurnResult`: the room is wanted at startup
   too, where no `TurnResult` exists, and widening the struct would touch every
   test that constructs one.
1. **`PregenGuard`** in `pregen.hpp`, mirroring `AiHttpGuard`:
   ```cpp
   struct PregenGuard {  // declare AFTER AiHttpGuard so it is destroyed BEFORE it
       PregenGuard() { pregenStart(); }
       ~PregenGuard() { pregenStop(); }        // join precedes aiHttpShutdown()
       PregenGuard(const PregenGuard&) = delete;
       PregenGuard& operator=(const PregenGuard&) = delete;
   };
   ```
   Declared as `main`'s **second** local, immediately below `httpGuard`
   (`main.cpp:18`). Reverse-destruction then gives join-before-`curl_global_cleanup`
   on *every* exit path — normal return, `quit`, EOF, `SchemaMismatch`, and both
   catches — without a single explicit call (REQ-PREGEN-19).
2. **Move the flush.** Today stdout is flushed only at the *top* of the next
   iteration, alongside the `"> "` prompt (`main.cpp:34-35`) — so anything placed
   after `fputs(result.output...)` runs while the player's text is still sitting
   in the buffer. Add an `fflush(stdout)` immediately after that `fputs`, so
   REQ-PREGEN-4's "after game output has been written" is literally true rather
   than nearly true. Same bytes in the same order; only the timing of the write
   syscall changes, so the byte-identical script diff below still holds.
3. **Startup queue:** `architectQueuePregen(db, playerRoom(db))` right after
   `renderStartup` (`main.cpp:30`), before the first prompt (REQ-PREGEN-4).
4. **Per-turn queue:** `architectQueuePregen(db, playerRoom(db))` after the flush
   above, for every outcome except `Quit` (D3). The tick has committed and the
   player already has their text on screen, so queuing cannot delay the turn they
   waited on.
5. **Dwell:** a `ScopedDwell` covering the gap from the prompt flush to `getline`
   returning (REQ-PREGEN-25, D6). This needs the loop's
   `if (!std::getline(std::cin, line)) break;` broken into a block so the timer
   destructs *before* the `break` decision:
   ```cpp
   bool eof = false;
   { const ScopedDwell dwell; eof = !std::getline(std::cin, line); }
   if (eof) break;   // EOF behaves as quit, unchanged
   ```
   The record is emitted on the EOF path too, which is correct — that was a real
   wait.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li><code>cmake --build build</code> clean; the game starts, plays, and quits by hand.</li>
<li><b>The AI-off byte-identical script</b> (this is the step where it starts mattering): <code>TEXTWORLD_AI=0</code> + a fixed scripted stdin, run on this branch and on <code>main</code>, <code>diff</code> of stdout empty. Note in the findings that this is REQ-PREGEN-2's check, <b>not</b> REQ-PREGEN-1's — Step 11 isolates that.</li>
<li>Code inspection recorded in the notes: <code>PregenGuard</code> is declared below <code>AiHttpGuard</code> and there is no other <code>pregenStop()</code> call site. <i>(REQ-PREGEN-19.)</i></li>
<li><code>playerRoom(db)</code> returns the id the fixture's <code>location</code> row holds and agrees with <code>renderStartup</code>'s subject — one small direct test.</li>
<li>Full suite green (<code>loop.hpp</code> only gained a declaration; no <code>TurnResult</code> construction changed).</li>
</ul>
</blockquote>

### Step 10 — Deterministic validation sweep
**Requirements:** all offline-checkable ones (sweep). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
**Files:** notes + a validation findings stub

No new code. Re-run every mechanical check as a batch and record the literal
output, so Step 11 is the only thing left that costs tokens or money.

- `cmake --build build` clean, `./build/tests` passes with **no** API key set.
- `grep -En "INSERT|UPDATE|DELETE|SELECT" src/pregen.cpp` → empty (paste it).
- `grep -n "Db" src/pregen.hpp src/pregen.cpp` → no parameter or member (paste it).
- `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` → empty.
- The AI-off byte-identical diff from Step 9, re-run.
- Inspection checklist, each with a `file:line`: worker owns its handle; the
  shared handle is never named in `pregen.cpp`; `pregenStop` precedes
  `aiHttpShutdown` on every exit path.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every command above run and its <b>actual output</b> recorded. A check that was not run is reported as not run — never inferred from an adjacent one.
</blockquote>

### Step 11 — One bounded live run
**Requirements:** REQ-PREGEN-1 (isolated), -9, -10, -14, -15, -16, -19, -20, -22, -23, -24, -25 (presence). **Size:** S code / M run · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>
**Files:** `.lore/work/validation/background-room-pregeneration/findings.md`

> ⚠️ **The one high-token-risk step** ([[verification-must-be-bounded]]). Follow
> the prior round's shape (`.lore/work/validation/turn-latency-polish/`): scripted
> stdin, `TEXTWORLD_PROFILE=1`, stderr captured to a log, **observations only**.
> Prompt quality is not under test and **no prompt is tuned here**. One session
> per row below; a row that fails is a finding, not a reason to re-run.

| # | Script | Expected observation | Reqs |
|---|--------|---------------------|------|
| 1 | enter a room, several non-movement turns, then walk a latent exit | `outcome=hit` with `age_turns>0`; that turn's `total` near an ordinary turn (~3–5 s), not ~10.8 s | -14, -23 |
| 2 | walk a latent exit immediately on entering | `outcome=miss`, `waited`, or `ran_queued`; turn completes either way | -15, -16 |
| 3 | **script 1 twice: `TEXTWORLD_PREGEN=0`, then default** | first log has **no** `kind=pregen` and **no** `background=1` records; second has both. The only check separating REQ-PREGEN-1 from -2 | -1 |
| 4 | any of the above | background `kind=call` records carry `background=1`, appear **outside** any turn's stage timings, and no line is interleaved or truncated across the two threads | -22, -24 |
| 5 | any of the above | one `kind=dwell` record per turn, right shape. **Value correctness is Step 1's unit test** — a piped script's true dwell is ~0, so this checks presence only | -25 |
| 6 | `quit` typed while a background generation is in flight | exits in well under a second; no crash, no hang, no libcurl warning. Under a leak/UB checker if one is already to hand | -19, -20 |
| 7 | a session with a deliberately invalid `ANTHROPIC_API_KEY` | background jobs all fail silently, the player sees the ordinary wall, and the log shows **no** retry storm — at most one background call per room entry | -9, -10 |

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b>
<ul>
<li>Findings written as measurements, each with the log lines that support it.</li>
<li>Any expectation that did not hold is written down as a finding, not explained away. A single-sample number is labeled n=1, as the brainstorm did for the 7.3 s generate figure.</li>
<li>The honest headline: did the worst turn stop being distinguishable from an ordinary one, and <b>what was the hit rate</b>? REQ-PREGEN-23 exists so this is measured rather than assumed.</li>
</ul>
</blockquote>

---

## Requirement coverage map

| Req | Covered by |
|---|---|
| REQ-PREGEN-1 gate `TEXTWORLD_PREGEN` | 4 (impl + unit), 5, 8, 9, **11 §3 (isolated)** |
| REQ-PREGEN-2 requires `architectEnabled()` | 5 (`pregenWorkerRunning` hook), 8, 9 |
| REQ-PREGEN-3 depth 1 | 8 |
| REQ-PREGEN-4 what/when queued | 8, 9 |
| REQ-PREGEN-5 snapshot on the main thread | 4 (job shape), 8 (the snapshot), 10 (grep) |
| REQ-PREGEN-6 exactly one serial worker | 5 |
| REQ-PREGEN-7 worker touches no DB | 4, 10 (grep, pasted) |
| REQ-PREGEN-8 own easy handle | 3, 5, 10 (inspection) |
| REQ-PREGEN-9 transport semantics preserved | 3, 4 (failure test), 11 §7 |
| REQ-PREGEN-10 failure clears, re-queues on re-entry | 4, 8 (occupancy test), 11 §7 |
| REQ-PREGEN-11 store keyed, never evicted | 4 |
| REQ-PREGEN-12 four distinguishable states | 4 (three), 5 (Running), 6 (queued-vs-running behavior) |
| REQ-PREGEN-13 staleness stamp, recorded only | 4, 7 (`age_turns`), 8 |
| REQ-PREGEN-14 commit = all of Phase 2 | **2 (the extraction)**, 7 (hit + enemy tests), 11 §1 |
| REQ-PREGEN-15 miss = today, exactly | 7 (unedited existing tests) |
| REQ-PREGEN-16 running waits, queued runs sync | 4 (queued), 6 (running + unrelated-job test) |
| REQ-PREGEN-17 single-writer preserved | 4/10 (no SQL in pregen), 7 (writes stay in the tick) |
| REQ-PREGEN-18 enemy selection unchanged | 2, 7 (both enemy cases), 8 (menu snapshot) |
| REQ-PREGEN-19 join before `aiHttpShutdown` | 6, 9 (guard ordering), 10 (inspection) |
| REQ-PREGEN-20 prompt quit | 3 (abort callback), 6 (stop-while-blocked), 11 §6 |
| REQ-PREGEN-21 bounded memory | 4 (one slot per key, by construction) |
| REQ-PREGEN-22 serialized emission | 1 (mutex + two-thread test), 11 §4 |
| REQ-PREGEN-23 one outcome record per walk | 7, 11 §1–2 |
| REQ-PREGEN-24 background calls marked | 1 (format), 3 (flag), 11 §4 |
| REQ-PREGEN-25 dwell time | 1 (**unit test = correctness**), 9 (call site), 11 §5 |

Every requirement has at least one **deterministic** owner; live checks
corroborate, they never stand alone. The three that carry the most risk —
REQ-PREGEN-16's queued-behind-unrelated-job case, REQ-PREGEN-14's
canon-equivalence, and REQ-PREGEN-22's interleaving — are each pinned by an
offline test, deliberately, because none of them is reliably observable in a
single scripted live session.

## Out of scope (restated from the spec, so no step drifts into it)

Parallel prefetch · depth 2 / momentum prefetch · candidate invalidation ·
coherent story generation · parser-first resolution · streaming narration ·
prompt caching · narrate-on-Sonnet. The staleness stamp is **recorded and
reported, never acted on**.
