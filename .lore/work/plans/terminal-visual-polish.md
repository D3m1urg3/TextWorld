---
title: "Implementation plan: terminal visual polish"
date: 2026-09-07
status: executed
tags: [plan, ui, terminal, ansi-color, typography, linenoise, line-editing, spinner, ascii-art, readability]
modules: [term, band, render, loop, main, world, systems]
related: [.lore/work/specs/terminal-visual-polish.md, .lore/work/research/terminal-visual-polish-implementation.md, .lore/work/brainstorm/terminal-visual-polish.md, .lore/work/specs/terminal-status-band-ui.md, .lore/work/specs/blocked-directions-exit-display.md]
---

# Implementation plan: terminal visual polish

Fixes what is on screen: prose width, indentation, the duplicated `Exits:` and
`You see:` lines, dimmed refusals, health bars, the room paragraph that reprints
on every `look`, line editing with history, a spinner during the blocking call,
and a title screen. Source of truth:
**[.lore/work/specs/terminal-visual-polish.md]** (33 requirements, prefix
`POLISH`). No combat number, no turn cost, no DDL, no new event verb.

Twenty-one steps. Steps 1-18 are deterministic and verified offline; step 19 is
the spinner's concurrency half, step 20 is the one step that spends live-LLM
tokens, and step 21 walks the spec's twenty validation checks.

## Guiding constraints

- **Deterministic work first, tokens last.** Everything through step 18 runs
  under `TEXTWORLD_AI=0` with no network. Step 20 is the only step that makes a
  live call, and its assertions are mechanical so it cannot become a tuning loop.
- **One file, one behaviour, one commit per step** where that is possible.
  Twenty-one small steps rather than eight large ones.
- **Reuse what the band already proved.** `BandSpan` keeps plain text separate
  from styling and `layoutBand` measures `text` only, so a bar needs no
  arithmetic change. `TermStyle` already gates every escape byte. The derivation
  in step 11 copies `discoveredResistances`' posture: compute from the events
  transcript, no cache, no shadow table.
- **Marks.** Most steps carry none. Step 19 is marked MED (a thread writing to
  stdout while another thread writes turn output). Step 20 is marked HIGH (live
  tokens). The spinner is split into those two precisely so the token-spending
  half is small and last.

## Two spec statements that do not match the tree

Both were checked against the code and resolved with the author before this plan
was written. Both are amendments to shipped requirements and should be folded
back into the specs.

**1. The starting room is never "seen."** REQ-POLISH-15 derives "have I been
here" from `moved` events. The dormitory cell never gets one: `seed/base.sql:37`
places the player there and `initialize()` (`world.cpp:272`) writes no events at
all. Under a literal reading the cell is unseen forever, and the spec's own
checks 11 and 13 both fail. **Resolved:** `initialize()` writes a
`meta.start_room` row and that room counts as seen from turn zero. A row, not a
shape — the same zero-DDL move `meta.setting` makes at `world.cpp:288` and the
story arc makes at `seed/base.sql:171`, so REQ-POLISH-32's "no schema change"
holds and `SCHEMA_VERSION` stays at 8. It is a creation-time constant rather
than a cache of something that changes, so it cannot drift from the transcript,
which is what REQ-POLISH-15's "not stored" clause is protecting. **REQ-POLISH-15
needs amending to name the row.**

**2. `examine <room>` does not work today.** REQ-POLISH-14 calls it the full
reread and says it "already routes to `roomBlock` (`render.cpp:118`)". Neither
half holds. The `examined` branch is at `render.cpp:131` and prints the
`description` row directly — harmless, because after step 5 that is byte-identical
to what `roomBlock` emits — but `resolveExamine` (`systems.cpp:187`) refuses a
room outright: scope is "has a `location` row whose container is the player's
room, or is the player", and rooms have no `location` row, so `containerOf`
returns `nullopt` and `x cell` answers "You don't see that here." **Resolved:**
step 14 widens the scope so the player's own room is examinable. That amends
**REQ-EXAMINE-7** in `.lore/work/specs/`, and adds `src/systems.cpp` to the file
list — neither was in the brief.

## Where this lands in the tree

| What | File:line | What changes |
|------|-----------|--------------|
| prose wrap call | `loop.cpp:194`, `:201` | `detectWidth()` → `proseWidth(detectWidth())` — steps 2, 3 |
| wrap + indent helpers | `term.hpp` / `term.cpp` | `kProseMaxWidth`, `kProseIndent`, `proseWidth`, `indentProse` — steps 2, 3 |
| background colour | `term.hpp` / `term.cpp:98` | one function beside `colorize`/`bolden`/`boldColor` — step 7 |
| the duplicated lines | `render.cpp:81`, `:86` | deleted; `roomBlock` becomes prose and nothing else — step 5 |
| band-failure fallback | `loop.cpp:52` `bandOrEmpty` | plain exits line + a `warn` log entry — step 5 |
| bars | `band.cpp:337` `playerRow`, `:283` `hostileRows` | a `BandSpan` of spaces per REQ-POLISH-11 — steps 8, 9 |
| first sight | `render.cpp:118`, `:125`, `:225`, `:244` | four `roomBlock` call sites, not three — steps 11-13 |
| the starting room | `world.cpp:288` | one more `meta` row inside the same transaction — step 10 |
| examine scope | `systems.cpp:187` | the player's own room becomes in scope — step 14 |
| input | `main.cpp:153` `std::getline` | linenoise on a terminal, `getline` in a pipe — step 16 |
| prompt | `main.cpp:140` | one blank line before it — step 4 |
| title screen | `main.cpp:126`, `seed/base.sql` | a `meta` row printed before everything — step 15 |
| build | `CMakeLists.txt:12-14` | `vendor/linenoise.c` beside `vendor/sqlite3.c` — step 1 |
| tests | `tests/tests.cpp` | ~25 assertion sites and two golden literals — step 5 onward |

