---
title: "Session logging: internal messages leave the game screen"
date: 2026-08-05
status: implemented
tags: [logging, diagnostics, terminal, log-levels, session-log, observability]
modules: [logger, profile, main, bard, architect, prose, nlresolve, pregen, mutations, world]
related: [.lore/work/brainstorm/error-logging.md, .lore/work/specs/turn-latency-polish.md, .lore/work/specs/background-room-pregeneration.md, .lore/work/specs/terminal-status-band-ui.md]
req-prefix: LOG
---

# Session logging: internal messages leave the game screen

The terminal is the game screen. Today about 40 places in the engine print
internal messages straight onto it, mid-play. This spec replaces those with a
logging module: levels, timestamps, one file per session, and nothing on screen.

Source brainstorm: [.lore/work/brainstorm/error-logging.md](../brainstorm/error-logging.md)

## Context

Every internal message is currently a direct `std::fprintf(stderr, …)`, spread
across `bard.cpp`, `architect.cpp`, `prose.cpp`, `nlresolve.cpp`, `pregen.cpp`,
`mutations.cpp`, `world.cpp`, and `main.cpp`. Game text goes to stdout via
`fputs`. Both land on the same terminal, which is the whole problem.

`profile.cpp` already solves two-thirds of the mechanism — a cached startup
switch, a swappable output target for tests, and writes serialized so one record
is always one whole line. **Three threads emit concurrently**: the main game, the
pre-generation worker, and the bard worker. Any logger that does not inherit that
serialization discipline will braid half-written lines the first time two threads
speak at once.

### What this supersedes

`REQ-LAT-5` already permits profiling output to go to "stderr **or a dedicated
log**", so relocating the timing records is inside what that spec allows. Two
other requirements are genuinely superseded:

- **REQ-LAT-1** — "Profiling is gated behind an environment variable (proposed
  `TEXTWORLD_PROFILE`), **off by default**." The gate becomes
  `TEXTWORLD_LOG_LEVEL`; the variable is retired.
- **REQ-PREGEN-22** — "All profiling remains behind `TEXTWORLD_PROFILE` and off
  by default. […] One record is still one line **on stderr**." Both halves
  change: the gate, and the destination. Its third clause — that emission from
  multiple threads is serialized so no record is interleaved or truncated — is
  **preserved and widened** by REQ-LOG-15, which now covers three threads rather
  than two.

Measured scope of that retirement: **42 references** across `tests/tests.cpp`
(`twprof`, `profileSetSink`, `TEXTWORLD_PROFILE`) and **3 passages** in
`README.md` (lines 25, 97, 103).

## Requirements

### The screen

<a id="req-log-1"></a>
**REQ-LOG-1** — Between the first prompt and quit, no internal engine message
appears on the terminal. A session's stdout is byte-for-byte what it is today.

<a id="req-log-2"></a>
**REQ-LOG-2** — Two messages are exempt, because they fire when there is no game
on screen: the schema-mismatch refusal (`world.cpp:178`, the binary refusing to
start) and the fatal-exception message (`main.cpp:114`, the binary already
dying). These continue to reach the terminal *and* are also written to the log
when one exists.

Reaching the terminal is **not automatic** once REQ-LOG-3 is active — at that
point the ordinary error channel *is* the log file. Before any redirect happens,
startup duplicates the original terminal channel and holds that duplicate open
for the life of the process. The two exempt messages are written to it
explicitly. This duplicate is used for nothing else, ever: it is the only
sanctioned route to the screen besides game text on stdout.

<a id="req-log-3"></a>
**REQ-LOG-3** — A process-level backstop redirects the error channel to the log
file at startup, so output from code this spec does not touch — libcurl, SQLite,
the C++ runtime, a future stray print statement — cannot reach the game screen
either. REQ-LOG-1 is thereby a property the program enforces, not a discipline
each future commit must remember.

### The file

<a id="req-log-4"></a>
**REQ-LOG-4** — One log file per session, created once at startup, before the
first prompt.

