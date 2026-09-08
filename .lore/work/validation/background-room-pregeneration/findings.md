---
title: "Validation findings: background-room-pregeneration"
date: 2026-08-02
status: current
tags: [validation, pregeneration, prefetch, threading, latency, profiling, measurement]
modules: [pregen, architect, aihttp, profile, systems, loop, main]
related: [.lore/work/specs/background-room-pregeneration.md, .lore/work/plans/background-room-pregeneration.md, .lore/work/notes/background-room-pregeneration.md, .lore/work/validation/turn-latency-polish/findings.md]
---

# Validation findings: background-room-pregeneration

Two halves, deliberately separated by cost:

- **Step 10 — the deterministic sweep.** Every mechanical check, run as a batch,
  with its literal output recorded below. **Complete.**
- **Step 11 — the bounded live run.** Seven scripted sessions against the real
  API. **Not run** — see the section at the bottom for why and for exactly what
  it will measure.

A check that was not run is reported as not run. Nothing here is inferred from
an adjacent result.

## Step 10 — deterministic sweep

Run on branch `feat/turn-latency-polish`, 2026-08-02, from a clean
`rm -rf build && cmake -S . -B build && cmake --build build`.

### 1. Build and offline regression

```
$ cmake --build build
[100%] Built target tests

$ env -u ANTHROPIC_API_KEY ./build/tests
2834 checks, 0 failures
```