`roomBlock` has **four** callers, not the three the brief names: `moved`
(`render.cpp:118`), `looked` with a null detail (`:125`), `downed` (`:225`), and
`renderRoomOf` for the startup render (`:244`). `examined` is not among them.
Steps 12, 13 and 14 split along that real division.

## Three micro-decisions

1. **How `runTurn` tells prose from a reference table from a refusal.**
   REQ-POLISH-3b exempts `spells` from the indent and REQ-POLISH-7a requires
   refusals to keep it, but both return `TurnOutcome::NoTick`, so the outcome
   cannot carry the decision. The one-line version — indent unless `NoTick` —
   is wrong for exactly that reason. Smallest correct change: a defaulted third
   member on `TurnResult` (`loop.hpp:17`) naming how the text should be
   presented — `Prose`, `Reference`, `Error`. Aggregate initialisation of the
   existing two members still compiles, and no test constructs a `TurnResult`
   (37 mentions in `tests.cpp`, all of them reading one), so the widening costs
   nothing. Step 3 adds the first two values, step 6 adds the third.
2. **Dim the refusal in `loop.cpp`, not in `render.cpp`.** `wrapProse` measures
   with `utf8Length`, which counts escape bytes as characters — `\x1b[90m` reads
   as five columns. Styling inside `renderError` would corrupt the wrap of any
   refusal long enough to wrap. So `renderError` keeps returning plain text and
   the dim is applied per line after wrapping and indenting, where the band's
   own styling already happens. `render.cpp` stays free of `TermStyle`.
3. **The blank line before the prompt is gated on `currentStyle().attrs`.**
   Check 4 asserts both halves: a blank line before each `>` in an interactive
   capture, and no gained blank line per turn in a piped one. `main.cpp:140`
   prints `> ` into a pipe as well as onto a terminal, so an unconditional
   `"\n> "` fails the second half. `attrs` is already false for a pipe and for
   `TERM=dumb`, and is the flag REQ-POLISH-26 uses for the spinner.

## Step sequence

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>build</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;1 vendor linenoise (compiles, unused)<br>
<b>layout</b>&nbsp;&nbsp;&nbsp;2 wrap cap ─▶ 3 indent + presentation kind ─▶ 4 blank line<br>
<b>dedup</b>&nbsp;&nbsp;&nbsp;&nbsp;5 delete Exits:/You see: + band-failure fallback ─▶ 6 dim refusals<br>
<b>bars</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;7 background colour ─▶ 8 pure bar builder ─▶ 9 bars in the band<br>
<b>look</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;10 meta.start_room ─▶ 11 roomSeen ─▶ 12 first sight ─▶ 13 startup ─▶ 14 examine a room<br>
<b>art</b>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;15 title screen<br>
<b>input</b>&nbsp;&nbsp;&nbsp;&nbsp;16 linenoise replaces getline ─▶ 17 history file ─▶ 18 re-capture baselines<br>
<b>spinner</b>&nbsp;&nbsp;19 deterministic half (MED) ─▶ 20 live observation (HIGH)<br>
<b>sweep</b>&nbsp;&nbsp;&nbsp;&nbsp;21 the spec's twenty checks<br>
</div>

Steps 1, 10 and 15 depend on nothing and can move. Everything else runs in the
order shown. Step 5 must not be split: deleting the lines makes the band the
only route the exits reach the player, so the fallback has to land in the same
commit or there is a revision where a band failure silently costs them.

---

### Step 1 — Vendor linenoise into the build, unused

**Requirements:** REQ-POLISH-20 (the vendoring half).

Drop upstream `antirez/linenoise`'s `linenoise.c` and `linenoise.h` into
`vendor/`, beside `sqlite3.c` and `json.hpp`. Add a `linenoise` static library
to `CMakeLists.txt` in the shape the `sqlite3` target already has — `add_library`
plus `target_include_directories(linenoise PUBLIC vendor)`, so anything linking
it can `#include "linenoise.h"`. Record the upstream commit in a one-line
comment, as a vendored file with no package manager behind it should. **Nothing
calls it yet.**

**Link it into `twcore` as well as `textworld`, not `textworld` alone.** Two
call sites land in two different targets: step 16 calls `linenoise()` from
`main.cpp`, which is compiled straight into the `textworld` executable
(`CMakeLists.txt:26-27`), while step 19 calls `linenoiseHide`/`linenoiseShow`
from `runTurnCore` in `loop.cpp`, which is compiled into the `twcore` static
library (`CMakeLists.txt:17`). Adding `linenoise` to `twcore`'s
`target_link_libraries` (`:20`, beside `sqlite3` and `CURL::libcurl`) is what
makes step 19 link at all — and it reaches `tests` (`:31`) transitively, which
step 19's own gate needs to construct a `Spinner` in a unit test. `textworld`
needs the direct link too, for `main.cpp`'s own call and for the header path.

Isolated and first, so that when step 16 changes how a line is read, the build
change is already known good.

