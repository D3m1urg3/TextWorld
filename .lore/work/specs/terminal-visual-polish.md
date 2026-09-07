---
title: Terminal visual polish
date: 2026-09-07
status: draft
tags: [ui, terminal, ansi-color, typography, linenoise, line-editing, spinner, ascii-art, readability]
modules: [term, band, render, loop, main]
related: [.lore/work/brainstorm/terminal-visual-polish.md, .lore/work/research/terminal-visual-polish-implementation.md, .lore/work/specs/terminal-status-band-ui.md, .lore/work/specs/blocked-directions-exit-display.md, .lore/work/specs/ai-prose-renderer.md]
req-prefix: POLISH
---

# Terminal visual polish

The game reads as a debug console. Prose, engine statements, refusals, and the
status band are all the same colour at the same column, wrapped to whatever
width the terminal happens to be, with no blank line anywhere and `Exits:`
printed twice. This spec fixes what is on screen without adding images, without
a second frontend, and without changing a combat number or a turn cost.

`REQ-UI-` is taken through 48 by
[terminal-status-band-ui](.lore/work/specs/terminal-status-band-ui.md), so
requirements here are `REQ-POLISH-N`.

## Scope

In: prose width, indentation, blank lines, the duplicated exits and items lines,
error styling, health bars, the repeated room paragraph, line editing with
history, a spinner during blocking calls, and a title screen.

Out: room images and any second image vendor; streaming narration; a browser or
native window; per-room ASCII art; the map; collapsing the band when unchanged;
showing what the resolver decided. All are recorded in
[the brainstorm](.lore/work/brainstorm/terminal-visual-polish.md) as parked, not
rejected.

## Existing requirements this changes

**REQ-UI-30** wraps prose to the *detected* width. REQ-POLISH-1 narrows that to a
capped width. REQ-UI-31 (count characters, not bytes) and REQ-UI-32 (preserve
paragraph breaks) are unchanged and still govern.

**REQ-EXITS-4** names `render.cpp`'s `roomBlock` as the home of the `Exits:`
line. REQ-POLISH-5 moves that duty to the band alone. The invariant REQ-EXITS-4
protects — all realized exits always, latent exits only when
`architectEnabled()`, no marker distinguishing them — is unchanged, and
`band.cpp`'s `exitsRow` already implements it with a byte-identical query under
REQ-UI-10.

**REQ-PROSE-14** requires exits, visible items, and inventory to be emitted
deterministically by the engine after the AI prose. Still true: the band is
engine-composed and appended after the prose. Inventory is untouched — the band
has no inventory row and `inventory` output is unchanged.

## Requirements

### Prose layout

**REQ-POLISH-1** — Narration and template prose wrap to
`max(kMinWidth - kProseIndent, min(detectWidth(), 66) - kProseIndent)`. 66 is the
midpoint of the 45-75 range from Bringhurst and from Tinker and Paterson's
eye-movement studies; past ~80 the eye misses the start of the next line. The
indent is **subtracted from** the wrap width, never added to the result, so at
REQ-UI-27's 20-column floor no line exceeds 20 columns. This is the only
statement of the wrap width in this spec.

**REQ-POLISH-2** — The status band keeps the full detected width. It is a table,
not prose. REQ-UI-33's wrapping and hanging indent are untouched.

**REQ-POLISH-3** — Every line of the turn's narration is indented by
`kProseIndent` = 2 spaces. The band stays at column 0. This is what separates
"the world talking" from "the engine reporting". It is applied at the single
composition site in `runTurn` (`loop.cpp:193-194`), before the band is appended,
and at the matching site in `renderStartup` (`loop.cpp:200`).

**REQ-POLISH-3a** — Indentation never produces a line of only whitespace. A blank
line inside prose, and the trailing newline `wrapProse` preserves, stay empty.

**REQ-POLISH-3b** — `renderSpellRules` output (the `spells` command) is **not**
indented. It is a reference table like the band, not narration, and it reads at
column 0 with it.

**REQ-POLISH-4** — One blank line precedes each prompt, so turns are visually
separated. It is emitted with the prompt in `main.cpp`, not appended to turn
output, so a piped run gains no trailing blank line per turn.

### The duplicated lines

**REQ-POLISH-5** — `roomBlock` (`render.cpp:60`) stops emitting the `Exits:` and
`You see:` lines. Both are printed again by the band one or two lines below, from
byte-identical queries. `roomBlock` becomes the room's prose and nothing else.
This applies to all three of its callers — `look`/`moved`, `examine` on a room,
and `renderStartup`.

