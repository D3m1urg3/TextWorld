---
title: "Live validation findings: turn-latency-polish"
date: 2026-07-26
status: current
tags: [validation, latency, profiling, connection-reuse, model-tiering, measurement]
modules: [loop, nlresolve, prose, architect, aihttp, profile]
related: [.lore/work/specs/turn-latency-polish.md, .lore/work/plans/turn-latency-polish.md, .lore/work/notes/turn-latency-polish.md]
---

# Live validation findings: turn-latency-polish

Step 9 of the plan: one bounded live run per configuration, mechanical
observations only. Numbers here are single-sample wall-clock over a home
connection — they are for **shape**, not benchmarking. No configuration was
re-run to obtain a nicer number.

Logs in this directory (stderr = profiling, stdout = game text, always
separate):

| Log | Configuration |
|---|---|
| `prof-ai-on.log` / `.out` | `TEXTWORLD_PROFILE=1`, AI on, the 9-line `session.txt` with a 5 s idle gap |
| `prof-ai-on-generate.log` / `.out` | same, `session-generate.txt` — the leg that reaches a latent exit |
| `prof-ai-off.log` / `.out` | `TEXTWORLD_PROFILE=1 TEXTWORLD_AI=0`, same script |
| `prof-model-override.log` / `.out` | `TEXTWORLD_MODEL=claude-sonnet-5`, two turns |
| `prof-invalid-key.log` / `.out` | `ANTHROPIC_API_KEY=invalid-…`, two turns |
| `live-smokes.out` / `.err` | `TEXTWORLD_AI_LIVE_TEST=1 ./build/tests` |

## The headline: connection reuse works, and it is not where the time is

**Reuse is proven** (REQ-LAT-3, REQ-LAT-8). The first call of a process pays a
full handshake; every later call — of any role, across turns, across the idle
gap — pays essentially nothing:

| | namelookup | connect | appconnect (TLS) |
|---|---|---|---|
| Cold (first call, `prof-ai-on.log` turn 1) | 2 905 µs | 16 321 µs | 32 766 µs |
| Warm (every subsequent call, n = 15) | 18–58 µs | **0** | **0** |

`connect_us` and `appconnect_us` are exactly 0 on every warm call — libcurl
reports no new connection and no new TLS handshake at all, which is stronger
evidence than "small". `total_us` is dominated by `starttransfer_us` in every
single record.

**And the honest part:** that saves ~33 ms on a turn that costs ~4 000 ms.

| Stage | AI on | AI off |
|---|---|---|
| `resolve` (Haiku) | 0.90–1.60 s | 0.001–0.017 ms |
| `narrate` (Opus) | 2.37–5.41 s | 0.09–0.19 ms |
| `generate` (Opus, nested in tick) | 7.27 s | absent |
| `tick` (engine + SQLite) | 2.5–3.7 ms | 0.31–0.45 ms |
| `total` | 3.34–6.35 s (10.77 s generating) | 0.48–0.64 ms |

The engine is not the cost and neither is the transport. **A turn is ~99.9 %
model latency**, effectively all of it TTFB. Connection reuse removes under 1 %
of a turn. It is still worth having — it is free, it is now permanent, and it
made the measurement possible — but the remaining levers are the ones the spec
deferred: streaming the narration (perceived latency) and pre-generating
neighbor rooms (moves `generate` off the turn path entirely). Profiling now
exists to measure both.

Per-role tiering is worth more than reuse: resolve on Haiku returns in ~1 s
against narrate's ~3.5 s on Opus, for a call whose output is a schema-gated
tool invocation of 54–158 tokens.

## Observation ledger

Each spec item, and the log line that satisfies it.

- **REQ-LAT-2** (per-stage, absent ≠ zero) — `prof-ai-on.log`: ticked turns emit
  `resolve, tick, narrate, total`; turns 7–9 (gibberish, the borderline line,
  `quit`) emit only `resolve, total` — no faked tick or narrate.
  `prof-ai-on-generate.log` turn 4 adds `generate`.
- **REQ-LAT-3** (five `_T` fields, cold-vs-warm) — the table above. Every call
  record carries all five.
- **REQ-LAT-4** (role, model, tokens, no fabrication) — every 200 record carries
  `role`, `model`, `input_tokens`, `output_tokens`. Every 401 in
  `prof-invalid-key.log` carries `failed=1` and **no token key at all**.
- **REQ-LAT-5** (parseable, non-interleaved) — one `twprof key=value` line per
  record on stderr; the `.out` files contain game text and nothing else.
- **REQ-LAT-6** (AI-off baseline) — `prof-ai-off.log`: all four stages present
  for every ticked turn, **zero** `kind=call` records, **no** `generate`.