> **Validation gate:** `cmake -B build && cmake --build build` succeeds and
> `./build/tests` is green — byte-identical behaviour, since no call site exists.
> Then prove the linkage both later steps depend on, rather than discovering it
> at step 19: add a throwaway `linenoiseHistorySetMaxLen(64);` call to
> `loop.cpp`, confirm **`tests` and `textworld` both link**, and remove it before
> committing. A `twcore`-only or `textworld`-only link passes the plain build and
> fails this.
> `git show --stat` on the commit touches only `vendor/linenoise.[ch]` and
> `CMakeLists.txt`. `testBandNonGoals` (`tests.cpp:10384`) still passes: it scans
> `CMakeLists.txt` for `ncurses`/`termbox`/`ftxui` and the sources for
> `readline`, none of which this adds.

### Step 2 — Cap the prose wrap width

**Requirements:** REQ-POLISH-1, REQ-POLISH-2.

Add to `term.hpp` beside `kMinWidth` and `kDefaultWidth`:
`kProseMaxWidth = 66` (the Bringhurst / Tinker-Paterson midpoint, sourced in the
research) and `kProseIndent = 2`; and a pure
`int proseWidth(int detected)` returning
`max(kMinWidth - kProseIndent, min(detected, kProseMaxWidth) - kProseIndent)`.
The indent is **subtracted**, which is what keeps a 20-column terminal at 20
columns. Change `loop.cpp:194` and `loop.cpp:201` to wrap at
`proseWidth(w)` while `bandOrEmpty(db, w)` keeps the raw `w` — that one-line
difference is REQ-POLISH-2.

> **Validation gate:** new `testTermProseWidth` drives `proseWidth` directly at
> 200 → 64, 80 → 64, 66 → 64, 40 → 38, 20 → 18, 1 → 18, 0 → 18, so the floor is
> exercised without a terminal. Then **spec check 1's first half** —
> `COLUMNS=200 TEXTWORLD_AI=0 ./build/textworld < script`, no narration line over
> 66 code points after `stripSgr` — and **spec check 2**: the band's rules still
> run the full 200 columns in the same capture. `./build/tests` green apart from
> the width assertion in `testBandWiring` (`tests.cpp:9975`), which asserts
> `<= 40` at width 40 and still holds.

### Step 3 — Indent narration two spaces

**Requirements:** REQ-POLISH-3, REQ-POLISH-3a, REQ-POLISH-3b.

Add a pure `std::string indentProse(const std::string& text, int spaces)` to
`term.cpp`: prefix every line **except an empty one** with `spaces` spaces, and
leave the trailing newline `wrapProse` preserves alone. That single skip is
REQ-POLISH-3a, and it is what stops a blank paragraph break from becoming two
stray spaces.

Add the presentation kind to `TurnResult` (micro-decision 1) with `Prose` and
`Reference`. `runTurnCore`'s `Verb::Spells` return marks itself `Reference`;
every other return keeps the default. In `runTurn`: `Prose` wraps to
`proseWidth(w)` then indents; `Reference` wraps to the full `w` and is not
indented, so the spell table lines up with the band at column 0 exactly as
REQ-POLISH-3b argues it should. `renderStartup` indents too. The band is
appended after, untouched.

> **Validation gate:** `testTermIndent` covers a normal line, a blank line
> between paragraphs, a trailing newline, and the empty string. Then **spec
> check 3** on a capture: narration lines start with exactly two spaces, band
> rows and `spells` output do not, and no line is whitespace-only
> (`grep -nE '^[[:space:]]+$'` finds nothing). **Spec check 1's second half** at
> `COLUMNS=20`: no line over 20 columns after `stripSgr` — the case a naive
> indent breaks.

### Step 4 — A blank line before the prompt

**Requirements:** REQ-POLISH-4.

`main.cpp:140`: when `currentStyle().attrs` is true, print `"\n> "`; otherwise
`"> "` unchanged (micro-decision 3). Emitted with the prompt, so no turn's
output gains a trailing newline and nothing downstream of `runTurn` changes.

> **Validation gate:** **spec check 4.** Interactive (`CLICOLOR_FORCE=1`, a pty
> or `script -q`): a blank line precedes each `>`. Piped: `diff` the capture
> against one taken before this commit — identical. `./build/tests` green; the
> tests call `runTurn` directly and never see the prompt.

### Step 5 — Delete the duplicated lines, and catch the band when it falls

**Requirements:** REQ-POLISH-5, REQ-POLISH-6, REQ-POLISH-6a.

The largest deterministic diff in the plan, and one commit on purpose.

`render.cpp`: delete the exits block (`:69-82`) and the items block (`:84-87`)
from `roomBlock`, leaving canon prose and nothing else. All four callers inherit
it. `architectEnabled()` may become unused in that translation unit — check
before removing the include. Safe because `band.cpp:232`'s `exitsRow` runs a
byte-identical query, latent-exit gate included, which REQ-UI-10 states outright.

`loop.cpp:52` `bandOrEmpty`: on the catch path, write a `warn` entry naming the
failure (`logEmitf(LogLevel::Warn, "band", ...)` — today it logs nothing) and
return a plain-text `Exits: ...` line built from the same query, at column 0,
appended where the band would have been. Wrap **that** in its own try/catch
returning `""` — REQ-POLISH-6a, and the likely case, since a band that threw for
want of a player or location row will usually deny the fallback its room too.

