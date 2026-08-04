---
title: "Implementation plan: bard-overture-and-scheduling"
date: 2026-08-04
status: executed
tags: [plan, bard, overture, cadence, scheduling, threading, worker, coalescing, shutdown-ordering, degradation]
modules: [bard, bardworker, world, main, aihttp, mutations, pregen]
related: [.lore/work/specs/bard-overture-and-scheduling.md, .lore/work/design/bard-overture-and-scheduling.md, .lore/work/specs/bard-catalog-selection.md, .lore/work/specs/bard-fact-store.md, .lore/work/plans/bard-catalog-selection.md, .lore/work/specs/background-room-pregeneration.md]
---

# Implementation plan: bard-overture-and-scheduling

Source spec: [bard-overture-and-scheduling](../specs/bard-overture-and-scheduling.md) (REQ-BARD-WAKE-1..-26).
Source design: [the overture and wake scheduling](../design/bard-overture-and-scheduling.md).

Brick **3 of 4**. Bricks 1 ([fact store](bard-fact-store.md)) and 2 ([catalog selection](bard-catalog-selection.md)) shipped a schema, write helpers, eligibility, both context payloads, both request bodies, both validation gates, and admission — all of it **inert**. This brick is the one that runs them: one blocking call at world creation, a trigger predicate over the event log, a second worker thread, and the commit path. It is also the only brick with concurrency in it, so *"the bard failing never makes the game worse than not having a bard"* stops being a sentence in a design and becomes test 21.

**What ships:** a new `bardworker` translation unit (thread + singleton state machine, no database), the overture and the post-turn hook in `bard.cpp`, an `OpenedWorld` return from `openWorld`, a fourth `AiRole`, a per-call transport timeout, one new mutation helper, and the `main()` guard ordering that makes shutdown safe on every exit path.

**What does not ship:** the architect's `story` field, materialization at room generation, and `catalogForHandle` reaching a room-scoped selection — all brick 4 ([bard-architect-integration](../specs/bard-architect-integration.md)).

## Guiding constraints

1. **Degradation is the acceptance criterion, not a caveat.** Every step below is written so that its failure mode is "the bard did nothing" rather than "the turn behaved differently." Step 10 tests exactly that, five ways.
2. **The single-writer rule is not negotiable.** `bardworker.cpp` takes no `Db`, holds no `Db`, and issues no SQL. Two greps enforce it, exactly as they do for pregen (`src/pregen.hpp:12-24`).
3. **Shutdown ordering falls out of the shape of `main()`, never out of remembering.** Two worker guards, both below `AiHttpGuard`, reverse-destructed on every exit path including the throws.
4. **Nothing the player waited on is delayed.** Trigger evaluation and result commit both run after the tick has committed and after the text has been flushed — the same rule and the same call site shape as `architectQueuePregen` (`src/main.cpp:74-81`).
5. **No new prompt content, no new wire format, no new eligibility rule.** Brick 2 owns all of that and is not reopened. This brick only decides *when* those functions run and *on which thread*.
6. **Every test runs with a fake transport and no network.** The suite's hermetic rule (`tests/tests.cpp:9744` main) is unchanged.

## Seams this touches (verified in tree)

| Seam | Where | What happens to it |
|---|---|---|
| `openWorld` | `src/world.hpp:43`, `src/world.cpp:165` | returns `OpenedWorld{db, created}`; 145 call sites in `tests/tests.cpp` updated mechanically |
| `main()` guard order | `src/main.cpp:21-45` | `PregenGuard` **moves** from second local to below the overture; `BardGuard` added below it |
| turn loop tail | `src/main.cpp:66-81` | `bardAfterTurn(db)` added after `architectQueuePregen`, after the `fflush` |
| `AiRole` | `src/aihttp.hpp:20`, `src/aihttp.cpp:161,173` | gains `Bard` → `roleName` `"bard"`, `modelForRole` `claude-opus-4-8` |
| `CURLOPT_TIMEOUT` | `src/aihttp.cpp:109` (hardcoded `8L`) | becomes a `performPost` parameter defaulted to `kAiHttpTimeoutSeconds` |
| threading contract | `src/aihttp.hpp:57-73` ("the ONE sanctioned second handle") | rewritten for **two** sanctioned worker handles (REQ-BARD-WAKE-17) |
| pregen's "SECOND local" instruction | `src/pregen.hpp:196-202` | reworded: the invariant is *below `AiHttpGuard`*, not *second* |
| bard unit | `src/bard.hpp`, `src/bard.cpp` (957 lines, SELECT-only) | gains the overture, the post-turn hook, the trigger query, the commit; stays free of literal `INSERT`/`UPDATE`/`DELETE` |
| mutations | `src/mutations.hpp:253-262` | gains `writeBardWakeTurn` |
| CMake | `CMakeLists.txt` twcore source list | gains `src/bardworker.cpp` |
| test helpers | `tests/tests.cpp:4911` `BlockingTransport`, `:4952` `spinUntil`, `:5643` `tickT` | reused as-is; bard tests are appended **after** `tickT`'s definition |

Prior art copied deliberately, not invented: `src/pregen.cpp:158` `workerMain` (thread body, own handle inside the thread), `:212` `stopWorker` (signal → notify → join), `src/architect.cpp:488` `architectQueuePregen` (main-thread snapshot at queue time), `src/loop.cpp:113-134` (a transaction with rollback on any throw).

## Micro-decisions pinned before drafting

Three were put to the author and answered; the rest are recorded so implementation does not re-litigate them.

**1. `openWorld` returns `OpenedWorld`, all 145 call sites updated mechanically.** *(author-approved)* One signature, no second door. `Db db = openWorld(p, s).db;` still move-constructs: member access on a prvalue is an xvalue. The edit is a scripted regex, verified by a clean build rather than by reading 145 diffs.

**2. A fourth `AiRole::Bard`.** *(author-approved)* `roleName` `"bard"`, `modelForRole` `claude-opus-4-8`. Reusing `Generate` would make a profile log unable to distinguish a bard wake from a pregen room job — precisely the two workloads design decision 2.4 separates.

