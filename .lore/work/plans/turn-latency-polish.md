---
title: "Implementation plan: turn-latency-polish"
date: 2026-07-26
status: approved
tags: [plan, performance, latency, profiling, libcurl, connection-reuse, model-tiering, haiku]
modules: [loop, nlresolve, prose, architect, aihttp, profile]
related: [.lore/work/specs/turn-latency-polish.md, .lore/work/brainstorm/performance-polish-action-latency.md, .lore/work/research/per-turn-latency-remedies.md]
---

# Implementation plan: turn-latency-polish

Make a turn measurable, then cheaper: permanent gated profiling, a persistent
libcurl handle, and a per-role model default. Source of truth:
**[.lore/work/specs/turn-latency-polish.md]** (14 requirements, prefix `LAT`).
Supporting context: the
[brainstorm](../brainstorm/performance-polish-action-latency.md) and the
[research](../research/per-turn-latency-remedies.md).

Scope is **fixed** (see the spec's Out of Scope): no pregen, no SSE, no prompt
caching, no fast mode, no HTTP/2, no curl multi. The forward-compat hooks the
spec names — `curl_global_init` before any thread, `CURLOPT_NOSIGNAL`, and the
one-handle-per-thread rule — land here so pregen can be added later without
reworking transport init.

## Guiding constraints (from memory + spec)

- **Deterministic skeleton first, live LLM last.** Steps 1–8 are fully
  offline-verifiable; the single live run is Step 9, isolated and gated
  ([[verification-must-be-bounded]]). One short scripted session, mechanical
  observations only — never a tune-retry loop.
- **The transport seam does not change.** `HttpResponse` / `HttpTransport`
  (`src/prose.hpp:48-53`) stay exactly as they are, so **every existing
  fake-transport test keeps compiling and passing untouched**. All new
  information (curl timings, role, model, tokens) is emitted from *inside* the
  production transport, never plumbed back through the return type.
- **Off means off.** With `TEXTWORLD_PROFILE` unset, the only added work on the
  turn path is a monotonic clock read and a cached bool test (REQ-LAT-1).
- **One seam / one file / one testable behavior per step.**

## Seams this touches (verified in tree)

| Seam | File:line | What this plan does with it |
|------|-----------|------------------------------|
| `runTurn` resolve dispatch | `src/loop.cpp:47-48` | wrap in the **resolve** stage timer — Step 2 |
| `runTurn` tick transaction | `src/loop.cpp:69-81` | wrap in the **tick** stage timer — Step 2 |
| `runTurn` narration dispatch | `src/loop.cpp:92-97` | wrap in the **narrate** stage timer — Step 2 |
| `architectGenerate` (injected form) | `src/architect.cpp:422` | wrap in the **generate** stage timer — Step 3 |
| generate call site | `src/systems.cpp:97-101` | unchanged (timer sits one level down) |
| `curlTransport` ×3 (anon ns, byte-identical) | `src/prose.cpp:189`, `src/nlresolve.cpp:124`, `src/architect.cpp:88` | **deleted**, replaced by one shared client — Steps 6, 8 |
| model default ×3 | `src/prose.cpp:243-245`, `src/nlresolve.cpp:218-220`, `src/architect.cpp:200-202` | replaced by `modelForRole()` — Steps 4, 5 |
| `HttpResponse` / `HttpTransport` | `src/prose.hpp:48-53` | **unchanged** (deliberately) |
| `main()` | `src/main.cpp:12-44` | curl global init/shutdown guard — Step 7 |
| tests `main()` | `tests/tests.cpp:4638` | same guard, above the live smokes; register new tests — Steps 1–3, 5, 7 |
| `ScopedModelEnv` | `tests/tests.cpp:2282` | reused for the per-role model tests — Step 5 |
| twcore sources | `CMakeLists.txt:17` | add `src/profile.cpp`, `src/aihttp.cpp` — Steps 1, 4 |

**Fact correction against the launch brief:** the `generate` call does **not**
happen inside the AI *resolve* stage. `loop.cpp:77` calls the engine's mutating
`resolve()` (`systems.cpp`), and `resolveGo` (`systems.cpp:97-101`) makes the
architect call — so **generate runs inside the `tick` stage**, wrapped in the
tick's SQLite transaction. Consequence for REQ-LAT-2: `generate` **nests inside
`tick`**; tick's duration includes it. The plan records this in the profile
output rather than pretending the stages are disjoint (micro-decision 6).

## Micro-decisions (flagged, recommended, non-blocking)

1. **One shared easy handle for all three roles** *(recommended)* vs. one per
   role. All three POST the same host with an identical option set, sequentially,
   on one thread — a single handle means the `narrate` call reuses the
   connection/TLS/DNS the `resolve` call just opened (best case for
   REQ-LAT-8), and there is exactly one thing to destroy at shutdown. Per-role
   handles would only matter under concurrency, which is out of scope and
   would want per-*thread* handles anyway (REQ-LAT-11).
2. **Reverse the "deliberately REIMPLEMENTS" stance.** `prose.cpp:176-225`,
   `nlresolve.cpp:107-160` and `architect.cpp:72-123` each carry a comment
   saying the triplication is intentional (only the *type* was shared). That
   decision predates this work; keeping it would mean triplicating persistent-handle
   ownership, `curl_easy_reset` + full re-application, `curl_easy_getinfo`
   timing capture, and record emission — three chances to leak an option.
   **Recommend one shared TU `src/aihttp.{hpp,cpp}`**, and rewrite those three
   comments to say so explicitly (the seam type stays shared and unchanged, so
   the original goal — tests substitute fakes — is untouched).
3. **`TEXTWORLD_PROFILE=0` counts as off** *(recommended)*. The spec only
   mandates unset/empty → off. Treating `"0"` as off matches `TEXTWORLD_AI`'s
   existing convention (`prose.cpp:434`) and avoids the foot-gun where
   `TEXTWORLD_PROFILE=0` turns profiling *on*. This is a superset of REQ-LAT-1.
4. **A test-visible sink for profile records** *(recommended)*. `profile.cpp`
   writes to `stderr` by default; `profileSetSink()` lets a test capture records
   into a vector. Without it, REQ-LAT-2/-4/-6 are only eyeball-verifiable in the
   live run; with it, stage presence/absence, no-curl-record-when-offline, and
   the no-fabricated-tokens rule all become **offline** gates. Cost: one
   `std::function` static.
5. **`CURLOPT_TCP_KEEPALIVE, 1L`** *(recommended, one line)*. Not required by
   any REQ, but it is what keeps the pooled connection warm across a player's
   idle gap — exactly the spec's idle-gap validation item. Inside the
   connection-reuse scope already agreed; nothing else changes.
6. **`generate` is emitted as a nested stage.** Its record carries
   `nested_in=tick` so an aggregator does not double-count it into a turn total.
   (See the fact correction above.)

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.5; padding: 8px 0;">
<b>1</b> profile TU (formatter + gate + sink) ─┬─▶ <b>2</b> phase timers in runTurn ──┐<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>3</b> generate stage timer ────┤<br>
<b>4</b> aihttp: AiRole + modelForRole + usage parse (pure) ─┬─▶ <b>5</b> wire model into 3 builders ─┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>6</b> persistent curl client ──┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>6</b> ─▶ <b>7</b> global init/shutdown guard ─▶ <b>8</b> swap 3 transports ─┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>9</b> live bounded run <span style="color:#b00">[HIGH]</span> ─▶ <b>10</b> spec sweep<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> untestable-offline C code (live-only), review-heavy · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification.

Steps 1–3 (profiling) and 4–5 (model) are **independent of each other** and of
the curl work; either branch can land first. Step 9 needs everything.

---

### Step 1 — Profiling TU: gate, record formatter, sink
**Requirements:** REQ-LAT-1, REQ-LAT-5. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/profile.hpp` / `src/profile.cpp`**; add `src/profile.cpp` to twcore
(`CMakeLists.txt:17`). No call sites yet — this step ships the mechanism and its
tests only.

Contents:

- `bool profilingEnabled()` — `TEXTWORLD_PROFILE` set, non-empty, and not exactly
  `"0"` (micro-decision 3). **Cached in a file-static bool** so the `getenv`
  happens once per process, unlike `aiNarrationEnabled()`'s per-call re-read
  (`src/prose.cpp:428-437`). Because it caches, also expose
  `void profileRefreshEnabled()` — documented **test-only**, re-reads the env —
  so Steps 1–3 can flip the gate inside one test process. Without it the
  on-vs-off assertions (REQ-LAT-1) would be unwritable offline.
- `struct StageRecord { const char* stage; int64_t turn; double ms; const char* nestedIn; }`
  and `struct CallRecord { const char* role; std::string model; bool failed; long status;
  int64_t namelookupUs, connectUs, appconnectUs, starttransferUs, totalUs;
  long long inputTokens, outputTokens; bool tokensKnown; }`.
- **Pure formatters** `std::string formatStage(const StageRecord&)` and
  `std::string formatCall(const CallRecord&)` producing one machine-parseable
  `key=value` line each (REQ-LAT-5), e.g.
  `twprof kind=stage turn=7 stage=narrate ms=1843.221` and
  `twprof kind=call turn=7 role=narrate model=claude-opus-4-8 status=200 namelookup_us=12 connect_us=0 appconnect_us=0 starttransfer_us=1731004 total_us=1843102 input_tokens=1420 output_tokens=212`.
  A failed call emits `failed=1` and **no** token keys (REQ-LAT-4 — no
  fabrication); a 200 whose body carries no readable `usage` emits
  `tokens=unknown` rather than silently omitting the keys, so an aggregator can
  tell "not reported" from "not parsed". Making these pure string functions is
  what makes the format testable without capturing a stream.
- `void profileEmit(const StageRecord&)` / `void profileEmit(const CallRecord&)` —
  no-op unless `profilingEnabled()`; otherwise format + write one line to the
  sink.
- `void profileSetSink(std::function<void(const std::string&)>)` (micro-decision 4).
  Default sink writes to **stderr**, never stdout, so player output is never
  interleaved (REQ-LAT-5).
- `int64_t profileNextTurn()` / `int64_t profileCurrentTurn()` — a process-local
  turn sequence so every record of one turn shares a `turn=` value (no extra
  SELECT; independent of `meta.turn`).
- `class ScopedStage` — RAII: constructor reads `std::chrono::steady_clock::now()`
  unconditionally; destructor computes the delta and calls `profileEmit`. A stage
  that never runs constructs no timer, so it is **absent, not zero-faked**
  (REQ-LAT-2). Header comment states: `generate` nests inside `tick`
  (micro-decision 6), and no record ever contains an API key, a prompt, or
  player text.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testProfileRecords</code> — (a) under a
<code>ScopedEnvVar("TEXTWORLD_PROFILE")</code>: unset → <code>profilingEnabled()</code>
false; <code>"1"</code> → true; <code>""</code> → false; <code>"0"</code> → false, each after a
<code>profileRefreshEnabled()</code> (REQ-LAT-1); (b) <code>formatStage</code>/<code>formatCall</code> outputs parse as
key=value, contain the expected keys, and a <code>failed</code> call record contains
<b>no</b> <code>input_tokens</code>/<code>output_tokens</code> key (REQ-LAT-4/-5);
(c) with a capturing sink installed and profiling <b>off</b>, <code>profileEmit</code>
delivers <b>nothing</b>. <code>cmake --build build</code> clean; full offline suite
green. Because <code>profilingEnabled()</code> caches, the test must exercise it
through a dedicated non-caching probe or be ordered before first use — pin this in
the test comment.
</blockquote>

### Step 2 — Phase timers in `runTurn` (resolve / tick / narrate / total)
**Requirements:** REQ-LAT-2, REQ-LAT-6, REQ-LAT-1 (identity). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In `src/loop.cpp` `runTurn`: `profileNextTurn()` first, then a `ScopedStage
total` for the whole function, and one `ScopedStage` scope each around

- the resolution expression (`loop.cpp:47-48`) — **resolve**, covering both the
  AI resolver and the fixed-verb parser (REQ-LAT-2/-6: the stage is *semantic*,
  not "was there a network call");
- the transaction block (`loop.cpp:69-81`) — **tick**;
- the narration dispatch (`loop.cpp:92-97`) — **narrate**, covering AI prose and
  the template renderer alike.

Early returns (no-Action, `Quit`, cast denial, `EngineError`) must simply *not*
construct the later timers — RAII gives this for free, which is exactly the
"absent, not zero-faked" requirement. No other line of `runTurn` changes; no
output string changes.

Also add the `TEXTWORLD_PROFILE` row to the README env table (`README.md:82-83`)
— it is a permanent, user-visible knob, and the docs belong with the change that
introduces it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testProfileTurnStages</code>, AI off
(hermetic suite already unsets the key), profiling on with a capturing sink:
(a) a normal <code>look</code> turn emits exactly <code>resolve</code>,
<code>tick</code>, <code>narrate</code>, <code>total</code> — and <b>no</b>
<code>kind=call</code> record and <b>no</b> <code>generate</code> record
(REQ-LAT-6); (b) an unparseable line emits only <code>resolve</code> +
<code>total</code> (REQ-LAT-2 absence); (c) <code>quit</code> likewise;
(d) <b>identity check</b>: the <code>TurnResult.output</code> for the same input
is byte-identical with profiling on and off, and with profiling off the sink
receives nothing (REQ-LAT-1). Existing <code>testLoop</code> passes unchanged.
</blockquote>

### Step 3 — `generate` stage timer
**Requirements:** REQ-LAT-2 (generate stage). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Wrap the body of the **injected-transport** `architectGenerate`
(`src/architect.cpp:422`) in a `ScopedStage` for **generate**, tagged
`nested_in=tick`. Instrumenting the injected form (not the production overload at
`architect.cpp:464`, which delegates to it) means one timer covers both paths and
makes the stage assertable with a fake transport. Phase-1/Phase-2 catch structure
is untouched: a failed generation still emits a stage record (it consumed
wall-clock) — the *call* record is where success/failure of the API call is
reported (Step 6).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> extend <code>testProfileTurnStages</code> (or add
<code>testProfileGenerateStage</code>) reusing the latent-exit fixture from the
existing <code>testResolveGoGenerate</code>: walking a latent exit with a canned
<code>create_room</code> fake transport emits a <code>generate</code> stage record
carrying <code>nested_in=tick</code>; a non-movement turn emits none; a
gate-failing fake (wall path) still emits one <code>generate</code> record and the
wall text is unchanged. Offline, no network.
</blockquote>

### Step 4 — `aihttp` TU: `AiRole`, per-role model, pure usage parser
**Requirements:** REQ-LAT-12, REQ-LAT-13, REQ-LAT-4 (token parse). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/aihttp.hpp` / `src/aihttp.cpp`**; add `src/aihttp.cpp` to twcore
(`CMakeLists.txt:17`). This step adds **no curl code** — only pure, testable
pieces, so the model change and the transport change are separately reviewable.

- `enum class AiRole { Resolve, Narrate, Generate }` + `const char* roleName(AiRole)`
  (`"resolve"` / `"narrate"` / `"generate"` — the strings the profile records use).
- `std::string modelForRole(AiRole)` — **exactly two precedence levels**
  (REQ-LAT-13), documented in a header comment as the single place the rule lives:
  1. `TEXTWORLD_MODEL` when set **and** non-empty → applies to **all** roles;
  2. otherwise the per-role default: `Resolve` → `claude-haiku-4-5`,
     `Narrate` → `claude-opus-4-8`, `Generate` → `claude-opus-4-8`.
  Per-role env overrides are explicitly **out of scope** — say so in the comment
  so a later reader doesn't assume the omission is a bug.
- `struct AiUsage { bool known = false; long long inputTokens = 0, outputTokens = 0; }`
  and pure `AiUsage parseUsage(const std::string& responseBody)` — reads
  `usage.input_tokens` / `usage.output_tokens` via the vendored nlohmann/json,
  **never throws**, and returns `known = false` on a non-JSON body, a missing
  `usage`, or non-integer fields (REQ-LAT-4: note the failure, don't fabricate).
- pure `std::string modelFromRequestBody(const std::string& body)` — reads
  `"model"` back out of the request JSON so the profile record reports the model
  **actually sent** (REQ-LAT-4) without changing the `HttpTransport` signature.
  Returns `""` on any parse failure. Only ever called when profiling is on.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testAiRoleModel</code> under
<code>ScopedModelEnv</code> (tests.cpp:2282): <code>TEXTWORLD_MODEL</code> unset →
resolve = <code>claude-haiku-4-5</code>, narrate = generate =
<code>claude-opus-4-8</code> (REQ-LAT-12); set non-empty → all three return it
(REQ-LAT-13); set empty → back to the per-role defaults. Plus
<code>testAiUsageParse</code>: a canned body with <code>usage</code> yields the two
counts; garbage / missing-<code>usage</code> / wrong-typed bodies yield
<code>known=false</code> and never throw. Pure, no network. Offline suite green
(nothing is wired to these yet).
</blockquote>

### Step 5 — Wire `modelForRole` into the three request builders
**Requirements:** REQ-LAT-12, REQ-LAT-13. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Replace the three duplicated `TEXTWORLD_MODEL`-or-`claude-opus-4-8` reads with a
single `modelForRole` call each:

- `src/nlresolve.cpp:216-220` → `modelForRole(AiRole::Resolve)` (**this is the
  behavior change**: Haiku by default);
- `src/prose.cpp:243-245` → `modelForRole(AiRole::Narrate)` (unchanged default);
- `src/architect.cpp:200-202` → `modelForRole(AiRole::Generate)` (unchanged default).

Update the header docs that currently promise a shared Opus default:
`prose.hpp:55-59`, `nlresolve.hpp:42-50`, `architect.hpp:61-70` — each should name
its role's default and point at `aihttp.hpp` for the precedence rule. The README's
`TEXTWORLD_MODEL` row (`README.md:83`) currently reads "Default
`claude-opus-4-8`" and becomes **wrong** at this step: rewrite it as the two-level
per-role rule (resolve Haiku, narrate/generate Opus, `TEXTWORLD_MODEL` overrides
all three).

Test updates (the only existing tests this plan changes):
`tests/tests.cpp:2393-2396` and `:2447-2450` (`testNlResolveRequestBody`) flip
from `claude-opus-4-8` to `claude-haiku-4-5`; `testProseRequestBody`
(`:2319`, `:2375`) and `testArchitectRequestBody` (`:3755`, `:3809`) stay Opus and
must still pass **unchanged** — that is the per-role proof. Leave the test-local
judge helper at `tests.cpp:3217` alone (it is a validation judge, not a game role);
add a one-line comment saying so.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>./build/tests</code> green with
<code>testNlResolveRequestBody</code> asserting Haiku, prose/architect body tests
asserting Opus <b>untouched</b>, and each of the three suites' existing
<code>TEXTWORLD_MODEL</code>-override assertions still passing (REQ-LAT-13).
<code>grep -n "claude-opus-4-8" src/*.cpp</code> shows the literal only in
<code>aihttp.cpp</code>. No stray-key regression: the three exact-top-level-key
guards still hold.
</blockquote>

### Step 6 — The shared persistent-handle curl client
**Requirements:** REQ-LAT-8, REQ-LAT-9, REQ-LAT-3, REQ-LAT-4, REQ-LAT-10, REQ-LAT-11. **Size:** M · **Token-risk:** <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span>

The one step with no direct unit test — it is exercised live in Step 9, exactly as
the repo already treats `curlTransport`. Keep it small and review it closely.

Add to `src/aihttp.{hpp,cpp}`:

- `void aiHttpInit()` — `curl_global_init(CURL_GLOBAL_DEFAULT)`, once, idempotent
  (REQ-LAT-7 plumbing; the call site is Step 7).
- `void aiHttpShutdown()` — destroys the persistent handle **first**
  (`curl_easy_cleanup`, the only one in the codebase), **then**
  `curl_global_cleanup()`. Ordering is the whole point; state it in a comment.
  Must be safe when no handle was ever created (the common case: the offline test
  run and every AI-off session) and idempotent, since the guard's destructor runs
  on error paths too.
- `HttpResponse anthropicPost(const std::string& requestBody, AiRole role)` — the
  single production transport, behavior-identical to today's three
  (REQ-LAT-10): same URL, same three headers with `ANTHROPIC_API_KEY` read **at
  call time** into `x-api-key` only, same `CURLOPT_TIMEOUT 8L`, same
  `transportError`-on-any-failure, `CURLINFO_RESPONSE_CODE` read only on
  `CURLE_OK`, **no retries**. Differences:
  - a **file-local persistent handle** (micro-decision 1), created on first use
    and never cleaned up until `aiHttpShutdown()`;
  - each call begins with `curl_easy_reset(handle)` and then **re-applies every
    per-call option** — URL, POST, POSTFIELDS, POSTFIELDSIZE, HTTPHEADER,
    TIMEOUT, WRITEFUNCTION, WRITEDATA, **plus `CURLOPT_NOSIGNAL, 1L`** and
    `CURLOPT_TCP_KEEPALIVE, 1L` (micro-decision 5). `curl_easy_reset` clears
    options — including NOSIGNAL — so both belong in the per-call block, not in a
    one-time setup path (REQ-LAT-9). Nothing leaks between calls (REQ-LAT-10);
  - the header `curl_slist` is rebuilt per call (preserving read-the-key-at-call-time)
    under an RAII guard and freed **after** `curl_easy_perform` /
    `curl_easy_getinfo`, never before — the handle holds the pointer until the
    next `reset`;
  - **profiling hook, gated:** when `profilingEnabled()`, after `perform` read the
    five `CURLINFO_*_TIME_T` microsecond fields (`NAMELOOKUP`, `CONNECT`,
    `APPCONNECT`, `STARTTRANSFER`, `TOTAL`) (REQ-LAT-3), build a `CallRecord` with
    `roleName(role)`, `modelFromRequestBody(requestBody)`, the status, and
    `parseUsage(resp.body)` (REQ-LAT-4), and `profileEmit` it. A transport error
    or non-200 sets `failed=1` and omits token keys. When profiling is off, none
    of this runs. **Scope boundary for REQ-LAT-4's "or fell back":** the transport
    cannot see a *downstream* rejection (a 200 that the prose/resolver/architect
    validation gate refuses) — real tokens were spent, so the record reports them
    honestly, and the fallback itself is already noted on the same stderr stream
    by each unit's existing one-line clause diagnostic (`prose.cpp:231-235`,
    `nlresolve.cpp:87`). Say this in the header comment so nobody later
    "fixes" it by plumbing gate results back through the seam.
- `HttpTransport makeAnthropicTransport(AiRole role)` — returns a lambda binding
  the role into the existing seam, so **`HttpTransport`'s signature is unchanged**.
- Header comment records the deferred-pregen contract (REQ-LAT-11): this handle is
  **main-thread only**; one easy handle per thread, never shared; `aiHttpInit()`
  must precede any thread that uses libcurl; never share the connection cache
  across threads.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> build clean; offline suite green (nothing calls it
yet). <b>Code-inspection gate</b> (the spec's deterministic checklist):
<code>grep -n "curl_easy_cleanup" src/*.cpp</code> → exactly one hit, inside
<code>aiHttpShutdown</code>; <code>curl_easy_reset</code> present and every option
from the pre-change transport re-applied after it (diff the option list against
<code>prose.cpp:205-213</code> line by line — REQ-LAT-8/-10);
<code>CURLOPT_NOSIGNAL, 1L</code> inside the per-call block (REQ-LAT-9);
<code>ANTHROPIC_API_KEY</code> appears only in the header build and no record or
log line can contain it.
</blockquote>

### Step 7 — Global init/shutdown at process boundaries
**Requirements:** REQ-LAT-7, REQ-LAT-11. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add a tiny RAII guard (`struct AiHttpGuard { AiHttpGuard(){aiHttpInit();}
~AiHttpGuard(){aiHttpShutdown();} }`, in `aihttp.hpp`) and instantiate it:

- **`src/main.cpp:13`** — as the first local inside `main`'s `try`, before
  `openWorld`. Its destructor then covers every exit path the binary has: normal
  return, `quit`, EOF, `SchemaMismatch`, and the generic `catch` (`main.cpp:37-43`).
  Explicit RAII, **not** a function-local static, because a static's destructor
  would run *after* `main` returns — i.e. after any `curl_global_cleanup` called
  from `main` — which is precisely the ordering bug this avoids.
- **`tests/tests.cpp:4638`** — first line of `main()`, **above** the four live
  smokes (they use production transports and must run with libcurl explicitly
  initialized). The default offline run pays one `curl_global_init` and nothing else.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> build clean; <code>./build/tests</code> green (offline)
and the gated live smokes still work under
<code>TEXTWORLD_AI_LIVE_TEST=1</code> — run <b>once</b>, folded into Step 9's live
session, not as a separate live run. A no-key <code>./build/textworld</code>
session (<code>look</code>, <code>go north</code>, <code>quit</code> from a heredoc)
behaves exactly as before and exits 0. Inspection: <code>curl_global_init</code>
on the startup path and <code>curl_global_cleanup</code> on the exit path
(REQ-LAT-7), reached on the error paths too.
</blockquote>

### Step 8 — Swap the three transports onto the shared client
**Requirements:** REQ-LAT-8, REQ-LAT-10. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Delete `curlTransport` and its `appendToString` from `src/prose.cpp:176-225`,
`src/nlresolve.cpp:107-160`, `src/architect.cpp:72-123` (the write callback moves
into `aihttp.cpp`), drop the now-unused `#include <curl/curl.h>` from all three,
and point the three production overloads at the shared client:

- `prose.cpp:473` → `aiRender(db, turn, makeAnthropicTransport(AiRole::Narrate))`
- `nlresolve.cpp:404` / `:419` → `makeAnthropicTransport(AiRole::Resolve)`
- `architect.cpp:466` → `makeAnthropicTransport(AiRole::Generate)`

Rewrite the three "Deliberately REIMPLEMENTS …" comment blocks to state the new
stance (micro-decision 2): one shared client owns the persistent handle and the
timing/profiling hook; the `HttpTransport` **type** remains the seam and tests
still inject fakes. Update `architect.hpp:135-139`'s "binds a local libcurl
transport" wording likewise. `CMakeLists.txt` needs no change beyond Step 4's new
source (twcore already links `CURL::libcurl`; the tests' direct CURL link at
`CMakeLists.txt:31` stays for the live judge call).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> full offline suite green with <b>zero test changes</b>
in this step — every fake-transport test still compiles and passes, which is the
proof the seam was preserved (REQ-LAT-10). <code>grep -n "curl_easy_init\|curl_easy_cleanup"
src/prose.cpp src/nlresolve.cpp src/architect.cpp</code> → empty;
<code>grep -rn "curl_easy_init" src/</code> → only <code>aihttp.cpp</code>. Build clean.
</blockquote>

### Step 9 — One bounded live run (the measurement)
**Requirements:** REQ-LAT-3, -4, -5, -6, -9, -10, -12, -13, -14. **Size:** S (code) · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>

> ⚠️ **The one high-token-risk step** ([[verification-must-be-bounded]]). It is a
> **fixed script, run once**, with mechanical observations. If prose or resolve
> quality looks off, that is a separate bounded effort — do **not** iterate here.

Write a fixed input script (~8 lines) to
`.lore/work/validation/turn-latency-polish/session.txt`: `look`, a non-movement
action, at least one **latent-exit walk** (so `generate` appears), a couple more
turns with a short idle pause mid-script, a gibberish line, one **borderline
well-formed** line (names a noun that isn't in the room), `quit`. Then run, in
this order, capturing stderr separately from stdout:

1. `TEXTWORLD_PROFILE=1` **AI on** → `prof-ai-on.log`. Read off: stage records for
   resolve/tick/narrate/generate/total (REQ-LAT-2); a curl record per network call
   with the five timing fields (REQ-LAT-3); role + model + token counts per call
   (REQ-LAT-4); one parseable line per record on stderr with game text clean on
   stdout (REQ-LAT-5). **Reuse evidence:** first call to `api.anthropic.com` shows
   ms-scale `namelookup/connect/appconnect`; a later same-host call shows those
   **sub-100 µs**, with `total_us` dominated by `starttransfer_us` (REQ-LAT-3/-8).
   The idle gap mid-script exercises the reconnect path (REQ-LAT-9). **Per-role
   model:** resolve records show `claude-haiku-4-5`, narrate/generate
   `claude-opus-4-8` (REQ-LAT-12). **REQ-LAT-14:** the gibberish line still yields
   `I don't understand that.` and no tick; the borderline line yields the ordinary
   engine outcome, not a spurious action.
2. `TEXTWORLD_PROFILE=1` **AI off** (`TEXTWORLD_AI=0`) → `prof-ai-off.log`:
   resolve/tick/narrate/total present, **no** curl records, **no** generate
   (REQ-LAT-6) — the engine-only baseline the brainstorm asked for.
3. `TEXTWORLD_PROFILE=1 TEXTWORLD_MODEL=claude-sonnet-5`, **two turns only** →
   all roles report that model (REQ-LAT-13).
4. `TEXTWORLD_PROFILE=1 ANTHROPIC_API_KEY=invalid`, two turns → silent fallback to
   parser/template, turns still complete, call records show `failed=1` with **no**
   token keys (REQ-LAT-10, REQ-LAT-4).
5. Once, in the same session: `TEXTWORLD_AI_LIVE_TEST=1 ./build/tests` — the four
   existing gated smokes still pass through the new shared client (Step 7's live half).

Commit the logs (keys never appear in them) plus a short findings note next to the
script, so the before/after the brainstorm wanted has a home and the numbers
replace its estimates.

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> all six observations above confirmed from the captured
logs, each annotated with the REQ id it satisfies. Budget: one short session per
configuration — roughly a dozen resolve/narrate calls plus one generate, cents of
spend. <b>No</b> re-runs to chase nicer numbers; a surprising number is a finding,
not a retry trigger.
</blockquote>

### Step 10 — Final sweep against the spec checklist
**Requirements:** all 14 (validation sweep). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section top to bottom and confirm each bullet,
naming the step that satisfies it:

- Build & regression: `cmake --build build` clean, `./build/tests` green.
- The four code-inspection greps (no per-call `cleanup`; `global_init` **and**
  `global_cleanup`; `reset` + full re-application; `NOSIGNAL` on every handle).
- `TEXTWORLD_PROFILE` unset → output identical to pre-change (diff a scripted
  AI-off session against a `git stash`ed build, or against the byte-identity
  assertion from Step 2).
- Profiling, reuse, and per-role observations → Step 9's logs.
- Confirm the README env table (`README.md:82-83`) carries the
  `TEXTWORLD_PROFILE` row (Step 2) and the corrected per-role `TEXTWORLD_MODEL`
  row (Step 5) — written at those steps, only verified here.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every spec checklist bullet ticked with the step and
evidence that satisfies it; the coverage map below has no empty cell. Spec status
moves to <code>implemented</code>. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) | Verified by |
|-------------|---------|-------------|
| REQ-LAT-1 (gated, off by default, zero behavior delta) | 1, 2 | `testProfileRecords` env matrix + Step 2 byte-identity check |
| REQ-LAT-2 (per-stage wall-clock, absent ≠ zero) | 2, 3 | `testProfileTurnStages` (normal / no-tick / quit / latent walk) |
| REQ-LAT-3 (five curl `_T` fields, cold-vs-warm split) | 6 (emit), 9 (observe) | inspection + Step 9 log: warm call sub-100 µs |
| REQ-LAT-4 (role, model, token counts, no fabrication) | 4 (pure parse), 6 (emit), 9 | `testAiUsageParse` + `formatCall` failure case + Step 9 log |
| REQ-LAT-5 (stderr, one parseable record per phase/call) | 1, 9 | formatter tests + stdout/stderr separation in Step 9 |
| REQ-LAT-6 (works AI-on and AI-off; offline stages, no curl records) | 2, 9 | `testProfileTurnStages` (AI off) + `prof-ai-off.log` |
| REQ-LAT-7 (`curl_global_init` / `curl_global_cleanup` once) | 6 (API), 7 (call sites) | inspection + no-key session on every exit path |
| REQ-LAT-8 (persistent handle, reset + re-apply, destroy at shutdown) | 6, 8 | single-`cleanup` grep, option-list diff, Step 9 warm-call evidence |
| REQ-LAT-9 (`NOSIGNAL` on every handle; transparent reconnect) | 6, 9 | inspection (per-call block) + idle-gap session |
| REQ-LAT-10 (behavior-preserving: bodies, 8 s, silent fallback, no leaks) | 6, 8, 9 | **zero test changes** in Step 8 + invalid-key fallback run |
| REQ-LAT-11 (main-thread only; pregen rules recorded) | 6, 7 | `aihttp.hpp` contract comment; no threading introduced |
| REQ-LAT-12 (resolve → Haiku; narrate/generate → Opus) | 4, 5, 9 | `testAiRoleModel` + unchanged prose/architect body tests + logs |
| REQ-LAT-13 (`TEXTWORLD_MODEL` overrides all roles; exactly 2 levels) | 4, 5, 9 | `testAiRoleModel` + existing override tests + Step 9 run 3 |
| REQ-LAT-14 (no correctness regression from Haiku resolve) | 9 | gibberish → `I don't understand that.`, borderline line → ordinary outcome |

All 14 LAT requirements land in a step. Nothing in this plan adds a build
dependency, changes the `HttpTransport` seam, introduces a thread, or writes to
the world outside a tick.

## Deliberately not done here

Background pregen, SSE streaming, prompt caching, fast mode, HTTP/2 tuning, the
curl multi interface — all deferred per the spec's Out of Scope. What this plan
leaves *ready* for pregen: explicit `curl_global_init` before any thread could
exist (Step 7), `CURLOPT_NOSIGNAL` already set (Step 6), and a documented
one-handle-per-thread rule with the connection-cache-sharing prohibition recorded
in `aihttp.hpp` — so the worker thread gets its own handle and transport init
needs no rework.