<a id="req-log-5"></a>
**REQ-LOG-5** — The file name embeds the local date and time, year first, so a
directory listing sorts chronologically by name: `textworld-YYYYMMDD-HHMMSS.log`
(e.g. `textworld-20260805-143022.log`).

<a id="req-log-6"></a>
**REQ-LOG-6** — Files live in a `logs/` directory in the current working
directory — beside `world.db`, matching the existing contract that the game finds
its world file wherever it is run. The directory is created if absent.

<a id="req-log-7"></a>
**REQ-LOG-7** — Logging is **best-effort**. Creation is attempted exactly once at
startup. On failure the game plays normally and nothing is recorded: no retry, no
re-open mid-session, no message on screen, no non-zero exit. A game that quietly
starts logging on turn 40 is worse than one that never logs.

<a id="req-log-8"></a>
**REQ-LOG-8** — When the log file cannot be created, the REQ-LOG-3 backstop
points at a discard sink rather than the terminal. Leaving it aimed at the screen
would reintroduce exactly what REQ-LOG-1 forbids.

The REQ-LOG-2 terminal duplicate is **structurally independent** of this sink: it
is captured before any redirect and is unaffected by whether the log file opened.
A failed log creation must never swallow the fatal-crash message the exemption
exists to preserve.

<a id="req-log-9"></a>
**REQ-LOG-9** — Retention: at startup, after the new file is created, all but the
**20 most recent** log files in `logs/` are deleted. Recency is judged by
modification time, not by parsing the file name, so files whose names collide at
the one-second resolution of REQ-LOG-5 still order deterministically. Only files
matching the REQ-LOG-5 name pattern are eligible for deletion — an unrelated file
in `logs/` is never removed. Failure to enumerate or delete is non-fatal and
never reaches the screen.

### Entry format

<a id="req-log-10"></a>
**REQ-LOG-10** — Every entry is one line with six fields in fixed order:
timestamp, level, thread, turn, source, message.

```
2026-08-05 14:30:22.184  INFO   main    turn=0   world   opened world.db (existing)
2026-08-05 14:30:41.902  DEBUG  bard    turn=3   bard    rejected: motive not in vocabulary
2026-08-05 14:30:44.310  WARN   main    turn=3   prose   render failed, using template text
2026-08-05 14:31:02.771  ERROR  pregen  turn=7   pregen  job failed: timeout after 20s
```

<a id="req-log-11"></a>
**REQ-LOG-11** — Timestamps are **local** wall-clock time with millisecond
precision, formatted `YYYY-MM-DD HH:MM:SS.mmm`.

<a id="req-log-12"></a>
**REQ-LOG-12** — The thread field names which part of the program was running:
`main`, `pregen`, or `bard`. Without it the three threads' messages are
indistinguishable in the file.

<a id="req-log-13"></a>
**REQ-LOG-13** — The turn field carries the existing process-local turn counter,
read without any database access, and is valid from any thread. It is the primary
key for correlating a message with what the player was doing.

<a id="req-log-14"></a>
**REQ-LOG-14** — The source field is its own column, not a prefix inside the
message text. Every existing message already begins with one (`aiRender:`,
`bard:`, `architect:`, `writeGeneratedRoom:`); migration lifts it out so the file
can be filtered by subsystem.

<a id="req-log-15"></a>
**REQ-LOG-15** — One entry is one line written in a **single** call, serialized
across all threads. Entries are never interleaved, never truncated, never split
across two writes — the invariant `profile.cpp` already holds.

<a id="req-log-16"></a>
**REQ-LOG-16** — Each entry is flushed to disk as it is written, so a session
killed mid-turn still has everything up to that point.

### Levels

<a id="req-log-17"></a>
**REQ-LOG-17** — Four levels, ordered: `ERROR`, `WARN`, `INFO`, `DEBUG`.

<a id="req-log-18"></a>
**REQ-LOG-18** — The default threshold is **INFO** when nothing is configured.