**3. `bardWaitingCountForTest` counts threads inside a test-only drain wait.** *(author-approved)* The bard's tick never blocks, so spec test 15's "once the tick has provably reached its wait" has no production waiter to count. `bardWaitForIdleForTest()` blocks until an in-flight wake has finished and its result has been taken; the count reports threads inside it. Test 15's shape survives verbatim: spawn a drain thread → `spinUntil(count == 1)` → release the blocking transport. `bardStateNow()` is exposed alongside it so tests can also synchronize on `Running` the way pregen's do with `pregenStateOf`.

**4. Two units, split exactly as pregen/architect are.** `bardworker.{hpp,cpp}` owns the thread, the state machine, the abort flag, and the guard — no `Db`, no SQL. `bard.cpp` owns everything that touches the database: the overture, the trigger query, the snapshot, the stamp, and the commit. This is the same division as `pregen.cpp` (thread) / `architect.cpp` (scheduler), and it is what makes the two greps in REQ-BARD-WAKE-19 mechanically true rather than aspirational.

**5. No `Queued` state.** Pregen distinguishes `Queued` from `Running` because its tick treats them differently (`src/pregen.hpp:61-67`). Nothing in the bard ever waits on that distinction — there is one job, one worker, and no acquire path. The state machine is exactly the design's: `Idle → Running → Ready → Idle`, plus a dirty flag.

**6. The job carries a finished request body.** `buildWakeContext` + `motiveKeys` + `buildWakeRequestBody` all run on the main thread at queue time; what crosses to the worker is `{requestBody, motives, snapshotTurn}`. The worker calls the transport and then `validateWakeResponse` — both pure — and stores a `WakeProposal`. The `motives` vector rides along because the gate takes it (`src/bard.hpp:244`).

**7. The AI/bard guard lives at the top of `bardOverture`, not textually in `main()`.** REQ-BARD-WAKE-2's requirement is that a disabled run **constructs no transport and makes no call**; a guard as the function's first two lines gives that, and — unlike a guard buried in `main()` — spec test 12b can actually assert it. `main()` keeps the `created` half of the condition, which is the half it alone knows.

**8. `bardAfterTurn(db)` is one call site: commit first, then evaluate.** Committing first is what lets REQ-BARD-WAKE-14's "one further evaluation after that wake commits" happen on the same turn rather than the next one.

**9. The dirty flag is belt to the query's braces, and is documented as such.** Because `bard_last_wake_turn` is stamped at *queue* time (REQ-BARD-WAKE-11), the event query re-finds the events that arrived mid-wake, so the flag is not the only thing preventing loss. It is still built, set, and consumed as REQ-BARD-WAKE-14 requires — `if (!dirty && !hasTriggeringEvent(...)) return;` — so it is read on every evaluation rather than being dead state, and test 14 pins its behavioral meaning: exactly one further wake, not zero and not two.

**10. The stamp precedes the submit.** `writeBardWakeTurn(db, turn)` in its own transaction, then `bardSubmit(job)`. If the stamp throws, nothing was queued; if it succeeds, no in-flight wake can re-trigger itself (REQ-BARD-WAKE-11). Only the main thread submits and only the worker moves `Running → Ready`, so the `Idle` check cannot be invalidated underneath the caller.

**11. `kBardOvertureTimeoutSeconds` lives in `bard.hpp`, beside `bardOverture`.** The exception belongs to the bard; `aihttp` stays generic with a defaulted parameter. The justifying comment sits on the constant, where mechanical check 5 greps for it.

**12. Wake calls keep the ordinary 8 s timeout.** Only the overture is the exception. `AiHttpWorkerClient::post` is unchanged.

**13. REQ-BARD-WAKE-24's "selections re-checked live at commit" is already `applyWakeProposal`.** A wake proposal carries no room-scoped selection — it carries appended entries (re-checked by `catalogEntryRefusal`, `src/bard.hpp:266`) and `mark_seeded` handles (resolved live by `catalogIdForHandle`). `catalogForHandle`'s room-scoped re-check belongs to brick 4, where a selection finally has a room. Spec test 18 is therefore driven through a handle that became unresolvable between snapshot and commit — see step 8's gate.

**14. `AiRole::Bard` is wired at four sites, not one.** The enum member is useless unless every bard call carries it. Brick 2 already hardcodes `modelForRole(AiRole::Generate)` in both request-body builders (`src/bard.cpp:580` and `:656`) — both become `AiRole::Bard`. The overture binds `makeAnthropicTransport(AiRole::Bard, kBardOvertureTimeoutSeconds)`, and the worker calls `client->post(body, AiRole::Bard)`. The model string is unchanged (`claude-opus-4-8` for both roles), so brick 2's request-body tests are expected to pass unedited — if one fails, it was asserting the role, and that assertion is what changes.

**15. Both public entry points are total functions: nothing escapes them.** `bardOverture` and `bardAfterTurn` each wrap their **entire** body — not just the transport call — in `try` / `catch (const std::exception&)` / `catch (...)`, each emitting one stderr diagnostic. This is not belt-and-braces: brick 2's builders and the trigger query are ordinary `db.hpp` callers, and `db.hpp` throws `std::runtime_error` for any SQLite fault, so an unguarded builder or `SELECT` would propagate into `main()`'s `catch (const std::exception&)` — printing `fatal:` and ending the session on world creation, or killing a turn the player had already been shown. That is the *inverse* of the degradation claim. The transaction's own `rollback` sits **nested inside** that outer guard, so a commit failure still rolls back before the outer catch reports it.

**16. Test 21 drives turns through `tickT` + `render`, not `runTurn`.** The bard requires `aiNarrationEnabled()`, and `runTurn` with narration enabled reaches the *production* resolver and prose transports (`src/loop.cpp:73,141`) — which the suite forbids. `tickT` (`tests/tests.cpp:5643`) takes an injected transport and the template `render` produces the bytes compared. This is the same accommodation every pregen commit test already makes (`tests/tests.cpp:5832`). The `main()`-side ordering that `tickT` cannot exercise — flush before hook — is pinned by a source-text assertion instead (step 7), following the precedent of the append-only source assertions from brick 1.

