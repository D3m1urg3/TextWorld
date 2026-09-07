---
title: "Implementation notes: terminal visual polish"
date: 2026-09-07
status: in_progress
tags: [notes, ui, terminal, ansi-color, typography, linenoise, line-editing, spinner, ascii-art, readability]
modules: [term, band, render, loop, main, world, systems]
related: [.lore/work/plans/terminal-visual-polish.md, .lore/work/specs/terminal-visual-polish.md, .lore/work/research/terminal-visual-polish-implementation.md]
---

# Implementation notes: terminal visual polish

Execution of [the plan](.lore/work/plans/terminal-visual-polish.md) — 21 steps
against [the spec](.lore/work/specs/terminal-visual-polish.md)'s 33 requirements.
Run inline (no subagents) at the author's instruction.

Baseline before step 1: `./build/tests` green, 9608 checks, 0 failures.

## Progress

- [x] 1 — vendor linenoise into the build, unused
- [x] 2 — cap the prose wrap width
- [x] 3 — indent narration two spaces
- [ ] 4 — a blank line before the prompt
- [ ] 5 — delete the duplicated lines, catch the band when it falls
- [ ] 6 — dim refusals
- [ ] 7 — background colour in `term`
- [ ] 8 — the bar itself, as a pure function
- [ ] 9 — bars in the band rows
- [ ] 10 — record the starting room
- [ ] 11 — `roomSeen`, derived from the transcript
- [ ] 12 — first sight at the three render sites
- [ ] 13 — the startup render prints the paragraph only at world creation
- [ ] 14 — `examine <room>` is the full reread
- [ ] 15 — the title screen
- [ ] 16 — linenoise replaces `getline`
- [ ] 17 — the history file
- [ ] 18 — re-capture the validation baselines
- [ ] 19 — the spinner, deterministic half (MED)
- [ ] 20 — the spinner, live observation (HIGH)
- [ ] 21 — walk the spec's twenty checks
- [ ] spec amendments folded back (REQ-POLISH-15, REQ-EXAMINE-7)

## Log

### Setup

Plan status flipped `draft` → `approved`. No `.lore/work/tasks/terminal-visual-polish/`
directory exists, so the plan's 21 steps are the phases directly.

### Step 1 — vendor linenoise (done)

Upstream `antirez/linenoise` @ `a473823d` (2026-05-01), BSD-2, 2380 lines.
`linenoise.c` / `.h` copied byte-exact into `vendor/`; the commit is recorded in
`CMakeLists.txt` beside the target rather than by editing the vendored file.

Linked into `twcore` and `textworld` both, as the plan insisted. The throwaway
`linenoiseHistorySetMaxLen(64)` in `loop.cpp` linked `tests` and `textworld`
alike, then was removed. `twcore`'s link is `PRIVATE`, which is what `sqlite3`
already uses: CMake still carries a static library through as
`$<LINK_ONLY:...>`, so `tests` gets the objects transitively even though the
include directory does not propagate. Step 19 may need `vendor` on `tests`'
include path if the `Spinner` type ends up in a header — noted, not needed yet.

### Step 2 — cap the prose wrap (done)

`kProseMaxWidth = 66`, `kProseIndent = 2`, pure `proseWidth`. Gate green.

**Divergence from the plan's expectations, recorded rather than escalated:** the
plan predicted the two golden literals would first move at step 5, and that
step 2 would be green apart from a `testBandWiring` width assertion. They moved
here. The suite pins width to 40, so `proseWidth(40) = 38` re-wraps two lines of
`spells` output and one telegraph line. Same class of change the plan already
authorises at steps 5, 9 and 12 — a deliberate layout change, spec check 20's
exemption — so it was re-captured with `TW_DUMP_GOLDEN=1` and named in the
commit message. `testBandWiring`'s `<= 40` assertion held untouched, as
predicted.

Two structural assertions (`tests.cpp:4796`, `:4863`) compare `runTurn`'s output
against a hand-composed `wrapProse(...) + composeBand(...)`. They now pass
`proseWidth`. The plan's "~25 assertion sites" estimate for step 5 did not count
these; they belong to step 2.

Re-capture is scripted rather than hand-run:
`scratchpad/recapture.py` runs the suite under `TW_DUMP_GOLDEN=1` and rewrites
both `R"GOLDEN(...)"` bodies in place.

### Step 3 — indent narration (done)

`indentProse` plus `TurnPresentation{Prose, Reference}` on `TurnResult`.
Six more hand-composed assertions needed the indent — three layout ones and the
three talk refusals at `tests.cpp:12091`, `:12133`, `:12175`, which compare
against `kNoOneToTalkTo`/`kNoReply` verbatim. Goldens re-captured again.

**Observation, out of scope, not fixed.** At `COLUMNS=20` one line still exceeds
20 columns: `AI narration off — template mode` (32 columns), printed by
`main.cpp:126` with a raw `fputs` and never wrapped. It predates this spec, it
is not narration, and REQ-POLISH-1 governs "narration and template prose". Spec
check 1's literal wording ("no line exceeds 20") would fail on it. Raised here
rather than silently widened into the spec's scope.