Then the test sweep: about 25 assertion sites reference the deleted lines
(`tests.cpp:1900`, `:1909`, `:2864`, `:2878`, `:4695-4701`, `:5261`, `:9439-9440`
and the two golden literals). Re-capture the literals with `TW_DUMP_GOLDEN=1`
rather than hand-editing them. The `kStoryGoldenSession` comment forbids
re-capture, and that instruction is aimed at the arc brick hiding a printed
advance; REQ-POLISH-5 is a deliberate layout change, which is the exemption spec
check 20 grants. Note the re-capture in the commit message.

> **Validation gate:** **spec check 5** — `Exits:` appears zero times in turn
> output and the band's `Exits` row once per turn, same for `You see:` against
> `Objects`, checked at all four `roomBlock` callers (`go north`, `look`,
> `x <room>`, and the startup render), not just `look`. **Spec check 6** — run
> the script with the architect on and off and confirm the band's exits differ
> exactly as `blocked-directions-exit-display`'s existing check expects; this is
> the regression the deletion could plausibly cause. **Spec check 7** — a fixture
> with no `location` row for the player makes `composeBand` throw: the fallback
> exits line prints and a `warn` entry reaches the session log; then break the
> fallback query too and confirm the turn still prints its narration and the
> game continues. Add both as `testBandFallbackExits` beside the existing degrade
> case at `tests.cpp:9959`. Full suite green.

### Step 6 — Dim refusals

**Requirements:** REQ-POLISH-7, REQ-POLISH-7a.

Add `Error` to the presentation kind. Every `renderError` return in
`runTurnCore` — tier a, the cast denial, tier c — marks itself `Error`.
`runTurn` wraps and indents it exactly as prose (REQ-POLISH-7a), then applies
`colorize(line, Color::BrightBlack, currentStyle())` per line, after the width
arithmetic (micro-decision 2). `BrightBlack` is the colour `band.cpp:28` already
gives a spell that has receded out of reach, so no new colour enters the
vocabulary and REQ-UI-24's one-role-one-colour rule is unaffected.

> **Validation gate:** **spec check 8** — capture `xyzzy the frobnitz` three
> ways: with `CLICOLOR_FORCE=1` the text is wrapped in `\x1b[90m`; with
> `NO_COLOR=1` and with `TERM=dumb` it is identical plain text; all three are
> indented two spaces. A unit test on `runTurn`'s output asserts the same, and
> asserts the escape bytes sit **outside** the two-space indent so
> `stripSgr` yields the same string in all three.

### Step 7 — Background colour in `term`

**Requirements:** REQ-POLISH-13.

One function beside `colorize` / `bolden` / `boldColor`:
`std::string bgColorize(std::string_view, Color, TermStyle)`, emitting the
`40-47` / `100-107` parameters through the existing `sgrWrap`, gated on
`style.color` exactly as `colorize` is. Basic 16 only (REQ-UI-19), so a bar
resolves through the user's theme and stays readable on a light background. Add
a `bgSgrParam` switch next to `sgrParam`; no 256-colour, no truecolor. Its own
step, before anything draws a bar with it.

> **Validation gate:** `testTermBackgroundColor` — `bgColorize("   ", Color::Red,
> {true, true})` is `"\x1b[41m   \x1b[0m"`; `stripSgr` of it is `"   "` and
> `utf8Length(stripSgr(...))` is 3; with `{false, false}` and with
> `{false, true}` it is the bare spaces and contains no `\x1b` (REQ-UI-22);
> `Color::None` emits nothing. Every one of the sixteen colours maps to a
> distinct parameter.

### Step 8 — The bar itself, as a pure function

**Requirements:** REQ-POLISH-9, REQ-POLISH-10, REQ-POLISH-12.

A pure builder in `band.cpp` — no db, no environment:
`std::vector<BandSpan> healthBarSpans(int64_t current, int64_t max, TermStyle)`.

- Ten columns of **spaces**, never a block character: `█` and `░` are East Asian
  Ambiguous width and would let the terminal decide the column count, the defect
  REQ-UI-29 already rules out for `─` and `·`.
- Filled = `round(10 * current / max)` in integer arithmetic — `(20 * current +
  max) / (2 * max)` — with a floor of 1 while `current > 0`, so a living enemy
  never shows an empty bar. `max <= 0` yields no spans at all.
- Two steps of degradation and no third: background colour when `style.color`,
  otherwise ASCII `[####......]`. **No reverse video.** Reverse video swaps
  foreground and background and is a colour effect, so emitting it under
  `NO_COLOR` would stretch REQ-UI-23 past what it says.
- Always the same column count either way, so a row's width never moves as
  health drops.

> **Validation gate:** `testBandHealthBar` over 12/12, 6/12, 1/12, 1/10, 0/12,
> 3/4, 0/0: `utf8Length` of the concatenated `text` is constant across every
> living case; 1/10 gives exactly one filled column, not zero; 0/12 gives zero;
> every byte is ASCII (`< 0x80`); the plain-style form contains `[` and `#`; no
> output anywhere contains `\x1b[7m`.

### Step 9 — Bars in the band rows

**Requirements:** REQ-POLISH-8, REQ-POLISH-8a, REQ-POLISH-10a, REQ-POLISH-11.

Insert the step-8 spans into `playerRow` (`band.cpp:337`) and each row in
`hostileRows` (`band.cpp:283`), **alongside** the existing `HP: n/m` text and
never instead of it — a bar alone cannot tell 3 HP from 4, and the band's job is
carrying numbers the player can trust over the prose. Cooldowns get no bar
(REQ-POLISH-8a): a 2-of-2 cooldown is a two-column bar, which is noise.

