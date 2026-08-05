---
title: "Implementation plan: session logging"
date: 2026-08-05
status: executed
tags: [plan, logging, diagnostics, terminal, log-levels, session-log, observability]
modules: [log, profile, main, bard, bardworker, architect, prose, nlresolve, pregen, mutations, world]
related: [.lore/work/specs/session-logging.md, .lore/work/brainstorm/error-logging.md]
---

# Implementation plan: session logging

Source spec: [.lore/work/specs/session-logging.md](../specs/session-logging.md) — REQ-LOG-1
through REQ-LOG-30.

Eleven steps. The logger is built and tested before a single call site moves; the
call sites move before the profiling records are rerouted through them; the tests
and the docs close last. Every step compiles and leaves the suite green.

## Decisions taken before drafting

Three spec ambiguities were resolved with the user. They are settled — the plan
implements these answers, and the spec should be amended to match.

<table>
<tr><th align="left">Question</th><th align="left">Resolution</th></tr>
<tr>
<td><b>REQ-LOG-20's inventory is incomplete.</b> Seven <code>bard.cpp</code> sites
(692, 721, 765, 776, 849, 883, 900) write to stderr but appear in no row.</td>
<td><b>All seven → DEBUG</b>, source <code>bard</code>. Same family as the
<code>failClause</code> rejections: the model proposed something the engine didn't
take. WARN stays reserved for degradations the <i>player</i> feels.</td>
</tr>
<tr>
<td><b>REQ-LOG-9 vs. validation item 6.</b> "All but the 20 most recent" (20 files
total) vs. "exactly 20 remain plus the new one" (21).</td>
<td><b>20 total, the session's own file included.</b> REQ-LOG-9 as written wins;
validation item 6's wording is the error. Seed 25 → 20 remain (19 survivors + the
new one).</td>
</tr>
<tr>
<td><b>What the REQ-LOG-27 sink receives.</b> Formatted line (breaks the twprof
assertions) vs. bare message (breaks validation items 7/9/10).</td>
<td><b>The formatted six-field line</b>, mirroring <code>profileSetSink</code>
exactly. Tests gain one <code>twprofPayload()</code> helper wrapped around the ~10
existing twprof assertion sites; the assertions themselves are untouched.</td>
</tr>
</table>

Three further decisions are mine, made from the code rather than the spec:

**The backstop and the logger share one `FILE*`.** Startup does
`freopen(path, "a", stderr)`; the logger's default writer is then
`std::fprintf(stderr, …)` + `fflush` — the same shape `profile.cpp:52` already has.
One `FILE` object touches the log file, so there is no second buffer to interleave
with and no dual-offset problem. On `freopen` failure the stream is already closed
by the C standard, so the discard fallback is a second `freopen("/dev/null", "w", stderr)`.
The REQ-LOG-2 terminal duplicate is taken by `dup(fileno(stderr))` *before* any of
this, which is exactly why REQ-LOG-29 orders it first.

**The turn counter moves into the logger.** REQ-LOG-13 makes it a logging concern,
and `profile.cpp` will depend on the logger (REQ-LOG-22), so leaving the counter
there makes the dependency circular. `g_turn` moves to `log.cpp`;
`profileNextTurn()` / `profileCurrentTurn()` survive as four-line forwarders so
every existing call site and test compiles untouched. Dependency runs one way:
`profile` → `log`.

**`logToTerminal()` lives in `log.cpp`.** Validation item 3 permits surviving
`fprintf(stderr` at the two REQ-LOG-2 exemptions plus the logger. Routing both
exemptions through a logger-owned helper makes the guard stricter and simpler:
after this work, `src/` has **zero** `fprintf(stderr` outside `src/log.cpp`.

**Why the test binary needs no special case for REQ-LOG-28.** `main.cpp` is not
linked into `tests` (`CMakeLists.txt:26-33` — the game executable owns it, `twcore`
does not). `logInit()` is the only thing that dups, redirects, or opens a file, and
nothing in the test binary calls it. The logger is inert with neither file nor sink,
so REQ-LOG-28 falls out of the build graph rather than needing a flag.

## Measured scope

