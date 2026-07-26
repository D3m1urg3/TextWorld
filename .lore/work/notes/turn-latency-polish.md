---
title: "Implementation notes: turn-latency-polish"
date: 2026-07-26
status: in_progress
tags: [implementation, notes, performance, latency, profiling, libcurl, connection-reuse, model-tiering]
source: .lore/work/plans/turn-latency-polish.md
modules: [loop, nlresolve, prose, architect, aihttp, profile]
related: [.lore/work/specs/turn-latency-polish.md, .lore/work/brainstorm/performance-polish-action-latency.md, .lore/work/research/per-turn-latency-remedies.md]
---

# Implementation notes: turn-latency-polish

Executing the **approved** plan inline (no implement subagents —
[[no-implement-subagents]]). One step per commit on branch
`feat/turn-latency-polish`. Gate per step: `cmake --build build` clean +
`./build/tests` green + that step's named test or inspection grep. Steps are not
batched.

Live-LLM work is confined to **Step 9**, which is a hard stop for user
go-ahead ([[verification-must-be-bounded]]).

## Progress tracker

- [x] 1 — `src/profile.{hpp,cpp}`: gate, record formatters, sink → `testProfileRecords` ✅ `95102b8`
- [x] 2 — phase timers in `runTurn` → `testProfileTurnStages` ✅ `5354bd4`
- [x] 3 — `generate` stage timer → `testProfileGenerateStage` ✅ `a9e4e48`
- [x] 4 — `src/aihttp.{hpp,cpp}`: `AiRole`, `modelForRole`, `parseUsage` → `testAiRoleModel`, `testAiUsageParse` ✅ `205df2e`
- [x] 5 — wire `modelForRole` into the three request builders ✅ `f783189`
- [x] 6 — shared persistent-handle curl client (inspection gate) ✅ `0411fc0`
- [x] 7 — `curl_global_init`/`cleanup` guard at process boundaries ✅ `8ad774f`
- [x] 8 — swap the three transports onto the shared client (zero test changes) ✅ `9bd89ae`
- [ ] 9 — **STOP: awaiting go-ahead** — one bounded live run
- [ ] 10 — final sweep against the spec checklist

## Log

**Steps 1–8 (offline half) — complete.** Suite grew 2520 → 2599 checks, 0
failures throughout. Each step built clean and passed its named gate before the
next began; one commit per step, no batching.

Gates as run:

- **1** `testProfileRecords` — env matrix (unset/`1`/empty/`0`), both formatters
  parsed back as key=value, failed record carries no token keys, sink silent
  while the gate is off.
- **2** `testProfileTurnStages` — a ticked turn emits exactly
  `resolve,tick,narrate,total` and no call record; unresolvable line and `quit`
  emit only `resolve,total`; the same first turn on two identically seeded
  worlds is byte-identical on and off. Confirmed again at the binary level: a
  scripted `TEXTWORLD_AI=0` session diffs clean between on and off, with the
  records on stderr and stdout untouched.
- **3** `testProfileGenerateStage` — latent walk emits one `generate` record
  tagged `nested_in=tick`; `look` and a realized move emit none; the wall path
  still emits one and leaves the wall text and the latent row alone.
- **4** `testAiRoleModel` + `testAiUsageParse` — nine unreadable bodies all read
  as `known=false` with zeroed (never reported) counts.
- **5** `grep -n "claude-opus-4-8" src/*.cpp` → only `aihttp.cpp`; the only
  `getenv("TEXTWORLD_MODEL")` left in `src/` is there too. Prose and architect
  body tests still assert Opus, untouched — that contrast is the per-role proof.
- **6** Option-list diff, old `prose.cpp` transport vs new `anthropicPost`: same
  options, same order, plus exactly `CURLOPT_NOSIGNAL` and
  `CURLOPT_TCP_KEEPALIVE`. Key appears only in the header build.
- **7** No-key scripted session prints the identical template-mode transcript
  and exits 0 with silent stderr.
- **8** **Zero test changes** (`git diff --stat HEAD -- tests/` empty) with the
  suite green — the seam proof. `curl_easy_init` and `<curl/curl.h>` now appear
  only in `aihttp.cpp`; exactly one `curl_easy_cleanup` in the codebase, inside
  `aiHttpShutdown`.

### Divergences from the plan

None. Two small additions, both inside the plan's stated intent:

1. The suite's hermetic block in `tests/tests.cpp` `main()` now unsets
   `TEXTWORLD_PROFILE` and refreshes the cached gate, matching the existing
   discipline for `ANTHROPIC_API_KEY` / `TEXTWORLD_AI`. Without it a developer
   shell with the variable set would spray records through every `runTurn` test.
2. `CallRecord` carries a `turn` field (the plan's struct sketch omitted it
   while the format example showed `turn=`).

### Incident

An early Step-2 smoke of `./build/textworld` was run without `TEXTWORLD_AI=0`.
The shell exports a real `ANTHROPIC_API_KEY`, so it made live calls — roughly
three resolve and two narrate calls on Opus (Step 5 had not landed, so resolve
was still Opus), a few cents. Unintended: the live budget belongs to Step 9.
Every later smoke pins `TEXTWORLD_AI=0`.
