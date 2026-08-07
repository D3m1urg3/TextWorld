---
title: "Background room pregeneration: candidates for the latent exits"
date: 2026-08-01
status: resolved
tags: [performance, latency, pregeneration, prefetch, threading, architect, story-coherence]
modules: [loop, architect, mutations, aihttp, nlresolve, prose]
related: [.lore/work/brainstorm/performance-polish-action-latency.md, .lore/work/specs/turn-latency-polish.md, .lore/work/validation/turn-latency-polish/findings.md]
---

# Background room pregeneration: candidates for the latent exits

Picking up the optimization deferred out of `turn-latency-polish`. The game is
slow to react, and it is worst when the player walks into a room that does not
exist yet. This brainstorm re-reads the profiling data, ranks the remaining
levers, and settles on background pregeneration as the first one to build.

## Re-reading the measurements

The prior work concluded "a turn is ~99.9 % model latency, effectively all
TTFB." True, but under-read. Splitting the per-call records by role
(`prof-ai-on.log`, `prof-ai-on-generate.log`) tells two different stories.

**Haiku / resolve is almost perfectly linear in output tokens:**

| output tokens | latency |
|---|---|
| 54 (n=6) | ~983 ms |
| ~70 (n=6) | ~1177 ms |
| 158 (n=1) | 2068 ms |

Fits `~420 ms fixed + 10.4 ms/token` closely. A predictable machine.

**Opus / narrate shows no correlation with output length at all:**

| bucket | mean output | mean latency |
|---|---|---|
| out ≤ 120 (n=4) | 109 tok | 3425 ms |
| out ≥ 139 (n=6) | 166 tok | 3153 ms |

More tokens, slightly *less* time. Range 2367–5406 ms for near-identical work,
σ ≈ 1 s on a ~3.2 s mean. The hypothesis going in was that the system is
output-token-bound and "write shorter prose" is a free latency lever. **That
hypothesis is wrong for narrate**, at least at these lengths — the variance is
server-side and not ours to control.

A cold-Opus-first-call effect was also considered and killed: the 5406 ms
outlier was one run's first narrate, but the *other* run's first narrate was the
fastest of the whole sample at 2394 ms.

### The turn budget

| turn kind | composition | total |
|---|---|---|
| ordinary | resolve ~1.0 s + narrate ~3.2 s | **~4.7 s** |
| walking a latent exit | resolve ~1.0 s + generate ~7.3 s + narrate ~2.5 s | **~10.8 s** |

`generate` is n=1. One sample. Treat 7.3 s as a shape, not a constant.

## The three remaining levers

1. **Background pregeneration** — removes ~7.3 s from room-entry turns, making
   the worst turn cost about what an ordinary turn costs.
2. **Parser-first resolution** — `resolveOrParse` (`nlresolve.cpp:367`) always
   calls Haiku first and only falls back to `parse`. Inverting that for inputs
   the fixed-verb parser already handles exactly (`north`, `take candle`,
   `look`) drops a whole network call, ~1 s, from those turns. Small change, no
   new machinery. Risk is feel-consistency: the parser taking a literal reading
   where the resolver would have caught intent.
3. **Streaming the narration** — does not lower the total, but converts a
   2.4–5.4 s dead prompt into text that starts flowing early. Probably the
   single biggest change to how fast the game *feels*, since it hits every turn.

**Chosen order: 1, then 2, then 3.** Pregen first because new rooms are the
named pain and the biggest single number.

### A note on why streaming cannot be decided from the current data

The spec deferred streaming "until profiling shows how much wall-clock is TTFB
vs. output generation." That question is **unanswerable with the profiling we
built**: `STARTTRANSFER` is measured on a *non-streaming* request, where the API
sends nothing until the whole response is generated, so TTFB necessarily
swallows the entire generation. One streaming call would decompose it. Worth
recording so the deferral is not re-read as "we measured and it did not matter."

## Two things the code already gets right

- **`architectGenerate` already splits at the pregen boundary.** Phase 1
  (context → request → one transport call → validate) performs **no DB write**;
  Phase 2 (`writeGeneratedRoom`) is the only persistence path. Phase 1 is
  threadable as-is.
- **The world is a graph, not a grid.** `exits(room, direction, dest)` carries no
  coordinates, so there is no "north-then-east vs. east-then-north must land in
  the same room" collision problem to solve. Worry retired before it was raised.

## The shape of the thing

- On entering a room, queue a generation job for each **latent** exit (`dest IS
  NULL`).
- One background thread runs **Phase 1 only** — the API call and validation. It
  touches no database.
- Results land in an in-memory map keyed by `(room, direction)`.
- When the player walks that exit, the tick does the cheap local write from the
  candidate. No network on that turn.
- Candidate not ready? Generate synchronously, exactly as today.

The DB write stays on the main thread inside the tick, so the single-writer
invariant holds untouched. A cache miss must remain **indistinguishable from
today's behavior** — pregen is a pure optimization with a clean fallback.

## The buffer framing, and where it strains

The framing raised was stronger than "prefetch on entry": *always keep a
one-room buffer in every available direction*. That is a maintained invariant,
and the difference is where the hard parts live.