**17. The overture's player-facing line is authored content and flushed explicitly.** One line to stdout via `std::fputs`, followed by `std::fflush(stdout)` — without the flush a piped stdout would hold the line in the buffer for the whole 60 s, which is exactly the "looks hung" failure REQ-BARD-WAKE-4 exists to prevent. Proposed text is in step 6 and is the one thing in this plan the author may simply want to reword.

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.7; padding: 8px 0;">
<b>1</b> <code>OpenedWorld</code> + 145 call sites <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
<b>2</b> <code>AiRole::Bard</code> + per-call timeout + threading contract <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
<b>3</b> <code>writeBardWakeTurn</code> <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;↓ (1,2,3 independent of each other; all three precede 4)<br>
<b>4</b> <code>bardworker</code> skeleton: TU, CMake, env gate, thread, guard <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>5</b> the job, the state machine, coalescing, the worker body <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> ← concurrency<br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>6</b> the overture + <code>main()</code> guard ordering <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> ← needs 1,2,4<br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>7</b> trigger evaluation (ceiling, query, snapshot, stamp, submit) <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> ← needs 3,5<br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>8</b> committing a ready result <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>9</b> coalescing end-to-end through the engine <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> ← concurrency<br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>10</b> the degradation claim, five arms <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> ← the point of the brick<br>
&nbsp;&nbsp;&nbsp;&nbsp;↓<br>
<b>11</b> docs + the full mechanical sweep <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>
</div>

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified ·
<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> threads or ordering — a bug shows up as a hang or a flake, so every gate synchronizes on state rather than on sleeps.

**Suggested session split** (token risk): steps **1–5** in one `/implement` session (mechanical churn + the worker, no engine wiring), steps **6–8** in a second (the three database-touching paths), steps **9–11** in a third (the concurrency and degradation suites, which are test-heavy and read a lot of existing test code). Step 1's 145-site edit is scripted; do not let it be done by hand.

---

### Step 1 — `OpenedWorld`, and the 145 call sites

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> · REQ-BARD-WAKE-1

In `src/world.hpp`, replace the `Db openWorld(...)` declaration with:

```cpp
struct OpenedWorld {
    Db db;
    bool created = false;   // true iff initialize() ran on THIS call
};
OpenedWorld openWorld(const std::string& path,
                      const std::string& seedPath = "seed/base.sql",
                      const std::string& settingPath = "seed/setting.txt");
```

Carry the existing contract comment (`src/world.hpp:24-42`) forward and add one clause: `created` is the once-ever hook the overture hangs on, and it is false for every resumed session.

In `src/world.cpp:165`, the initialize branch returns `{std::move(db), true}` and the existing-file branch `{std::move(db), false}`. The `SchemaMismatch` path is untouched — it still throws before constructing anything.

In `src/main.cpp`, `auto world = openWorld("world.db"); Db& db = world.db;` — no other change in this step; guard reordering is step 6.

Then the mechanical edit over `tests/tests.cpp`. All 145 sites are the single-line form `… = openWorld(…);` (verified: `grep -c "openWorld(.*);"` = 145 = total occurrences):

```
perl -pi -e 's/(=\s*openWorld\([^;]*\))\s*;/$1.db;/' tests/tests.cpp
```

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean. <code>grep -c "openWorld(" tests/tests.cpp</code> unchanged at 145 and <code>grep -c "openWorld(.*)\.db" tests/tests.cpp</code> = 145 — no site missed, none double-edited. The suite runs with the <b>same check count and zero failures</b> as before the edit (record the count from the pre-edit run and compare), which is the real proof that a mechanical rewrite changed no behavior. One new assertion in <code>testWorld</code>: opening a fresh temp path yields <code>created == true</code>; opening the <b>same</b> path a second time yields <code>created == false</code> with the world intact.
</blockquote>

---

### Step 2 — `AiRole::Bard`, a per-call timeout, and the threading contract

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> · REQ-BARD-WAKE-5, REQ-BARD-WAKE-17

`src/aihttp.hpp`:

- `enum class AiRole { Resolve, Narrate, Generate, Bard };` and extend the "three AI call sites" comment (`:18`) to four.
- `inline constexpr long kAiHttpTimeoutSeconds = 8;` — the uniform budget, named so the exception has something to be an exception *to*.
- `HttpResponse anthropicPost(const std::string&, AiRole, long timeoutSeconds = kAiHttpTimeoutSeconds);`
- `HttpTransport makeAnthropicTransport(AiRole, long timeoutSeconds = kAiHttpTimeoutSeconds);`
- Rewrite the threading contract block (`:57-73`): `AiHttpWorkerClient` is no longer "the ONE sanctioned second handle" but **the type every sanctioned worker handle uses**, of which there are now two live instances — the pregen worker's and the bard worker's — each owned by, constructed on, and destroyed on its own thread, and each joined before `aiHttpShutdown()`. Name both guards and their required position below `AiHttpGuard`.

`src/aihttp.cpp`: thread `long timeoutSeconds` into `performPost` (`:77`) and use it at `:109` in place of the literal `8L`; add `Bard` to `roleName` (`"bard"`) and to `modelForRole` (falls in with `Narrate`/`Generate` → `claude-opus-4-8`, and the `TEXTWORLD_MODEL` override still wins at level 1). `AiHttpWorkerClient::post` passes `kAiHttpTimeoutSeconds`, unchanged in behavior.

`src/bard.cpp:580` and `:656`: both request-body builders switch `modelForRole(AiRole::Generate)` → `modelForRole(AiRole::Bard)` (micro-decision 14). Do this **here**, in the same step that introduces the enumerator — a role added in step 2 and wired in step 6 is a role that spends four steps being decorative, and the two remaining sites (the overture's transport binding and the worker's `post`) are then the only ones left to get right.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> build clean, existing suite green — <b>including every profile record test and brick 2's request-body tests</b>, since a fourth enumerator must not disturb the three existing <code>role=</code> strings and the model string is identical across the switch. New assertions in the aihttp tests: <code>roleName(AiRole::Bard) == "bard"</code>; <code>modelForRole(AiRole::Bard) == "claude-opus-4-8"</code>; with <code>TEXTWORLD_MODEL</code> set, <code>modelForRole(AiRole::Bard)</code> returns the override. <code>grep -n "AiRole::Generate" src/bard.cpp</code> is <b>empty</b> — the wiring check that makes micro-decision 14 real rather than nominal, and the one a role-in-isolation test cannot give. Mechanical check 4 (spec): <code>grep -n "ONE sanctioned second handle" src/aihttp.hpp</code> is <b>empty</b>, and the contract block names two worker guards.
</blockquote>