<a id="req-log-19"></a>
**REQ-LOG-19** — `TEXTWORLD_LOG_LEVEL` sets the threshold, read once at startup
and cached, matching the convention the game already uses for its AI settings. An
unset, empty, or unrecognized value falls back to the REQ-LOG-18 default without
comment.

<a id="req-log-20"></a>
**REQ-LOG-20** — Existing messages map to levels as follows. This is the complete
migration inventory; no site keeps a direct write to the error channel.

| Level | Sites |
|-------|-------|
| ERROR | Caught crashes and refusals: bard wake failure (`bardworker.cpp:111,114`), bard overture and after-turn failures (`bard.cpp:1008,1010,1139,1141`), pregen job failure (`pregen.cpp:111,114`), architect phase-1 failure (`architect.cpp:555,558`), schema mismatch (`world.cpp:178`), fatal (`main.cpp:114`) |
| WARN | Silent downgrades: prose render fell back to templates (`prose.cpp:429,433`), resolver fell back to the parser (`nlresolve.cpp:354,358`), and the precondition violation in `mutations.cpp:516` |
| DEBUG | Every validation rejection: `architect.cpp` `failClause` sites, `bard.cpp:235`, `nlresolve.cpp:86`, `prose.cpp:188` — **and** the seven `bard.cpp` sites at 692, 721, 765, 776, 849, 883, 900, which the first draft of this table omitted. They are the same family as the `failClause` rejections: the model proposed something the engine declined to take. WARN stays reserved for degradations the *player* feels. |

<a id="req-log-21"></a>
**REQ-LOG-21** — New INFO entries are added, because today the engine speaks only
on failure and a healthy session would produce an empty file — indistinguishable
from broken logging. At minimum: session start (with build/version if available),
world opened and whether it was created or resumed, whether AI narration is on,
and session end with the turn count.

### Timing records

<a id="req-log-22"></a>
**REQ-LOG-22** — The turn-profiling records become **DEBUG-level log entries**.
`TEXTWORLD_PROFILE` is retired; `TEXTWORLD_LOG_LEVEL=debug` is the single switch.
This supersedes REQ-LAT-1 and REQ-PREGEN-22.

<a id="req-log-23"></a>
**REQ-LOG-23** — The `twprof key=value` payload is preserved **byte-for-byte**
after the standard six-field prefix. The format is machine-parseable and pinned
by tests; this spec relocates it and changes its gate, not its content.

<a id="req-log-24"></a>
**REQ-LOG-24** — All other profiling semantics survive intact: per-stage
durations and the absent-not-zero-faked rule (REQ-LAT-2), curl sub-records only
on network calls (REQ-LAT-3), failed calls carrying no fabricated token counts
(REQ-LAT-4), AI-on and AI-off both instrumented (REQ-LAT-6), the `background=1`
marker (REQ-PREGEN-24), and dwell records (REQ-PREGEN-25).

<a id="req-log-25"></a>
**REQ-LOG-25** — At the default INFO threshold, the per-turn cost of the timing
instrumentation is at most what REQ-LAT-1 already allowed: reading a monotonic
clock and testing a cached value. No formatting or writing occurs for entries
below the threshold.

### Privacy

<a id="req-log-26"></a>
**REQ-LOG-26** — No log entry at any level contains an API key. No entry at INFO
or above contains a prompt, a response body, or the player's typed input.
Rejection *reasons* ("motive not in vocabulary") are not response bodies and are
permitted.

### Startup order

<a id="req-log-29"></a>
**REQ-LOG-29** — Startup runs in this exact order, and the order is load-bearing:

1. Duplicate the original terminal error channel and hold it (REQ-LOG-2).
2. Read `TEXTWORLD_LOG_LEVEL` and cache the threshold (REQ-LOG-19).
3. Create `logs/` if absent and open this session's file (REQ-LOG-4, -6).
4. Point the backstop at the file, or at the discard sink if step 3 failed
   (REQ-LOG-3, -8).
5. Apply retention (REQ-LOG-9).
6. Emit the session-start INFO entries (REQ-LOG-21).
7. Everything else — `openWorld`, the schema check, the overture, the workers.

