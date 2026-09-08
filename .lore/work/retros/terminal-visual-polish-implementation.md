---
title: "Terminal visual polish implementation"
date: 2026-09-08
status: open
tags: [ui, terminal, ansi-color, background-color, typography, prose-width, linenoise, line-editing, history-file, spinner, threading, tsan, pty, golden-tests, test-recapture, ascii-art, figlet, band, health-bar, first-sight, validation-baselines, inline-implementation, implement-skill, cpp, sqlite, spec-contradiction]
modules: [term, band, render, loop, main, world, systems, prose, spinner]
related: [.lore/work/plans/terminal-visual-polish.md, .lore/work/specs/terminal-visual-polish.md, .lore/work/notes/terminal-visual-polish.md, .lore/work/research/terminal-visual-polish-implementation.md, .lore/work/brainstorm/terminal-visual-polish.md, .lore/work/specs/examine-perception-verb.md]
---

# Terminal visual polish implementation

One session, 2026-09-07 into 2026-09-08, executing all 21 steps of
`.lore/work/plans/terminal-visual-polish.md` against the spec's 33 requirements.
22 commits. Suite went from 9608 checks to 11096, 0 failures throughout, and
green under `-fsanitize=thread` with zero race warnings. Spec marked
`implemented`, plan `executed`.

Run **inline**, not orchestrated. The invoking prompt said "Do not dispatch
subagents… keep `/implement`'s phase order, its per-step validation gates, its
task-file status updates and its notes file, and do the work inline." That is the
opposite of the engine-foundation-prototype run, which put every action through a
sub-agent. The skill's own Initialize step (dispatch `lore-researcher`) was
skipped for the same reason. No task files existed for this plan, so the plan's
21 steps were the phases directly.

## Plan versus reality

The step sequence held. Every step ran in the order written, none was skipped,
and step 5 was not split as the plan insisted. Four divergences, all recorded in
the notes file as they happened:

**`prose.cpp` was a second emitter the plan's file map missed.**
`deterministicAppends` carried its own copy of the `Exits:` / `You see:` tail,
appended after validated AI prose. REQ-POLISH-5 names only `render.cpp`'s
`roomBlock`. Left in place, the requirement would have held on the template path
and failed on the AI path, and spec check 5 would fail whenever AI was on. The
spec had already anticipated this in its "Existing requirements this changes"
section — REQ-PROSE-14 is satisfied by the band — so this was a gap in the plan,
not in the spec. Its query also lacked the latent-exit gate the other two
emitters share, so a latent exit was listed unconditionally on the AI path; that
went with it.

**Steps 12 and 13 became one commit.** `renderRoomOf` takes the `seen` flag as a
parameter, so step 12's signature change breaks step 13's call site in the same
compile. There is no revision between them that builds.

**The spinner does not use `linenoiseHide` / `linenoiseShow`.** Both take a
`struct linenoiseState*` and belong to linenoise's non-blocking API
(`linenoiseEditStart` / `Feed` / `Stop`). Step 16 uses the blocking `linenoise()`,
which owns its state internally and has already returned by the time `aiRender`
runs — there is no line being edited during the spinner, so there is nothing to
hide. The research's "line editing and the spinner are one job" was reasoning
about the async API. Erasure ended up as carriage return, one space, carriage
return.

**`tokenize` erased the first bar.** `band.cpp`'s tokenizer splits span text on
spaces and drops the pieces, so a span whose text is nothing but spaces produced
no tokens at all and vanished. REQ-POLISH-11's "no change to `layoutBand`'s width
arithmetic" was true of the arithmetic and silent about the tokenizer. Fixed by
keeping a space-only span as one atomic token.

## The spec contradiction

REQ-POLISH-12 spells the no-colour bar as `[####......]` — 12 columns. REQ-POLISH-10a
budgets 11. Under colour the two agree; without it they cannot. Raised to the
author rather than decided, with the three resolutions costed. The author kept the
budget and coarsened the degraded form to `[` + 8 cells + `]`. REQ-POLISH-10,
-10a and -12 were all amended in the spec with the reasoning attached.

Three other amendments were folded back as the plan required: REQ-POLISH-15 now
names the `meta.start_room` row; REQ-EXAMINE-7 in
`.lore/work/specs/examine-perception-verb.md` now puts the player's own room in
scope, and its "Out of scope" bullet that explicitly refused this is struck
through; and the spec's first open question (where the bar sits in a hostile row)
is settled in place. The second open question — an env switch for the title
screen — was not built, as instructed.

## Golden re-captures

The plan expected the two session literals to move three times, at steps 5, 9 and
12. They moved five times: steps 2, 3, 5, 9 and 12. Step 2 alone re-wrapped two
lines of `spells` output and one telegraph line at the suite's pinned width of 40,
which the plan had predicted would leave the goldens untouched. `testBandGoldens`'
seven inline literals moved once more at step 9.

Re-capture was scripted rather than hand-run: `recapture.py` rewrites the two
`R"GOLDEN(...)"` bodies from `TW_DUMP_GOLDEN=1`, `recapture_bands.py` rewrites the
seven inline `golden(...)` literals from `TW_DUMP_BANDS=1`. Both are in the
session scratchpad, not committed. The band script needed two fixes before it
worked: its label regex was `[a-z ]+`, which silently skipped `mid-telegraph`,
and its last block ran to end-of-output and swallowed the rest of the suite's
stdout into the literal.

## Trip-ups