**REQ-POLISH-6** — Because exits now reach the player only through the band, a
band failure must not silently take the exits with it. `bandOrEmpty`
(`loop.cpp:52`) currently swallows any exception and returns `""`. On that path
the engine emits a plain-text exits line as a last resort, and writes a `warn`
entry to the session log, which today it does not.

**REQ-POLISH-6a** — If the fallback exits query itself throws, the turn still
prints its narration and the game continues. Nothing in this spec may turn a
display failure into a failed turn.

### Error styling

**REQ-POLISH-7** — Text from `renderError` is dimmed (`BrightBlack`, the colour
`band.cpp:28` already uses for a spell that has receded out of reach). A refusal
is the game speaking, not the world, and it should not read with the same weight
as prose. It is styled through the existing `TermStyle` gates, so it degrades to
plain text under `NO_COLOR`, under `TERM=dumb`, and in a pipe.

**REQ-POLISH-7a** — Error text is still indented per REQ-POLISH-3. It occupies
the narration column because it is the answer to what the player typed.

### Bars

**REQ-POLISH-8** — The player's health and each hostile's health render as a bar
**alongside** the existing numbers, never replacing them. The band's job is to
carry numbers the player can trust over the prose; a bar alone cannot distinguish
3 HP from 4.

**REQ-POLISH-8a** — Spell cooldowns get **no bar**. A cooldown of 2 out of 2 is a
two-column bar, which reads as noise rather than information. Cooldowns stay as
they are.

**REQ-POLISH-9** — A bar is a run of **space characters** carrying a background
colour. It is never drawn with block characters. `█` (U+2588) and `░` (U+2591)
are East Asian Ambiguous width and would let the terminal decide the column
count — the defect REQ-UI-29 already rules out for `─` and `·`.

**REQ-POLISH-10** — A health bar is exactly 10 columns wide plus one space of
separation, regardless of the value shown **and regardless of which of
REQ-POLISH-12's two forms is drawn**, so a row's width never changes as health
drops or as colour is turned off. Filled cells are `round(cells * current / max)`
where `cells` is 10 under colour and 8 without it (REQ-POLISH-12 spends the other
two columns on its delimiters), with a floor of 1 filled cell while
`current > 0` — a living enemy never shows an empty bar.

*Amended during implementation, 2026-09-07.* As first written this said
`round(10 * current / max)` unconditionally, which contradicted REQ-POLISH-10a
once REQ-POLISH-12's `[####......]` is counted: brackets plus ten cells is 12
columns, and 13 with the separator. The budget was kept and the resolution of
the degraded form was coarsened instead.

**REQ-POLISH-10a** — A bar adds at most 11 columns to a row, **in both of
REQ-POLISH-12's forms**. Hostile rows already carry a name, health,
`[WINDING UP]`, and discovered resistances; the growth must be bounded so
REQ-UI-33's wrapping stays the exception rather than the rule, and a degraded
row is not the place to relax that.

**REQ-POLISH-11** — A bar lives in a `BandSpan` whose `text` is the spaces and
whose styling is applied at emission, like every other span. No change to
`layoutBand`'s width arithmetic: `stripSgr` removes the escape bytes and
`utf8Length` counts the spaces. Verified — `\x1b[41m      \x1b[0m` measures 6.

**REQ-POLISH-12** — Bars degrade in **two** steps: background colour when colour
is on, ASCII fill characters (`#` filled, `.` empty, inside `[` `]`) when it is
not. The bracketed form is `[` + **8** cells + `]`, which is the same 10 columns
the coloured form occupies — the two delimiters are paid for out of the bar's
own width rather than added to it, so REQ-POLISH-10a's budget holds in both
modes. There is no reverse-video step. Reverse video swaps foreground and
background and is therefore a colour effect; emitting it under `NO_COLOR` would
stretch REQ-UI-23 past what it says, and `TermStyle.attrs` was defined for bold.

**REQ-POLISH-13** — `term.hpp` gains background-colour emission beside
`colorize` / `bolden` / `boldColor`. It uses the basic 16 named colours only
(REQ-UI-19), so a bar resolves through the user's terminal theme and stays
readable on a light background. No 256-colour, no truecolor.

### The repeated room paragraph

**REQ-POLISH-14** — The room's description paragraph prints on **first sight** of
that room and on **explicit request**, not on every `look`. First sight is
arrival: a `moved` event whose destination has no earlier `moved` event naming
it. Explicit request is `examine <room>` / `x room`, which already routes to
`roomBlock` (`render.cpp:118`) and is already the verb for looking closely at one
thing.