---

### Step 3 — `writeBardWakeTurn`

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> · REQ-BARD-WAKE-11, REQ-BARD-STORE-18

`meta.bard_last_wake_turn` is written by this brick, and REQ-BARD-STORE-18 says `mutations.cpp` is the only unit that may write it — brick 1 created the row but shipped no setter, so one is added here beside `writeBardJournal`/`writeBardFocus` (`src/mutations.hpp:253-262`):

```cpp
// Upsert meta.bard_last_wake_turn. Stamped when a wake is QUEUED, never when
// it completes (REQ-BARD-WAKE-11), so an in-flight wake cannot re-trigger
// itself. Never begins, commits, or rolls back — the caller owns the
// transaction, exactly like every other helper in this file.
void writeBardWakeTurn(Db& db, int64_t turn);
```

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardStoreMeta</code> gains: a fresh world reads <code>bard_last_wake_turn == 0</code>; after <code>writeBardWakeTurn(db, 7)</code> it reads 7; a second call with 12 stores 12 (free rewrite, not append); the write inside a caller-opened transaction that is then <b>rolled back</b> leaves 0. Brick 1's mechanical check 2 still passes: <code>grep -En "INSERT|UPDATE|DELETE" src/*.cpp | grep -E "catalog|bard_journal|bard_focus|bard_last_wake_turn"</code> returns <b>only</b> <code>src/mutations.cpp</code> lines.
</blockquote>

---

### Step 4 — The `bardworker` skeleton: unit, CMake, env gate, thread, guard

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> · REQ-BARD-WAKE-16, -18, -19, -20, -26 (partial)

New `src/bardworker.hpp` / `src/bardworker.cpp`, added to the `twcore` source list in `CMakeLists.txt`. **Skeleton first**: the thread starts, parks, and stops; it processes nothing yet. Its header opens with the same "why this is its own translation unit" block `src/pregen.hpp:1-33` carries, stating the two greps.

This step ships:

```cpp
bool bardEnabled();                  // TEXTWORLD_BARD, off only when exactly "0"
void bardRefreshEnabledForTest();    // mirrors pregenRefreshEnabledForTest
void bardStart();                    // no-op unless bardEnabled() && aiNarrationEnabled()
void bardStop();                     // signal → notify → join; idempotent
bool bardWorkerRunning();
void bardSetWorkerTransportForTest(HttpTransport);
void bardResetForTest();
struct BardGuard { BardGuard() { bardStart(); } ~BardGuard() { bardStop(); } /* non-copyable */ };
```

`readBardEnv` copies `src/pregen.cpp:27` exactly, including the "unset means on" rationale. `stopWorker` copies `src/pregen.cpp:212`, including the comment about deliberately not holding the mutex across the join. The worker body is `workerMain`'s skeleton: construct an `AiHttpWorkerClient(&g_stopping)` inside the thread (or take the injected test transport), park on the condvar, break on stop, destroy the client on the way out.

`aiNarrationEnabled()` is the AI half of the gate, matching `pregenStart`'s use of `architectEnabled()` (`src/pregen.cpp:266`).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> build clean. The two REQ-BARD-WAKE-19 greps: <code>grep -En "INSERT|UPDATE|DELETE|SELECT" src/bardworker.cpp</code> <b>empty</b>; <code>grep -n "Db" src/bardworker.hpp src/bardworker.cpp</code> shows <b>no parameter and no member</b> (comment prose only, as pregen's does). New <code>testBardWorkerLifecycle</code> (spec tests 19, 20-partial): with <code>TEXTWORLD_BARD=0</code>, <code>bardStart()</code> creates <b>no thread</b> — asserted via <code>bardWorkerRunning() == false</code>, never via absent log lines; with the bard on but no API key (AI off), likewise no thread; with both on and a fake transport installed, <code>bardWorkerRunning()</code> is true after start and false after stop; <code>bardStart()</code> twice creates one thread; <code>bardStop()</code> twice and <code>bardStop()</code> with no thread ever started both complete without hanging; a <code>BardGuard</code> in a scope starts and joins.
</blockquote>

---

### Step 5 — The job, the state machine, coalescing, and the worker body

<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> · REQ-BARD-WAKE-13, -14, -16, -19, -25, -26

Fill in `bardworker`. No database, no engine wiring — this step is testable entirely against fake transports and hand-built jobs, which is why it is separate from step 7.

```cpp
struct BardJob {
    std::string requestBody;              // built on the MAIN thread at queue time
    std::vector<std::string> motives;     // the vocabulary the gate checks against
    int64_t snapshotTurn = 0;             // recorded and reported only
};

enum class BardState { Idle, Running, Ready };

BardState bardStateNow();                       // safe from either thread
bool bardSubmit(BardJob job);                   // Idle → Running, true; else dirty := true, false
bool bardTakeDirty();                           // consume the flag (main thread)
std::optional<WakeProposal> bardTakeReady();    // Ready → Idle, hands the result over
std::size_t bardWaitingCountForTest();
void bardWaitForIdleForTest();                  // blocks until no wake is in flight
```

The worker body: park → take the job → `Running` is already set by `submit` → drop the mutex → `transport(job.requestBody)` (production: `client->post(job.requestBody, AiRole::Bard)`, the 8 s budget, micro-decision 12) → `validateWakeResponse(resp, job.motives)` → retake the mutex → store `Ready` (with the proposal, which may be an empty-but-successful wake) or fall back to `Idle` on a rejected response. A `try`/`catch (const std::exception&)`/`catch (...)` wraps the transport call with one stderr diagnostic saying `bard`, copying `src/pregen.cpp:103-117`'s reasoning: a throwing transport must never escape onto a thread with no caller to catch it.

The mutex is never held across the transport call. `g_stopping` doubles as the `AiHttpWorkerClient` abort flag, so `bardStop` tears down an in-flight transfer at libcurl's next progress callback rather than waiting out the timeout. A `Ready` result at stop is simply dropped (REQ-BARD-WAKE-25) — `bardResetForTest` and `bardStop` both leave nothing persisted.