- **REQ-LAT-9** (idle gap) — the 5 s pause sits before turn 6 of
  `prof-ai-on.log`; turn 6's resolve call shows `connect_us=0
  appconnect_us=0`. The pooled connection survived the gap; no reconnect was
  needed, so the transparent-reconnect path was not exercised (as the spec
  already notes, no drop was fault-injected).
- **REQ-LAT-10** (behavior-preserving fallback) — `prof-invalid-key.log`: both
  turns' calls 401, both stages complete, and `prof-invalid-key.out` shows the
  ordinary template output (`You take the candle.`). The player sees nothing.
- **REQ-LAT-12** (per-role default) — every `role=resolve` record reads
  `model=claude-haiku-4-5`; every `role=narrate` and `role=generate` reads
  `model=claude-opus-4-8`.
- **REQ-LAT-13** (global override) — `prof-model-override.log`: all four calls,
  both roles, read `model=claude-sonnet-5`.
- **REQ-LAT-14** (no correctness regression from Haiku) — `prof-ai-on.out`: the
  gibberish line yields `I don't understand that.` with no tick, and the
  borderline well-formed line (`put the candle in the brass urn`, naming a noun
  absent from the room) yields the same ordinary outcome — **not** a spurious
  action. Both are turns 7 and 8 in the log: `resolve, total` only, no tick.

## Two things worth recording

### 1. A downstream gate rejection reports its tokens honestly

`prof-ai-on-generate.log` turn 4 contains, in order:

```
twprof kind=call turn=4 role=narrate model=claude-opus-4-8 status=200 … input_tokens=921 output_tokens=185
aiRender: response rejected, clause c failed: canon room description not present verbatim
```

This is the REQ-LAT-4 scope boundary documented in `aihttp.hpp`, observed live:
the call succeeded and really cost 1 106 tokens, so the record says so; the
*fallback* is announced by the unit's own clause diagnostic on the same stream.
A reader correlating the two lines sees both facts. Do not "fix" this by
plumbing gate results back through the transport seam.

### 2. The `generate` nesting is visible and correct

Same turn: `generate ms=7272.394 nested_in=tick` inside `tick ms=7275.799`.
Tick's duration contains generate's, with ~3 ms of actual engine work around it.
An aggregator must not add the two.

## Pre-existing failure found, NOT caused by this work

`TEXTWORLD_AI_LIVE_TEST=1 ./build/tests` → **2661 checks, 2 failures**, both in
`testArchitectLiveSmoke` (`tests.cpp:3717` `!descriptions.empty()`, `:3724`
`roomsWithOnward > 0`). The prose, resolver, and combat live smokes pass.

Cause: the smoke walks the chain `east, north, east` starting from room 1, but
the shipped seed gives room 1 exactly one exit — `north`, realized to the
corridor. `go east` from the cell therefore hits `resolveGo` case (c), a hard
wall that **returns before constructing any transport**. Zero AI calls, no
diagnostic, `descriptions` stays empty. The log confirms it: the only line the
smoke printed is `(generation declined for 'east' — clean fallback, chain
stops)`, with no `validateRoomProposal` or `architectGenerate` line before it.

This predates the branch. The chain was written in `c886017`, when every
undeclared direction was generatable; `a6a0b24` ("declare room exits at birth")
then made generation fire only on **latent** exits and moved the seed's frontier
to the corridor (`(2,'north',NULL)`, `(2,'up',NULL)`). The smoke has been
failing since. Nothing on that path — transport, model, or profiling — is
touched by this plan.

Left unfixed deliberately: it is outside this spec. The fix is one line in the
smoke (start the chain from the corridor, or plant a latent exit on room 1),
and it wants its own change with its own reasoning.

The architect's production path **was** exercised live through the new shared
client regardless — by `prof-ai-on-generate.log` turn 4 (a real
`role=generate` 200 with tokens) and by the combat live smoke, which generated
a contested room and passed.

## Spend

Roughly 45 API calls across all six runs, the bulk of them Opus narration.
Cents. One additional short leg (`session-generate.txt`) was run because the
original script could not reach a latent exit — the seeded goblin blocks a
latent flee (`You can't flee into the unknown with an enemy at your back.`), so
no `generate` could ever appear. That is a script defect, not a retry for a
better number; `prof-ai-on.log` is kept unedited.

## Re-captured for terminal-visual-polish, 2026-09-07

`.lore/work/specs/terminal-visual-polish.md` deliberately changes what this
directory's captures look like: prose is capped at 66 columns and indented two
spaces, the `Exits:` and `You see:` lines are gone (the band states both), health
bars sit beside the numbers, a repeat `look` prints the room name instead of the
paragraph, and a title screen prints first. Diffing the new captures against the
old ones would therefore show this spec's work and prove nothing.

Per REQ-POLISH-22 and spec check 14, **three properties** were asserted instead,
against a binary built from `859451d` — the commit before this spec began:

1. no escape byte anywhere in the piped output (`grep -c $'\x1b'` is 0);
2. the same sequence of turn outcomes;
3. the same `events` rows — `turn, verb, subject, object, detail`, in id order.

All three hold for every offline script here. Any difference in the events rows
would have been a bug in this spec's work, not a baseline to accept.

Re-run with `.lore/work/validation/revalidate.py <baseline-binary>`. The new
captures are the `polish-*.txt` files in this directory; the old ones are kept
as the record of what the game looked like before.
