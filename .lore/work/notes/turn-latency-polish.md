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

- [ ] 1 — `src/profile.{hpp,cpp}`: gate, record formatters, sink → `testProfileRecords`
- [ ] 2 — phase timers in `runTurn` → `testProfileTurnStages`
- [ ] 3 — `generate` stage timer → `testProfileGenerateStage`
- [ ] 4 — `src/aihttp.{hpp,cpp}`: `AiRole`, `modelForRole`, `parseUsage` → `testAiRoleModel`, `testAiUsageParse`
- [ ] 5 — wire `modelForRole` into the three request builders
- [ ] 6 — shared persistent-handle curl client (inspection gate)
- [ ] 7 — `curl_global_init`/`cleanup` guard at process boundaries
- [ ] 8 — swap the three transports onto the shared client (zero test changes)
- [ ] 9 — **STOP** — one bounded live run
- [ ] 10 — final sweep against the spec checklist

## Log

(appended per step)
