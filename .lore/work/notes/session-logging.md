---
title: "Implementation notes: session logging"
date: 2026-08-05
status: complete
tags: [implementation, notes, logging, diagnostics, log-levels, session-log]
source: .lore/work/plans/session-logging.md
modules: [log, profile, main, bard, bardworker, architect, prose, nlresolve, pregen, mutations, world]
related: [.lore/work/specs/session-logging.md, .lore/work/plans/session-logging.md]
---

# Implementation notes: session logging

Executing [.lore/work/plans/session-logging.md](../plans/session-logging.md) —
thirteen steps, REQ-LOG-1 through REQ-LOG-30.

Run inline rather than through implementation/testing/review sub-agents, per a
standing preference on this project that `/implement` work is done in the main
thread.

## Progress

- [x] Step 0 — capture the pre-change baselines (the plan's stated prerequisite)
- [x] Step 1 — the logger module: types, format, levels, sink
- [x] Step 2 — the file: creation, retention, backstop
- [x] Step 3 — `main.cpp`: startup order, session entries, exemptions
- [x] Step 4 — thread labels *(behavioral half deferred to step 9 — see log)*
- [x] Step 5 — migrate `bard.cpp` + `bardworker.cpp` (14 sites)
- [x] Step 6 — migrate `architect.cpp` (12 sites)
- [x] Step 7 — migrate prose, nlresolve, pregen, mutations (9 sites)
- [x] Step 8 — reroute the profiling records
- [x] Step 9 — migrate the 42 test references
- [x] Step 10 — the two exemptions
- [x] Step 11 — README and `.gitignore`
- [x] Step 12 — real-binary validation
- [x] Step 13 — spec validation sweep

Final state: **7653 checks, 0 failures**. `grep -rn 'fprintf(stderr\|std::cerr'
src/` returns one hit, in `src/log.cpp`. `TEXTWORLD_PROFILE` survives nowhere
outside `.lore/` historical documents.

## Log

### Step 0 — baselines (complete)

The plan's Risks section calls out that validation items 1 and 15 diff against a
binary built *before* the work starts. Rather than capture only the outputs, the
whole pre-change executable is stashed at
`.lore/work/validation/session-logging/baseline/textworld-baseline` (built from
`fa3db63`), so any baseline run can be reproduced later instead of being frozen
at one capture.

`.lore/work/validation/session-logging/run.sh` drives one scripted session
against a named binary in a pristine temp directory — fresh `world.db` and
`seed/` copied in every time, since a run mutates the world.

**Discovery: the ambient environment is not hermetic.** `ANTHROPIC_API_KEY` is
set in this shell, and the first baseline attempt made live AI calls; two runs of
the identical script produced *different* stdout (diverging at line 23). Validation
item 1's "byte-identical stdout" is only a meaningful assertion under a hermetic
run, so `run.sh` unsets `ANTHROPIC_API_KEY` and forces `TEXTWORLD_AI=0`. Two
consecutive baseline runs then matched byte-for-byte (2826 B).

Captured, all in `.lore/work/validation/session-logging/baseline/`:

| File | What it is |
|---|---|
| `textworld-baseline` | the `fa3db63` executable |
| `REVISION` | the commit it was built from |
| `stdout.txt` / `stderr.txt` | hermetic run — 2826 B of game text, **0 B** of stderr |
| `stdout-profile.txt` / `twprof.txt` | the same script under `TEXTWORLD_PROFILE=1` — 39 twprof records |

### Steps 1-2 — the logger and the file (complete)

`src/log.hpp` / `src/log.cpp`, added at the head of `twcore`'s source list.
Suite green throughout: 7106 checks after step 1, 7121 after step 2, 0 failures.

**Divergence from the plan, taken deliberately: the inert flag is two flags, not
one.** The plan called for a single file-static `g_active` that "only `logInit`
and `logSetSink` turn on". A latch that only turns on is wrong for the test
binary: the first test to install a sink would leave the logger active for the
rest of the run, and every migrated message from every later test would land on
the suite's real stderr. `log.cpp` carries `g_hasFile` and `g_hasSink` instead,
and `logSetSink({})` clears the second — so clearing a sink returns the test
binary to inert, which is what REQ-LOG-28 actually asks for.

`logShutdown()` earns its keep for the same reason: `logInit()` is the one thing
that redirects the error channel, and `testLogFile` calls it deliberately
against a temp directory. `logShutdown()` `dup2`s the terminal duplicate back
over `stderr`, so the suite's own stderr survives the test. Every path out of
that test goes through it.

Retention breaks a modification-time tie by filename, descending. REQ-LOG-9
requires mtime ordering precisely because names can collide at one-second
resolution; when the mtimes *also* collide, an unbroken tie would make deletion
order depend on directory enumeration. The name is the only other total order
available.

### Step 3 — main.cpp (complete)

`logInit("logs")` and the `SessionLogGuard` sit above the `try`, so the
session-end entry is reached by every exit including both catches. The
`testBardOvertureContract` source-order guard gained `logInit < AiHttpGuard`
ahead of its existing chain rather than replacing it.

Real-binary check (validation item 14) passed on the first run: `logs/` appears
with one file holding session-start, ai-narration, world-opened *(resumed)* and
`session end: 9 turns` — the 9 matching the 9-line script. Stdout was
byte-identical to the baseline, which is validation item 1 already holding at
step 3.

### Steps 4-7 — the 35 migrated call sites (complete)

All 35 sites moved; `grep -rn 'fprintf(stderr\|std::cerr' src/` now returns
exactly the three the plan predicted — `log.cpp` (the writer), `profile.cpp:47`
(step 8's target) and `world.cpp:178` (step 10's target). Suite green at 7123
checks after each of steps 5, 6 and 7.

**Resolved: the plan contradicts itself about the `source` column.** Step 5's
prose says each edit "lifts the `who:` / `validateWakeResponse:` prefix out of
the format string into the `source` argument", but step 5's own table assigns
source `bard` to all fourteen sites, and REQ-LOG-14 wants the column to be a
subsystem so the file can be filtered by one. The two existing `who` values are
`validateOvertureResponse` and `validateWakeResponse` — function names, not
subsystems. Taken: **the table wins**. The source column carries the subsystem
(`bard`, `architect`, `prose`, `nlresolve`, `pregen`, `mutations`) and the
function name stays as the leading token of the message, so nothing is lost and
the column stays filterable. `architect.cpp`'s `failClause` takes a clause
letter rather than a `who`, so it was never in question.

**REQ-LOG-26 confirmed by inspection before keeping the two `e.what()`
interpolations** (`prose.cpp`, `nlresolve.cpp`), as the plan's step 7 required.
Response bodies cannot reach an exception message: both are parsed with
`json::parse(…, allow_exceptions=false)` (`prose.cpp:225`, `nlresolve.cpp:234`)
and the validators are documented as total. The API key never leaves the
`x-api-key` header (`aihttp.cpp:98`). The production transport has no `throw` at
all — the reachable exceptions are engine-built `runtime_error`s over world
data.

**Deferred: step 4's behavioral gate.** The plan puts the thread-label test at
step 4, but it asserts that a *migrated* worker message carries thread `pregen`
or `bard` — and those sites were not migrated until steps 5 and 7. The test
therefore lands with step 9's REQ-LOG-20 coverage, which drives exactly those
two failure paths with a fake transport. Sequencing only; the assertion itself
is unchanged.

**Known-vacuous assertion, as the plan predicted.** `tests.cpp:10332`'s
`bardCapturedStderr` absence check now asserts the absence of something that can
no longer be present by any route. It reads green from here until step 9
repairs it. Not a real pass.

**Note for validation item 15.** The hermetic baseline's twprof records are
`kind=stage` and `kind=dwell` only; with AI off there are no `kind=call`
records. Their `ms=` values are wall-clock and so differ run to run — "byte-identical
field for field" can only mean the *key sequence* is identical once the six-field
prefix is stripped, not the values. Recorded here as the interpretation item 15
will be checked against; a live-key run in step 12 can add the `kind=call` shape.

### Step 8 — the profiling reroute (complete)

`profile.cpp` lost `readProfileEnv`, `g_enabled`, `sink()`, `sinkMutex()` and
`write()`. It owns **format only** now; the gate, the serialization and the
writing are the logger's. `profilingEnabled()` is `logEnabled(LogLevel::Debug)`,
`profileRefreshEnabled()` and `profileSetSink()` are forwarders, and each
`profileEmit` keeps an early return so nothing is formatted below the threshold
(REQ-LOG-25).

The gate held: `TEXTWORLD_PROFILE` reached zero occurrences in `src/`, and the
four `format*` functions were extracted from `git show HEAD:src/profile.cpp` and
from the worktree and compared — **byte-identical, all four** (REQ-LOG-23).

**The red between steps 8 and 9 was a crash, not a set of failures**, which is
worth knowing if this sequence is ever replayed. With the gate defaulting to
INFO, the profiling tests captured nothing and then indexed `captured.back()` on
an empty vector — undefined behavior, SIGSEGV. The `FAIL` lines that would have
explained it were sitting unflushed in stdout's buffer when the process died, so
the run looked like a silent crash. Nothing was wrong; step 9 is its other half.

### Step 9 — the test migration (complete)

The mechanical rules applied as written: 5 guards, 6 `"1"`, 2 `"0"` and 8
`unsetenv` sites moved to `TEXTWORLD_LOG_LEVEL`, `"1"`→`debug` and `"0"`→`error`.
The `twprofPayload()` helper wraps every `parseProfileRecord` call that reads a
*captured* line; the ones that parse a `format*` result directly are untouched,
and no record assertion changed by a character.

**Discovery the plan did not anticipate: a profiling sink is now the LOGGER's
sink, so `captured` stopped meaning "the twprof records".** With the level at
`debug`, a test that drives a real turn also captures the DEBUG rejections of
whatever subsystem it touched — and `twprofPayload` rightly `CHECK`-failed on
them. Fixed with a one-line `isTwprofEntry()` predicate on the five capture
lambdas, restoring `captured` to exactly the set it held before. This is the
cost of the REQ-LOG-27 decision to give the sink the whole formatted line, and
it is small, but it is not free the way the plan implies.

`bardCapturedStderr` is gone, replaced by `bardCapturedEntries`, which captures
through the REQ-LOG-27 sink **and raises the level to `debug` for the duration**.
Both halves are needed: without the sink the old freopen capture would come back
empty whatever happened, and without the raised level the bard's DEBUG entries
sit below the bar — either way the absence assertion at what was `tests.cpp:10332`
would be vacuous a second time.

Three new tests carry the rest:

- `testLogSourceGuards` — 19 source files contain no `fprintf(stderr` and no
  `std::cerr`; `CMakeLists.txt` lists `src/log.cpp` (a file that compiled
  nowhere would make the first check vacuously true); the retired variable
  appears in none of ten files. That last needle is **assembled from two string
  literals**, because `tests.cpp` is on the list it checks and spelling the name
  out would fail the guard by being it.
- `testLogMigrationCoverage` — every row of the REQ-LOG-20 table driven with a
  fake transport and asserted at the stated level with the stated source, plus
  the REQ-LOG-12 thread labels (a real pregen worker and a real bard worker,
  each through a throwing transport, asserted to say `pregen` and `bard` rather
  than `main`), plus the REQ-LOG-26 privacy check.
- The REQ-LOG-15 three-thread stress lives in `testLogFormatAndLevels`.

### Steps 10-11 — exemptions and docs (complete)

`world.cpp`'s schema refusal goes to `logToTerminal` **and** to the log as one
flattened ERROR line. `main.cpp`'s stale comment ("openWorld already printed the
refusal message to stderr") is corrected. README's three passages are rewritten,
a new **The session log** section documents location, naming, retention, the
six-field format and the privacy rule, `logs/` is git-ignored, and the project
layout names `log.cpp`.

### Step 12 — real-binary validation (complete)

All six items pass; the transcripts and the full write-up are in
[.lore/work/validation/session-logging/findings.md](../validation/session-logging/findings.md).

Two things there are worth carrying forward:

**`VAR=x cmd | binary` sets `VAR` for `cmd`, not for `binary`.** This produced
two runs that looked like clean passes and were actually the game running with
the variable unset — once on the profiling baseline, once on the combined-stream
check. Every run in the findings applies the environment with
`env … ./build/textworld` on the right-hand side of the pipe.

**The spec's "unreachable endpoint" needed an alternative.** The API URL is
hardcoded at `aihttp.cpp:103` with no override, so item 2 arranges failure with
`ALL_PROXY=http://127.0.0.1:9`: libcurl fails at connect and nothing leaves the
machine — a stricter reading than pointing a bogus key at the real API, and it
avoids live traffic entirely.

### Step 13 — spec sweep (complete)

Every requirement walked against the implementation.

| Req | Where it is met |
|---|---|
| REQ-LOG-1 | 35 call sites migrated; `testLogSourceGuards`; findings item 1 (byte-identical stdout) |
| REQ-LOG-2 | `logToTerminal` + `world.cpp`, `main.cpp`'s catch; findings item 2b, both halves |
| REQ-LOG-3 | `logInit` step 4's `freopen`; findings item 2 — 79 messages logged, 0 on screen |
| REQ-LOG-4, -5, -6 | `logInit`; `testLogFile` (a) |
| REQ-LOG-7, -8 | `logInit`'s swallowed failures + the `/dev/null` fallback; `testLogFile` (c); findings item 5 |
| REQ-LOG-9 | `applyRetention`; `testLogFile` (b); findings item 6 and 6b |
| REQ-LOG-10, -11 | `formatEntry`; `testLogFormatAndLevels` (a); items 7 and 8 run over a real 83-line log — 0 unparseable, timestamps non-decreasing |
| REQ-LOG-12 | `logSetThreadName` in both workers; `testLogMigrationCoverage` drives real worker threads |
| REQ-LOG-13 | counter moved to `log.cpp`, `profile*Turn` forwarders |
| REQ-LOG-14 | source is its own column; the subsystem decision recorded above |
| REQ-LOG-15 | one mutex, one write; the 600-entry three-thread stress |
| REQ-LOG-16 | `fflush` per entry in the default writer |
| REQ-LOG-17, -18, -19 | `readLevelEnv`; the threshold sweep in-suite, and item 11 on the real binary (0 → 0 → 4 → 83) |
| REQ-LOG-20 | every row driven and asserted in `testLogMigrationCoverage` |
| REQ-LOG-21 | four INFO entries; verified on a real session at step 3 |
| REQ-LOG-22, -24, -25 | `profilingEnabled()` is the DEBUG threshold; early return before formatting; the existing profiling tests pass with only their gate changed |
| REQ-LOG-23 | four `format*` byte-identical to HEAD; findings item 15, both comparisons |
| REQ-LOG-26 | confirmed by inspection for the two `e.what()` sites; privacy test in-suite; key absent from the real item-2 log |
| REQ-LOG-27 | `logSetSink`; the sink receives the formatted line (spec amended to say so) |
| REQ-LOG-28 | falls out of the build graph — `main.cpp` is not linked into `tests`, and the two-flag inert rule keeps a cleared sink inert |
| REQ-LOG-29 | `logInit` first in `main()`, pinned by the source-order guard |
| REQ-LOG-30 | zero occurrences of the retired variable outside `.lore/`; `testLogSourceGuards` pins it |

**One consequence worth stating rather than burying.** At
`TEXTWORLD_LOG_LEVEL=error` or `warn`, a healthy session produces an *empty* log
file, because REQ-LOG-21's session entries are INFO. REQ-LOG-21 exists precisely
to avoid an empty file being confused with broken logging — and it does so at
the REQ-LOG-18 default, which is INFO. Narrowing the threshold below that is the
operator explicitly asking for less. Noted as designed behavior, not a defect.

The spec was then amended with the plan's three settled decisions: REQ-LOG-20's
DEBUG row gained the seven `bard.cpp` sites, validation item 6's arithmetic was
corrected to 20 total, and REQ-LOG-27 now states that the sink receives the
formatted line. Spec is `implemented`; plan is `executed`.

## Divergences from the plan, collected

1. **Two inert flags, not one** (step 1) — a single latch would have left the
   test binary active after the first sink; `logSetSink({})` must return it to
   inert for REQ-LOG-28.
2. **The `source` column is the subsystem** (step 5) — resolving a contradiction
   between step 5's prose and its own table; the table and REQ-LOG-14 agree.
3. **Step 4's behavioral test moved to step 9** — it asserts on call sites that
   steps 5 and 7 had not yet migrated. Sequencing only.
4. **`isTwprofEntry` filter on the profiling sinks** (step 9) — not in the plan;
   required because a profiling sink is now the logger's sink.
5. **`ALL_PROXY` instead of an unreachable endpoint** (step 12) — the API URL is
   hardcoded and has no override.

None changed what was built; each is recorded above with its reasoning.

## Simplify pass

Four review agents (reuse, simplification, efficiency, altitude) over the whole
diff. Reuse found nothing. Four fixes applied, two findings skipped.

**Applied.**

1. **`logExempt()` — the REQ-LOG-2 pairing became a primitive** (altitude). Both
   exempt sites were hand-assembling "reach the terminal *and* the log" as a
   `logToTerminal` + `logEmitf` pair with separately worded arguments, and the
   two had *already* drifted: `world.cpp`'s log copy was a hand-condensed
   rewrite of its terminal text rather than the same text. `log.cpp` now owns
   one call that writes the message verbatim to the terminal and the same bytes
   newline-flattened to the log. Re-verified on the real binary — terminal still
   two verbatim lines, log one entry, no trailing space — and the fatal path
   exercised separately with a corrupt `world.db`.
2. **`isTwprofEntry` / `twprofPayload` go through `parseLogLine`** (altitude).
   Both were doing substring surgery on `" twprof "` while a field-aware parser
   sat forty lines above them. They now classify on the parsed `source` field
   and return the parsed `message`. Removes the failure mode where any DEBUG
   message elsewhere in the engine containing that literal token would be
   misread as a timing record.
3. **One capture-sink builder instead of five inline lambdas**
   (simplification) — `captureTwprof()` / `captureTwprofLocked()`. The five
   copies had already begun to diverge, one having grown a mutex, which is
   exactly the drift the helper prevents.
4. **Dropped the now-dead `#include <unistd.h>` from `tests.cpp`**
   (simplification). It existed only for the `dup`/`dup2`/`fileno` juggling in
   `bardCapturedStderr`, which this work deleted; nothing else in the file
   touches a POSIX file descriptor.

**Skipped, with reasons.**

- *"`logEmitf` evaluates the gate twice."* True — it gates before `vsnprintf`,
  then `logEmit` gates again. Both are load-bearing: `logEmitf` needs its own so
  nothing is formatted below the threshold (REQ-LOG-25), and `logEmit` needs its
  own because it is public API reached directly by ~20 call sites. The proposed
  fix — inlining `formatEntry` + `write` into `logEmitf` — trades one atomic
  load and an enum compare for a duplicated write path.
- *"`fflush` per entry serializes the threads."* This is REQ-LOG-16, stated
  outright: each entry is flushed as it is written so a session killed mid-turn
  keeps everything up to that point. The suggested alternatives (flush on an
  interval, or at shutdown) contradict the requirement, and line-buffering via
  `setvbuf` would issue the same syscalls. The cost lands only at `debug`, and
  it is the cost the requirement asked for.

Suite green after the pass: **7653 checks, 0 failures**.
