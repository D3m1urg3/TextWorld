---
title: "Terminal UI approaches: color gating, width detection, and pinned status prior art"
date: 2026-08-02
status: active
tags: [ui, terminal, ansi-color, no-color, tiocgwinsz, wcwidth, glk, scrollback, prior-art]
modules: [render, loop, main]
related: [.lore/work/brainstorm/ui-improvements.md]
---

# Terminal UI approaches: color gating, width detection, and pinned status prior art

Research backing the status-band direction chosen in
[ui-improvements](.lore/work/brainstorm/ui-improvements.md). Four questions: how
to gate color, whether to take a dependency, how to learn the terminal width, and
what prior art says about showing status permanently without losing scrollback.

## Key Findings

1. **`NO_COLOR` alone is not the full convention.** The complete de-facto logic is
   a four-step precedence chain including `CLICOLOR_FORCE` and `CLICOLOR`. Notably
   `CLICOLOR_FORCE` is what lets someone pipe our output into `less -R` and keep
   color — without it, an isatty gate makes that impossible.
2. **`NO_COLOR` governs *color only*, not bold/underline.** The spec is explicit.
   So the telegraph can keep its **bold** under `NO_COLOR` and stay visually
   distinct — the design survives a colorless terminal rather than going flat.
3. **Take no dependency.** Hand-rolled SGR is ~30 lines. Every candidate library
   is either trivial to inline (termcolor, rang) or forces the full-screen model
   we already rejected (ftxui, ncurses, notcurses).
4. **`std::format` works** on this toolchain at C++20 — verified by compiling.
   No `fmt` needed to compose the band.
5. **`TIOCGWINSZ` fails in exactly the contexts we care about** — verified
   locally: `rc=-1, cols=0` when there is no controlling tty. A fallback width is
   mandatory, not defensive padding.
6. **Box-drawing characters are *not* width-safe** — this corrects the brainstorm.
   `─` (U+2500) and `·` (U+00B7, already used in the current status line) are both
   East Asian **Ambiguous** width. Only ASCII is unconditionally single-column.
7. **Glk is the strongest prior art and it validates the split we chose:** text
   *grid* (fixed-width, addressable — status) vs text *buffer* (flowing, wrapped —
   prose). Plotkin generalized this from Infocom, and it is the same seam as our
   engine-band / model-prose division.
8. **A fourth layout option exists that the brainstorm missed:** DECSTBM scroll
   regions pin a header/footer *without* the alternate screen. It is how fzf's
   `--height` mode keeps scrollback. But it carries real cross-terminal
   compatibility risk — see below.
9. **Putting the band in `render()` would break the test suite**; putting it in
   the loop would not. Empirical support for the architectural call already made.

## 1. Color gating

### The precedence chain