`bardWaitForIdleForTest` increments a guarded counter, waits on the condvar until the state is not `Running`, decrements, and returns; that counter is what `bardWaitingCountForTest` reports (micro-decision 3).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardWorkerJobs</code>, modeled on <code>testPregenWorker</code> (<code>tests/tests.cpp:4960</code>) and using the existing <code>BlockingTransport</code> (<code>:4911</code>) and <code>spinUntil</code> (<code>:4952</code>) — no sleeps anywhere. (a) One submit with a canned wake response reaches <code>Ready</code>, and <code>bardTakeReady</code> yields the parsed <code>WakeProposal</code> once and <code>nullopt</code> the second time, leaving <code>Idle</code>. (b) <b>Singleness (REQ-BARD-WAKE-13):</b> with the worker held mid-call, three further <code>bardSubmit</code> calls each return <b>false</b>, and <code>BlockingTransport::concurrentEntry</code> is false and <code>callCount() == 1</code> after release. (c) <b>Dirty (REQ-BARD-WAKE-14):</b> those refused submits set the flag; <code>bardTakeDirty()</code> returns true <b>once</b> and false after. (d) <b>The drain hook:</b> spawn a thread in <code>bardWaitForIdleForTest</code>, <code>spinUntil(bardWaitingCountForTest() == 1)</code>, release, join — proving the release cannot race the call. (e) A transport that <b>throws</b> and one that returns a non-200 both leave the state <code>Idle</code>, the thread alive, and a subsequent submit accepted. (f) <code>bardStop()</code> with the transport blocked mid-call completes — the abort flag path (REQ-BARD-WAKE-20 shutdown half, spec test 20). (g) A <code>Ready</code> result is <b>discarded</b> by <code>bardStop</code> (REQ-BARD-WAKE-25).
</blockquote>

---

### Step 6 — The overture, and `main()`'s guard ordering

<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> · REQ-BARD-WAKE-2..-7, -18

`src/bard.hpp` gains:

```cpp
// A DELIBERATE EXCEPTION to the uniform 8 s budget (REQ-BARD-WAKE-5). The
// overture is a bulk generation of the whole catalog at high effort, run ONCE
// per world file, with the player already waiting and told so, with nothing
// else running, and with no fallback that produces a better catalog — the
// alternative to waiting is an empty one. 8 s would cut it off mid-write.
// Do NOT normalize this back to kAiHttpTimeoutSeconds.
inline constexpr long kBardOvertureTimeoutSeconds = 60;

// The one cold call, on the MAIN thread, before any worker exists. Blocking.
// Runs only when the world was created THIS launch (the caller's half of the
// condition) and only when the bard and AI are enabled (checked here, first
// thing, so a disabled run constructs no transport and makes no call at all).
// NEVER throws: every failure yields an empty catalog and today's game
// (REQ-BARD-WAKE-6). `transport` overrides the production transport; pass
// nullptr in production, exactly as pregenAcquire does.
void bardOverture(Db& db, const HttpTransport* transport);
```

`bard.cpp`'s implementation. The guard is the only thing outside the exception boundary; **everything after it is inside one** (micro-decision 15), because the context builders and the admission path are ordinary `db.hpp` callers and an escape from here reaches `main()`'s `catch` and ends the session:

```cpp
void bardOverture(Db& db, const HttpTransport* transport) {
    if (!bardEnabled() || !aiNarrationEnabled()) return;   // no transport, no call
    std::fputs("The school is being written…\n", stdout);
    std::fflush(stdout);                                   // REQ-BARD-WAKE-4
    try {
        const HttpTransport t = transport != nullptr
            ? *transport
            : makeAnthropicTransport(AiRole::Bard, kBardOvertureTimeoutSeconds);
        const std::vector<std::string> motives = motiveKeys(db);
        const std::string body =
            buildOvertureRequestBody(buildOvertureContext(db), motives);
        const auto proposal = validateOvertureResponse(t(body), motives);
        if (!proposal) return;                             // empty catalog, supported
        db.begin();
        try {
            admitOvertureProposal(db, *proposal);
            db.commit();
        } catch (...) {
            db.rollback();                                 // REQ-BARD-WAKE-7: zero rows
            throw;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bard: overture failed: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "bard: overture failed (non-std)\n");
    }
}
```

The nested `catch` exists only to roll back before rethrowing, so a helper throwing mid-admission leaves **zero** catalog rows rather than a partial set (REQ-BARD-WAKE-7) *and* still gets its one diagnostic from the outer handler. There is exactly one diagnostic per failure, never two.

Proposed line (authored content — reword freely):

```
The school is being written…
```

`src/main.cpp` becomes:

```cpp
const AiHttpGuard httpGuard;          // curl_global_init — must precede any handle
auto world = openWorld("world.db");
Db& db = world.db;
if (world.created) bardOverture(db, nullptr);   // main thread, blocking, 60 s
const PregenGuard pregenGuard;        // worker one
const BardGuard   bardGuard;          // worker two — joined FIRST on every exit
```