Because the schema check is step 7, the schema-mismatch refusal always has both a
live log file and the terminal duplicate available, so REQ-LOG-2's "also written
to the log when one exists" is unambiguous for that message. Only a failure
*inside steps 1–5* can produce a message with no log to write to.

<a id="req-log-30"></a>
**REQ-LOG-30** — Retiring `TEXTWORLD_PROFILE` includes migrating everything that
references it, not merely the production code: the **42 references** in
`tests/tests.cpp` (`twprof`, `profileSetSink`, `TEXTWORLD_PROFILE`) and the
**3 passages** in `README.md` (lines 25, 97, 103). After this work no reference
to `TEXTWORLD_PROFILE` survives anywhere in the repository. Tests keep asserting
the same record content; only their gate mechanism changes.

### Testability

<a id="req-log-27"></a>
**REQ-LOG-27** — The output target is swappable, so tests observe entries without
touching a real stream or file — the mechanism `profileSetSink` already provides.

The sink receives the **formatted entry**: the six fields of REQ-LOG-10 and the
message together, as one line without its trailing newline. This mirrors
`profileSetSink` exactly, so a test sees what the file would have held. Timing
records are therefore delivered with the standard prefix in front of the
`twprof` payload; a test that wants the bare payload strips the prefix, and the
record assertions themselves are unchanged (REQ-LOG-23, REQ-LOG-30).

<a id="req-log-28"></a>
**REQ-LOG-28** — The REQ-LOG-3 backstop does not engage in the test binary, so
the suite's existing error-channel capture (`tests.cpp:10320`) and its
absence-assertion (`tests.cpp:10332`) keep working or are migrated to the
swappable target.

## Non-goals

- **Telling the player anything.** No on-screen notice, no status-band mark, not
  even when the AI degrades to template prose. Deliberate: see below.
- **Capturing prompts or response bodies at DEBUG.** Arguably useful for
  debugging malformed AI responses; out of scope here, and constrained by
  REQ-LOG-26 if revisited.
- **Log rotation within a session.** One file per session, however long it runs.
- **Structured/JSON output, or shipping logs anywhere.** Plain text, local file.
- **An in-game command to view the log.**

## Accepted cost

When the AI fails to write a description, the game silently falls back to canned
text. Today the only sign is the message this spec hides. Afterwards the player
gets duller writing with no explanation — and so does the developer, until
someone opens the log. This is the accepted trade: the game screen is for the
game.

## AI Validation

How to verify this is done. Each item is observable or runnable.

**Existing harnesses these items run against** — none of this needs new
infrastructure:

- **Scripted sessions against the real binary**:
  `.lore/work/validation/turn-latency-polish/drive.pl` feeds a fixed input script
  to `./build/textworld`, with the input scripts in
  `.lore/work/validation/*/script-*.txt`. This is how items that must exercise
  the real process (rather than the test binary) are driven.
- **Fake HTTP transports**: the `HttpTransport` seam in `src/aihttp.hpp`, already
  used ~145 times in `tests/tests.cpp`, injects canned or failing responses with
  no network and no API key.
- **Record capture**: the swappable output target of REQ-LOG-27, modeled on
  `profileSetSink`.

Items 1, 2, 5, 6 and 15 run against the **real binary** via `drive.pl`, because
REQ-LOG-28 disables the backstop in the test binary and those items are precisely
what verifies the backstop. Everything else runs in-suite.

### Screen cleanliness

1. **Baseline diff.** Capture a scripted session's stdout on the current build
   via `drive.pl`. Run the identical script on the new build. The two stdout
   streams are **byte-identical** (REQ-LOG-1).
2. **Combined-stream check.** Run the same script against the **real binary**
   with stdout and the error channel merged into one capture, with the AI pointed
   at an unreachable endpoint so every call fails. The merged capture contains
   **no** internal message — confirming the backstop, not just the migrated call
   sites (REQ-LOG-3). Must use the real binary: the test binary has the backstop
   disabled per REQ-LOG-28.