Check in this exact order ([bixense](http://bixense.com/clicolors/),
[no-color.org](https://no-color.org/)):

| Step | Condition | Result |
|---|---|---|
| 1 | `NO_COLOR` set and **non-empty** | never emit color |
| 2 | `CLICOLOR_FORCE` set and non-`0` | always emit color, tty or not |
| 3 | `CLICOLOR` set and non-`0` | emit color **iff** `isatty()` |
| 4 | none set | implementation's choice — new programs may default to as-if `CLICOLOR` |

Empty variables are treated as unset throughout. The value of `NO_COLOR` is
irrelevant — only presence and non-emptiness.

`CLICOLOR_FORCE` matters more than it looks. A pure isatty gate means
`textworld | less -R` and `script`-based transcript capture both lose all color.
`CLICOLOR_FORCE` is the documented escape hatch, and it is three lines.

### The bold nuance

no-color.org: *"This standard applies exclusively to ANSI color output. Other
styling attributes like bold, underline, and italic remain unaffected."*

This is genuinely useful for us. The telegraph — the one-turn decision window —
was going to be bold + bright red. Under `NO_COLOR` it keeps the bold and stays
the loudest thing on screen. The design degrades rather than collapses.

Worth deciding deliberately: do we honor `NO_COLOR` for *attributes* too (safer,
flatter) or follow the spec strictly (color off, bold retained)? The spec says the
latter and it serves the design better.

### Prior art on defaults

LLVM added `NO_COLOR` support in [D152285](https://reviews.llvm.org/D152285);
CMake documents [`CLICOLOR`](https://cmake.org/cmake/help/latest/envvar/CLICOLOR.html).
The convention is well-settled — no need to invent flags.

## 2. Dependency or hand-roll

| Option | Verdict |
|---|---|
| **Hand-rolled SGR** | ~30 lines: a `Style` enum, a gate function, a wrap helper. **Recommended.** |
| [termcolor](https://github.com/ikalnytskyi/termcolor) | Header-only, iostream manipulators, BSD-3. Uses Win32 API on Windows. Small enough that inlining what we need is cheaper than vendoring. |
| [rang](https://agauniyal.github.io/rang/) | Header-only, handles tty detection internally. Same conclusion. |
| ftxui / ncurses / notcurses | Full-screen widget/window models. These **are** the pinned-split approach we rejected for scrollback reasons. Adopting one re-opens a closed decision. |

The project vendors amalgamations (sqlite3, nlohmann/json) and links only system
libcurl. A new third-party header for what is a handful of escape strings runs
against that grain.

**Verified locally:** `std::format` compiles and runs under `-std=c++20` on this
machine's Apple Clang, so band composition needs no formatting dependency either.

## 3. Terminal width

### Method and fallback chain

Standard approach, in order:

1. `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` — the portable-enough Unix answer.
   Query the fd you actually **write** to, not stdin.
2. `getenv("COLUMNS")` — less portable, and stale unless the shell exports it.
3. Hard default — 80 is the conventional floor.

### The failure is not hypothetical

Compiled and ran the probe in this session:

```
std::format OK; ioctl rc=-1 cols=0 rows=0
```

`rc=-1`, `cols=0`. Any code that trusts `ws.ws_col` without checking `rc` will
compute a **zero-width** band and emit either nothing or a division-by-zero in the
wrapper. The tool-run environment reproduces exactly what CI and piped output do.
Clamp: `if (rc != 0 || ws.ws_col == 0) width = 80;` and probably also clamp a
sane minimum (say 40) so the band degrades instead of shredding.

### SIGWINCH — probably not needed here

The usual companion is catching `SIGWINCH` to redraw on resize. **Our design does
not need it.** The band is reprinted each turn, and the world only ticks on enter,
so re-querying the width at print time is sufficient and there is nothing on
screen to redraw. This is a genuine simplification the printed-band model buys —
a pinned layout *would* require the signal handler.

## 4. The character-width trap

This corrects the brainstorm's "box drawing is safe, drop the emoji."

Per [Markus Kuhn's wcwidth reference](https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c)
and Unicode's East Asian Width classes: characters fall in **F**ullwidth,
**W**ide, **H**alfwidth, **Na**rrow, and **A**mbiguous. For the Ambiguous class
"the width choice depends purely on a preference of backward compatibility with
either historic CJK or Western practice" — meaning **the terminal decides**, and
CJK-configured terminals render them double-width.

Both characters the sketch relies on are Ambiguous:

- `─` U+2500 BOX DRAWINGS LIGHT HORIZONTAL — **Ambiguous**
- `·` U+00B7 MIDDLE DOT — **Ambiguous** (and already shipping in
  `combatStatusLine()`)

So the emoji advice holds and strengthens, but "box-drawing is safe" was wrong.
Three honest options:

1. **Pure ASCII rules** (`--`, `|`, `-`) — unconditionally correct, plainer look.
2. **Accept the risk** — misalignment only in CJK-width terminals; the existing
   `·` already takes this bet.
3. **Vendor a `wcwidth`** and measure properly — correct, but it is real code for
   a game with no CJK content, and it only fixes *our* rules, not the model's
   prose.

Given the band is decorative framing around load-bearing text, option 1 or 2.
Option 3 is disproportionate.

Separately: the **prose wrapper** must count characters, not bytes. The narration
is UTF-8 and the model will emit em-dashes and curly quotes. Naive `size_t`
wrapping on `std::string` will wrap early and raggedly on those lines.

## 5. Layout prior art

### Glk — the model that matches ours

Andrew Plotkin's [Glk history essay](https://eblong.com/zarf/essays/glk-history/)
describes the design directly: windows are each either a **text grid** ("an
addressable grid of fixed-width characters") or a **text stream** (flowing,
wrapped). He took this from Infocom, whose status line "was just a terminal
window," and generalized it. Windows cannot overlap.

The rationale is the useful part: the abstraction had to "run on everything from
GUI systems down to a raw telnet stream," covering "the least common capability
set."

This is *our* seam. Engine-authored structured status = text grid. Model narration
= text stream. The [Z-Machine Standards
Document §8](https://www.inform-fiction.org/zmachine/standards/z1point0/sect08.html)
is the formal treatment. That the canonical IF interface standard independently
lands on the same split is decent evidence the division is real and not an
artifact of our architecture.

### MUDs — the printed-prompt precedent

MUDs solved exactly our problem decades ago and mostly chose the **printed
prompt**: `<100hp 50m 30mv>` emitted after each command's output, immediately
above where you type. Scrollback fully intact; status always adjacent to the
cursor; the cost is repetition down the buffer. Some MUDs send `HP:Healthy
MV:Fresh` style qualitative prompts.

Clients that wanted more went two ways: ncurses split windows (mcl, smudge — and
mcl's fixed-size windows "created issues in tiling window managers"), or
scroll-region pinning (Blightmud's split mode; clients that pin a map above
scrolling text "using standard VT100 cursor positioning").

Our chosen approach is the MUD prompt, widened. That is a well-trodden path.

### DECSTBM — the option the brainstorm missed

[DECSTBM](https://ghostty.org/docs/vt/csi/decstbm) (`CSI top ; bottom r`) sets a
scrolling region. Text scrolls only within it; rows outside stay put. This pins a
header/footer **without** the alternate screen, so scrollback survives — the
mechanism behind fzf's `--height` mode, which "opens fzf below the cursor in
non-fullscreen mode so you can still see the previous commands."

Tempting. But the compatibility record is poor:

- Terminals disagree on whether DECSTBM constrains *cursor movement* or only
  scrolling — [termux #1340](https://github.com/termux/termux-app/issues/1340)
  documents getting this wrong; the spec says margins should limit scrolling only.
- On some terminals content scrolled out of a region is **lost rather than pushed
  to scrollback**, which would silently defeat the entire reason for choosing it.
- Cleanup is fiddly: `CSI r` resets the region *and* moves the cursor to home;
  leaving a region set on exit corrupts the user's shell
  ([sbbs #1161](https://gitlab.synchro.net/main/sbbs/-/issues/1161) is exactly
  this bug). Needs a guard object covering every exit path, including signals.
- xterm has a documented history of save/restore-cursor bugs interacting with
  regions.

**Assessment:** DECSTBM is the only way to get *true* pinning with scrollback, and
it is worth knowing it exists. But it buys us little — per the brainstorm, a
turn-based game's printed band is already always-current, so pinning only saves
scrolling we never do. Paying that compatibility tax for an ergonomic non-problem
is a bad trade. **Recommend: stay with the printed band; keep DECSTBM documented
as the escape hatch if the repetition-in-scrollback cost ever proves intolerable.**

### Alternate screen — confirmed cost

The scrollback loss is well documented as a UX complaint, including against
Claude Code itself
([#42670](https://github.com/anthropics/claude-code/issues/42670)) and Codex
([#10331](https://github.com/openai/codex/issues/10331)). The brainstorm's
reasoning holds.

## 6. Impact on this codebase

Checked `tests/tests.cpp` while researching, and it sharpens the architectural
decision from the brainstorm:

- Tests assert **exact string equality** on `render()` output — e.g.
  `tests.cpp:1700`: `CHECK(render(db, turn) == "You take the lantern.\n");`
- `tests.cpp:1506-1512` specifically asserts the byte-identity of the
  `combatStatusLine()` append across paths — the hand-kept promise the brainstorm
  flagged.

So: a band composed **inside `render()` breaks a large number of exact-match
assertions**. A band composed in `runTurn()` and appended to whichever text came
back leaves every `render()` test untouched, and makes the byte-identity test
obsolete in the good way (there stops being a second append to keep in step). The
brainstorm reached this from design symmetry; the test suite independently agrees.

Also confirms the isatty gate's value: with color gated on a tty, the test binary
never sees an escape code, so no golden string needs updating for color at all.

## Open questions this research does not settle

- **ASCII rules or accept ambiguous-width?** A style call, not a technical one.
- **Does `NO_COLOR` suppress bold too, or only color?** Spec says color only;
  design is better served by the spec. Worth an explicit decision rather than an
  accident.
- **Minimum viable width.** What does the band do at 40 columns? At 20? Truncate
  exits, wrap the row, or drop rules entirely?
- Nothing here addresses the two open questions carried from the brainstorm — HP
  visibility out of combat, and where spell/resistance legibility lives. Those are
  design calls, not research ones.

## Sources

- [no-color.org](https://no-color.org/) — the NO_COLOR spec
- [Standard for ANSI Colors in Terminals (bixense)](http://bixense.com/clicolors/) — CLICOLOR/CLICOLOR_FORCE precedence
- [LLVM D152285](https://reviews.llvm.org/D152285) — NO_COLOR adoption in a major toolchain
- [CMake CLICOLOR docs](https://cmake.org/cmake/help/latest/envvar/CLICOLOR.html)
- [termcolor](https://github.com/ikalnytskyi/termcolor), [rang](https://agauniyal.github.io/rang/) — header-only C++ color libraries
- [Markus Kuhn, wcwidth.c](https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c) — East Asian width classes and cell counting
- [Glk! A universal user interface! (Plotkin)](https://eblong.com/zarf/essays/glk-history/) — text grid vs text buffer
- [Z-Machine Standards Document §8](https://www.inform-fiction.org/zmachine/standards/z1point0/sect08.html) — status line specification
- [Ghostty VT docs: DECSTBM](https://ghostty.org/docs/vt/csi/decstbm) — scroll region semantics
- [termux #1340](https://github.com/termux/termux-app/issues/1340) — DECSTBM cursor-constraint divergence
- [sbbs #1161](https://gitlab.synchro.net/main/sbbs/-/issues/1161) — unreset scroll region corrupting the shell
- [fzf layout docs](https://deepwiki.com/junegunn/fzf/5.3-layout-and-display-configuration), [fzf --height issue #782](https://github.com/junegunn/fzf/issues/782) — non-alt-screen constrained rendering
- [claude-code #42670](https://github.com/anthropics/claude-code/issues/42670), [codex #10331](https://github.com/openai/codex/issues/10331) — alternate-screen scrollback loss as a real UX regression
- [MUSHclient features](https://gammon.com.au/scripts/doc.php?general=features), [Mudlet status prompt thread](https://forums.mudlet.org/viewtopic.php?t=2047) — MUD status-prompt conventions
