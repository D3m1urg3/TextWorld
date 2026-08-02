---
title: "Implementation notes: terminal-status-band-ui"
date: 2026-08-02
status: complete
tags: [implementation, notes, ui, terminal, ansi-color, no-color, status-band, prose-wrapping, combat-legibility, resistance-discovery]
source: .lore/work/plans/terminal-status-band-ui.md
modules: [term, band, render, loop, combat, main, mutations]
related: [.lore/work/specs/terminal-status-band-ui.md, .lore/work/research/terminal-ui-status-band.md, .lore/work/brainstorm/ui-improvements.md]
---

# Implementation notes: terminal-status-band-ui

Executing the **approved** plan inline (no implement subagents —
[[no-implement-subagents]]). Gate per step: `cmake --build build --target tests`
clean + `./build/tests` green + that step's named test.

## Progress

**Phase 1 — terminal services**

- [x] Step 1 — color gate and SGR helpers (`testTermColorGate`)
- [x] Step 2 — width detection, fallback chain, clamp (`testTermWidth`)
- [x] Step 3 — UTF-8 prose wrapper (`testTermWrap`)

**Phase 2 — band content**

- [x] Step 4 — band frame, row model, hanging indent (`testBandLayout`)
- [x] Step 5 — room name, Exits, Objects rows (`testBandContent`)
- [x] Step 6 — hostile rows (`testBandContent`)
- [x] Step 7 — player row: HP always, readiness in combat (`testBandContent`)
- [x] Step 8 — telegraph and active states + goldens (`testBandGoldens`)
- [x] Step 9 — apply color (`testBandColor`)

**Phase 3 — wiring**

- [x] Step 10 — remove both appends; wire the band into `runTurn`
- [x] Step 11 — startup band
- [x] Step 12 — `spells` verb and the no-tick route

**Phase 4 — group G (gated, droppable)**

- [x] Step 13 — tag elemental damage in `events.detail`; shield it from the model
- [x] Step 14 — derive discovery; render it on the hostile row

- [x] Step 15 — final validation against the spec

## Log

### Phase 1 — terminal services

`src/term.{hpp,cpp}` added to twcore. Each capability is split into a pure
predicate plus a thin environment-reading wrapper, so every truth table is
driven without a tty and without mutating the developer's shell.

**Divergence taken — `CLICOLOR=0`.** REQ-UI-20's table gives rows 3 and 4
identical results ("color iff stdout is a tty"), which makes row 3 carry no
information unless `CLICOLOR=0` differs from unset. Implemented as a
suppression, which is also the variable's established meaning elsewhere.
Recorded in `term.hpp` at the declaration. **Worth folding back into the spec**
— as written, row 3 is dead.