The suite passes with **no API key set** — the default run makes no network
access. 2834 checks against 2616 before this work: 218 added, none removed, and
no existing test edited (see the notes file's diff evidence).

### 2. The worker's database boundary (REQ-PREGEN-5, REQ-PREGEN-7)

```
$ grep -En "INSERT|UPDATE|DELETE|SELECT" src/pregen.cpp
(no output; exit 1)

$ grep -n "Db" src/pregen.hpp src/pregen.cpp
src/pregen.cpp:2:// file free of database access. Nothing in here takes a Db, and no SQL of any
src/pregen.hpp:18://     grep -n  "Db" src/pregen.hpp src/pregen.cpp             -> no parameter,
src/pregen.hpp:21:// If a change here ever wants a Db, that is the signal the work belongs in
```

The worker's translation unit contains no SQL of any kind. All three `Db` hits
are comment prose — two of them are the check itself, written into the header so
the constraint travels with the file. **No function takes a `Db&` and no struct
holds one**, `PregenJob` included.

### 3. The architect's write contract (REQ-ARCH-6), still intact

```
$ grep -En "INSERT|UPDATE|DELETE" src/architect.cpp
(no output; exit 1)
```

Step 8 added two reads to this file (the latent-exit scan and the `meta.turn`
stamp) and no writes.

### 4. AI-off byte-identical session (REQ-PREGEN-2)

Script: `ai-off-script.txt` in this directory (`look`, `north`, `take key`,
`inventory`, `south`, `wait`, `quit`).

```
$ diff baseline.txt branch.txt
(no output — stdout and stderr both identical)

$ TEXTWORLD_PREGEN=0 ... ; diff baseline.txt branch_pregen0.txt
(no output — identical)
```

**Baseline deviation, recorded deliberately.** The plan says to compare against
`main`. The baseline used is **`e0ac408`**, this branch's HEAD before the
pregeneration work began, because `main` (`b598fcb`) predates the whole
turn-latency-polish round that is already committed on this branch — a diff
against it would show that round's changes, not this one's, and would prove
nothing about pregeneration. `e0ac408` is the commit that isolates this feature.

**This is REQ-PREGEN-2's check, not REQ-PREGEN-1's.** AI-off already forces
`architectEnabled() == false`, which suppresses pregeneration regardless of
`TEXTWORLD_PREGEN`. That is why the two runs above are identical to each other
as well as to the baseline, and it is why isolating REQ-PREGEN-1 needs the
**AI-on** comparison in Step 11 §3. The offline half of REQ-PREGEN-1 is covered
instead by direct unit tests of the gate (`testPregenStore` (a),
`testPregenWorker` (a), `testArchitectQueuePregen` (f)), which exercise the
`TEXTWORLD_PREGEN=0` path with the architect **on**.

### 5. Inspection checklist, with file:line

| Claim | Evidence |
|---|---|
| The worker constructs its own easy handle **inside the thread body** | `src/pregen.cpp:164` — `make_unique<AiHttpWorkerClient>(&g_stopping)` inside `workerMain` |
| …and destroys it on the same thread | `src/aihttp.cpp:257` — `~AiHttpWorkerClient` → `curl_easy_cleanup`, reached at the end of `workerMain` before the thread ends |
| `pregen.cpp` never names the shared handle — or libcurl at all | `grep -n "g_handle\|curl_easy\|CURL" src/pregen.cpp` → **no output** |
| `performPost` takes the handle as a **parameter**, so it cannot reach for the shared one | `src/aihttp.cpp:77` |
| The abort option pair sits in the **per-call** block, below `curl_easy_reset` | reset `src/aihttp.cpp:89`; `CURLOPT_NOPROGRESS` / `XFERINFOFUNCTION` / `XFERINFODATA` `:121-123`; `curl_easy_perform` `:126` |
| `pregenStop()` has exactly **one** call site | `src/pregen.hpp:199` — `~PregenGuard`. No explicit call anywhere in `src/` |
| The join precedes `aiHttpShutdown()` on **every** exit path | `src/main.cpp:21` `AiHttpGuard httpGuard`, `:28` `PregenGuard pregenGuard`. Reverse destruction ⇒ `pregenStop()` before `aiHttpShutdown()` on normal return, `quit`, EOF, `SchemaMismatch`, and both catches, with no explicit call to forget |

### 6. Suite wall-clock

```
$ time ./build/tests
0.6s total
```

Unchanged from before the concurrency steps. A concurrency test that got slower
would be a test sleeping somewhere it should not; none of them sleep — every
sequencing point is an explicit promise, condition variable, or state spin the
test itself releases.


## Step 11 — bounded live run: COMPLETE

Run 2026-08-02 against the live API, `TEXTWORLD_PROFILE=1`, stdout and stderr
captured separately. Logs in this directory (`s*-*.log` = profiling,
`s*-*.out` = game text). **Total API spend: $0.32.** Observations only; no
prompt was tuned, and no row was re-run to obtain a nicer number.

### The headline, measured

The same script, same seed world, same latent exit — the only difference is
`TEXTWORLD_PREGEN`:

| turn 6 (`north`, a latent exit) | `TEXTWORLD_PREGEN=0` | default (on) |
|---|---:|---:|
| `generate` stage inside the tick | **5222 ms** | *absent* |
| `tick` stage | 5225 ms | **2.147 ms** |
| **turn total** | **9160 ms** | **4345 ms** |
| outcome record | `outcome=miss` | `outcome=hit age_turns=5` |

**The worst turn stopped being distinguishable from an ordinary one.** 9.16 s →
4.35 s, a 4.8 s reduction (−53%), and the tick — the part that used to carry the
architect call — collapsed from 5.2 s to 2.1 **milliseconds**. Ordinary turns in
the same session ran 3.3–4.6 s, so the committed-candidate turn now sits inside
that band. n=1 per arm.

### Row-by-row

| # | Row | Result |
|---|---|---|
| 1 | dwell then walk a latent exit | ✅ `outcome=hit age_turns=5`, total 4345 ms (`s1-dwell.log`) |
| 2 | walk a latent exit with no dwell | ✅ `outcome=ran_queued run_ms=5059`, total 8840 ms (`s2-immediate.log`) |
| 3 | gate isolated: `TEXTWORLD_PREGEN=0` vs default | ✅ `background=1` count **0** vs **2**. See the divergence below |
| 4 | background calls marked + not folded into stages | ✅ every background call carries `background=1` and sits outside every stage span; **0 torn or interleaved lines** across all sessions |
| 5 | one `kind=dwell` per turn | ✅ 7 records / 7 turns, all ~0.01 ms — correct for piped stdin (presence only, per the plan) |
| 6 | `quit` while a generation is in flight | ✅ **~235 ms** of user-visible delay; no hang, no crash, no libcurl warning |
| 7 | invalid `ANTHROPIC_API_KEY` | ✅ 2 background calls for 2 latent exits, **no retry storm**; player sees the ordinary wall |

### Findings that did not match the expectation

**1. A `kind=pregen` record is still emitted when `TEXTWORLD_PREGEN=0`.** The
plan's §3 expected the disabled arm to show *no* `kind=pregen` records. It shows
one per latent walk, with `outcome=miss` — because `resolveGo` emits the outcome
record whenever the latent branch runs, and a disabled `pregenAcquire` returns
`Miss`. The substantive half of the check is unaffected and passed cleanly:
`background=1` count is **0** disabled vs **2** enabled, which is what actually
isolates REQ-PREGEN-1 from REQ-PREGEN-2. Recorded rather than explained away.
Arguably the record is *more* useful this way — it makes latent-walk counts
comparable across both arms — but it is a divergence from the written plan and
the decision to keep or suppress it is open.

**2. The shipped seed cannot exercise the original script 1.** The seed
hand-places a goblin grunt in the corridor (`seed/base.sql:95`) — the only room
with latent exits — and `resolveGo`'s flee guard refuses a latent walk while a
hostile is present. The first attempt at script 1 ended with the player downed
and teleported back to the cell, so its final `north` was a *realized* move and
produced no pregen record at all. The script was changed to kill the goblin
first (2 basic attacks at 4 damage vs 8 HP). This is a validation-script defect,
not a code defect — and the replacement is the **better** test, because it
exercises plan decision **D8** directly: the two jobs were queued on entering
the corridor and ran *during the fight*, so the candidate was waiting when the
fight ended. Prefetching during combat is free, as D8 predicted.

**3. Half the background generations did not produce a candidate** — 4 of 8
returned 200. Two of the four failures were quit-aborts (by design). The other
two were **full 8 s `CURLOPT_TIMEOUT` expiries with `connect_us=0`** — they
never got a connection. Worth knowing why: REQ-PREGEN-8 deliberately gives the
worker its own handle with **no shared connection cache**, so every background
call pays a cold DNS+TCP+TLS handshake (`namelookup_us≈2800, connect_us≈15800,
appconnect_us≈35200` on every background call, versus `connect_us=0` on every
warm foreground call). Successful generations took 5.2–5.9 s against an 8 s
total budget, so the headroom is only ~2–3 s and the cold handshake eats into
it. **n=2 failures out of 8 — too small to diagnose, large enough to flag.** If
the background failure rate holds at this level in longer sessions, the 8 s
budget for the background path deserves revisiting (it is the *same* constant
the foreground path uses, but the foreground path starts from a warm socket).

### Cost, and the hit rate that justifies it

The plan asks for the hit rate; the cost per hit is the other half of the answer.

| | |
|---|---:|
| Total billed calls | 53 |
| **Total spend** | **$0.3160** |
| — foreground (49 calls) | $0.2391 |
| — **background, speculative** (4 billed) | **$0.0769** |
| Latent-exit walks, properly-configured sessions | 3 |
| — `hit` | 2 |
| — `ran_queued` | 1 |
| **Cost per hit** | **~$0.038** |

Unit costs measured this run: an ordinary turn is ~$0.009 (resolve on
haiku-4-5 + narrate on opus-4-8); one room generation is ~$0.019.

**The structural cost change is the thing to carry forward.** Entering a room
queues one job per latent exit, speculatively. The corridor has two, so entering
it costs two generations and the player walks at most one. In this run
$0.077 of background spend bought 2 hits — roughly **2× the generation spend to
remove the latency spike**, bounded per room (REQ-PREGEN-11 never evicts, so a
room is paid for once ever, not once per visit). Whether that trade is worth it
is a product decision, not a technical one; it is now measured rather than
assumed.

### Honest headline

Yes — the worst turn stopped being distinguishable from an ordinary one
(9.16 s → 4.35 s, −53%), the tick's generation cost went to ~2 ms, quitting
mid-flight costs ~235 ms, and an API outage degrades silently with no retry
storm. The hit rate was 2 of 3 latent walks, at ~2× generation spend and a
background failure rate of 4-in-8 that wants a longer look.

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