2b. **Exemption reachability.** Run the real binary against a world file with a
   wrong schema version and confirm the refusal **does** appear on the terminal
   while the backstop is active, and appears in the log as well (REQ-LOG-2,
   REQ-LOG-29). Repeat with `logs/` unwritable and confirm the refusal still
   reaches the terminal (REQ-LOG-8).
3. **Source scan.** Grep `src/` for `fprintf(stderr` / `cerr`. The only surviving
   hits are the two REQ-LOG-2 exemptions and the logger's own implementation.
   Assert this as a source-text regression guard, matching the existing guards for
   band read-only-ness and `main()` declaration order.

### The file

4. Run a session; assert exactly one new file appears in `logs/`, that its name
   matches `textworld-\d{8}-\d{6}\.log`, and that sorting the directory by name
   equals sorting it by modification time (REQ-LOG-4, -5, -6).
5. **Best-effort.** Make `logs/` unwritable, run the baseline script, and assert:
   exit code unchanged, stdout byte-identical to run 1, nothing on the error
   channel, no crash (REQ-LOG-7, -8).
6. **Retention.** Seed `logs/` with 25 synthetic files of known ages, start a
   session, assert exactly **20 remain in total — the session's own file among
   them** — and that the survivors are the newest (REQ-LOG-9). (An earlier
   wording here said "20 plus the new one", which contradicts REQ-LOG-9 as
   written; REQ-LOG-9 governs.) Seed one undeletable file; assert startup still
   succeeds silently.

### Entry format

7. Parse every line of a real session log against the six-field grammar; assert
   zero unparseable lines (REQ-LOG-10).
8. Assert timestamps are monotonically non-decreasing within a file and match the
   millisecond format (REQ-LOG-11).
9. **Thread interleaving.** Drive concurrent emission from all three threads
   in-suite, reusing the shape of the existing 400-record profiling stress test
   (`tests.cpp:2543-2601`) with the REQ-LOG-27 capture target, and assert every
   line parses and no line is truncated or braided (REQ-LOG-15).
10. Assert each thread's entries carry the correct thread label, and that a
    background pre-generation entry is labeled `pregen`, not `main` (REQ-LOG-12).

### Levels

11. **Threshold sweep.** Run the same failure-injected script at each of the four
    levels; assert entry counts are monotonically non-decreasing as the level
    widens, and that no `DEBUG` line appears in an `INFO` run (REQ-LOG-17, -19).
12. Assert an unset variable yields INFO, and that a garbage value (`banana`)
    also yields INFO with no complaint (REQ-LOG-18, -19).
13. **Migration completeness.** For each row of the REQ-LOG-20 table, drive the
    condition and assert an entry appears at the stated level with the stated
    source (REQ-LOG-20).
14. Run a clean session with no failures at default level; assert the log is
    **non-empty** and contains session-start, world-opened, and session-end
    entries (REQ-LOG-21).

### Timing records

15. Run one scripted session via `drive.pl` on the current build with
    `TEXTWORLD_PROFILE=1`, and the same script on the new build with
    `TEXTWORLD_LOG_LEVEL=debug`. Strip the six-field prefix from the new build's
    `twprof` lines; assert the payloads are **byte-identical** to the old build's,
    field for field (REQ-LOG-23).
16. Assert the existing profiling tests pass with only their gate mechanism
    updated — the record-format assertions themselves unchanged (REQ-LOG-24).
17. Assert `TEXTWORLD_PROFILE` appears **nowhere** in the repository —
    `grep -rn TEXTWORLD_PROFILE .` returns only matches inside `.lore/`
    historical documents (REQ-LOG-22, REQ-LOG-30). The README's three passages
    (lines 25, 97, 103) are rewritten for the new variable.

### Privacy

18. Run a session in-suite with a dummy key set, using an `HttpTransport` fake
    that echoes a known sentinel string in both the request body it receives and
    the response body it returns. Assert the captured log contains neither the
    key nor the sentinel at INFO, and never the key at any level, including DEBUG
    (REQ-LOG-26).