**Added beyond the plan — `widthFrom(bool ioctlOk, int ioctlCols, const char* columns)`.**
The plan named only `clampWidth` and `detectWidth`, but check 10 ("ioctl fails +
`COLUMNS=52` → 52") is not reachable through `detectWidth()` alone: whether the
harness' stdout is a tty is not under the test's control, so the ioctl branch
cannot be forced to fail. Extracting the chain as a pure function makes check 10
hermetic. Same split style as `styleFor`.

`stripSgr()` added as the inverse the band's suppression check leans on
(check 22 compares a colored run, stripped, against a colorless one).

### Phase 2 — band content

`src/band.{hpp,cpp}` added to twcore. Read-only by contract, stated in the
header comment in `render.cpp:1-3`'s style.

**Design choice — styled spans, not pre-colored strings.** Row content is a
`vector<BandSpan>` (text + color + bold + separator) rather than a `std::string`
as the plan sketched. This is what makes "color never enters the width
arithmetic" structural rather than disciplined: layout measures plain code
points and styling is applied at emission. The plan's `BandRow{label, content}`
shape would have forced either a post-layout re-scan for substrings to color, or
escape bytes inside the measured text — the exact bug step 9's gate exists to
catch.

**Bug found and fixed by check 11 — the surviving comma.** First implementation
folded each field's separator into one `pad` string and right-trimmed at a line
break. For `", "` that leaves the *comma* behind while the arithmetic assumed the
whole separator had gone, so `east, west,` emitted 11 columns into a 10-column
field at width 20. Fixed by splitting the separator into **glue** (visible,
counted, stays with its word) and **gap** (whitespace, dropped at a break). The
distinction is now documented at the `Token` struct, because it is not obvious
and it is exactly what REQ-UI-33 turns on.

**Divergence taken — HP renders as `HP: 12/12`, not `HP 12/12`.** Plan Decision
3's golden shows no colon. Following it literally would have broken three
*further* existing assertions (`tests.cpp:1484` `contains(out, "HP: 12/12")` and
its neighbours in `testCombatStatusLine`), pushing step 10's gate past its
budgeted "one deleted block + three inverted assertions" and producing a false
REQ-UI-7 signal. The plan marks the SGR table normative and the golden band
illustrative, so the colon is within bounds. The seven golden bands in
`testBandGoldens` are authored against the actual layout.

**Reading recorded — over-long words overflow.** REQ-UI-33 says the band never
emits a line wider than the detected width; REQ-UI-33a forbids truncation. At
width 20 the content field is 10 columns, and a name like `rime-touched` cannot
satisfy both. Resolved the way REQ-UI-27's own rationale does — "only a single
word longer than the floor can overflow" — so rows wrap at every whitespace,
including inside a two-word monster name, and a single over-long word takes its
own line rather than being split or dropped.

Golden bands were baselined by running the suite with `TW_DUMP_BANDS=1`, which
prints the actuals instead of asserting. That switch stays in the test for
deliberate re-baselining.

### Phase 3 — wiring

`render.cpp:222` and `prose.cpp:148` appends deleted, `playerEntity` removed
from `render.cpp` with the former, `combat.hpp` include dropped from both.
`runTurn` split into a file-local `runTurnCore` plus an outer wrapper that owns
`profileNextTurn()`, the `total` stage, wrapping, and the single `composeBand`
call. `renderStartup` gained the same treatment.

**Empirical form of check 3.** Wiring the band broke exactly seven assertions,
and the classification is the whole point of the check:

| Site | Class | Sanctioned by |
|---|---|---|
| `tests.cpp:1482, 1608, 1631` | `!contains(…, "HP:")` → `contains` | REQ-UI-15, plan finding 1 |
| `tests.cpp:1506-1518` | byte-identity block, deleted | REQ-UI-7a |
| `tests.cpp:3592, 3601, 3602` | `runTurn output == render(db, N)` | **not in the plan** — see below |

**Zero assertions on `render()`'s own output text changed**, which is the
substance of REQ-UI-7: every verb template is byte-identical.

**Gap in the plan's finding 1 — three more edits.** Finding 1 enumerated only
the `!contains("HP:")` assertions. `testProseNarrationEnabled` also pins
`runTurn` output equal to `render()` exactly (three assertions). That identity is
precisely what REQ-UI-1/-3/-4 ends — runTurn now wraps and appends — so they are
restated as the composition (`wrapProse(render(...), w) + composeBand(db, w)`)
rather than deleted. Their original purpose survives: the text still derives from
`render()`, not from a model. Step 10's gate should read **"one deleted block,
three inverted HP assertions, three restated composition assertions, zero
modified `render()` output assertions"**.

**Added beyond the plan — `termSetWidthOverride(int)`.** Once `runTurn` calls
`detectWidth()`, every `runTurn`-based assertion in the suite depends on the
developer's terminal: a piped run lays out at 80, a tty run at whatever the
window is, and substring assertions on wrapped prose fail differently in each.
The suite pins 80. Shape and rationale mirror `profileSetSink()`; the game binary
never calls it.

**Same hazard, worse — the suite also had to pin the terminal STYLE.** The first
wired run emitted `-- \x1b[1mcell\x1b[0m ---` inside the test suite, because
Claude Code (and most CI runners) allocate a pty, so `isatty(STDOUT_FILENO)` is
true and color was on. The suite now sets `TERM=dumb` and refreshes the cache
alongside the width pin. Without it, the band assertions were silently
environment-dependent.

`spells` wired across five files as planned. One further test edit was required
and is expected: `testNlResolveRequestBody`'s tool-enum assertion gained
`"spells"` — an `nlresolve` assertion, not a `render()` one.

### Phase 4 — group G

Shipped. Both gating checks pass: `SCHEMA_VERSION` is still 5, and the
`sqlite_master` table list is byte-identical to the pre-feature one, so
REQ-UI-45's "defer rather than bump" never triggered.

`damageEntity` gained a defaulted trailing `detail`; only `combat.cpp:443`
passes one. The `mutations.hpp` contract comment now states that `events.detail`
carries two kinds of value, and that adding a third tagged verb means updating
the `prose.cpp` shield too.

The wildcard regression test is in and passes: a hand-inserted
`goblinXgrunt|fire` event does not leak into `discoveredResistances("goblin_grunt")`.
A `LIKE ? || '|%'` implementation would fail it.

### Step 15 — final validation

Full suite: **5239 checks, 0 failures**. Clean build, no warnings.

**Check 7 caught a real bug — bold was leaking into pipes.** `styleFor` set
`attrs = true` in every case but `TERM=dumb`, so a redirected run emitted
`\x1b[1m` around the room name. Check 7 demands *zero* escape bytes from a piped
binary. Fixed by making `attrs` a **capability** test evaluated independently of
the color precedence: `attrs = TERM != dumb AND (isTty OR CLICOLOR_FORCE)`.
REQ-UI-23 still holds — NO_COLOR keeps bold on a *terminal* — it just no longer
resurrects bold into a pipe. Verified against the real binary: 0 escape bytes
piped, 16 under `CLICOLOR_FORCE=1`, and the two runs identical once stripped.
This is the one bug the unit tests could not have found, because the pure
predicate was being asserted against its own restatement.

All 35 spec checks map to a passing test. Two deliberate deviations, both
predicted by the plan's findings: check 3 is scoped as above (and widened by
three), and check 26b is scoped to *successful* commands, since the unparseable
line and the denied cast already emitted output without an events row before
this feature — the precedent REQ-UI-39a itself cites.

## Observations for follow-up

- **The `"> "` prompt overflows the wrap by two columns.** `main.cpp` writes the
  prompt without a trailing newline, so the first line of every response renders
  as `"> " + <a line already wrapped to the full width>`. At `COLUMNS=48` that is
  a 49-column line. The band itself is exactly 48. Not a REQ-UI-33 violation
  (the band never emits an over-wide line) and not in scope, but it is visible.
- **Spec fix worth making independently:** REQ-UI-20's table has rows 3 and 4
  with identical results, making row 3 dead unless `CLICOLOR=0` suppresses.
  Implemented as a suppression; the spec should say so.
- **Spec fix, already noted by the plan:** check 26b contradicts REQ-UI-39a.
- The band duplicates `render()`'s `Exits:` / `You see:` lines on move turns, as
  the plan's "Out of scope, noted" section anticipated. It is now visible on
  every move — see the piped output in the validation run. Worth its own decision.