**It forces the concurrency question.** Rooms have B latent exits — the seed
corridor has 2, architect-declared rooms realistically 1–3. A serial worker at
~7.3 s/room fills a B=3 buffer in ~22 s. Whether that keeps up depends on player
dwell time, which **we have never measured**.

**The buffer never gets ahead.** Depth-1 means entering room C *empties* the
buffer — C's own latent exits have nothing prefetched. The steady state is not
"buffered", it is "permanently chasing", re-placing the same bet at every step.
Depth 2 fixes it and was rejected earlier as speculation-squared (~9 rooms per
step). A middle option exists and may be nonsense: **momentum-prioritized depth
2** — a player who went north twice probably goes north again, so chase only
that one candidate's neighbors. One extra room in flight, not nine.

**Candidates should probably never be evicted.** Keyed by `(room, direction)` and
kept for the session, they cost a few hundred words each. Two payoffs:
backtracking (which text-adventure players do constantly) becomes instant, and
the pathological case dies — a player pacing between two rooms re-triggers
nothing, because those slots are already full.

## The forward constraint: coherent story generation

Stated as eventual, explicitly **out of scope for now**: coherent overall story
generation spanning NPCs, objects, monsters, and puzzles. It changes two
decisions cheaply, and both in the direction of *less* machinery.

### Keep the worker serial

The instinct was a thread pool so several neighbors fill in parallel. Don't.
Coherent story generation means rooms cannot be authored in ignorance of each
other, and parallel independent generation is precisely what breaks that. One
worker doing one job at a time is simpler now **and** is the thing that can later
be taught to consult shared story state without races. Simplicity and
future-proofing point the same way here, which is rare enough to take.

### The worker produces text only; every allocation happens at commit

This is the rule that protects the future. Anything scarce or unique — an id, an
enemy placement, a key item, a puzzle piece, a story beat — is decided by the
**engine, on the main thread, inside the tick**. Not by the worker.

Today the code nearly does this already. The one thing leaning over the line is
the **enemy blurb**: `buildArchitectRequestBody(context, enemyBlurbs)` takes an
eligible menu computed from live world state (front intensity, gating,
bootstrap), and the model picks from it during generation. A candidate generated
40 turns ago carries a choice made against a menu that no longer exists.

Extend that same pattern to puzzles and the failure modes get worse: a candidate
that "used up" the third rune and is then never visited; two neighbors that each
contain the only key.

### Stamp candidates with the state they were generated against

The turn number plus whatever inputs the context used. At commit, check validity
and regenerate if stale. Costs almost nothing now and is exactly the hook an
ordered story system needs.

### Decided: enemy selection stays in the architect call

The open question was whether enemy selection should move to commit time,
gated on whether the room description names the creature. **Decision: keep it
as is.** The future game master AI is what will control enemy spawning.

This does not bend the text-only rule. Enemy *selection* happens in the model
call, but the *placement* — the DB write — still happens at commit in Phase 2, so
nothing scarce is allocated in the background. When the GM AI takes over spawns,
the enemy leaves the architect call entirely and the architect returns to pure
room prose. The boundary drawn here is already where that handoff wants it.

Consequence accepted knowingly: a candidate generated at turn 10 and walked at
turn 50 carries an enemy chosen against a stale eligibility menu. The effect is
mild — difficulty pacing slightly off on a stale candidate, not a correctness
bug — and not worth building invalidation for something the GM will own. This
also retires the "does the prose name the enemy?" check: it only mattered if we
intended to strip a stale enemy, and we don't.

## Open questions

- **What is actual player dwell time?** Never measured. It decides whether a
  serial worker can hold the buffer at all. The existing profiler could
  instrument the gap between turn end and next input — free, and the same
  "measure before optimizing" move that made the last round work.
- **Retry policy.** A maintained-buffer invariant plus a down API is a worker
  retrying forever, burning calls silently. Needs a per-slot attempt cap. The
  weaker fire-once framing never had this problem.
- **Threading friction.** `aihttp.hpp` states the shared handle is main-thread
  only, one handle per thread, connection cache never shared. The worker needs
  its own handle. Anticipated in the header, so this is known cost — but it is
  cost, and it partly re-opens work just closed.
- **Is narrate on Sonnet worth testing?** The per-role tiering exercise stopped at
  resolve and never revisited the expensive role. Unknown whether it moves the
  mean, the variance, or the quality.
- **Prompt caching, re-checked.** The prior spec deferred it on a claimed 4096-token
  minimum. Resolve's input is a near-constant **1390 tokens** every call; generate's
  is **2480**. Those sit close enough to plausible thresholds that the number
  should be verified against current docs rather than quoted from memory. A cost
  lever regardless of latency.

## Honest ceiling

Pregen takes the worst turn from ~10.8 s to ~3.5 s. It does **not** improve an
ordinary turn, which stays ~4.7 s. Its value is not a lower mean — it is killing
the variance spike that makes the game feel erratic. That is probably the right
goal, but it should be argued that way rather than sold as "faster turns."

## Next action

Move to spec. Nothing blocking remains — the enemy question is decided, and the
open questions above are either measurable during implementation (dwell time) or
plan-level decisions (retry cap, handle-per-thread).