The comment on `PregenGuard` (`src/main.cpp:22-27`) and the "DECLARE IT AS main()'s SECOND LOCAL" instruction in `src/pregen.hpp:196-202` both need rewording: the load-bearing invariant is *below `AiHttpGuard`*, not *second*, and there are now two guards that share it. `openWorld` throwing `SchemaMismatch` before either guard is constructed is fine and unchanged — there is nothing to join.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> mechanical check 5: <code>kBardOvertureTimeoutSeconds</code> is 60 and its comment contains "DELIBERATE EXCEPTION" — substring check in a test, not a manual grep. New <code>testBardOverture</code>: (spec test 6) a <b>new</b> world with a canned <code>write_catalog</code> response writes the expected rows and <code>meta.bard_journal</code>; reopening the <b>same</b> path invokes the transport <b>zero</b> times, driven off <code>OpenedWorld::created</code>. (spec test 7) each of non-200, transport <code>throw</code>, unparseable body, and no tool call leaves <b>zero</b> catalog rows, throws nothing, and a following <code>tickT</code> + <code>render</code> behaves normally. (spec test 8) a canned response whose entries are valid but whose admission is made to throw mid-way leaves <b>zero</b> catalog rows — the single-transaction proof — and <code>bardOverture</code> itself <b>returns normally</b>, asserted by executing a statement after the call rather than by the absence of a crash. One further arm beyond the spec's list, from micro-decision 15: with the world's <code>motive_catalog</code> dropped so <code>buildOvertureContext</code>/<code>motiveKeys</code> throw, <code>bardOverture</code> still returns normally and writes nothing — the <b>builders</b> are inside the guard, not merely the transport. (spec test 12b) with <b>AI disabled</b> (no key), a new world invokes a counting transport <b>zero</b> times, and with <code>TEXTWORLD_BARD=0</code> likewise zero. A source-text assertion on <code>src/main.cpp</code>: the offsets of <code>AiHttpGuard</code> &lt; <code>openWorld</code> &lt; <code>bardOverture</code> &lt; <code>PregenGuard</code> &lt; <code>BardGuard</code> are strictly increasing (REQ-BARD-WAKE-3, -18) — the ordering is undefined behavior if broken, so it is pinned by a regression guard rather than by review.
</blockquote>

---

### Step 7 — Trigger evaluation: ceiling, query, snapshot, stamp, submit

<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> · REQ-BARD-WAKE-8..-12, -19

`src/bard.hpp` gains the engine-owned constant and the one post-turn entry point:

```cpp
// The rate ceiling (REQ-BARD-WAKE-9). Engine-owned, NOT model-visible and not
// configurable at runtime by the model — the same standing the combat
// constants have. At a 5-turn floor the bard cannot exceed 0.2 wakes/turn no
// matter how frantically the player generates irreversible events.
// "Tunable" means this one line.
inline constexpr int64_t kBardMinTurnGap = 5;

// THE ONE CALL THE TURN LOOP MAKES, on the main thread, AFTER the tick's
// transaction has committed and AFTER the player's text has been flushed —
// so neither half can ever delay the turn the player waited on. Commits a
// ready result first (its own transaction), then evaluates the trigger.
// No-op when the bard or AI is off. Never throws.
void bardAfterTurn(Db& db);
```

The evaluation half, in `bard.cpp`. The whole body is inside `bardAfterTurn`'s exception boundary (micro-decision 15) — the trigger query, the snapshot builders, and `writeBardWakeTurn` are all `db.hpp` callers, and an escape from here kills a turn the player has already been shown. Order is spelled out as code because the order **is** the requirement and prose left two readings open:

```cpp
// caller: bardAfterTurn, inside its try. Returns void; never throws.
void evaluateTrigger(Db& db) {
    if (!bardEnabled() || !aiNarrationEnabled()) return;

    const int64_t turn     = readMetaInt(db, "turn");
    const int64_t lastWake = readMetaInt(db, "bard_last_wake_turn");

    // (1) THE CEILING, FIRST (REQ-BARD-WAKE-9) — cheapest, and it must hold
    // however many triggers fired. A trigger arriving inside the gap is NOT
    // lost: bard_last_wake_turn is stamped at queue time, so the query below
    // re-finds those events on the first evaluation past the gap.
    if (turn - lastWake < kBardMinTurnGap) return;

    // (2) The flag is CONSUMED here, unconditionally, and may be re-set at
    // (3) below. Consume-then-maybe-reset, deliberately: reading it after the
    // busy check would leave it set across an evaluation that already ran,
    // and REQ-BARD-WAKE-14 says ONE further evaluation, not one per turn.
    const bool dirty = bardTakeDirty();
    if (!dirty && !hasTriggeringEvent(db, lastWake)) return;

    // (3) Busy: record the trigger, build nothing, queue nothing.
    if (bardStateNow() != BardState::Idle) { bardSetDirty(); return; }

    // (4) The snapshot, on the MAIN thread (REQ-BARD-WAKE-19). What crosses
    // the thread boundary is text.
    BardJob job;
    job.motives     = motiveKeys(db);
    job.requestBody = buildWakeRequestBody(buildWakeContext(db), job.motives);
    job.snapshotTurn = turn;

    // (5) Stamp BEFORE submit (micro-decision 10): if the stamp throws,
    // nothing was queued; if it lands, no in-flight wake can re-trigger
    // itself (REQ-BARD-WAKE-11).
    db.begin();
    try { writeBardWakeTurn(db, turn); db.commit(); }
    catch (...) { db.rollback(); throw; }

    bardSubmit(std::move(job));
}
```

`hasTriggeringEvent` is the spec's query verbatim (REQ-BARD-WAKE-10):

```sql
SELECT 1 FROM events
 WHERE turn > ?                      -- meta.bard_last_wake_turn
   AND verb IN ('generated','defeated','learned','materialized')
 LIMIT 1;
```

All four verbs verified live in tree: `generated` at `src/mutations.cpp:557`, `defeated` at `:399`, `materialized` at `:675`, `learned` at `src/combat.cpp:374`. No verb's semantics change.

`bardAfterTurn` itself is the two halves plus the guard:

```cpp
void bardAfterTurn(Db& db) {
    try {
        commitReady(db);        // step 8 — its own transaction, first
        evaluateTrigger(db);    // above
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bard: after-turn failed: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "bard: after-turn failed (non-std)\n");
    }
}
```

