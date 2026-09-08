---
title: "Live validation findings: session-logging"
date: 2026-08-05
status: current
tags: [validation, logging, diagnostics, session-log, retention, backstop, privacy]
modules: [log, profile, main, world, prose, nlresolve, architect, bard, pregen]
related: [.lore/work/specs/session-logging.md, .lore/work/plans/session-logging.md, .lore/work/notes/session-logging.md]
---

# Live validation findings: session-logging

Step 12 of the plan: the six items REQ-LOG-28 puts out of the test binary's
reach, run against the real `./build/textworld`. Everything else in the spec's
validation list runs in-suite and is recorded in the notes instead.

All runs go through `run.sh` or the same shape by hand: a **pristine temp
directory** with a fresh `world.db` and `seed/` copied in, because a run mutates
the world and two runs of one script are only comparable from the same start.

## What is in this directory

| Path | What it is |
|---|---|
| `baseline/textworld-baseline` | the executable built from `fa3db63`, *before* any of this work — **git-ignored**: rebuild it by checking out the commit in `REVISION` if a comparison is ever needed again |
| `baseline/REVISION` | the commit it was built from |
| `baseline/stdout.txt`, `stderr.txt` | the hermetic pre-change run — 2826 B of game text, **0 B** of stderr |
| `baseline/stdout-profile.txt`, `twprof.txt` | the same script under the old `TEXTWORLD_PROFILE=1` |
| `run.sh` | drives one scripted session against a named binary in a clean temp dir |
| `captures/` | the transcripts the items below refer to |

## The environment is not hermetic by default — and it mattered

`ANTHROPIC_API_KEY` is set in the developer shell. The first baseline attempt
made **live** AI calls, and two runs of the identical script produced different
stdout (diverging at line 23). Item 1 asserts byte-identical stdout, which is
only a meaningful claim under a hermetic run, so `run.sh` unsets the key and
forces `TEXTWORLD_AI=0`. Two consecutive baseline runs then matched exactly.

Worth recording because it bit twice: `VAR=x cmd | binary` applies `VAR` to
**`cmd`**, not to `binary`. Two early runs looked like clean passes and were
actually the game running with the variable unset. Every run below applies the
environment with `env … ./build/textworld` on the right-hand side of the pipe.

## The six items

### 1. Baseline diff — REQ-LOG-1 ✅

Identical script on `baseline/textworld-baseline` and on the new build.

```
stdout: 2826 B  vs  2826 B   →  cmp: byte-identical
stderr: 0 B     vs  0 B
```

A `logs/` directory appears beside the new build's `world.db` with exactly one
file in it. Nothing about the player's screen changed.

### 2. Combined-stream check — REQ-LOG-3 ✅

The real binary, stdout and the error channel merged into one capture, with a
key set and **every AI call failing**. The endpoint is hardcoded
(`aihttp.cpp:103`), so "unreachable" is arranged with `ALL_PROXY=http://127.0.0.1:9`
— libcurl fails at connect and no traffic leaves the machine, which is a
stricter reading of the spec's intent than pointing a bogus key at the real API.

Run at `TEXTWORLD_LOG_LEVEL=debug`, deliberately: at the INFO default a
transport error is a DEBUG-level rejection and the engine would have had almost
nothing to say. The point is to make it maximally chatty and confirm **none of
it** reaches the screen.

| | |
|---|---|
| merged capture (`captures/item2-combined-stream.txt`) | 2911 B, game text only |
| internal messages in it | **0** — no `twprof`, no `rejected`, no `falling back`, no level name |
| session log (`captures/item2-session.log`) | 83 lines: **79 DEBUG**, 4 INFO |
| the key in the log | **0 occurrences** (REQ-LOG-26) |

The backstop is what this proves: 79 messages had somewhere to go, and the
terminal was not it.

### 2b. Exemption reachability — REQ-LOG-2, -8, -29 ✅

A world file with `schema_version = 999999`:

- **Terminal** (`captures/item2b-schema-terminal.txt`) — the two-line refusal,
  verbatim, exit 1.
- **Log** (`captures/item2b-schema-session.log`) — the same text flattened to a
  single ERROR entry, source `world`, between the session-start and session-end
  INFO entries. REQ-LOG-29's ordering is what makes this available: the schema
  check is step 7, so the log is always open by then.

Both channels come from **one** `logExempt()` call, which is what keeps their
wording from drifting: the terminal copy is verbatim, the log copy is the same
bytes with newlines folded to spaces because an entry is one line. The fatal
exemption was exercised the same way (a corrupt `world.db`): terminal reads
`fatal: file is not a database`, and the log carries it as one ERROR entry with
no trailing whitespace.

Repeated with `logs/` unwritable: the refusal **still reaches the terminal**,
`logs/` stays empty, exit 1 unchanged. This is REQ-LOG-8's structural
independence holding — the terminal duplicate is captured before any redirect
and does not care whether the file opened.

### 5. Best-effort — REQ-LOG-7, -8 ✅

`chmod a-w logs/`, then the baseline script:

```
exit code : 0            (unchanged)
stdout    : 2826 B       (cmp against baseline/stdout.txt → byte-identical)
stderr    : 0 B
logs/     : empty
```

No retry, no message, no crash, and nothing aimed at the screen.

### 6. Retention — REQ-LOG-9 ✅

25 seeded files with staggered modification times, then one session:

```
before: 25    after: 20    (the session's own file among the 20)
```

The five oldest by mtime are gone. The precise "survivors are the newest"
ordering claim is carried by the in-suite test, which controls mtimes exactly;
this run confirms the arithmetic on a real process.

**Undeletable entry.** Seeded alongside 25 logs: a non-empty *directory* named
`textworld-20260101-000000.log` — it matches the REQ-LOG-5 pattern but cannot be
removed. Startup succeeded silently: exit 0, **0 B** on stderr, the session log
written, 20 `.log` files remaining, and the blocker untouched. Note the
implementation skips it at the `is_regular_file` check rather than attempting
the delete and swallowing the error, so this exercises the "never removes what
it should not" half more sharply than the "swallows a failed delete" half. The
latter is covered in-suite by the read-only-directory case.

### 15. twprof payload identity — REQ-LOG-23 ✅

One scripted session on the old build with `TEXTWORLD_PROFILE=1`, the same
script on the new build with `TEXTWORLD_LOG_LEVEL=debug`, the six-field prefix
stripped from the new build's lines with `sed`.

```
old build : 39 twprof records
new build : 39 twprof records
```

Two comparisons, both clean:

1. **Key sequence** (every value masked) — identical, line for line.
2. **Strict** (only `ms=` and `*_us=` masked, every other value compared
   literally) — identical, line for line.

Only the wall-clock numbers differ, which they must: they measure two different
runs. Captures are in `captures/item15-old-build-twprof.txt` and
`captures/item15-new-build-twprof.txt`.

This is the claim the plan protects by never touching `profile.cpp`'s four
`format*` functions — confirmed separately by extracting them from
`git show HEAD:src/profile.cpp` and from the worktree and comparing: all four
byte-identical.

## Not covered here

Item 15's records are `kind=stage` and `kind=dwell` only — a hermetic run makes
no network calls, so no `kind=call` record exists to compare. The `kind=call`
format is pinned by the in-suite formatter tests, which are unchanged by this
work, and the item 2 run above does produce real `kind=call` records (with
`failed=1`) in the expected shape.

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