**A healthy player's health bar rendered as ten plain spaces.** The bar first
took the HP number's own colour, which is `Color::None` while health is fine
(REQ-UI-16), and `bgColorize` with `Color::None` correctly emits nothing. Every
band golden is captured with `kBandPlain`, so no golden could see a colour-only
defect. Found by running an actual fight and reading the escape bytes. Healthy is
green now, low is yellow.

**The bar's separator cost a column too many.** The band's usual gap between
fields is two spaces, which made the bar cost 12 columns against REQ-POLISH-10a's
11 — the budget that had just been defended in the amendment. The preceding
span's pad is narrowed to one space for exactly that join. The first version of
the test for this passed either way; it was tightened to assert there is exactly
one space before the bar.

**Two edits were silently lost to permission-classifier timeouts.**
`claude-sonnet-5[1m] is temporarily unavailable (timed out)` came back on a Bash
call twice; both times the retry ran only the build, not the Python edit that had
been in the same command. The `appendHealthBar` refactor and `testTitleScreen`
both had to be re-applied after a grep showed they were not in the file. Verifying
the edit landed, rather than trusting the retry, is what caught both.

**The suite outgrew the 120-second Bash timeout** partway through, so
build-and-test runs moved to `run_in_background` with an `until grep` wait.

**The pty harness needed two cursor answers, not one.** linenoise sends `ESC[6n`
twice per prompt — once for the cursor column and once after `ESC[999C` to measure
the terminal width. Answering only the first hangs the program with a prompt on
screen and no turn ever processed, which reads exactly like the game being broken.

**The first analysis of the live spinner turn reported zero frames and was
wrong.** The pty capture was read in text mode, where Python's universal-newline
handling converts lone `\r` to `\n` — destroying precisely the carriage returns
the frames are made of. Re-read in binary it showed 21 frames, one erase, no
residue.

**`tests` could not include `linenoise.h`.** `twcore` links linenoise `PRIVATE`,
so CMake carries the objects through transitively but not the `PUBLIC` include
directory. Step 1's gate proved the symbols reached `tests`; the header did not.
`tests` now names linenoise explicitly.

**Neither `figlet` nor `toilet` is installed on this machine**, and the plan's
fallbacks (`brew install figlet`, an online generator) were not available. The
nine glyphs were hand-authored on a 5×5 grid and assembled by a script, which is
what kept the letterforms consistent. `#` and spaces only, natural width 53.

**A gated test was broken for eight steps without failing.** The live architect
walkability test (`TEXTWORLD_AI_LIVE_TEST=1`, so the default suite never runs it)
parsed its displayed-exit set out of `renderRoomOf`, which step 5 had emptied. It
was found by a compile error at step 12, not by a failure. Left alone it would
have failed the first time anyone ran it live.

**`testBandWrap` went vacuous rather than red.** It drove a repeat `look`, which
after REQ-POLISH-17 returns one short line, so its "more than four lines"
assertion still passed while testing nothing. Changed to drive a first arrival.

## Live spend

Step 20 was the only step that made live calls: three turns, as budgeted. Piped
(zero escape bytes, zero carriage returns), on a terminal (21 frames, one erase,
no residue), and with an invalid key. The invalid-key path was chosen from
`prof-invalid-key.log`, which records a ~162 ms failure — long enough for exactly
one frame at the 100 ms interval, which is what made the failure-path erase a real
test rather than a vacuous one. That turn cost nothing, being rejected.

## Context worth preserving

- **Band goldens are all `kBandPlain`.** Nothing in the golden set can catch a
  colour-only regression. The colour assertions live in `testBandColor`,
  `testErrorStyling` and `testBandBarsInRows` instead.
- **`meta.start_room` is derived from the seed's own `location` row**, so no seed
  or fixture names a number. `combat.hpp:22`'s `kDormitoryCell = 1` is still
  hardcoded as the respawn destination. They agree in all three seeds today; a
  fixture that starts the player elsewhere would break first sight on respawn.
- **`main.cpp:126`'s `AI narration off — template mode`** is 32 columns and
  unwrapped, so at `COLUMNS=20` it is the one line that exceeds the terminal, and
  its em-dash is the one non-ASCII byte in a startup capture. Both predate this
  spec. Spec check 19's "contains only ASCII" was scoped to the art for that
  reason.
- **`TurnResult` gained a defaulted third member** (`TurnPresentation`). 37
  mentions in `tests.cpp`, none constructing one, so the widening cost nothing —
  the plan's verification of that held exactly.
- **`Spinner` writes to stdout from a second thread** while the main thread is
  inside libcurl. Safe only because of where it is scoped: `runTurn` composes a
  string and returns it, and `main()` prints that string after the destructor has
  run. Moving the print earlier would introduce a real race.
- **The spinner's frame wait is a condition variable, not a sleep**, so a turn
  returning in microseconds is not delayed a frame interval by its own spinner.
  A spinner destroyed before its first frame writes nothing at all, not even the
  clear.
- **Validation baselines were compared by property, not by diff**
  (`.lore/work/validation/revalidate.py`): no escape bytes, same outcome
  sequence, same `events` rows, against a binary built from `859451d` in a
  throwaway worktree. Layout was the thing deliberately changed, so a diff would
  have proved nothing.
- **A final coverage sweep found two requirements cited only in commit
  messages** — REQ-POLISH-24 (implemented as an absence) and REQ-POLISH-33 (a
  cross-cutting property the suite cannot see, since it runs with styling
  suppressed). Both now have assertions. The sweep is one shell loop over
  `grep -oE 'REQ-POLISH-[0-9]+[a-b]?'` against `src/ tests/ seed/`.
