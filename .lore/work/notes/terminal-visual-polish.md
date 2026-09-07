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
- [x] 4 — a blank line before the prompt
- [x] 5 — delete the duplicated lines, catch the band when it falls
- [x] 6 — dim refusals
- [x] 7 — background colour in `term`
- [x] 8 — the bar itself, as a pure function
- [x] 9 — bars in the band rows
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

### Step 4 — blank line before the prompt (done)

Gated on `currentStyle().attrs`. Piped capture diffed byte-identical against the
pre-change one; all nine prompts in a `CLICOLOR_FORCE=1` capture have a blank
line above them.

### Step 5 — delete the duplicated lines (done)

**Divergence from the plan, not from the spec — the plan's file map missed a
file.** `prose.cpp`'s `deterministicAppends` carried a SECOND copy of the
`Exits:` / `You see:` tail, appended after validated AI prose. REQ-POLISH-5
names only `render.cpp`'s `roomBlock`, but the spec's "Existing requirements this
changes" section already says REQ-PROSE-14 is satisfied by the band. Left in
place, REQ-POLISH-5 would hold on the template path and fail on the AI path, and
spec check 5 would fail whenever AI is on. Deleted. Its query also lacked the
latent-exit gate the other two emitters share, so a latent exit was listed
unconditionally on the AI path — a pre-existing REQ-EXITS-4 bug that goes with it.

Spec check 6 is asserted by `testExitDisplayInvariant` against `composeBand` with
the architect toggled in-process, rather than by two runs of the binary.
`architectEnabled()` returns `aiNarrationEnabled()`, so the binary half would
spend tokens, and step 5 is an offline step.

Nine assertion sites rewritten. The plan's unverified list (`:1900`, `:1909`,
`:2864`, `:2878`, `:4695-4701`, `:5261`, `:9439-9440`) was close: the real lines
were 1900, 1909, 2865, 2879, 4696-4702, 5267, 9518-9519, plus three the plan did
not name at all (`testBandProseUnstyled`, the band-content parity block, and the
AI-prose tail test).

### Steps 6, 7 — dim refusals, background colour (done)

Both clean, both gates green first time. `dimEachLine` styles per line and
*inside* the indent, so `stripSgr` of a dimmed refusal equals the plain one.

### Steps 8, 9 — the bars (done)

**Spec contradiction, raised and resolved by the author.** REQ-POLISH-12's
literal `[####......]` is 12 columns; REQ-POLISH-10a budgets 11. Author chose to
keep the budget and coarsen the degraded form to `[` + 8 cells + `]`.
REQ-POLISH-10, -10a and -12 all amended in the spec.

**Three mechanical things the plan did not anticipate.**

1. `BandSpan` needed a `background` flag. A bar's text is spaces, which carry no
   foreground. Styling still happens at emission, so REQ-POLISH-11's "no change
   to `layoutBand`'s width arithmetic" holds.
2. `tokenize` split span text on spaces and dropped the pieces, so a spaces-only
   span was **erased entirely**. It is now one atomic token — which is what a bar
   wants anyway, since a bar broken across two lines is not a bar. The plan's
   "no change to `layoutBand`" was true of the arithmetic and false of the
   tokenizer.
3. The band's usual two-space gap would have made the bar cost 12 columns, so the
   preceding span's pad is narrowed to one space for exactly that join.

**One defect the suite did not catch and a real fight did.** The player's bar
first took the HP number's own colour, which is `Color::None` while health is
fine (REQ-UI-16). `bgColorize` with `Color::None` emits nothing, so a healthy
player's bar rendered as ten plain spaces. Healthy is now green, low is yellow.
Worth recording as a method note: the band goldens are all `kBandPlain`, so no
golden could have caught a colour-only defect.

**Open question 1 settled:** the bar sits immediately after `HP: n/m`. Folded
back into the spec.

REQ-UI-22's byte-for-byte suppression parity had to be narrowed: REQ-POLISH-12's
two-step degrade means the colourless band is deliberately *not* the coloured one
stripped. The test maps the plain band's bars back to spaces first.

Golden re-capture is scripted: `scratchpad/recapture_bands.py` for
`testBandGoldens`' seven inline literals, `scratchpad/recapture.py` for the two
session literals.