**REQ-POLISH-15** — "Has this room been seen" is **derived from the `events`
transcript**, not stored in a new column or table: room R is unseen at turn T if
no event with `verb='moved'` and `object=R` exists at a turn earlier than T. This
follows REQ-UI-46's precedent, where discovered resistances are computed from the
event log with no cache and no shadow table, so the fact cannot drift from the
transcript or be lost across a restart.

**REQ-POLISH-16** — `renderStartup` (`loop.cpp:198`) prints the description
paragraph **only when the `events` table is empty** — that is, only at world
creation, which is the one launch where the player has never seen the room. Every
later launch prints the room name and the band. This closes the hole REQ-POLISH-15
would otherwise leave: the starting room has no `moved` event naming it and would
read as unseen forever.

**REQ-POLISH-17** — A `look` in a room already seen prints the room name. Exits,
objects, and hostiles come from the band, as they do on every other turn.

**REQ-POLISH-18** — **Accepted consequence, stated rather than hidden:** `look`
costs a turn, and after REQ-POLISH-17 a repeat `look` mid-fight costs a turn,
takes chip damage, and returns only the room name. This is the one place a
change in this spec is felt as gameplay. It is accepted because the band already
prints the room name, exits, objects, and hostile state every single turn, so a
repeat `look` was close to redundant before this change too. `look` is **not**
made free: REQ-UI-39b states that the no-tick exception must not generalize, and
this is exactly the kind of appeal it forbids.

**REQ-POLISH-19** — No new verb and no two-word command form. `examine <room>` is
the full reread. The parser has no two-word verbs today and this spec does not
add the first one.

### Line editing

**REQ-POLISH-20** — Input gains history and in-line editing. `linenoise`
(upstream `antirez/linenoise`, BSD-2, one `.c` file, ~1600 lines) is vendored
beside `vendor/sqlite3.c`. Not `readline` — GPL. Not `linenoise-ng` or `replxx` —
those forks exist to add UTF-8 to a version that lacked it, and upstream has
UTF-8 now.

**REQ-POLISH-21** — History persists across sessions in a file beside `world.db`,
loaded at startup, with a bounded maximum length. Same placement rule as `logs/`.
The file is added to `.gitignore`.

**REQ-POLISH-21a** — History is saved on **every** exit path: the `quit` verb,
EOF, and the fatal-error path. A session that ends badly does not cost the player
their history.

**REQ-POLISH-21b** — A history file that cannot be read or written is not an
error. The game plays normally with in-session history only, and records the
failure in the session log. This matches the best-effort posture logging already
has.

**REQ-POLISH-22** — Behaviour with input that is not a terminal is unchanged.
Piping a script into the game — which is how the captures under
`.lore/work/validation/` were produced — still works, with no editing, no escape
bytes, and no length limit.

**REQ-POLISH-23** — EOF still behaves as quit, as it does at `main.cpp:155`.

**REQ-POLISH-24** — No tab completion. Completing nouns present in the room would
tell the player what is there before they look.

### Spinner

**REQ-POLISH-25** — While a turn is blocked on a network call, the terminal shows
that the game is working. A turn is ~3.4 s today and ~7.3 s when a room is
generated, and the terminal is currently frozen and silent for all of it.

**REQ-POLISH-26** — The spinner is gated on `currentStyle().attrs` — already
false for a pipe and for `TERM=dumb`. No escape byte reaches a non-terminal, so
the golden-output comparisons in `tests/tests.cpp` are unaffected.

**REQ-POLISH-27** — The spinner erases itself completely before any turn output
is written. No residue on any line, including when the call fails or times out.

**REQ-POLISH-28** — The spinner never appears in template mode, where there is no
network call and nothing to wait for.

### Title screen

**REQ-POLISH-29** — The game prints a title screen at every launch, as the
**first** thing on screen — before `main.cpp:126`'s template-mode notice and
before the first room. It is static text stored with the seed data, rendered once
while authoring; no FIGlet renderer, no font files, no runtime dependency.

**REQ-POLISH-30** — The title art is plain ASCII only (REQ-UI-29). No box
drawing, no ambiguous-width characters, so it cannot misalign on any terminal.

**REQ-POLISH-31** — The title screen respects the width floor: it must not
overflow at REQ-UI-27's 20 columns. Below the art's natural width it degrades to
the game's name as plain text rather than wrapping into rubble.

### Cross-cutting

**REQ-POLISH-32** — No combat number, no turn cost, no schema change, no new
event verb. The one gameplay consequence in this spec is REQ-POLISH-18, which is
stated rather than hidden. The `events` table stays the source of every
player-visible line except the two bounded exceptions already granted (REQ-UI-39b's
spell list, and the startup courtesy render).

