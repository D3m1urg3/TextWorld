---
title: "Performance polish: profiling action latency before optimizing"
date: 2026-07-26
status: open
tags: [performance, latency, profiling, ai-calls, prompt-caching, cost]
modules: [loop, nlresolve, prose, architect]
---

# Performance polish: profiling action latency before optimizing

The game feels unresponsive. Before touching anything, we want a diagnostic that
shows where a turn's time actually goes, then decide remedies. This brainstorm
captured the diagnosis, three remedy directions, and the cost analysis behind the
riskiest one (background pre-generation).

## What the architecture already tells us

A single turn, with AI enabled, runs three things **in strict sequence, all
blocking the prompt** (`src/loop.cpp` `runTurn`):

1. **Resolve** — one HTTP call to Claude (`nlresolve`) lowering raw text to an engine action.
2. The **tick** — one SQLite transaction (a few small SELECTs, the mutation, combat, commit). Almost certainly noise next to the network.
3. **Narrate** — a *second* HTTP call (`prose`) rendering the result.
4. (**Generate** — a *third* call to the architect, only when walking a latent exit.)

So steady-state cost is **two sequential round-trips to a large model**;
stepping through a doorway is **three**.

Three things jump out before measuring:

- **Model is `claude-opus-4-8` for all three roles** — including the trivial
  "which verb is this" resolve step, a job Haiku could do far faster.
- **No connection reuse.** Every call does a fresh `curl_easy_init()` →
  `curl_easy_cleanup()` (see `nlresolve.cpp`, `prose.cpp`, `architect.cpp`), so a
  full DNS + TCP + TLS handshake happens *per call*, twice a turn. 8s timeout each.
- **Zero feedback during the wait.** `main.cpp` reads the line, calls `runTurn`,
  prints nothing until everything is done. Part of "not responsive" is *perceived*
  latency (dead prompt), a distinct problem with a different fix (streaming / a
  "thinking…" indicator).

## The diagnostic to run (step 1)

The key unknown inside each network call: **connection setup vs. the model
thinking.** curl already knows this — no external profiler needed.
`curl_easy_getinfo` gives, per call:

- `NAMELOOKUP_TIME` (DNS), `CONNECT_TIME` (TCP), `APPCONNECT_TIME` (TLS) → the "setup" bucket
- `STARTTRANSFER_TIME` (TTFB — the model thinking)
- `TOTAL_TIME`

That split decides the remedy:
- Setup a big fraction → **connection reuse / keep-alive** is the cheap win.
- Nearly all TTFB → the levers are **model choice**, **token count**, **parallelism/streaming**; reuse barely matters.

Plan: instrument the three phases in `runTurn` with a monotonic clock, dump curl's
timing breakdown **and per-role token counts** (input/output for resolve, narrate,
generate) for each call, run a scripted ~20-turn sequence, print a table. Also run
an **AI-off baseline** of the same sequence to confirm the engine/SQLite cost is
negligible (expected) and isolate the AI contribution. A sampling profiler is the
wrong tool for a network-bound workload — it'd just show the process asleep in curl.

Strong prior: **network-dominated, mostly TTFB.** Hold it loosely; measure.

## The three remedy directions

1. **Connection reuse** — keep a persistent curl handle / keep-alive so calls skip the per-call TLS handshake.
2. **Model choice** — Haiku for the resolve step (the trivial, most-frequent call); keep Opus (or re-evaluate) for narrate/generate.
3. **Pre-generation (pregen)** — prefetch neighbor rooms in the background so movement turns don't pay the generate call. Most valuable but most invasive; see below.

### Sequencing (agreed)

1. **Profile** (step 1 above).
2. **Connection reuse + Haiku-for-resolve** — cheap, low-risk, help *every* turn.
3. **Pregen** — last, so its win is measurable once the surrounding noise is gone,
   and we're not threading concurrency through a system whose other latencies we
   haven't characterized. (Last ≠ demoted — it's where the payoff is cleanest.)