<table>
<tr><th align="left">Surface</th><th align="right">Count</th><th align="left">Verified by</th></tr>
<tr><td><code>fprintf(stderr</code> in <code>src/</code></td><td align="right">38</td><td>grep — 35 migrate, 2 become REQ-LOG-2 exemptions, 1 is <code>profile.cpp:52</code></td></tr>
<tr><td>&nbsp;&nbsp;↳ <code>bard.cpp</code> / <code>architect.cpp</code></td><td align="right">12 / 12</td><td>steps 5 and 6</td></tr>
<tr><td>&nbsp;&nbsp;↳ prose, nlresolve, pregen, bardworker, mutations</td><td align="right">11</td><td>step 7</td></tr>
<tr><td><code>TEXTWORLD_PROFILE</code> / <code>twprof</code> / <code>profileSetSink</code> in <code>tests.cpp</code></td><td align="right">42</td><td>grep — matches REQ-LOG-30</td></tr>
<tr><td><code>README.md</code> passages</td><td align="right">3</td><td>lines 25, 97, 103</td></tr>
</table>

---

## Step 1 — the logger module: types, format, levels, sink

**Size:** medium · **Token risk:** low — two new files, one CMake line, no reading
of existing large files.

New `src/log.hpp` and `src/log.cpp`, added to the `twcore` source list in
`CMakeLists.txt:17`.

```
enum class LogLevel { Error, Warn, Info, Debug };

bool     logEnabled(LogLevel level);        // cached threshold test — the hot path
void     logRefreshLevel();                 // TEST-ONLY, mirrors profileRefreshEnabled
void     logSetThreadName(const char* name);// thread_local, defaults to "main"
int64_t  logNextTurn();                     // moved from profile.cpp
int64_t  logCurrentTurn();                  // atomic, safe from any thread

struct LogEntry { LogLevel level; const char* thread; int64_t turn;
                  const char* source; std::string message; };

std::string formatEntry(const LogEntry&);   // pure — one line, no trailing newline
void logEmit(LogLevel, const char* source, const std::string& message);
void logEmitf(LogLevel, const char* source, const char* fmt, ...);  // printf-shaped
void logSetSink(std::function<void(const std::string&)>);
```

- `logEnabled` reads a file-static `LogLevel` cached at static-init from
  `TEXTWORLD_LOG_LEVEL`, case-insensitive; unset, empty, or unrecognized → `Info`,
  silently (REQ-LOG-18, -19).
- `formatEntry` produces the six fields at the spec's column widths:
  `YYYY-MM-DD HH:MM:SS.mmm  LEVEL  thread  turn=N  source  message`. Local time via
  `localtime_r`, milliseconds from `system_clock` (REQ-LOG-10, -11).
- `logEmit` returns immediately if `!logEnabled(level)`, then formats and writes
  under one file-static mutex — sink lookup, sink call, and default write all
  inside the lock, copying `profile.cpp:41-53` exactly (REQ-LOG-15).
- Default write is a single `fprintf(stderr, "%s\n", …)` followed by `fflush`
  (REQ-LOG-16). **Inert when no file is open and no sink is set** — a file-static
  `g_active` that only `logInit` and `logSetSink` turn on.
- `logEmitf` exists so the 35 migrating sites keep their existing format strings
  verbatim; it `vsnprintf`s into a bounded buffer and calls `logEmit`.
- `profile.hpp` / `profile.cpp`: `profileNextTurn()` and `profileCurrentTurn()`
  become forwarders to the `log` versions. `g_turn` is deleted from `profile.cpp`.

> **Validation gate.** New in-suite tests: `formatEntry` output parses against the
> six-field grammar for all four levels; threshold sweep (unset → Info, `debug`,
> `DEBUG`, `banana` → Info); `logEmit` below threshold reaches no sink; the
> existing 400-record concurrency stress (`tests.cpp:2543-2601`) re-run against
> `logSetSink` with three threads, every line parsing and none truncated
> (REQ-LOG-15). Full suite green — the profile forwarders must not have moved a
> single existing assertion.
>
> **Covers:** REQ-LOG-10, -11, -12, -13, -14, -15, -16, -17, -18, -19, -27

---

## Step 2 — the file: creation, retention, backstop

**Size:** medium · **Token risk:** low — additive within `log.cpp`.

```
struct LogInit { bool fileOpen; std::filesystem::path path; };
LogInit logInit(const std::filesystem::path& dir);  // dir parameter so tests use a temp dir
void    logShutdown();
void    logToTerminal(const char* fmt, ...);        // the REQ-LOG-2 duplicate
```

`logInit` performs REQ-LOG-29 steps 1 and 3–5, in order:

1. `g_terminal = fdopen(dup(fileno(stderr)), "w")`, held for process life. Nothing
   else ever writes to it (REQ-LOG-2).
2. `create_directories(dir)`; build `textworld-YYYYMMDD-HHMMSS.log` from local time
   (REQ-LOG-4, -5, -6).
3. `freopen(path, "a", stderr)`. On `nullptr`, `freopen("/dev/null", "w", stderr)`
   and return `fileOpen = false` — no retry, no message, no non-zero exit
   (REQ-LOG-3, -7, -8).
4. Retention: `directory_iterator` over `dir`, keep only names matching
   `^textworld-\d{8}-\d{6}\.log$`, sort by `last_write_time` descending, remove
   everything past index 19 — **the new file counts toward the 20**. Every
   filesystem call inside a `try`/catch that swallows (REQ-LOG-9).

`logToTerminal` writes to `g_terminal` when it exists and falls back to the real
`stderr` when it doesn't — the test-binary path, which keeps `bardCapturedStderr`
usable. It is structurally independent of whether the file opened (REQ-LOG-8).

> **Validation gate.** In-suite against a temp directory: one file created, name
> matches `textworld-\d{8}-\d{6}\.log`, name-sort equals mtime-sort; seed 25
> synthetic files of staggered mtimes → exactly 20 remain and they are the newest;
> seed a non-matching file (`notes.txt`) → it survives; make the directory
> read-only → `logInit` returns `fileOpen=false`, throws nothing, writes nothing.
>
> **Covers:** REQ-LOG-3, -4, -5, -6, -7, -8, -9

---

## Step 3 — `main.cpp`: startup order, session entries, exemptions

**Size:** small · **Token risk:** low — 117-line file, already read.

- `logInit("logs")` becomes the **first statement in `main()`**, above
  `const AiHttpGuard httpGuard`. `logRefreshLevel()` is not needed — static-init
  caching already ran (REQ-LOG-29 steps 1–5).
- Session-start INFO entries immediately after, before `openWorld` (REQ-LOG-29
  step 6): session start, and — after `openWorld` returns — `world opened
  (created|resumed)` and `ai narration on|off` from `aiNarrationEnabled()`
  (REQ-LOG-21).
- Session end with the turn count: emitted from a small RAII guard declared next to
  `logInit`, so `quit`, EOF, `SchemaMismatch`, and the generic catch all reach it
  (REQ-LOG-21).
- `main.cpp:114` fatal: `logToTerminal("fatal: %s\n", e.what())` **and**
  `logEmitf(Error, "main", "fatal: %s", e.what())` (REQ-LOG-2).

Two source-order guards in `tests.cpp` pin `main()`'s layout and will need their
anchors updated: `testBardTriggerContract` (`tests.cpp:12443-12455`, the
flush-before-`bardAfterTurn` ordering) and the startup-order test at
`tests.cpp:12494-12513`, which asserts `http < open < overture < pregen < bard`.
Add `logInit < http` to the second rather than replacing it.

> **Validation gate.** Suite green with both source-order guards updated and
> `logInit < http` newly pinned. Build the real binary, run
> `.lore/work/validation/turn-latency-polish/drive.pl` against a short script:
> `logs/` appears with one file, and it contains session-start, world-opened, and
> session-end INFO entries with a plausible turn count (validation item 14).
>
> **Covers:** REQ-LOG-2 (main half), -21, -29

---

## Step 4 — thread labels

**Size:** small · **Token risk:** low — two one-line insertions.

`logSetThreadName("pregen")` as the first statement of `workerMain` in
`src/pregen.cpp:158`; `logSetThreadName("bard")` likewise in
`src/bardworker.cpp:126`. Main needs nothing — `"main"` is the `thread_local`
default.

> **Validation gate.** In-suite: drive a background pre-generation job through the
> real worker thread with a fake transport and assert its captured entries carry
> thread `pregen`, not `main`; same for a bard wake and `bard` (validation item 10).
>
> **Covers:** REQ-LOG-12 (the behavioral half)

---

## Step 5 — migrate `bard.cpp` + `bardworker.cpp` (14 sites)

**Size:** medium · **Token risk:** medium — `bard.cpp` is 1143 lines; edit by
grepped line number, do not read whole-file.

| Sites | Level | Source |
|---|---|---|
| `bardworker.cpp:111,114` (wake failed) | ERROR | `bard` |
| `bard.cpp:1008,1010` (overture failed), `1139,1141` (after-turn failed) | ERROR | `bard` |
| `bard.cpp:235` (`failClause`) | DEBUG | `bard` |
| `bard.cpp:692,721,765,776,849,883,900` | DEBUG | `bard` |

Each edit lifts the `who:` / `validateWakeResponse:` prefix out of the format
string into the `source` argument (REQ-LOG-14) and drops the trailing `\n`. Note
`bard.cpp:235` takes a `who` parameter — pass it through as the source rather than
hardcoding, so `failClause`'s existing call sites keep distinguishing themselves.

> **Validation gate.** `grep -c 'fprintf(stderr' src/bard.cpp src/bardworker.cpp`
> → 0 for both. Suite green. The `bardCapturedStderr` assertion at
> `tests.cpp:10320-10332` will now capture nothing where it once captured nothing —
> a vacuous pass. It is **not** repaired here; step 9 owns it. Flag it in the
> commit message so it isn't mistaken for a real green.
>
> **Covers:** REQ-LOG-1, -20 (bard rows), -26

---

## Step 6 — migrate `architect.cpp` (12 sites)

**Size:** small–medium · **Token risk:** low — sites cluster at 38 and 423–558.

`architect.cpp:38` (`failClause`) and the nine `failClause`-adjacent sites at
423–503 → DEBUG, source `architect`. `architect.cpp:555,558` (phase 1 failed) →
ERROR, source `architect`.

> **Validation gate.** `grep -c 'fprintf(stderr' src/architect.cpp` → 0. Suite
> green, including the architect validation-gate tests, which assert *behavior*
> (`std::nullopt` returned) and are indifferent to where the diagnostic went.
>
> **Covers:** REQ-LOG-1, -20 (architect rows)

---

## Step 7 — migrate prose, nlresolve, pregen, mutations (9 sites)

**Size:** small · **Token risk:** low.

| Site | Level | Source |
|---|---|---|
| `prose.cpp:188` (`failClause`) | DEBUG | `prose` |
| `prose.cpp:429,433` (fell back to templates) | WARN | `prose` |
| `nlresolve.cpp:86` (`failClause`) | DEBUG | `nlresolve` |
| `nlresolve.cpp:354,358` (fell back to parser) | WARN | `nlresolve` |
| `pregen.cpp:111,114` (job failed) | ERROR | `pregen` |
| `mutations.cpp:516` (latent origin exit absent) | WARN | `mutations` |

`prose.cpp:429` and `nlresolve.cpp:354` interpolate `e.what()` from a caught
exception. REQ-LOG-26 permits this — an exception message is a failure reason, not
a response body — but confirm by inspection that no `aihttp` exception carries the
response body or the key before keeping the interpolation.

> **Validation gate.** `grep -rn 'fprintf(stderr\|std::cerr' src/` returns **only**
> `src/log.cpp`, `src/profile.cpp:52` (step 8's target), `src/world.cpp:178`, and
> `src/main.cpp:114` — the last two being step 10's targets. Suite green.
>
> **Covers:** REQ-LOG-1, -20 (remaining rows), -26

---

## Step 8 — reroute the profiling records

**Size:** medium · **Token risk:** medium — 164-line `profile.cpp`, but ~20 test
call sites move in step 9 and must stay in step with this.

- `profilingEnabled()` becomes `return logEnabled(LogLevel::Debug);`. Delete
  `readProfileEnv()`, `g_enabled`, and the `TEXTWORLD_PROFILE` getenv.
- `profileRefreshEnabled()` becomes a forwarder to `logRefreshLevel()`, keeping the
  name so step 9's edits stay mechanical. Its comment changes from "re-reads
  `TEXTWORLD_PROFILE`" to "re-reads `TEXTWORLD_LOG_LEVEL`".
- `profile.cpp`'s local `write()`, `sinkMutex()`, and `sink()` are **deleted**. All
  four `profileEmit` overloads become
  `logEmit(LogLevel::Debug, "profile", format…(record))`. This is what preserves
  REQ-LOG-23 byte-for-byte: the four `format*` functions are not touched at all.
- `profileSetSink()` becomes a forwarder to `logSetSink()`. Keeping the name means
  the ~20 test sink call sites do not move; **but** they now receive the full
  six-field line, which step 9 handles.
- The `ScopedStage` constructor's unconditional clock read stays — with
  `profilingEnabled()` now a cached-enum comparison, REQ-LOG-25's budget is
  unchanged.
- Update the file-header comments in `profile.hpp:1-28` to name
  `TEXTWORLD_LOG_LEVEL` and to point the thread-safety paragraph at REQ-LOG-15.

> **Validation gate.** Do **not** expect a green suite here — step 9 is its other
> half. The gate is narrower: `grep -c TEXTWORLD_PROFILE src/` → 0, and the four
> `format*` functions are byte-identical to `git show HEAD:src/profile.cpp`
> (`git diff` shows no hunk inside them).
>
> **Covers:** REQ-LOG-22, -23, -24, -25

---

## Step 9 — migrate the 42 test references

**Size:** large · **Token risk:** high — 12708-line file, six separate clusters.
**Split into three commits if the context gets tight**; each cluster is independent.

Clusters, by grepped line: **2353-2372** (gate semantics), **2456-2493**
(sink + gate), **2545-2622** (concurrency + dwell), **2680-2756** (background flag),
**7199-7458** (two pregen suites), **12549-12551** (the hermetic-run unset).

Mechanical rules, applied uniformly:

1. `setenv("TEXTWORLD_PROFILE", "1", 1)` → `setenv("TEXTWORLD_LOG_LEVEL", "debug", 1)`;
   `"0"` → `"error"`; `unsetenv` → `unsetenv("TEXTWORLD_LOG_LEVEL")`.
   `ScopedEnvVar profGuard("TEXTWORLD_PROFILE")` → `("TEXTWORLD_LOG_LEVEL")`.
2. New test helper beside `parseProfileRecord` (`tests.cpp:~120`):
   ```
   // Strips the six-field log prefix, returning the bare twprof payload —
   // the string these assertions were written against (REQ-LOG-23).
   static std::string twprofPayload(const std::string& line);
   ```
   It finds `" twprof "` and returns from `"twprof"` onward, `CHECK`ing it was
   found. Wrap every `parseProfileRecord(captured…)` call in it. The record
   assertions themselves change by not one character (REQ-LOG-30).
3. `tests.cpp:2353-2372` asserts the "`0` means off" convention specific to
   `TEXTWORLD_PROFILE`. That convention is gone — REQ-LOG-19 has no "off" value.
   **Rewrite this block** as the REQ-LOG-17/-18/-19 threshold test: four named
   levels, unset → INFO, `banana` → INFO. This is the one non-mechanical edit.
4. `bardCapturedStderr` (`tests.cpp:10077`) and its use at `10320-10332`: replace
   with a `logSetSink` capture. The level must be raised to `debug` for the run, or
   the absence assertion at `10332` is vacuous — assert *no entry with source
   `bard`* while DEBUG is active (REQ-LOG-28). Delete `bardCapturedStderr` if it
   has no other caller.
5. Source-text regression guard, in the same family as the existing `main.cpp` and
   `band.cpp` guards (`tests.cpp:8548`, `9058`): assert `readFileBytes` of each of
   `bard.cpp`, `bardworker.cpp`, `architect.cpp`, `prose.cpp`, `nlresolve.cpp`,
   `pregen.cpp`, `mutations.cpp`, `world.cpp`, `main.cpp`, `profile.cpp` contains no
   `fprintf(stderr` and no `std::cerr` (validation item 3). Also assert
   `CMakeLists.txt` lists `src/log.cpp`.
6. New coverage for REQ-LOG-20 (validation item 13): for each row, drive the
   condition with an `HttpTransport` fake and assert an entry at the stated level
   with the stated source. The prose/nlresolve/architect/bard failure paths already
   have such fakes — reuse them.
7. New privacy test (validation item 18): a transport fake echoing a sentinel in
   both request and response; assert the captured log contains neither the sentinel
   nor a dummy `ANTHROPIC_API_KEY`, at INFO or at DEBUG (REQ-LOG-26).

> **Validation gate.** Full suite green. `grep -rn TEXTWORLD_PROFILE tests/ src/`
> → zero hits. The twprof assertions produce identical `kv` maps to before, which
> is what step 8's gate deferred.
>
> **Covers:** REQ-LOG-20 (verification), -26, -28, -30 (tests half)

---

## Step 10 — the two exemptions

**Size:** small · **Token risk:** low.

Deliberately last among the code steps, so step 7's grep gate can use these two as
known-remaining landmarks rather than having to distinguish them from unmigrated
sites.

- `world.cpp:178` (schema mismatch): `logToTerminal(…)` with the existing two-line
  message verbatim, **plus** `logEmitf(Error, "world", …)` with the same text
  flattened to one line. Both fire before the `throw SchemaMismatch`. REQ-LOG-29
  step 7 guarantees the log is open by then.
- `main.cpp:114` was already done in step 3 — verify only.
- `main.cpp:111`'s comment ("openWorld already printed the refusal message to
  stderr") is now wrong; update it.

> **Validation gate.** `grep -rn 'fprintf(stderr\|std::cerr' src/` returns hits in
> `src/log.cpp` **only**. Suite green. Real-binary check: point the game at a
> world file with a bad `schema_version` and confirm the refusal appears on the
> terminal *and* in the log; repeat with `logs/` unwritable and confirm it still
> reaches the terminal (validation item 2b).
>
> **Covers:** REQ-LOG-2

---

## Step 11 — README and `.gitignore`

**Size:** small · **Token risk:** low.

- `README.md:25` — the "Gated profiling (`TEXTWORLD_PROFILE`)" sentence inside the
  turn-latency paragraph.
- `README.md:97` — the `TEXTWORLD_PROFILE` row of the environment-variable table,
  replaced by a `TEXTWORLD_LOG_LEVEL` row: four levels, INFO default, and the fact
  that everything lands in `logs/textworld-*.log` rather than on the terminal.
- `README.md:103` — the "Turning on `TEXTWORLD_PROFILE` makes all of this visible"
  paragraph.
- A short new section documenting the log file: location, name, per-session, the
  20-file retention, and the six-field format with one example line.
- `.gitignore` — add `logs/`.

> **Validation gate.** `grep -rn TEXTWORLD_PROFILE .` returns matches **only**
> inside `.lore/` historical documents (validation item 17).
>
> **Covers:** REQ-LOG-30 (docs half)

---

## Step 12 — real-binary validation

**Size:** medium · **Token risk:** low (shell work, little file reading) — but it is
the only step that can fail for reasons the suite cannot see.

Six items that REQ-LOG-28 puts out of the test binary's reach. All run against
`./build/textworld` via `.lore/work/validation/turn-latency-polish/drive.pl`, with
outputs written to a new `.lore/work/validation/session-logging/`.

1. **Baseline diff** (item 1) — capture a scripted session's stdout on `HEAD~n`
   (pre-change build), rerun the identical script on the new build, `cmp` the two.
   Byte-identical (REQ-LOG-1). *Capture the baseline before starting step 1.*
2. **Combined-stream check** (item 2) — same script, `2>&1` into one capture,
   `ANTHROPIC_API_KEY` set and the endpoint unreachable so every call fails. No
   internal message in the merged capture (REQ-LOG-3).
3. **Exemption reachability** (item 2b) — covered by step 10's gate; record the
   transcript here.
4. **Best-effort** (item 5) — `chmod a-w logs/`, run the baseline script: exit code
   unchanged, stdout byte-identical to run 1, nothing on stderr, no crash.
5. **Retention** (item 6) — seed 25 files with `touch -t` staggered mtimes, run a
   session, assert 20 remain and they are the newest; seed one undeletable file and
   assert startup still succeeds silently.
6. **twprof payload identity** (item 15) — one scripted session on the old build
   with `TEXTWORLD_PROFILE=1`, the same script on the new build with
   `TEXTWORLD_LOG_LEVEL=debug`. Strip the six-field prefix from the new build's
   `twprof` lines with `sed`; `diff` against the old build's, field for field.
   Byte-identical (REQ-LOG-23). *The old-build capture must also be taken before
   step 1.*

> **Validation gate.** All six recorded in
> `.lore/work/validation/session-logging/findings.md`, in the shape of the two
> existing findings files. Items 1 and 6 fail closed — a non-empty `cmp`/`diff`
> stops the work.
>
> **Covers:** REQ-LOG-1, -3, -7, -8, -9, -23 (real-process half)

---

## Step 13 — spec validation sweep

**Size:** small · **Token risk:** low.

Walk REQ-LOG-1 through -30 against the implementation and confirm each is met or
consciously deviated from. Then amend the spec with the three decisions above:
REQ-LOG-20's DEBUG row gains the seven `bard.cpp` sites; validation item 6's
arithmetic is corrected to 20 total; REQ-LOG-27 states that the sink receives the
formatted line. Set the spec's `status: implemented` and this plan's to `executed`.

> **Validation gate.** Every one of the 18 AI-validation items has a recorded
> result. Full suite green. Working tree clean.

---

## Coverage map

Every requirement lands in at least one step.

| Req | Step | Req | Step |
|---|---|---|---|
| REQ-LOG-1 | 5, 6, 7, 12 | REQ-LOG-16 | 1 |
| REQ-LOG-2 | 3, 10 | REQ-LOG-17 | 1, 9 |
| REQ-LOG-3 | 2, 12 | REQ-LOG-18 | 1, 9 |
| REQ-LOG-4 | 2 | REQ-LOG-19 | 1, 9 |
| REQ-LOG-5 | 2 | REQ-LOG-20 | 5, 6, 7, 9 |
| REQ-LOG-6 | 2 | REQ-LOG-21 | 3 |
| REQ-LOG-7 | 2, 12 | REQ-LOG-22 | 8 |
| REQ-LOG-8 | 2, 12 | REQ-LOG-23 | 8, 12 |
| REQ-LOG-9 | 2, 12 | REQ-LOG-24 | 8 |
| REQ-LOG-10 | 1 | REQ-LOG-25 | 8 |
| REQ-LOG-11 | 1 | REQ-LOG-26 | 7, 9 |
| REQ-LOG-12 | 1, 4 | REQ-LOG-27 | 1 |
| REQ-LOG-13 | 1 | REQ-LOG-28 | 9 (+ build graph) |
| REQ-LOG-14 | 5, 6, 7 | REQ-LOG-29 | 3 |
| REQ-LOG-15 | 1 | REQ-LOG-30 | 9, 11 |

## Risks

**The one that can silently pass.** Step 5 leaves `tests.cpp:10332` asserting the
absence of something that can no longer be present by any route. It reads green
for four steps until step 9 repairs it. Named in the step-5 gate so it isn't
forgotten.

**The one that needs a build from before the work starts.** Validation items 1 and
6 both diff against the *current* binary. Capture both baselines before touching
step 1 — reconstructing them later means a stash-and-rebuild dance.

**The one the suite cannot catch.** REQ-LOG-1 is a property of the real process,
and the test binary deliberately disables the mechanism that enforces it. Step 12
item 2 is the only check that exercises it. If that step is skipped, the headline
requirement of the spec is unverified.

**Ordering that is load-bearing.** Step 8 and step 9 are two halves of one change —
between them the suite is red by design. Do not reorder them apart, and do not
interpret step 8's red as a defect.
