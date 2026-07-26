---
title: "Turn latency polish: profiling, connection reuse, per-role model"
date: 2026-07-26
status: implemented
tags: [performance, latency, profiling, libcurl, connection-reuse, model-tiering, haiku]
modules: [loop, nlresolve, prose, architect]
related: [.lore/work/brainstorm/performance-polish-action-latency.md, .lore/work/research/per-turn-latency-remedies.md]
req-prefix: LAT
---

# Turn latency polish: profiling, connection reuse, per-role model

Make a turn feel faster and gain the ability to measure why. Scope is the
diagnostic plus the two low-risk remedies from the
[research](../research/per-turn-latency-remedies.md); background pre-generation is
**deferred to its own spec** (see Out of Scope).

Three pieces:

1. **Profiling** — permanent, gated instrumentation that shows where each turn's
   time goes (per-phase wall-clock, per-call curl setup-vs-TTFB split, per-call
   token counts and model), so remedies can be measured before/after.
2. **Connection reuse** — stop tearing down the libcurl handle every call; reuse the
   connection, TLS session, and DNS cache across turns.
3. **Per-role model** — the input-resolution call uses a fast/cheap model (Haiku)
   instead of the prose default (Opus), without regressing correctness.

Background: with AI enabled a turn makes two sequential blocking HTTP calls (resolve,
narrate) — three when walking a latent exit (generate). Every call currently does a
fresh `curl_easy_init()` → `curl_easy_cleanup()`, paying a full DNS+TCP+TLS handshake
each time. The default model for all three roles is `claude-opus-4-8`.

## Requirements

### Profiling (permanent, gated)

**REQ-LAT-1** — Profiling is gated behind an environment variable (proposed
`TEXTWORLD_PROFILE`), **off by default**. When unset/empty, gameplay output,
behavior, and control flow are byte-for-byte unchanged from today, and any added
overhead is negligible (at most reading a monotonic clock).

**REQ-LAT-2** — When profiling is on, each turn emits per-phase wall-clock durations
measured with a monotonic clock for the **semantic stages** of the turn: **resolve**
(input resolution, whether via the AI resolver or the fixed-verb parser), **tick**
(the SQLite transaction: mutation + combat + commit), **narrate** (output production,
whether AI prose or the template renderer), **generate** (room generation, only on
turns where it runs), and **total turn**. A stage that does not run on a given turn
(generate on a non-movement turn; any stage on a no-tick input) is absent, not
zero-faked. Stages are semantic, not "was there a network call" — see REQ-LAT-3.

**REQ-LAT-3** — For each **network call** made within a stage (resolve/narrate/generate
when the AI path actually calls the API), profiling additionally emits libcurl's
timing breakdown via `curl_easy_getinfo` microsecond (`_T`) fields: `NAMELOOKUP`,
`CONNECT`, `APPCONNECT` (TLS), `STARTTRANSFER` (TTFB), and `TOTAL`. These curl
sub-records appear **only** when a stage makes a network call; a stage that ran offline
(parser, template) has a stage duration (REQ-LAT-2) but no curl record. The breakdown
must make the **setup-vs-thinking split visible per call**: a cold call shows the
handshake stages in the millisecond range; a warm (reused-connection) call shows
`NAMELOOKUP`/`CONNECT`/`APPCONNECT` **orders of magnitude smaller than the cold call
(sub-100 µs)** with `TOTAL` dominated by `STARTTRANSFER`.

**REQ-LAT-4** — For each network call, profiling emits the **role** (resolve /
narrate / generate), the **model id** actually sent, and the response **token counts**
(`input_tokens`, `output_tokens`) parsed from the response `usage`. On a call that
failed or fell back (no usable response), the record notes the failure instead of
fabricating token counts.

**REQ-LAT-5** — Profiling output goes to a channel that does **not** corrupt or
interleave with player-facing game output (stderr or a dedicated log), and is
structured enough to aggregate across a run (one machine-parseable record per
phase/call — e.g. a stable key=value or single-line format).

**REQ-LAT-6** — Profiling works in both AI-on and AI-off runs. With AI off the resolve
and narrate stages run offline (parser + template renderer), so they still emit stage
durations (REQ-LAT-2) but **no** curl sub-records (REQ-LAT-3), there is no generate
stage, and tick + total are emitted as usual. This yields the engine-only baseline the
brainstorm called for.

### Connection reuse

**REQ-LAT-7** — libcurl is explicitly initialized once at process start
(`curl_global_init(CURL_GLOBAL_DEFAULT)`) and cleaned up once at exit
(`curl_global_cleanup()`), rather than relying on lazy per-call init.

**REQ-LAT-8** — Each AI transport reuses a **persistent** easy handle across calls
instead of `curl_easy_init()`/`curl_easy_cleanup()` per request. The connection pool,
TLS session, and DNS cache survive between calls and between turns. Between calls the
handle is reset (`curl_easy_reset`) so no options leak, then per-call options are
re-applied. Handles are destroyed only at shutdown. (One shared handle vs. one per
role is a plan decision; the requirement is that same-host reuse is achieved across
turns.)

**REQ-LAT-9** — `CURLOPT_NOSIGNAL, 1L` is set on every handle (forward-compatibility
for the deferred threading work; harmless in the single-threaded present). A dropped
or server-closed connection reconnects transparently on the next call (libcurl's
default behavior) — a reused handle must never leave a turn permanently unable to
connect.

**REQ-LAT-10** — Reuse is behavior-preserving. Every existing semantic holds: same
request bodies, the 8s per-call timeout, and the **silent fallback** on any failure
(no key, HTTP error, timeout, refusal, validation-gate failure) exactly as today. No
state (headers, callback data, POST body) leaks from one call into the next.