No change to `layoutBand`. `stripSgr` removes the escape bytes and `utf8Length`
counts the spaces, verified in the research: `\x1b[41m      \x1b[0m` measures 6.
Growth is bounded at 11 columns per row — ten plus one separator — so REQ-UI-33's
wrapping stays the exception on a hostile row already carrying a name, health,
`[WINDING UP]` and discovered resistances.

Position within the row is the **first open question**: put the bar immediately
after `HP: n/m` for now and decide it against a real fight, not here.

> **Validation gate:** **spec check 9** — every band row has an identical column
> count after `stripSgr` at full health and at 1 HP; a living enemy at 1/10 shows
> one filled column; no bar byte is outside ASCII. **Spec check 10** — the same
> fight state with colour on and with `NO_COLOR=1`: health is readable in both,
> the second contains `[` and `#`, neither contains `\x1b[7m`. Re-capture
> `testBandGoldens`' seven literals (`tests.cpp:9650`) with `TW_DUMP_BANDS=1`;
> `testBandColor`, `testBandLayout` and `testBandResistance` green.

### Step 10 — Record the starting room

**Requirements:** the anchor REQ-POLISH-15 needs (see the amendment above).

In `initialize()`, inside the same transaction and after the seed SQL runs at
`world.cpp:283`, write one more `meta` row beside the `meta.setting` insert at
`world.cpp:288-292`:

```sql
INSERT INTO meta(key, value)
SELECT 'start_room', container FROM location
WHERE entity = (SELECT entity FROM player LIMIT 1);
```

Derived from the seed's own `location` row, so it is right for `seed/base.sql`
and for every fixture without any of them naming a number. A row, not a shape:
zero DDL, no `SCHEMA_VERSION` bump, exactly as the comment at `world.cpp:288`
records for `meta.setting`.

**One coupling this row does not remove.** `combat.hpp:22` already hardcodes
`kDormitoryCell = 1` as the seed room, and `combat.cpp:609` passes it to
`downPlayer` as the respawn destination whatever fixture is loaded — so respawn
is pinned to entity 1 while `meta.start_room` is derived. They agree today: the
player starts at container 1 in `seed/base.sql:37`, `tests/fixture.sql:31` and
`tests/combat_fixture.sql:79` alike. A future fixture starting the player
anywhere else would send a downed player to room 1 while `start_room` named
another, and step 12's "the cell counts as seen" would quietly stop holding.
Deriving the row rather than hardcoding it is still right — it keeps the new
fact honest — but it does not free the respawn path from the assumption, and
that is a `kDormitoryCell` problem to fix on the day a fixture breaks it, not
here.

A world file created before this change has no such row. Step 11 reads it as
absent → no room matches → the derivation falls back to `moved` events alone,
and such a world reprints its starting paragraph once. `world.db` is gitignored
and rebuilt from the seed, so this costs a developer one line of output.

