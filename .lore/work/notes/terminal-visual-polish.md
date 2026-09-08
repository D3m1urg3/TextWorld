---
title: "Implementation notes: terminal visual polish"
date: 2026-09-07
status: complete
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
- [x] 10 — record the starting room
- [x] 11 — `roomSeen`, derived from the transcript
- [x] 12 — first sight at the three render sites
- [x] 13 — the startup render prints the paragraph only at world creation
- [x] 14 — `examine <room>` is the full reread
- [x] 15 — the title screen
- [x] 16 — linenoise replaces `getline`
- [x] 17 — the history file
- [x] 18 — re-capture the validation baselines
- [x] 19 — the spinner, deterministic half (MED)
- [x] 20 — the spinner, live observation (HIGH)
- [x] 21 — walk the spec's twenty checks
- [x] spec amendments folded back (REQ-POLISH-15, REQ-EXAMINE-7, plus REQ-POLISH-10/-10a/-12 and open question 1)

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

### Steps 10, 11 — meta.start_room and roomSeen (done)

Both clean. `meta.start_room` is derived from the seed's own `location` row, so
no seed or fixture names a number. REQ-POLISH-15 amended in the spec.

One coupling recorded, not fixed: `combat.hpp:22` hardcodes `kDormitoryCell = 1`
as the respawn destination whatever fixture is loaded, while `meta.start_room` is
derived. They agree in all three seeds today. A future fixture starting the
player elsewhere would send a downed player to room 1 while `start_room` named
another — a `kDormitoryCell` problem for the day a fixture breaks it.

### Steps 12 + 13 — first sight (done, as ONE commit)

**Merged the plan's two steps.** `renderRoomOf` takes the `seen` flag as a
parameter, so step 12's signature change breaks step 13's call site in the same
compile. There is no revision between them that builds. Not a scope change —
the same work, one commit instead of two.

Two test repairs beyond the obvious:

- `testBandWrap` drove a repeat `look`, which now yields one short line and made
  its "more than four lines" assertion vacuous. It drives a first arrival now.
- The **live** architect walkability test (`TEXTWORLD_AI_LIVE_TEST=1`, so the
  default suite never runs it) parsed its displayed-exit set out of
  `renderRoomOf`, which step 5 had already emptied. It reads the band now. Left
  alone it would have failed the first time anyone ran it live — worth noting as
  a general risk: gated tests do not fail when you break them.

### Step 14 — examine a room (done)

REQ-EXAMINE-7 amended in `.lore/work/specs/examine-perception-verb.md`, and its
"Out of scope" bullet — which named this as explicitly refused — struck through.
`render.cpp`'s `examined` branch left exactly as it was; the assertion that it is
byte-identical to `roomBlock`'s unseen output is what lets it stay.

### Step 15 — title screen (done)

**Neither `figlet` nor `toilet` is installed here** and the plan's fallbacks
(`brew install figlet`, an online generator) were not available. The nine glyphs
were hand-authored on a 5x5 grid and assembled by a script, which is what makes
the letterforms consistent. `#` and spaces only. Natural width 53.

Open question 2 — an env switch to suppress it — **not built**, as instructed.

### Steps 16, 17 — linenoise and history (done)

Verified interactively through a **pty harness**
(`scratchpad/pty_test.py`), which had to answer linenoise's TWO `ESC[6n` cursor
queries per prompt — one for the cursor column and one after `ESC[999C` for the
terminal width. Answering only the first hangs the harness, which cost a
detour worth recording for whoever drives a terminal program from a test next.

Confirmed: typing, backspace, ctrl-a, cross-session up-arrow recall, a chmod-000
history file that still lets the game run and logs both failures, and — the case
the destructor exists for — a session killed mid-loop by a real fatal error
still writing both its lines to the file.

**`tests` needed an explicit `linenoise` link.** `twcore` links it `PRIVATE`, so
CMake carries the OBJECTS through but not the PUBLIC include directory: the test
could not `#include "linenoise.h"` at all. The step-1 note predicted this might
be needed; it is.

### Step 18 — validation baselines (done)