Wire it into `src/main.cpp`'s loop tail, after `architectQueuePregen` and after the existing `fflush` (`src/main.cpp:74-81`), with a comment naming REQ-BARD-WAKE-8.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardTrigger</code> against <code>tests/combat_fixture.sql</code>, driving turns with <code>tickT</code> + a fake bard transport. (spec test 9) a turn producing only <code>moved</code>/<code>took</code>/<code>looked</code>/<code>waited</code>/<code>failed</code> queues nothing — transport invoked <b>zero</b> times. (spec test 10) one sub-case <b>per verb</b> — <code>generated</code>, <code>defeated</code>, <code>learned</code>, <code>materialized</code> — each queues exactly one wake. (spec test 11) with <code>turn - bard_last_wake_turn &lt; kBardMinTurnGap</code> a qualifying event queues <b>nothing</b>; advancing past the gap then queues on the next qualifying event. (spec test 12) <code>bard_last_wake_turn</code> equals the queueing turn <b>while the blocking transport is still inside the call</b> — asserted before release, which is what makes "stamped at queue time" a fact rather than a comment. (spec test 12a) with a blocking transport, the turn's rendered player text is <b>complete and equal to the bard-off bytes</b> at the moment <code>bardAfterTurn</code> is entered, and the transport call count is <b>0</b> until then; plus a source-text assertion that <code>src/main.cpp</code>'s <code>fflush</code> precedes its <code>bardAfterTurn</code> call (micro-decision 16). REQ-BARD-WAKE-12 is asserted by absence: <code>grep</code> shows no new table, no cache, and no member holding trigger state. And the boundary of micro-decision 15: with <code>meta</code>'s <code>bard_last_wake_turn</code> row deleted so the evaluation path throws, a turn still renders its normal text and <code>bardAfterTurn</code> returns — the assertion that a broken bard cannot cost the player a turn they already paid for.
</blockquote>

---

### Step 8 — Committing a ready result

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> · REQ-BARD-WAKE-21..-24

The commit half of `bardAfterTurn`, run **first** (micro-decision 8): `bardTakeReady()` → if empty, return → `db.begin()` → `applyWakeProposal(db, *ready)` → `db.commit()`; any `std::exception` (or `...`) rolls back, emits **one** diagnostic, and returns. `meta.turn` is never touched here (REQ-BARD-WAKE-22) — a wake is not a turn — and `bard_last_wake_turn` is not advanced beyond its queue-time stamp on failure (REQ-BARD-WAKE-23), so the next irreversible event triggers a fresh wake.

The live re-check REQ-BARD-WAKE-24 asks for is already inside `applyWakeProposal`: appended entries pass `catalogEntryRefusal` against the *current* database, and `mark_seeded` resolves through `catalogIdForHandle` at commit time (micro-decision 13). Nothing new is written here; the requirement is discharged by calling brick 2's function at the right moment and testing that the snapshot's age does not matter.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardCommit</code>. (spec test 16) committing a wake leaves <code>meta.turn</code> byte-identical, and the wake's writes are present. (spec test 17) a helper made to throw during commit leaves <code>catalog</code>, <code>meta.bard_journal</code>, and <code>meta.bard_focus</code> <b>byte-identical</b> to their pre-commit values (compare a canonical snapshot, not field by field), with <code>bard_last_wake_turn</code> unmoved. (spec test 18) a wake snapshotted many turns earlier whose <code>append_catalog</code> handle has since been taken by another entry, and whose <code>mark_seeded</code> handle does not resolve, applies <b>the rest</b> of the wake — focus, journal, sibling entries — and drops only those two, with one diagnostic each. A commit that throws is followed by a fresh trigger producing a new wake on the next qualifying event.
</blockquote>

---

### Step 9 — Coalescing, end to end through the engine

<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> · REQ-BARD-WAKE-13, -14, -15

No new production code is expected here — steps 5, 7 and 8 have already built the pieces. This step exists because the spec's coalescing claims are about the *engine*, not the worker: three `defeated` events across three consecutive ticks must produce one call, and that is a statement about the trigger path, the state machine, and the commit path composed.

If a defect appears here it will be in the interaction, most likely in the dirty flag's consumption point; fix it in step 5's or step 7's code, not with a special case here.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardCoalesce</code>. (spec tests 13, 15) three <code>defeated</code> events across three consecutive ticks with the worker held mid-call by <code>BlockingTransport</code> produce exactly <b>one</b> transport invocation and <code>concurrentEntry == false</code>; the release happens only after a drain thread is <b>provably</b> inside the wait (<code>spinUntil(bardWaitingCountForTest() == 1)</code>), so the test cannot pass by accident of timing. (spec test 14) a trigger arriving during a running wake causes exactly <b>one</b> further wake after that wake commits — not zero, not two: advance the turn past <code>kBardMinTurnGap</code> before the commit-turn so the ceiling cannot mask the difference, then assert the total call count is exactly 2 and the state settles at <code>Idle</code> with the dirty flag clear. (c) <b>The realistic window — the ceiling masking the flag.</b> With the default gap of 5 and a wake held mid-call, triggers arriving 1–3 turns after the queue return at the ceiling and never reach the dirty branch at all (step 7's ordering, which REQ-BARD-WAKE-9 mandates). Assert the outcome that matters: those events are <b>not lost</b> — release, commit, keep ticking past <code>turn - bard_last_wake_turn >= kBardMinTurnGap</code>, and exactly <b>one</b> further wake is queued, because the stamp-at-queue-time rule leaves them inside the query's window. Without this case the plan's whole "the flag is belt to the query's braces" claim (micro-decision 9) is untested, and the common path — a 5-turn gap against a call that can run for seconds — is the one no other sub-case exercises.
</blockquote>

---

### Step 10 — The degradation claim, tested directly

<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> · REQ-BARD-WAKE-6, -20, -23, -25 — spec test 21

The single most important test in the spec, and the reason the brick exists in this shape. One scripted sequence of turns — movement, a take, a fight to `defeated`, a `learned`, a wait — is run **seven** times against fresh worlds from the same fixture, and every arm's concatenated player-facing bytes are compared against the **bard-disabled** arm. The last arm is not in the spec's list: it is micro-decision 15's failure mode, and it is the one that would otherwise reach `main()` and end the run rather than degrade:

| Arm | How it is induced |
|---|---|
| baseline | `TEXTWORLD_BARD=0` — no thread, no wake, no overture |
| overture failed | overture transport returns non-200 / throws |
| worker never started | AI enabled, `bardStart` skipped |
| worker threw | fake worker transport throws on every call |
| commit threw | a wake lands, admission is made to throw at commit |
| every wake failed | fake worker transport returns unparseable bodies all session |
| evaluation threw | `meta.bard_last_wake_turn` deleted, so the trigger path throws every turn |