## Pregen design (the invasive one)

Walking a latent exit is the **worst turn** (the only 3-call turn) and it hits at
the best moment: the instant you *enter* a room, the player spends seconds reading
prose — free latency budget to generate the neighbors. If it lands, a movement
turn collapses 3 calls → 2 and the fattest call leaves the critical path.

**The crux:** the engine's foundational rule is *one turn = one tick = one SQLite
transaction, no world write outside a tick.* Room generation is a **write**. Naive
"generate in the background" = a second concurrent writer to `world.db`, violating
that invariant. The fix splits the operation:

- The **slow part is the network call** (prose + declared exits) — no world state.
- The **write is cheap and local** (mint id, insert, plant reciprocal + latent exits).

So: the **background thread only does the API call and parks the result as an
in-memory "room candidate"** keyed by the latent exit. The **DB write still happens
on the main thread inside a normal tick**, when the player walks the exit:

- **Cache hit** → the tick does the cheap local write, no network, near-instant.
- **Cache miss** (not done / evicted) → fall back to today's synchronous generation, unchanged.

This keeps the single-writer invariant fully intact (background touches no DB) and
makes the feature a pure latency optimization with a clean fallback. **User agreed
on the in-memory-candidate approach.**

Decisions:
- **Depth 1 only.** Immediate neighbors. Depth 2 is speculation-squared (~9 rooms for one step) and the coherence chain gets long.
- **Candidates live in memory, committed only on the walk.** Sidesteps permanently writing rooms the player never visits; keeps the "coherent with the room you're leaving" premise honest.
- **It changes *which* room you get, not just when** (model is nondeterministic). Both are valid canon — so the test invariant is weaker: *a valid room appears, and a cache miss is indistinguishable from today.*
- **Eat the waste; no selective prediction** (see cost below).

## Cost analysis — pregen is not a money sink

Measured the architect prompt to ground the estimate. Per generated room:
`setting.txt` is 367 words (~500 tokens); the architect system prompt ~800, tool
schema ~300, origin prose + direction ~800 → **~2,000 input tokens**. Output capped
at `max_tokens: 1024`, ~500 in practice.

On `claude-opus-4-8` ($5 / $25 per 1M in/out):
- Input 2,000 × $5/1M = **$0.010**
- Output 500 × $25/1M = **$0.0125**
- **≈ 2 cents per generated room.**

Pregen waste (depth-1, ~2–3 openings, use one): **~2–4 cents wasted per room
entered**. A heavy 100-room session → **$2–4 wasted, ~$4–6 generation total.**

Framing that settles it: **generation is the rare call.** Every turn already pays
resolve + narrate (~2–3 cents/turn on Opus); a 1,000-turn session is ~$20–30, of
which generation is a slice and pregen waste ~10% on top. The real bill lever is
**Haiku-for-resolve (~5× cheaper on the most-frequent call)**, which saves far more
than pregen costs. **Cost is not a reason to hold back on pregen.**

Notes:
- Prompt-caching the fixed prefix (system prompt + tool schema, ~1,100 tokens) is
  the free lever *in principle*, but that's **below Opus's 4,096-token cache
  minimum**, so it wouldn't even kick in. The prompts are that small.
- These are **word-count estimates, not measured tokens** — hence the requirement
  fed back into step 1: capture **per-role token counts** so we replace every number
  here with a real one.

## Open threads / not yet decided

- **Perceived vs. actual latency:** even a fast turn is one model call; two
  sequential calls with a dead prompt feel sluggish regardless. Is "responsive"
  about total seconds or killing the dead-air feeling (stream the prose, show
  "…")? Different work. Not yet chosen.
- **Speculative resolve** (fire Haiku resolve the instant the player hits enter):
  same family as pregen; only worth considering once the numbers are in.

## Next action

Build the profiling instrumentation from step 1 (per-phase wall-clock + curl
setup-vs-TTFB split + per-role token counts, scripted run + AI-off baseline). That's
implementation — awaiting the user's go.