Compared against a binary built from `859451d` in a throwaway worktree. All five
offline scripts: zero escape bytes, identical outcome sequences, identical
`events` rows. New captures committed as `polish-*.txt`; the harness is committed
as `.lore/work/validation/revalidate.py`.

### Step 19 — the spinner, deterministic half (done, MED)

**Divergence from the plan's mechanism.** The plan says to use
`linenoiseHide` / `linenoiseShow`. Both take a `struct linenoiseState*` and
belong to linenoise's **non-blocking** API (`linenoiseEditStart`/`Feed`/`Stop`).
Step 16 uses the blocking `linenoise()`, which owns its state internally and has
already **returned** by the time `aiRender` runs — there is no line being edited
during the spinner, so there is nothing to hide. The research's "line editing and
the spinner are one job" assumed the async API. Erasure is a carriage return, one
space, a carriage return. No requirement changes: REQ-POLISH-25 through -28 say
nothing about `linenoiseHide`.

Three design points worth keeping:

- The frame wait is a **condition variable**, not a sleep, so a turn that returns
  in microseconds is not delayed a frame interval by its own spinner. Asserted.
- The erase runs **after the join**, so nothing can be written between the last
  frame and the clear.
- A spinner destroyed **before its first frame writes nothing at all** — not even
  the clear. An unconditional erase would move the cursor on an instant turn.

Ran the whole suite under `-fsanitize=thread`: 11061 checks, 0 failures, **zero**
ThreadSanitizer warnings.

### Step 20 — the spinner, live observation (done, HIGH)

Three turns, as budgeted. Piped: zero escape bytes, zero carriage returns.
Terminal: 21 frames during the wait, one erase, no residue. Invalid key
(~162 ms, from `prof-invalid-key.log` — long enough for exactly one frame, which
is what makes it a real test of the failure path): one frame, one erase, no
residue, and the turn fell back to templates and the game continued.

**Method note worth keeping.** The first analysis of the terminal turn reported
zero frames and was wrong: it read the pty capture in **text mode**, where
Python's universal-newline handling destroys exactly the carriage returns the
frames are made of. Read terminal captures in binary.

### Step 21 — the sweep (done)

All twenty checks pass. The runnable half is committed as
`.lore/work/validation/sweep-terminal-visual-polish.sh`.

- **REQ-POLISH-32.** `combat.cpp`, `combat.hpp` and `mutations.cpp` are
  **untouched** by the whole branch. No `CREATE`/`ALTER`/`DROP TABLE` added.
  `SCHEMA_VERSION` still 8. The one `appendEvent` line added uses `examined`, an
  existing verb. Every combat number in the golden literals is unchanged: the
  only differences are two **additions** (`HP: 1/10`, `HP: 1/12`) from the new
  bar tests' deliberate low-health states.
- **REQ-POLISH-33.** A piped run emits zero escape sequences. The only new
  `\x1b[` literal anywhere in `src/` is inside a **comment** in `term.hpp`
  explaining why reverse video is not offered.
- **Check 20.** Fifteen existing test functions were edited, every one of them
  asserting on layout this spec deliberately changes. No combat, turn-count or
  schema test was touched — verified by name and by diffing the numbers.

**One check narrowed, deliberately.** Check 19 says the title screen "contains
only ASCII". Asserted over the ART, not over the whole capture: the em-dash on
`AI narration off — template mode` is `main.cpp:126`'s pre-existing notice, not
the title screen. Same line already flagged at step 3 for exceeding 20 columns.

## Left undone, deliberately

- **The mode notice at `main.cpp:126`** is 32 columns and unwrapped, so at
  `COLUMNS=20` it is the one line that exceeds the terminal. It predates this
  spec, it is not narration, and REQ-POLISH-1 governs "narration and template
  prose". Raised rather than silently widened into scope.
- **`kDormitoryCell`** (`combat.hpp:22`) still hardcodes room 1 as the respawn
  destination while `meta.start_room` is derived. They agree in all three seeds.
- **The title screen's env switch** — open question 2 — not built, as instructed.