Turns are driven with `tickT` + `render` (micro-decision 16) so no production transport is ever reached; `bardAfterTurn` is called after each turn's text is captured, mirroring `main()`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardDegradation</code>: all six failing arms produce <b>byte-identical</b> concatenated output to the baseline arm — a single string comparison per arm, so a failure prints as a diff of the whole session rather than as a mystery. Additionally: each failing arm leaves <code>meta.turn</code> equal to the baseline's, and the <code>events</code> log of each failing arm is identical to the baseline's <b>except</b> for rows the bard is allowed to add (none, in every arm here, since no arm commits a wake successfully). The suite must still make <b>zero</b> network calls: assert the counting transports account for every invocation.
</blockquote>

---

### Step 11 — Docs and the full mechanical sweep

<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`README.md`: a third bard paragraph, in the register of the two that precede it (`README.md:29,31`) — the bard now *runs*: one blocking call when a world is first created (and why blocking: generated rooms are canon forever, so a story-less opening area would be permanent), waking only on irreversible change, at most once every five turns, on its own thread, never delaying a turn, and failing into exactly the game you had before it. Update the test-coverage paragraph (`README.md:172`) and the `src/` inventory line (`:183`) to name `bardworker.cpp` and to stop describing `bard.cpp` as "not yet wired to anything".

Then run every mechanical check the spec names, as one sweep:

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate (the spec's own list, 1–5):</b>
<b>1.</b> <code>cmake --build build</code> clean; full suite green <b>including every pregen test</b>, unchanged.
<b>2.</b> <code>grep -En "INSERT|UPDATE|DELETE|SELECT" src/bardworker.cpp</code> → empty.
<b>3.</b> <code>grep -n "Db" src/bardworker.hpp src/bardworker.cpp</code> → no parameter, no member.
<b>4.</b> <code>aihttp.hpp</code> no longer claims a single sanctioned second handle.
<b>5.</b> the 60 s constant carries its justifying comment.
Plus the wiring check from micro-decision 14 (<code>grep -n "AiRole::Generate" src/bard.cpp</code> → empty) and the inherited guards: <code>grep -En "INSERT|UPDATE|DELETE" src/bard.cpp</code> still <b>empty</b> (REQ-BARD-SEL-21 — the overture and commit paths must go through <code>mutations.cpp</code>), <code>grep -rn "place_catalog" src/</code> still empty, and brick 1's two append-only greps still pass. Finally, a read of this plan against the spec: every REQ-BARD-WAKE-N appears in the coverage map below with a step and a test that fails if it is removed.
</blockquote>

---

## Coverage map — requirement → step

| Requirement | Step | Pinned by |
|---|---|---|
| WAKE-1 `OpenedWorld` | 1 | `created` true/false assertions in `testWorld` |
| WAKE-2 overture from `main`, created + AI only | 6 | spec tests 6, 12b + source-order assertion |
| WAKE-3 main thread, guard ordering | 6 | source-order assertion on `main.cpp` |
| WAKE-4 blocking, one player-facing line | 6 | `testBardOverture` line + explicit `fflush` |
| WAKE-5 60 s timeout, commented | 2, 6 | mechanical check 5 |
| WAKE-6 any failure → empty catalog, no throw | 6 | spec test 7 (four failure modes) + the builder-throw arm |
| — both entry points total (micro-decision 15) | 6, 7, 10 | builder-throw arm, evaluation-throw arm, degradation arm 6 |
| WAKE-7 one transaction, all-or-nothing | 6 | spec test 8 |
| WAKE-8 post-commit, post-flush evaluation | 7 | spec test 12a + `main.cpp` source order |
| WAKE-9 ceiling checked first | 7 | spec test 11 |
| WAKE-10 the four-verb trigger query | 7 | spec test 10, one sub-case per verb |
| WAKE-11 stamped at queue time | 3, 7 | spec test 12 (asserted mid-call) |
| WAKE-12 derived from `events`, no cache | 7 | absence grep: no new table, no state member |
| WAKE-13 one call in flight, process-wide | 5, 9 | `concurrentEntry == false`, `callCount() == 1` |
| WAKE-14 dirty flag → exactly one further evaluation | 5, 9 | spec test 14 (total = 2) |
| WAKE-15 three events → one wake | 9 | spec test 13 |
| WAKE-16 own thread, own client, own abort flag | 4, 5 | lifecycle + abort-at-stop tests |
| WAKE-17 `aihttp.hpp` contract updated | 2 | mechanical check 4 |
| WAKE-18 `BardGuard` below `PregenGuard` | 6 | source-order assertion |
| WAKE-19 no database access in the worker | 4, 5 | mechanical checks 2, 3 |
| WAKE-20 both gates, `TEXTWORLD_BARD` convention | 4 | spec test 19 |
| WAKE-21 own transaction | 8 | spec test 16 |
| WAKE-22 `meta.turn` does not move | 8 | spec test 16 |
| WAKE-23 throw → rollback, one diagnostic, no advance | 8 | spec test 17 |
| WAKE-24 selections re-checked live at commit | 8 | spec test 18 (micro-decision 13) |
| WAKE-25 pending result discarded at exit | 5 | `bardStop` drops `Ready` |
| WAKE-26 four test hooks | 4, 5 | every hook is called by a test and by no production code |

## Spec test → step

| Spec test | Step | Spec test | Step |
|---|---|---|---|
| 1–5 (mechanical) | 11 | 12a, 12b | 7, 6 |
| 6, 7, 8 (overture) | 6 | 13, 14, 15 (coalescing) | 9 |
| 9, 10, 11, 12 (trigger) | 7 | 16, 17, 18 (commit) | 8 |
| 19, 20 (lifecycle) | 4, 5 | 21 (degradation) | 10 |

## What this plan deliberately does not do

- **No streaming overture progress.** Design open question; one static line ships, as REQ-BARD-WAKE-4 specifies.
- **No wake count in the profile record.** The design wants `kBardMinTurnGap` tuned against a real session's trigger distribution and notes the profile should carry the count to make that observable. That is a tuning aid, not a requirement in this spec — worth raising as a follow-up if the author wants the ceiling tuned from data rather than from a guess.
- **No `downed` trigger.** Excluded by REQ-BARD-WAKE-10's verb list; the design records it as the closest call.
- **No persistence of a pending result across sessions.** REQ-BARD-WAKE-25 discards it.
- **No `catalogForHandle` call site.** Nothing in a wake is room-scoped; brick 4 introduces the first one.