**REQ-LAT-11** — For this spec, the reused handle(s) are used only from the main
thread. (The one-handle-per-thread rule and `curl_global_init`-before-threads
ordering are recorded for the deferred pregen work; no threading is introduced here.)

### Per-role model

**REQ-LAT-12** — The **resolve** call defaults to a fast/cheap model
(`claude-haiku-4-5`); the **narrate** and **generate** calls keep the prose-quality
default (`claude-opus-4-8`). Each role has its own default rather than one shared
default.

**REQ-LAT-13** — The existing global override is preserved: when `TEXTWORLD_MODEL` is
set and non-empty it applies to **all** roles, overriding the per-role defaults, so
anyone relying on it today is unaffected. Precedence for this spec is exactly two
levels — the per-role default (REQ-LAT-12), overridden by `TEXTWORLD_MODEL` when set —
and must be documented in the code. **Per-role environment overrides are out of scope
for this spec**; if added later they would layer on top, but this spec neither
requires nor validates them.

**REQ-LAT-14** — No correctness regression from the cheaper resolve model. The
existing resolve validation gate (recognizes nouns only, introduces no new nouns,
tool-choice/`go`-direction constraints, no tool call on unknown/multi-intent) and the
parser fallback still govern the outcome: a resolve the model gets wrong fails the
gate and falls back to the fixed-verb parser exactly as today. A line the engine
can't resolve still produces the ordinary `I don't understand that.` No new tier of
silent misbehavior is introduced.

## AI Validation

How the AI verifies this is done. Keep live-LLM checks **bounded** — a single short
scripted session, not an open-ended loop.

**Build & regression (deterministic, offline):**
- `cmake --build build` succeeds; the `tests` binary passes its existing suite
  (offline path, no live API).
- Code inspection confirms: no `curl_easy_cleanup()` on the per-call path (only at
  shutdown); `curl_global_init` at startup **and** `curl_global_cleanup` on the exit
  path (REQ-LAT-7); `curl_easy_reset` **followed by full re-application of every
  per-call option** before each call, so nothing leaks between calls (REQ-LAT-8,
  REQ-LAT-10); `CURLOPT_NOSIGNAL, 1L` set on every handle (REQ-LAT-9).
- With `TEXTWORLD_PROFILE` unset, a normal AI-off session's output is identical to
  pre-change (no profiling lines, unchanged gameplay). (REQ-LAT-1)

**Profiling (one bounded live run):**
- Run the game with `TEXTWORLD_PROFILE=1` and AI enabled, piping a short fixed input
  script (a few movement + non-movement turns, including at least one latent-exit walk
  so a `generate` phase appears). Observe in the log: per-phase durations
  (resolve/tick/narrate/generate/total), per-call curl timings
  (namelookup/connect/appconnect/starttransfer/total), and per-call role+model+token
  counts. (REQ-LAT-2, -3, -4, -5)
- Run the same script with AI off: tick+total emitted, no network phases. (REQ-LAT-6)

**Connection reuse (observed from the same profiling run — model-independent):**
- The **first** network call to `api.anthropic.com` shows `NAMELOOKUP`/`CONNECT`/
  `APPCONNECT` in the millisecond range (cold). A **later** same-host call in the same
  run shows those stages **sub-100 µs / orders of magnitude below the cold call**, with
  `TOTAL` dominated by `STARTTRANSFER` — direct evidence the connection/TLS/DNS were
  reused. (REQ-LAT-3, -8)
- A multi-turn session (including an idle gap between turns, as a player would pause)
  behaves identically to before — no stale-state artifacts, no hangs, and a later turn
  still connects, exercising libcurl's transparent reconnect if the pooled connection
  was dropped. Note: an actual mid-session connection drop is not fault-injected here;
  recovery rests on libcurl's documented default plus the observed idle-gap session.
  (REQ-LAT-9)
- Fallback still fires on an induced failure (unset/invalid API key → silent fallback
  to parser/template, turn still completes). (REQ-LAT-10)

**Per-role model (from the same run):**
- The profiling record shows the resolve call using `claude-haiku-4-5` while narrate
  and generate use `claude-opus-4-8`. (REQ-LAT-12)
- Setting `TEXTWORLD_MODEL=<something>` forces all three roles to that model in the
  profiling record. (REQ-LAT-13)
- A deliberately unresolvable line (gibberish) still yields `I don't understand that.`
  and does not tick — the parser fallback is intact on the cheaper model. (REQ-LAT-14)
- A **borderline, well-formed** line that stresses the resolver (e.g. a phrasing that
  names a noun not present in the room, or a plausible-but-out-of-scope action) still
  produces the ordinary engine outcome, not a spurious action — confirming the
  validation gate still rejects an *incorrect-but-well-formed* resolution from the
  cheaper model, which is the actual regression risk of switching resolve to Haiku.
  (REQ-LAT-14)

## Out of Scope (deferred)

- **Background pre-generation** of neighbor rooms — its own spec. This spec only
  records the forward-compat hooks (`curl_global_init`, `CURLOPT_NOSIGNAL`,
  one-handle-per-thread rule) so pregen can be added without reworking transport init.
- **SSE streaming of narration** (perceived-latency lever) — decide after profiling
  shows how much wall-clock is TTFB vs. output generation.
- **Prompt caching** — prompts (~1–2K tokens) are below the model cache minimums
  (4,096 on Opus/Haiku), so it would silently no-op. Skip until prompts grow.
- **Fast mode, HTTP/2 tuning, the curl multi interface** — no concurrent in-flight
  requests yet; revisit only if pregen wants parallel prefetch.