> **Validation gate:** `testWorldStartRoom` — a fresh world from `base.sql` has
> `meta.start_room = 1`; so does one from `tests/combat_fixture.sql` and
> `tests/fixture.sql`; the row is written inside the transaction, so a seed that
> throws leaves no half-seeded world (drive it with a deliberately broken seed,
> as `initialize`'s existing rollback test does). `SCHEMA_VERSION` is still 8 and
> `testWorld`'s version gate is untouched.

### Step 11 — "Have I been here", derived from the transcript

**Requirements:** REQ-POLISH-15.

A read-only helper in `render.cpp`:
`bool roomSeen(Db& db, int64_t room, int64_t turn)` — true when
`meta.start_room` equals `room`, or when an `events` row exists with
`verb = 'moved'`, `object = room` and `turn < ?`. **Strictly earlier**, so the
arrival turn's own `moved` event does not mark the room seen before the turn
that reports it has printed. No cache, no shadow table, no new column — the
posture `discoveredResistances` (REQ-UI-46) already established, so the fact
cannot drift from the transcript or be lost across a restart.

> **Validation gate:** `testRoomSeen` on a hand-built events table: a room with
> no `moved` event is unseen; a `moved` at an earlier turn makes it seen; a
> `moved` at the **same** turn does not; the starting room is seen at turn 0 with
> no events at all; a world with no `start_room` row answers from `moved` events
> alone and does not throw. Read-only: 20 calls leave `meta.turn` and the events
> count unchanged, and `testBandWiring`'s source scan still finds no write verb
> in `render.cpp`.

### Step 12 — First sight at the three render sites

**Requirements:** REQ-POLISH-14 (arrival), REQ-POLISH-17, REQ-POLISH-18.

`roomBlock` gains a `bool seen`: unseen prints the canon paragraph as today,
seen prints the room's **name** and nothing else. Wire `roomSeen` into the three
event-driven callers — `moved` (`render.cpp:118`), `looked` with a null detail
(`:125`), and `downed` (`:225`). Exits, objects and hostiles come from the band
on every turn either way. `renderRoomOf` (`:244`) takes the flag as a parameter;
step 13 supplies it.

The `downed` site is why `meta.start_room` exists: respawn writes `downed`, not
`moved` (`mutations.cpp:429`), and the cell it returns you to is the room the
seed started you in, so nothing in the events table would ever mark it seen.

REQ-POLISH-18's consequence is accepted and recorded here rather than hidden: a
repeat `look` mid-fight still costs a turn, still takes chip damage, and now
returns only the room name. `look` is **not** made free — REQ-UI-39b forbids
generalising the no-tick exception, and this is exactly the appeal it names.

> **Validation gate:** **spec check 11's first three clauses**, from a fresh
> `world.db`: the paragraph prints at creation; `look` does not print it again;
> move away and back and it does not print on return. **Spec check 13** — get
> downed, wake in the dormitory cell, no paragraph. Both as `testFirstSight`
> driving `runTurn` over `tests/combat_fixture.sql`. Re-capture the two golden
> literals again (the return-to-cell blocks change) and say so in the commit
> message.

### Step 13 — The startup render prints the paragraph only at world creation

**Requirements:** REQ-POLISH-16.

`renderStartup` (`loop.cpp:199`) passes `seen = (SELECT COUNT(*) FROM events) > 0`
rather than asking `roomSeen` — the starting room is always seen by step 11, so
the courtesy render needs its own answer. Empty events means world creation, the
one launch where the player has never seen the room. Every later launch prints
the room name and the band.

> **Validation gate:** **spec check 12** — relaunch against an existing
> `world.db` and the startup render shows the room name and the band, no
> paragraph; delete it, relaunch, and the paragraph is back.
> `testBandStartup` (`tests.cpp:9980`) needs its "vaulted hall of grey stone"
> assertion re-pointed at the fresh-world case, which is the layout change spec
> check 20 permits.

### Step 14 — `examine <room>` is the full reread

**Requirements:** REQ-POLISH-14 (explicit request), REQ-POLISH-19.

`resolveExamine` (`systems.cpp:187`): the player's own room is in scope. Keep the
existing shape and add the room case ahead of the `containerOf` test, which
cannot answer for an entity with no `location` row. Amends REQ-EXAMINE-7, whose
comment at `systems.cpp:180` should be updated to say so.

`render.cpp`'s `examined` branch already prints the subject's `description` row
verbatim, which after step 5 is exactly what `roomBlock` emits for an unseen
room — so a room reads as its full paragraph with no change to that branch.
Confirm rather than assume, and leave the branch alone if it holds.

**No new verb and no two-word command.** `x room` is the reread. The parser has
no two-word verbs and this does not add the first one (REQ-POLISH-19).

> **Validation gate:** **spec check 11's last clause** — after a `look` that
> printed only the name, `x <room name>` prints the full paragraph.
> `testExamineRoom`: examining the current room by name writes an `examined`
> event and renders the paragraph; examining a room the player is **not** in
> still answers "You don't see that here."; `testSystems`' existing examine
> refusals are unchanged. `kExamineGoldenSession` re-captured.

### Step 15 — The title screen

**Requirements:** REQ-POLISH-29, REQ-POLISH-30, REQ-POLISH-31.

Render the game's name once with FIGlet while authoring — neither `figlet` nor
`toilet` is installed here, so `brew install figlet` or any online generator —
and paste the result into `seed/base.sql` as one more `meta` row. No renderer, no
font files, no runtime dependency, and the art is reviewable in a diff.

`main.cpp` prints it **first**, before the template-mode notice at `:126` and
before the startup render. Plain ASCII only (REQ-UI-29): `/ \ | _ - . ' ( ) # *`,
no box drawing, no ambiguous-width character, so it cannot misalign on any
terminal. Below the art's natural width — measure it and compare against
`detectWidth()` — print the game's name as plain text instead of letting it wrap
into rubble.

A world file created before this change has no art row; print the plain name in
that case too, on the same path as the narrow-terminal fallback.

Whether the screen should be suppressible by an env switch is the **second open
question**. REQ-POLISH-29 prints it every launch; do not build a switch until
somebody wants one.

> **Validation gate:** **spec check 19** — the art prints before
> "AI narration off — template mode" and before the first room; every byte is
> ASCII (`LC_ALL=C grep -n '[^[:print:][:space:]]'` finds nothing); at
> `COLUMNS=20` it degrades to the plain name and no line exceeds 20 columns.
> `testTitleScreen` asserts the seed row exists and is ASCII, and that the
> narrow-width path returns the bare name.

### Step 16 — linenoise replaces `getline`

**Requirements:** REQ-POLISH-20, REQ-POLISH-22, REQ-POLISH-23, REQ-POLISH-24.

`main.cpp:153`: on a terminal, read with `linenoise("> ")`; in a pipe, keep
`std::getline(std::cin, line)` untouched. Branching on `isatty` explicitly,
rather than relying on linenoise's own non-tty path, is what makes REQ-POLISH-22
structural — a piped run executes the same code it does today, so the captures
under `.lore/work/validation/` cannot drift.

`linenoise()` returns `malloc`'d memory; wrap it in a `unique_ptr` with a
`linenoiseFree` deleter rather than scattering frees. `NULL` means EOF and maps
straight onto the existing `if (eof) break`, so EOF still behaves as quit
(REQ-POLISH-23, `main.cpp:155`). The prompt string moves into the `linenoise`
call on the terminal path — keep step 4's leading newline with it.

**No tab completion** (REQ-POLISH-24). `linenoiseSetCompletionCallback` exists
and is easy; completing the nouns present in a room tells the player what is
there before they look.

The `ScopedDwell` block keeps its shape: the timer must still cover exactly the
gap between the prompt reaching the terminal and the line coming back.

> **Validation gate:** piped, `diff` against a pre-change capture of the same
> script — identical, no escape bytes (`grep -c $'\x1b'` is 0). Interactive:
> up-arrow recalls the previous line, ctrl-a reaches the start, a typo is fixable
> without retyping the sentence. `printf 'look\n' | ./build/textworld` with no
> `quit` exits cleanly (REQ-POLISH-23). `grep -n linenoiseSetCompletionCallback
> src/main.cpp` finds nothing. Update `testBandNonGoals`' "no line editing /
> history" comment — REQ-POLISH-20 reverses that non-goal deliberately; the
> `readline` scan itself still passes and stays.

### Step 17 — The history file

**Requirements:** REQ-POLISH-21, REQ-POLISH-21a, REQ-POLISH-21b.

`.textworld_history` beside `world.db`, the same placement rule `logs/` follows.
`linenoiseHistorySetMaxLen` bounds it; `linenoiseHistoryLoad` at startup;
`linenoiseHistoryAdd` per accepted line. Add the filename to `.gitignore` next
to `world.db`.

Saved on **every** exit path (REQ-POLISH-21a) — the `quit` verb, EOF, and the
fatal-error path. Do that with an RAII guard declared beside `SessionLogGuard`
(`main.cpp:30`), which exists for exactly this reason: a destructor is reached by
normal return, quit, EOF, `SchemaMismatch`, and the generic catch alike. A
session that ends badly does not cost the player their history.

A path that cannot be read or written is **not an error** (REQ-POLISH-21b): the
game plays on with in-session history only and records the failure in the session
log, matching the best-effort posture logging already has.

> **Validation gate:** **spec check 16** — the file appears beside `world.db`; a
> second session recalls the first session's lines; `git status` does not list
> it; `chmod 000` on the file (or a read-only directory) still lets the game run,
> and the session log carries the failure. **Spec check 15** — a piped script
> with no `quit` exits cleanly and history was saved. Kill a session with a
> deliberate fatal error and confirm the file was written.

### Step 18 — Re-capture the validation baselines

**Requirements:** REQ-POLISH-22 (as evidence).

Re-run the scripts under `.lore/work/validation/` and assert **three properties**
of the new output rather than diffing against the old, which this spec has
deliberately changed: no escape bytes anywhere, the same sequence of turn
outcomes, and the same set of `events` rows written. Then commit the new captures
as the baseline, with a note in each `findings.md` naming this spec as the reason
the numbers moved.

> **Validation gate:** **spec check 14.** For each script: `grep -c $'\x1b'` is
> 0; the outcome sequence matches the old capture; `sqlite3 world.db 'SELECT
> turn, verb, subject, object, detail FROM events ORDER BY id'` matches the old
> run's rows exactly. Any difference in the events rows is a bug in this spec's
> work, not a baseline to accept.

### Step 19 — The spinner, deterministic half — MED

**Requirements:** REQ-POLISH-26, REQ-POLISH-28, REQ-POLISH-25 (the mechanism).

MED because a second thread writes to stdout while the main thread is inside a
blocking network call, and that is the shape of problem that needs iterating on.
Split from step 20 so the concurrency work is verified offline and only the
observation costs tokens.

A small `Spinner` RAII type around the `aiRender` call in `runTurnCore`
(`loop.cpp:158-163`). It starts a thread only when `currentStyle().attrs` is
true — already false for a pipe and for `TERM=dumb`, so no escape byte reaches a
non-terminal and the golden comparisons in `tests.cpp` are untouched — and
**only when `aiNarrationEnabled()`**, so template mode starts no thread at all
(REQ-POLISH-28): there is no network call there and nothing to wait for.

Use linenoise's `linenoiseHide` / `linenoiseShow` (available since step 16)
rather than hand-rolling cursor management. That API exists to print
asynchronous output without wrecking the line being edited, which is why the
research treats line editing and the spinner as one job. Erasure is the
destructor's business — it runs on the success path, the failure path, and the
timeout path alike, which is what REQ-POLISH-27 needs.

> **Validation gate:** **spec check 18** — `TEXTWORLD_AI=0`: no thread starts
> (assert on a counter the type increments) and no frame byte is written; the
> full offline suite is green and byte-identical. A unit test constructs the
> spinner with `{false, false}`, sleeps past several frame intervals, and
> asserts the sink received nothing. Run the suite under
> `-fsanitize=thread` for this step; a data race here would be invisible
> otherwise.

### Step 20 — The spinner, live observation — HIGH

**Requirements:** REQ-POLISH-25, REQ-POLISH-27.

> The one step that spends live-LLM tokens. Deliberately last, deliberately
> small, and its assertions are mechanical so it cannot turn into a tuning loop.

With a real key and AI on, take one turn on a terminal and one turn piped. A
turn is ~3.4 s today and ~7.3 s when a room is generated, and that is the window
the spinner exists to fill. Then force a failure — an invalid key, which
`.lore/work/validation/turn-latency-polish/prof-invalid-key.log` shows the shape
of — and confirm the erasure still happens on the path where the call fails or
times out.

Three turns total. Not a re-run of the suite with AI on.

> **Validation gate:** **spec check 17** — on a terminal the spinner is visible
> during the wait and no frame character survives anywhere in the turn's output;
> piped, the capture contains no escape byte at all
> (`grep -c $'\x1b'` is 0). On the failed call, no residue on any line.

### Step 21 — Walk the spec's twenty checks

**Requirements:** REQ-POLISH-32, REQ-POLISH-33, and the sweep.

Run the spec's AI Validation list end to end and confirm each item against the
step that owns it (the table below). Then the two cross-cutting ones:

- **REQ-POLISH-32** — `git diff` on the whole branch touches no combat constant,
  no turn cost, no `CREATE TABLE`, and adds no event verb. `meta.start_room` and
  the title-art row are rows, not shapes; `SCHEMA_VERSION` is still 8.
- **REQ-POLISH-33** — every new styled byte goes through `TermStyle`. A run whose
  stdout is not a terminal emits no escape sequence of any kind:
  `TEXTWORLD_AI=0 ./build/textworld < script | grep -c $'\x1b'` is 0, against the
  real binary, as property check 7 of the status-band suite already asserts.
- **Spec check 20** — the full existing suite. Every failure must be a test
  asserting on layout this spec deliberately changes. A combat, turn-count, or
  schema test that needs editing is a REQ-POLISH-32 failure and must be **raised,
  not edited**.

> **Validation gate:** all twenty checks pass and each maps to a named
> requirement; the two cross-cutting greps come back clean; `./build/tests` is
> green with no combat, turn-count or schema test edited.

---

## The spec's twenty checks, by step

| Check | Step |
|---|---|
| 1 prose width (66, then 20) | 2, 3 |
| 2 band width unchanged | 2 |
| 3 indent | 3 |
| 4 blank line | 4 |
| 5 no duplicate exits | 5 |
| 6 latent-exit gate survives | 5 |
| 7 band failure keeps exits | 5 |
| 8 error styling | 6 |
| 9 bar widths | 9 |
| 10 bar degradation | 9 |
| 11 first sight | 12, 14 |
| 12 restart | 13 |
| 13 downed | 12 |
| 14 line editing invisible to pipes | 18 |
| 15 EOF | 17 |
| 16 history file | 17 |
| 17 spinner leaves nothing behind | 20 |
| 18 spinner absent in template mode | 19 |
| 19 title screen | 15 |
| 20 no rule changed | 21 |

## Requirement coverage map

| Requirement | Step(s) |
|---|---|
| REQ-POLISH-1 (capped wrap width) | 2 |
| REQ-POLISH-2 (band keeps full width) | 2 |
| REQ-POLISH-3 (two-space indent) | 3 |
| REQ-POLISH-3a (no whitespace-only line) | 3 |
| REQ-POLISH-3b (`spells` not indented) | 3 |
| REQ-POLISH-4 (blank line before the prompt) | 4 |
| REQ-POLISH-5 (delete `Exits:` / `You see:`) | 5 |
| REQ-POLISH-6 (band failure keeps the exits) | 5 |
| REQ-POLISH-6a (fallback failure never fails the turn) | 5 |
| REQ-POLISH-7 (refusals dimmed) | 6 |
| REQ-POLISH-7a (refusals still indented) | 6 |
| REQ-POLISH-8 (bar alongside the numbers) | 9 |
| REQ-POLISH-8a (no bar on cooldowns) | 9 |
| REQ-POLISH-9 (spaces, never block characters) | 8 |
| REQ-POLISH-10 (10 columns, floor of 1) | 8 |
| REQ-POLISH-10a (at most 11 columns of growth) | 9 |
| REQ-POLISH-11 (a bar is a `BandSpan`) | 9 |
| REQ-POLISH-12 (two-step degrade, no reverse video) | 8, 9 |
| REQ-POLISH-13 (background colour in `term`) | 7 |
| REQ-POLISH-14 (first sight and explicit request) | 12, 14 |
| REQ-POLISH-15 (derived from the transcript) | 10, 11 |
| REQ-POLISH-16 (startup paragraph only at creation) | 13 |
| REQ-POLISH-17 (repeat `look` prints the name) | 12 |
| REQ-POLISH-18 (accepted turn cost) | 12 |
| REQ-POLISH-19 (no new verb, no two-word form) | 14 |
| REQ-POLISH-20 (vendor linenoise) | 1, 16 |
| REQ-POLISH-21 (history file, bounded) | 17 |
| REQ-POLISH-21a (saved on every exit path) | 17 |
| REQ-POLISH-21b (an unusable path is not an error) | 17 |
| REQ-POLISH-22 (pipes unchanged) | 16, 18 |
| REQ-POLISH-23 (EOF is quit) | 16 |
| REQ-POLISH-24 (no tab completion) | 16 |
| REQ-POLISH-25 (show the game is working) | 19, 20 |
| REQ-POLISH-26 (gated on `attrs`) | 19 |
| REQ-POLISH-27 (erases itself completely) | 20 |
| REQ-POLISH-28 (absent in template mode) | 19 |
| REQ-POLISH-29 (title screen, first, from the seed) | 15 |
| REQ-POLISH-30 (plain ASCII only) | 15 |
| REQ-POLISH-31 (respects the width floor) | 15 |
| REQ-POLISH-32 (no rule, cost, schema or verb change) | 21 |
| REQ-POLISH-33 (every styled byte through `TermStyle`) | 21 |

All 33 numbered requirements and all eight lettered sub-requirements are covered,
and every one of the spec's twenty validation checks belongs to a step.

## Two questions this plan carries rather than settles

- **Where the health bar sits in a hostile row**, relative to the numbers and
  `[WINDING UP]`. Step 9 puts it after `HP: n/m` so there is something to look
  at. Decide it by looking at a real fight, not here.
- **Whether the title screen should be suppressible by an env switch**, in the
  shape of `TEXTWORLD_AI`. REQ-POLISH-29 prints it every launch. Not built until
  wanted.

## Out of scope

Room images and any second image vendor; streaming narration; a browser or
native window; per-room ASCII art beyond the title screen; the map; collapsing
the band when it is unchanged; showing what the resolver decided. All are
recorded in [the brainstorm](.lore/work/brainstorm/terminal-visual-polish.md) as
parked, not rejected.