**REQ-POLISH-33** — Every new byte of styling passes through `TermStyle`. A run
whose stdout is not a terminal emits no escape sequence of any kind, as property
check 7 in the status-band suite already asserts against the real binary.

## AI Validation

Each item is a command to run and an observation to make.

1. **Prose width.** `COLUMNS=200 TEXTWORLD_AI=0 ./build/textworld < script` and
   assert no narration line exceeds 66 columns after `stripSgr`. Repeat at
   `COLUMNS=20` and assert no line exceeds 20 — the case a naive indent breaks.
2. **Band width unchanged.** In the same capture, assert band rows still use the
   full width, so the cap did not leak into `composeBand`.
3. **Indent.** Assert narration lines begin with exactly two spaces, that band
   rows and `spells` output do not (REQ-POLISH-3b), and that no line consists of
   only whitespace (REQ-POLISH-3a).
4. **Blank line.** Assert a blank line precedes each `>` prompt in an interactive
   capture, and that a piped capture gains no trailing blank line per turn.
5. **No duplicate exits.** Assert `Exits:` appears zero times in turn output and
   the band's `Exits` row appears once per turn. Same for `You see:` against the
   band's `Objects` row. Check all three `roomBlock` callers, not just `look`.
6. **Latent-exit gate survives.** Run the same script with the architect enabled
   and disabled; assert the band's exits differ exactly as
   `blocked-directions-exit-display`'s existing check expects. This is the
   regression REQ-POLISH-5 could plausibly cause.
7. **Band failure keeps exits.** Force `composeBand` to throw (a fixture with no
   location row) and assert the fallback exits line prints and a `warn` entry
   reaches the session log. Then force the fallback query to throw too and assert
   the turn still prints its narration (REQ-POLISH-6a).
8. **Error styling.** Capture an unparseable line three ways — colour on,
   `NO_COLOR=1`, `TERM=dumb` — and assert the text is dim in the first, identical
   plain text in the other two, and indented two spaces in all three.
9. **Bar widths.** Assert every band row has an identical column count at full
   health and at 1 HP after `stripSgr`, that a living enemy at 1/10 shows one
   filled column and not zero, and that no bar byte is outside ASCII.
10. **Bar degradation.** Capture the same fight state with colour on and with
    `NO_COLOR=1`; assert health is readable in both, that the second contains
    `[` and `#`, and that no capture contains `\x1b[7m`.
11. **First sight.** From a fresh `world.db`: assert the paragraph prints at
    creation; `look` and assert it does not print again; move away and back and
    assert it does not print on return; `x room` and assert it does.
12. **Restart.** Relaunch against the same `world.db` and assert the startup
    render prints the room name without the paragraph — the REQ-POLISH-16 case.
13. **Downed.** Get downed, wake in the dormitory cell, and assert no paragraph
    prints. Respawn writes `downed`, not `moved` (`mutations.cpp:429`), so this
    is the case most likely to expose a wrong derivation.
14. **Line editing is invisible to pipes.** Re-run the scripts under
    `.lore/work/validation/` and assert three properties of the new output rather
    than diffing against the old: no escape bytes, the same sequence of turn
    outcomes, and the same set of event rows written. Then commit the new
    captures as the baseline.
15. **EOF.** Pipe a script with no `quit`; assert clean exit and that history was
    saved (REQ-POLISH-21a).
16. **History file.** Assert it is created beside `world.db`, that a second
    session loads it, that it is gitignored, and that making the path unwritable
    still lets the game run and logs the failure.
17. **Spinner leaves nothing behind.** With AI on, capture a turn and assert no
    spinner frame character survives in the output; with output piped, assert no
    escape bytes at all.
18. **Spinner absent in template mode.** `TEXTWORLD_AI=0`, assert no spinner
    thread starts and no frame is written.
19. **Title screen.** Assert it prints first, before the template-mode notice,
    contains only ASCII, and at `COLUMNS=20` degrades to the plain name without
    overflowing.
20. **No rule changed.** Run the full existing suite. Every failure must be a
    test asserting on layout this spec deliberately changes. A combat,
    turn-count, or schema test that needs editing is a REQ-POLISH-32 failure and
    must be raised, not edited.

## Open questions

- **Where the bar sits in the row.** Before or after the numbers changes how the
  eye scans a hostile row that also carries `[WINDING UP]`. Worth deciding by
  looking at a real fight rather than in the spec.
- **Whether the title screen should be suppressible.** REQ-POLISH-29 prints it
  every launch. If it grows past a few lines, an env switch in the shape of
  `TEXTWORLD_AI` may be wanted. Not built until it is.
