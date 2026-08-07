---
title: "Implementation plan: terminal-status-band-ui"
date: 2026-08-02
status: executed
tags: [plan, ui, terminal, ansi-color, no-color, status-band, prose-wrapping, combat-legibility, resistance-discovery]
modules: [term, band, render, loop, combat, main, mutations]
related: [.lore/work/specs/terminal-status-band-ui.md, .lore/work/research/terminal-ui-status-band.md, .lore/work/brainstorm/ui-improvements.md]
---

# Implementation plan: terminal-status-band-ui

Builds the engine-authored status band, the width/wrapping plumbing it needs, the
combat information that makes the locks-and-keys puzzle legible, and resistance
discovery. Source of truth: **[.lore/work/specs/terminal-status-band-ui.md]**
(56 requirement anchors across REQ-UI-1..48 including the eight lettered
sub-requirements, prefix `UI`; 35 validation checks).

The spec is deliberately comprehensive and explicitly delegates atomic sequencing
here. This plan slices it into **15 steps across 4 phases**, every one
deterministic — **no step requires a live model**. Phase 4 (group G) is gated at
the end and droppable without unpicking anything above it.

## Guiding constraints (from memory + spec)

- **Skeleton first, content second, color last.** Steps 1–4 build terminal
  services and the band's *shape* with no game data in them; content lands on a
  working frame. Color is applied only once the plain-text golden bands pass, so
  a color bug can never be confused with a layout bug.
- **Zero live-LLM verification.** Per [[verification-must-be-bounded]], the whole
  plan is unit-testable offline. The one AI-adjacent step (12's model-facing verb
  enum) is asserted by substring against the prompt, the way `testArchitectPrompt`
  already does.
- **One seam / one file / one testable behavior per step.**
- **Reuse proven seams.** `profileRefreshEnabled()` is the precedent for a cached
  env gate; `ScopedEnvVar` (`tests.cpp:2228`) is the precedent for hermetic env
  tests; `combatStatusLine()` survives as the readiness oracle (REQ-UI-6a).

---

## Findings that change the spec's assumptions

Three things surfaced during codebase exploration that the spec does not account
for. **Read these before approving** — the first one contradicts a validation
check as written.

### 1. Check 3 is not achievable as written — REQ-UI-15 forces three more test edits

Check 3 says the existing suite passes with *exactly one deleted block* and *no
modified assertions*. REQ-UI-15 ("HP is shown on every band, in and out of
combat") directly falsifies three existing assertions:

| Site | Assertion | Why it breaks |
|---|---|---|
| `tests.cpp:1480` | `CHECK(!contains(runTurn(db, "look").output, "HP:"))` | out-of-combat look now carries an HP row |
| `tests.cpp:1606` | same, in `testCombatRender` | same |
| `tests.cpp:1629` | `CHECK(!contains(out, "HP:"))` after the killing blow | combat ends, but the band still shows HP |

These are `runTurn()` assertions, **not `render()` assertions**, so they are *not*
REQ-UI-7 violations — REQ-UI-7 governs `render()`'s event-line output, which this
plan leaves byte-identical. But check 3's blanket "no modified assertions" wording
forbids them anyway.

**Resolution taken:** these three are inverted (from `!contains` to `contains`)
rather than deleted — they still test something real, namely that the HP row is
now unconditional. Check 3's intent is preserved by scoping it: *no `render()`
assertion is modified; exactly one block is deleted; exactly three `runTurn`
HP-absence assertions are inverted, all three attributable to REQ-UI-15.* Step 10's
gate enforces that number exactly. **Any fourth edit is a genuine REQ-UI-7 signal.**

### 2. Check 26b is already false today

Check 26b asserts spell inspection is *the only* path emitting player-visible text
without an `events` row. Two paths already do this: the unparseable-line refusal
(`loop.cpp:64`) and the denied-cast refusal (`loop.cpp:78`). Both return `NoTick`
with `renderError` text and no event.

**Resolution taken:** the check is scoped in step 12 to *successful* commands —
"every command that resolves inside the tick writes an `events` row; spell
inspection is the only path producing **successful** output without one." The
pre-existing refusals are the precedent REQ-UI-39a itself cites, not a violation.

Note this is a **spec-internal** contradiction, not something the plan discovered
about the code: REQ-UI-39a names `loop.cpp:64` and `:78` as precedent, and check
26b then asserts they do not exist. Worth correcting in the spec independently of
this plan.

### 3. Group G's real cost is a contract change on `events.detail`, not the logic

`prose.cpp:308` does `if (!detailIsNull) e["detail"] = detail;` — **every event's
detail is handed to the narrator model as a fact.** Damage events pass `nullptr`
today, so nothing goes. Writing `goblin_grunt|fire` there would put a raw archetype
tag in front of the model on every elemental hit, which it may echo into prose
(violating REQ-UI-25's spirit and REQ-PROSE-11's no-new-nouns rule).

The fix is one condition, but it changes what `events.detail` *is*: today a
human-readable, model-facing fragment; after group G, also an engine-internal tag
the model must be shielded from. **Step 13 makes that change explicitly and tests
it**, rather than letting it ride inside the `damageEntity` edit.

**Silver lining that shrinks group G:** only `burned` and `froze` route through
`resistedDamage` (`combat.cpp:443`). Basic attack, `aoe`, `dot`, `chip`, and
`struck` are not resistance-scaled, so they teach nothing and carry no tag. Group G
touches **one** damage call site, not six.

---

## Seams this touches (verified in tree)

| Seam | File:line | What this feature does with it |
|------|-----------|-------------------------------|
| status append (template path) | `src/render.cpp:222` | **removed** (REQ-UI-6); `playerEntity` at `:26` becomes unused → removed with it |
| status append (AI path) | `src/prose.cpp:148` in `deterministicAppends` | **removed** (REQ-UI-6) |
| `combatStatusLine()` | `src/combat.cpp:477` | **survives as helper** (REQ-UI-6a); the readiness oracle for check 20 |
| `runTurn` return paths | `src/loop.cpp:64, 69, 78, 105, 116, 119` | wrapped by an outer function that wraps prose + appends the band — **one** site (REQ-UI-3/-4/-6) |
| `renderStartup` | `src/loop.cpp:122` | gains the band (REQ-UI-5); tests at `tests.cpp:5226,5235` use `contains`, so they survive |
| `Verb` enum | `src/action.hpp:11` | `Verb::Spells` appended |
| fixed-verb parser | `src/parser.cpp:48-51` | `spells` joins the no-argument verbs |
| model ISA verb list | `src/nlresolve.cpp:93-102` (`verbFromWord`), `:189` (tool enum) | `spells` added to both, kept in step |
| model system prompt | `src/nlresolve.cpp:131` — "exactly **ten** verbs" + per-verb prose | count corrected to eleven, `spells` described (step 12) |
| `resolveImpl` switch | `src/systems.cpp:193` | `Verb::Spells` throws like `Quit` — it must never reach the tick |
| `damageEntity` | `src/mutations.cpp:63` | gains an optional `detail`; only `combat.cpp:443` passes one |
| model-facing detail | `src/prose.cpp:308` | shielded for the two tagged verbs (step 13) |
| `resistance` table | `src/world.cpp:40` | read for group G display; **no DDL, no schema bump** (REQ-UI-45) |
| twcore sources | `CMakeLists.txt:17` | add `src/term.cpp`, `src/band.cpp` |
| test runner | `tests/tests.cpp:6428` `main()` | register each new `testTerm*` / `testBand*` |

**New translation units:** `src/term.{hpp,cpp}` (color gate, SGR, width, wrapping —
no db) and `src/band.{hpp,cpp}` (composition — SELECTs only, the read-only contract
check 1 asserts).

---

## The three decisions the spec delegated

Fixed here as requested, and reviewable — override any of them at plan review.

### Decision 1 — the inspection verb is `spells`

Single word, no argument, joins `look`/`inventory`/`wait`/`quit` in `parser.cpp`.
Chosen over `spellbook` (implies a takeable object that does not exist) and `study`
(collides with the existing `read` grimoire verb). The model-facing tool enum gets
the same string, so the two verb lists stay literally identical.

### Decision 2 — role→color mapping

Ten roles, ten distinct treatments, no color doing two jobs (REQ-UI-24). Basic
16 only (REQ-UI-19). Every colored fact is also carried by its text.

| Role | Treatment | SGR | Why |
|---|---|---|---|
| Room name header | **bold**, no color | `1` | survives `NO_COLOR` (REQ-UI-23); it is the anchor |
| Exits | cyan | `36` | navigation |
| Objects | green | `32` | takeable world items |
| Hostile name + HP | red | `31` | danger |
| Telegraph `[WINDING UP]` | **bold** + bright red | `1;91` | loudest element (REQ-UI-34); bold survives `NO_COLOR`, text survives `TERM=dumb` |
| Player HP, low | yellow | `33` | warning (REQ-UI-24) |
| Spell ready | bright blue | `94` | your available actions |
| Spell cooling | bright black (grey) | `90` | recedes; unavailable |
| Status effects (either side) | magenta | `35` | active magical state, symmetric for player and enemy |
| Discovered resistance | bright magenta | `95` | knowledge *about* a state — deliberately adjacent to magenta |

The SGR column is normative, not illustrative — step 9's gate asserts these exact
codes per role, because a mapping that is merely *applied* but not *distinct* would
pass every other check while violating REQ-UI-24's "one role per color".

**"Low" HP is `current * 3 <= max`** — integer arithmetic, no floats, matching the
engine's no-float discipline (`world.cpp:40`). This threshold is invented by this
plan; nothing in the spec or the existing engine defines one, so step 9 tests its
boundary explicitly rather than assuming it.

Labels, rules, and punctuation are unstyled. White, blue, bright green, bright
yellow, bright cyan, bright white, and black go unused — "only the basic 16" is a
ceiling, not a quota.

### Decision 3 — column layout

ASCII only (REQ-UI-29). Fixed label column so every row shares one content column,
which makes the hanging indent (REQ-UI-33) uniform and the golden files stable.

- **Header:** `"-- "` + room name + `" "`, then `-` padding to the detected width.
  A room with no `name` row emits `-` × width (REQ-UI-9).
- **Rows:** one leading space, label left-aligned in an 8-wide field, one space,
  then content. **Content column = 11** (1-based).
- **Continuation lines:** 10 spaces, then content — aligned to the content column.
- Hostiles use the fixed label `Enemy` with the name *in the content*, so a long
  monster name never shifts the content column. Multiple hostiles → multiple
  `Enemy` rows in entity order (REQ-UI-13).

At the 20-column floor this leaves 10 content columns — narrow, but it wraps
rather than truncates (REQ-UI-33a), which is the whole point of the low floor.

Golden band at width 60, color disabled:

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">-- Stone Hall ----------------------------------------------
 Exits    north, up
 Objects  lantern, iron key
 Enemy    goblin grunt  HP 6/9  [WINDING UP]  dot 2  slow 1
 You      HP 11/12  Ward: ready  Fire: 2
</pre>

Out of combat the same room collapses to the compact form (REQ-UI-16):

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">-- Stone Hall ----------------------------------------------
 Exits    north, up
 Objects  lantern, iron key
 You      HP 11/12
</pre>

Status effects render as **kind + remaining**, never magnitude (REQ-UI-35):
`dot 2`, `slow 1`, `ward 1`. `barrier` has no duration and renders bare.
Spell readiness reuses `combatStatusLine`'s exact semantics and capitalization so
check 20's oracle comparison is a literal value match.

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 10px 0; font-size: 0.92em;">
<b>PHASE 1 — terminal services (no db)</b><br>
<b>1</b> color gate + SGR ──┐<br>
<b>2</b> width detection ───┼─▶ <b>4</b> band frame + hanging indent ──┐<br>
<b>3</b> prose wrapper ─────┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
<br>
<b>PHASE 2 — band content (plain text, then color)</b><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>5</b> room / exits / objects<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>6</b> hostile rows ──┐<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>7</b> player row ────┼─▶ <b>8</b> combat legibility ─▶ <b>9</b> color<br>
<br>
<b>PHASE 3 — wiring (the behavior change)</b><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>10</b> remove appends + wire runTurn <span style="background:#fff4e5;color:#9a5b00;padding:1px 6px;border-radius:3px;">MED</span><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>11</b> startup band<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>12</b> <code>spells</code> verb + no-tick route <span style="background:#fff4e5;color:#9a5b00;padding:1px 6px;border-radius:3px;">MED</span><br>
<br>
<b>PHASE 4 — group G (gated · droppable)</b><br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>13</b> detail tag + model shield ─▶ <b>14</b> discovery + display<br>
<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>15</b> final validation vs spec
</div>

Risk legend: unmarked = <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fff4e5;color:#9a5b00;padding:1px 6px;border-radius:3px;">MED</span> touches existing asserted behavior. **No step is HIGH — nothing here needs a live model.**

---

## PHASE 1 — terminal services

### Step 1 — Color gate and SGR helpers
**Requirements:** REQ-UI-19, -20, -21, -22, -23. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/term.hpp` / `src/term.cpp`**; add `src/term.cpp` to twcore at
`CMakeLists.txt:17`. No db include — this TU is pure.

Two layers, deliberately split so the truth table is testable without a tty:

- A **pure predicate** `TermStyle styleFor(const char* noColor, const char* clicolorForce, const char* clicolor, const char* term, bool isTty)` returning
  `{bool color; bool attrs;}`. Implements REQ-UI-20's precedence in order, treating
  empty strings and `nullptr` as unset. `TERM=dumb` → `{false, false}` (REQ-UI-21,
  the only case that kills bold). `NO_COLOR` → `{false, true}` (REQ-UI-23).
- A thin `TermStyle currentStyle()` reading the four env vars and
  `isatty(STDOUT_FILENO)`, plus `termRefreshStyle()` mirroring
  `profileRefreshEnabled()` (`profile.cpp:66`) so tests can flip env and re-read.

SGR helpers emit **nothing at all** when the corresponding flag is false —
`std::string colorize(std::string_view, Color, TermStyle)` returns its input
unchanged, never `"\x1b[0m"` around plain text (REQ-UI-22). Colors are the basic
16 SGR codes only: 30–37, 90–97, plus `1` for bold and `0` to reset (REQ-UI-19).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testTermColorGate</code> drives <code>styleFor</code> over the
full cross-product of {<code>NO_COLOR</code> unset/empty/set} × {<code>CLICOLOR_FORCE</code> unset/<code>0</code>/<code>1</code>} ×
{<code>CLICOLOR</code> unset/<code>0</code>/<code>1</code>} × {<code>TERM</code> unset/<code>dumb</code>/<code>xterm</code>} × {tty, not-tty} and asserts
color/attrs per REQ-UI-20/-21 — <b>check 4</b>, including the empty-string cases.
Assert every suppressed combination produces output containing no <code>0x1b</code> byte
(<b>check 5</b>). Assert bold survives <code>NO_COLOR</code> (<b>check 6</b>) and dies under
<code>TERM=dumb</code> (<b>check 6a</b>). Env-mutating cases use <code>ScopedEnvVar</code> (<code>tests.cpp:2228</code>)
so the suite leaves the shell untouched.
</blockquote>

### Step 2 — Width detection with fallback chain and clamp
**Requirements:** REQ-UI-26, -27, -28. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In `term.cpp`, same split: a pure `int clampWidth(int reported)` (floor 20, and the
80 default is supplied by the caller) and `int detectWidth()` doing
`ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` → `COLUMNS` → 80.

The research verified `rc=-1, cols=0` in exactly the piped/CI case, so **both** a
non-zero return *and* a zero `ws_col` fall through (REQ-UI-27). `COLUMNS` is parsed
strictly — any non-numeric or non-positive value falls through to 80 rather than
yielding 0. Queried fresh on each call; no `SIGWINCH` handler (REQ-UI-28).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testTermWidth</code>: with stdout not a tty, <code>detectWidth()</code>
returns non-zero (<b>check 9</b>). <code>COLUMNS=52</code> → 52; <code>COLUMNS</code> unset → 80;
<code>COLUMNS=banana</code>, <code>COLUMNS=0</code>, <code>COLUMNS=-5</code> → 80 (<b>check 10</b>).
<code>clampWidth(10) == clampWidth(1) == clampWidth(0) == 20</code> (<b>check 11a</b>).
</blockquote>

### Step 3 — UTF-8 prose wrapper
**Requirements:** REQ-UI-30, -31, -32. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`std::string wrapProse(const std::string& text, int width)` in `term.cpp`.

Counts **code points, not bytes** (REQ-UI-31) — a small `utf8Length` / lead-byte
scanner, no dependency, no `wcwidth` (the research judged full width measurement
disproportionate for a game with no CJK content). Breaks at whitespace only; a word
longer than `width` overflows rather than being split (REQ-UI-30 forbids mid-word
breaks, and REQ-UI-27's rationale explicitly accepts this single overflow case).
Existing newlines are preserved verbatim, so a blank line stays a paragraph break
and no two source lines are ever joined (REQ-UI-32).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testTermWrap</code>: a paragraph of em-dashes and curly
quotes wraps near the width, not early, and no multi-byte sequence is split —
re-decode every output line and assert it is valid UTF-8 (<b>check 13</b>). No line
ends mid-word for any word shorter than the width (<b>check 14</b>). A two-paragraph
input yields two paragraphs (<b>check 15</b>). A 40-char word at width 20 emits one
over-long line, not a split word.
</blockquote>

---

## PHASE 2 — band content

### Step 4 — Band frame: row model, layout, hanging indent
**Requirements:** REQ-UI-8, -9, -29, -33, -33a, -33b. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/band.hpp` / `src/band.cpp`**; add to twcore. Header comment states
the read-only contract verbatim in `render.cpp:1-3`'s style — **this TU contains
only SELECTs**, which is what check 1 statically asserts.

This step builds the *frame only*, with no db reads yet. An internal
`struct BandRow { std::string label; std::string content; }` and a pure
`std::string layoutBand(std::string_view roomName, const std::vector<BandRow>&, int width)`
implementing Decision 3: header rule, 1-space indent, 8-wide label field, content
column 11, continuation lines indented to column 11 (REQ-UI-33).

Rows with empty content are dropped by the layout function itself, which is what
makes REQ-UI-8's conditional rows fall out rather than being re-implemented per
row-type. **Nothing is ever truncated or elided** (REQ-UI-33a) — the only
width-driven concession is that the header rule degrades to bare padding when the
room name alone exceeds the width (REQ-UI-33b). All framing bytes are ASCII
(REQ-UI-29) — no `─` and no `·`, both of which the research showed are East Asian
Ambiguous.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBandLayout</code>, pure, no db. Across widths 20, 40, 80,
200 with a synthetic row set (many objects, a long exit list), assert <b>no emitted
line exceeds the width</b> (<b>check 11</b>). Continuation lines start at column 11, not
column 0 (<b>check 11c</b>). Every byte of the header rule and separators is
&lt; 0x80 (<b>check 12</b>). An empty <code>roomName</code> renders the rule with no title and does
not throw (<b>check 24</b>, frame half). A row with empty content emits nothing
(<b>check 17</b>, frame half).
</blockquote>

### Step 5 — Room name, Exits, Objects rows
**Requirements:** REQ-UI-9, -10, -11, -14. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

First db reads. `composeBand(Db&, int width)` resolves the player, their room, and
builds the first three rows.

Exits **reuse `roomBlock`'s exact query shape** (`render.cpp:83-88`) — the same
`ORDER BY direction` and the same `dest IS NOT NULL OR architectEnabled()`
visibility gate — so the band and the room block can never disagree, and latent
exits stay textually indistinguishable from realized ones (REQ-UI-10). Objects
reuse `portableNamesIn`'s shape (`render.cpp:44-54`), entity order, room as
container (REQ-UI-11). The room's *description* is deliberately not read — the band
repeats the name, never the prose (REQ-UI-14).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBandContent</code> against <code>tests/fixture.sql</code>: header
carries the room name; Exits and Objects match the room block's lists exactly.
Moving to an empty room drops both rows entirely (<b>check 17</b>). With the architect
disabled a latent exit is absent; enabled, it appears with no distinguishing
marker — the <code>testExitDisplayInvariant</code> pattern (<code>tests.cpp:2253</code>) applied to the
band (<b>check 18</b>). A room whose <code>name</code> row is deleted renders without throwing
(<b>check 24</b>). Assert the room's description text appears nowhere in the band.
</blockquote>

### Step 6 — Hostile rows
**Requirements:** REQ-UI-12, -13. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

One `Enemy` row per **living** hostile sharing the player's room, name plus
`HP cur/max` (REQ-UI-12). The query reuses `resolveCombat`'s swarm shape
(`combat.cpp:525-529` — join `hostile` to `location`, `ORDER BY h.entity`) so band
order and combat order are the same order by construction (REQ-UI-13).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> extend <code>testBandContent</code> against <code>tests/combat_fixture.sql</code>:
one hostile → one row with correct current/max HP (<b>check 12</b>). Three hostiles →
three rows whose order matches the entity ids <code>resolveCombat</code> iterates
(<b>check 23</b>). A hostile at 0 HP produces no row.
</blockquote>

### Step 7 — Player row: HP always, readiness in combat
**Requirements:** REQ-UI-15, -16, -17, -18. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

The `You` row. HP renders **unconditionally** (REQ-UI-15) — this is the behavior
change that forces the three test edits in finding 1.

"In combat" calls **`hostileInRoom(db, room) != 0`** — the engine's own predicate,
already exported at `combat.hpp:61` — so the band and combat resolution cannot
disagree (REQ-UI-18). Out of combat: HP only (REQ-UI-16). In combat: HP plus
per-known-spell readiness, reproducing `combatStatusLine`'s semantics exactly
(`combat.cpp:495-511`) — alphabetical by spell, first letter capitalized, `ready`
when `ready_turn - now <= 0` **or** no `cooldowns` row exists, else the integer
remaining (REQ-UI-17).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> out of combat, HP present and no spell readiness; in combat,
both (<b>checks 19</b>, REQ-UI-16/-17). <b>Check 20 — the oracle test:</b> for the same
world state, every readiness value in the band equals the value
<code>combatStatusLine(db, player)</code> returns, parsed out of its line. This is the payoff
of keeping the helper alive per REQ-UI-6a, and it is the gate that catches a
readiness reimplementation drifting from the original.
</blockquote>

### Step 8 — Combat legibility: telegraph and active states
**Requirements:** REQ-UI-34, -35, -36, -40. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Extend the `Enemy` and `You` rows.

- **Telegraph:** a `pending_strike` row for that hostile → `[WINDING UP]` on its
  row (REQ-UI-34). Text alone identifies it, which is what keeps it legible under
  `TERM=dumb` where step 9's bold is stripped (check 6a).
- **Enemy states:** `barrier` renders bare; each `status_effects` row renders as
  **kind + `remaining`**, never `magnitude` (REQ-UI-35) — the player can act on
  duration, not on magnitude. Long rows wrap per step 4 rather than dropping states.
- **Player states:** the same treatment on the `You` row, notably `ward`
  (REQ-UI-36), since it decides whether an incoming telegraphed strike lands.

Every number here is read from the world (REQ-UI-40) — `health`, `status_effects.remaining`,
`pending_strike`. No model-supplied quantity can reach the band, structurally: this
TU never sees narration text.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> with <code>pending_strike</code> present the marker appears; absent, it
does not (<b>check 25</b>). A hostile carrying a DoT and a slow shows each kind with
its remaining turns and <b>no magnitude</b> — assert the magnitude value's digits are
absent from the row when magnitude ≠ remaining (<b>check 26c</b>). A warded player
shows <code>ward</code> on the <code>You</code> row. Golden bands for all seven fixed states named in
<b>check 16</b> (empty room; objects; one hostile; three hostiles; mid-telegraph;
barrier; player warded), asserted as exact strings with color disabled.
</blockquote>

### Step 9 — Apply color
**Requirements:** REQ-UI-19, -22, -24, -25. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Wire step 1's helpers into the composed rows per Decision 2's mapping. Color is
applied **after** layout so escape bytes never enter the width arithmetic — a
sequence is zero columns wide, and counting it would silently shorten every colored
line. This ordering is the single most likely source of a width bug; the gate below
is what catches it.

Every colored fact remains present in the text (REQ-UI-24): `[WINDING UP]` is a
word, HP is digits, `ready` is a word. Color never touches narration — the band TU
is the only styling site and it never receives prose (REQ-UI-25).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBandColor</code>.
<b>Per-role mapping (REQ-UI-24)</b> — with color forced on, assert each role's content
is wrapped in <i>its own</i> SGR code from Decision 2's normative table: Exits in <code>36</code>,
Objects in <code>32</code>, hostile name/HP in <code>31</code>, telegraph in <code>1;91</code>, a ready spell in
<code>94</code>, a cooling spell in <code>90</code>, a status effect in <code>35</code>. Then assert the ten codes
used are <b>pairwise distinct</b> — this is what makes "one role per color" a test
rather than a comment, and it is the gate the previous draft was missing.
<b>Low-HP threshold</b> — three fixtures at <code>current*3 &lt; max</code>, exactly <code>current*3 == max</code>,
and <code>current*3 &gt; max</code>: the first two yellow (<code>33</code>), the third unstyled. Boundary is
tested because the plan invented the threshold.
<b>Ordering</b> — re-run step 4's width sweep with color forced on and assert no line
exceeds the width once escape sequences are stripped; a band that counted escape
bytes as columns fails here.
<b>Suppression</b> — with color suppressed, the golden bands from step 8 are
byte-identical to the colored run with sequences stripped (<b>check 22 / REQ-UI-22</b>).
<b>Check 11b</b> — the same fact set is present at width 200 and width 20, the
executable form of REQ-UI-33a and the check most likely to catch a truncation
shortcut.
</blockquote>

---

## PHASE 3 — wiring

### Step 10 — Remove both appends; wire the band into `runTurn`
**Requirements:** REQ-UI-1, -2, -3, -4, -6, -6a, -7, -7a, -30. **Size:** M · **Token-risk:** <span style="background:#fff4e5;color:#9a5b00;padding:1px 6px;border-radius:3px;">MED</span>

The behavior change. Three edits:

1. **`render.cpp`** — delete the append at `:222` and the now-unused `playerEntity`
   at `:26`. Everything above `:222` is untouched, so every verb template stays
   byte-identical (REQ-UI-7).
2. **`prose.cpp`** — delete the append at `:148` inside `deterministicAppends`.
3. **`loop.cpp`** — rename the existing body to a file-local `runTurnCore`, and
   give `runTurn` this shape:

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">TurnResult runTurn(Db&amp; db, const std::string&amp; line) {
    profileNextTurn();
    const ScopedStage totalStage("total");
    TurnResult r = runTurnCore(db, line);
    if (r.outcome == TurnOutcome::Quit) return r;   // no output, no band
    const int w = detectWidth();
    r.output = wrapProse(r.output, w) + composeBand(db, w);
    return r;
}
</pre>

This gives REQ-UI-1, -3, -4, and -6 **by construction rather than by discipline**:
one site, after every `runTurnCore` return path, band last, identical bytes on the
AI and template paths because there is only one composition. `Quit` returns empty
output and therefore no band, which is exactly REQ-UI-3's "every turn that produces
output". Composition runs after `db.commit()`, outside the tick transaction, and
reads only (REQ-UI-2).

`profileNextTurn()` and the `total` stage move to the **outer** function so band
composition and wrapping are inside the measured turn — otherwise `total` would
under-report from this release onward. The `resolve` / `tick` / `narrate` stage
names and nesting are unchanged.

**Composition is wrapped in a `try/catch`** returning an empty band on exception.
The spec does not address a throwing band, but REQ-UI-3 demands one on the
`EngineError` path — where the world was just rolled back — and REQ-UI-9's "render
without a title rather than failing" establishes the degrade-don't-throw posture. A
band that crashed the turn it was meant to explain would be worse than no band.
Because this is control flow the spec never asked for, the gate below tests it
directly rather than trusting it.

<blockquote style="border-left:4px solid #fff4e5;border-left-color:#9a5b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <b>Check 1</b> — static: <code>band.cpp</code> contains no <code>INSERT</code>/<code>UPDATE</code>/<code>DELETE</code>
(the same source-scan style as <code>tests.cpp:1321</code>'s RNG check); <code>meta.turn</code> and the
<code>events</code> row count are unchanged across N <code>composeBand</code> calls. <b>Check 2</b> — with
narration stubbed to succeed and to fail on the same state, the appended band bytes
are identical, and a source scan finds exactly <b>one</b> <code>composeBand</code> call site.
<b>Check 21</b> — an unparseable line and a denied cast each emit a band and neither
increments <code>meta.turn</code>. <b>Degrade path</b> — delete the <code>player</code> row so
<code>composeBand</code> throws, then assert <code>runTurn</code> still returns its narration text with an
empty band and does <b>not</b> propagate; and that the <code>EngineError</code> path (force a
mid-tick exception) likewise still returns its message. <b>Check 3, as scoped in finding 1</b> — diff <code>tests/tests.cpp</code>
and assert exactly: one deleted block (<code>:1506-1518</code>, REQ-UI-7a) and exactly three
inverted <code>runTurn</code> HP assertions (<code>:1480</code>, <code>:1606</code>, <code>:1629</code>), <b>zero</b> modified
<code>render()</code> assertions. Full suite green. <b>A fourth edit fails this gate</b> and means
REQ-UI-7 was broken.
</blockquote>

### Step 11 — Startup band
**Requirements:** REQ-UI-5, -30. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`renderStartup` (`loop.cpp:122`) wraps its room render and appends the band, so
`main.cpp:40` stays a single `fputs` and the binary keeps exactly one band-emitting
site (REQ-UI-6). The band lands after the room render and before the first `"> "`
prompt at `main.cpp:49`.

Existing callers at `tests.cpp:5226,5235` use `contains`, so they pass unmodified —
verified, and worth noting because it is the one place a startup change could have
cost a fourth test edit against step 10's gate.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> a fresh world's <code>renderStartup</code> output contains a band, and
the band's header names the starting room (<b>check 22 / REQ-UI-5</b>). The two
existing <code>testPlayerRoom</code> assertions pass unmodified.
</blockquote>

### Step 12 — `spells` verb and the no-tick route
**Requirements:** REQ-UI-37, -38, -39, -39a, -39b. **Size:** M · **Token-risk:** <span style="background:#fff4e5;color:#9a5b00;padding:1px 6px;border-radius:3px;">MED</span>

The spec calls this "the one novel piece of plumbing" — every existing verb resolves
inside the tick, and both current `NoTick` returns are refusals, not successful
output. Five coordinated edits:

1. **`action.hpp:11`** — append `Spells` to `Verb`.
2. **`parser.cpp:48-51`** — `spells` joins the no-argument verbs.
3. **`nlresolve.cpp:93-102` and `:189`** — `"spells"` added to `verbFromWord` **and**
   the tool-enum array, plus a clause in the `:321` no-argument verb group. These two
   lists must stay identical; the gate asserts it.
3b. **`nlresolve.cpp:131`, `kResolveSystemPrompt`** — the prompt states "The
   instruction set has exactly **ten** verbs" and then describes each of the ten.
   Correct the count to eleven and add a `spells` line. Without this the JSON schema
   would accept an eleventh enum value the prompt never mentions, so the model would
   never emit it and inspection would work *only* via the fixed-parser word — a
   silent half-wiring, not a design choice.
4. **`systems.cpp:193`** — `case Verb::Spells:` throws `std::logic_error` exactly as
   `Verb::Quit` does. It must never reach the tick, and throwing surfaces a routing
   bug immediately instead of silently costing a turn.
5. **`loop.cpp`, in `runTurnCore`** — immediately after the `Quit` check, before the
   `Cast` gate:

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">if (action-&gt;verb == Verb::Spells) {
    return {TurnOutcome::NoTick, renderSpellRules(db, playerId(db))};
}
</pre>

No transaction opens, `meta.turn` is untouched, no `events` row is written, and
`resolveCombat` never runs — so no hostile takes a turn (REQ-UI-39). Step 10's
wrapper still appends the band, so the player sees the fight state alongside the
rules.

`renderSpellRules` lives in `band.cpp` (read-only, same contract) and lists **only
`known_spells` for that player** joined to `spell_catalog` — element, cooldown, and
effect (REQ-UI-37). Unlearned spells are never enumerated; the catalog is not a
spoiler list (REQ-UI-38). `effect` is a keyword, and the catalog defines **seven** of
them — `ward`, `stun`, `damage`, `frost`, `dot`, `dispel`, `aoe`
(`seed/base.sql:115-122`) — so a fixed engine-authored gloss table maps each to one
plain sentence. All seven must be present: `stun` is a real, currently-learnable
spell (`combat.cpp:432`), and an incomplete table would leave a player who knows it
staring at a blank effect. An unrecognized keyword falls back to the raw keyword
rather than rendering empty, so a future spell added to the catalog degrades instead
of vanishing. A `NULL` element renders as `none`, not blank.

Header comments on both the `loop.cpp` branch and `renderSpellRules` record
REQ-UI-39b verbatim: **this exception must not generalize.**

<blockquote style="border-left:4px solid #fff4e5;border-left-color:#9a5b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> learn one spell; inspection shows its element, cooldown, and
effect, and an unlearned spell is absent (<b>check 26</b>, REQ-UI-38).
<b>Gloss completeness:</b> learn <i>all seven</i> catalog spells and assert every rendered
effect is a non-empty gloss, not a bare keyword and not blank — the direct guard on
the <code>stun</code> omission. <b>Check 26a</b> — inspect
with a hostile in the room and assert <b>all four</b>: <code>meta.turn</code> unchanged, <code>events</code>
row count unchanged, player HP unchanged, and no telegraph advanced. Turn cost is
not observable from the counter alone; the enemy must also not have acted.
<b>Check 26b, as scoped in finding 2</b> — every command that resolves <i>inside the tick</i>
writes an <code>events</code> row; <code>spells</code> is the only <b>successful</b> path producing output
without one. Assert <code>verbFromWord</code>'s verb set and the tool-enum array are
element-wise equal, so the two lists cannot drift.
</blockquote>

---

## PHASE 4 — group G (resistance discovery)

> **Gated and droppable.** Nothing in phases 1–3 depends on these two steps. If
> step 13's gate fails, drop both and ship A–F: REQ-UI-45 says defer rather than
> bump the schema, and there is no schema change here to defer.

### Step 13 — Tag elemental damage in `events.detail`; shield it from the model
**Requirements:** REQ-UI-44a, -45, -46. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Two halves, deliberately in one step because shipping the first without the second
leaks archetype tags into narration (finding 3).

**Half A — write the tag.** `damageEntity` (`mutations.cpp:63`) gains a trailing
`const char* detail = nullptr` and passes it through to its `appendEvent` at `:96`
instead of the hardcoded `nullptr`. Only **`combat.cpp:443`** passes one:
`archetype|element`, e.g. `goblin_grunt|fire`. That is the sole call site routed
through `resistedDamage`, so it is the only damage that teaches anything. The other
five sites (`:138` dot, `:345` attacked, `:471` aoe, `:557` struck, `:582` chip) are
not resistance-scaled and are **untouched** by the default argument.

No DDL. `events.detail` is declared free TEXT, "human-readable fragment or NULL"
(`world.cpp:60`), so `SCHEMA_VERSION` does not move (REQ-UI-45) and the `events`
table stays the single source of truth (REQ-UI-46). A pre-feature world file
carries `NULL` details on old damage events and therefore legitimately starts with
nothing discovered (REQ-UI-44a) — no backfill is attempted, and none is possible,
since defeat already destroyed the entity→archetype link.

**Half B — shield the model.** `prose.cpp:308` currently sends *every* non-null
detail to the narrator as a fact. Gate it so the two tagged verbs are excluded:

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">if (!detailIsNull &amp;&amp; verb != "burned" &amp;&amp; verb != "froze") e["detail"] = detail;
</pre>

`render.cpp:172-179` already ignores `detail` for both verbs (it reads `subject` and
`object`), and `aiRender`'s clause d (`prose.cpp:262`) only checks `failed` details,
so nothing else observes the change. Record in the `mutations.hpp` header comment
that `events.detail` now carries **two** kinds of value — a model-facing fragment,
and an engine-internal tag on `burned`/`froze` — because that is a real contract
change and the next person to add a detail needs to know which kind they are writing.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> cast fire at a hostile; the <code>burned</code> event's <code>detail</code> is
<code>&lt;archetype&gt;|fire</code>. The other five damage verbs still write <code>NULL</code>. <b>The shield
test:</b> build the narrator payload for that turn and assert the archetype tag
appears <b>nowhere</b> in it — the regression guard for finding 3.
<b>Check 31</b> — <code>SCHEMA_VERSION</code> is unchanged and a <code>world.db</code> written before this
change still opens without <code>SchemaMismatch</code>. <b>Check 32</b> — enumerate
<code>sqlite_master</code> and diff against the pre-feature table list: <b>no new table</b>.
This is the gate that decides whether group G ships.
</blockquote>

### Step 14 — Derive discovery; render it on the hostile row
**Requirements:** REQ-UI-41, -42, -42a, -43, -44. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`discoveredResistances(Db&, const std::string& archetype)` in `band.cpp` — read-only,
derived, no cache:

<pre style="background:#f6f8fa;padding:10px;border-radius:4px;overflow-x:auto;line-height:1.45;">SELECT DISTINCT detail FROM events
 WHERE verb IN ('burned','froze') AND detail IS NOT NULL
</pre>

then split each `detail` on the **first** `|` in C++ and compare the archetype half
with `==`.

**Do not filter with `LIKE ? || '|%'`.** SQLite's `LIKE` treats `_` as a
single-character wildcard, and the seed's only archetype is `goblin_grunt`
(`seed/base.sql:102`) — so that pattern also matches `goblinXgrunt|fire` for any
`X`, silently attributing one archetype's discoveries to another. `ESCAPE` would fix
it, but splitting in code removes the entire wildcard class of bug and costs
nothing at this table size. Splitting on the *first* separator also keeps archetype
names containing `|` from corrupting the parse.

Each row yields a tested element. For each, look up `resistance(archetype, element)`
and render:

| State | Renders | Why |
|---|---|---|
| never tested | *nothing* | REQ-UI-42 — the table is never surfaced wholesale |
| tested, resistance row exists | `fire x1/2`, `frost x2` | the discovered ratio |
| tested, **no** resistance row | `fire x1` | REQ-UI-42a — a discovered *absence*, positive and textually distinct from omission |

That third case is the requirement most easily missed: a missing `resistance` row
means neutral, and neutral-after-testing must not look the same as untested, or the
mechanic teaches nothing.

Keyed to the **archetype string**, never the entity, so discovery survives the
defeat that deletes the `hostile` row (REQ-UI-43) and applies to every later
instance. Persistence across restarts is free — `events` is on disk (REQ-UI-44).
Rendered on the `Enemy` row, wrapping per step 4 rather than dropping states.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> fresh world facing an archetype shows no resistance info
(<b>check 27</b>). Hit it with fire → its fire resistance shows and frost stays hidden
(<b>check 28</b>). <b>Check 34</b> — hit an archetype with an element it does not resist;
the result shows as an explicit <code>x1</code>, textually distinguishable from an element
never tried. <b>Check 29</b> — discover, kill, meet a <i>new instance</i> of the same
archetype; still known. <b>Check 30</b> — reopen the world file; persists.
<b>Check 33</b> — delete the <code>events</code> rows for that fight, reopen, assert the discovery
is <b>gone</b>; survival would prove state is stored outside the transcript
(REQ-UI-46). <b>Check 35</b> — a <code>world.db</code> carrying pre-feature <code>NULL</code>-detail damage
events opens with zero discovered resistances and no crash.
<b>Wildcard regression:</b> hand-insert a <code>burned</code> event with detail
<code>goblinXgrunt|fire</code> and assert <code>discoveredResistances("goblin_grunt")</code> returns
<b>empty</b>. This fails against a <code>LIKE</code>-based implementation and passes against the
split-in-code one; the fixtures have only one archetype, so without this test the
bug would ship invisibly until a second underscore-bearing archetype existed.
</blockquote>

---

### Step 15 — Final validation against the spec
**Requirements:** all. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's 35 checks against the suite and confirm each is covered by a named
test. Confirm the requirement coverage map below has no gaps. Run the full suite;
run the binary with stdout piped and assert zero escape bytes, then with
`CLICOLOR_FORCE=1` and assert they return (**check 7** — the only check that needs
the real binary rather than the test harness). Play a few turns interactively at
two terminal widths and confirm the band reads as intended.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every one of the 35 checks maps to a passing test or a
recorded deliberate deviation (findings 1 and 2 are the two known ones). Full
suite green. <b>Check 8</b> — with narration stubbed to return text containing an
entity name, assert no escape byte appears within the narration segment
(REQ-UI-25). Non-goals in REQ-UI-47 confirmed absent: no alternate screen, no
DECSTBM, no line editing, no third-party UI dependency added to <code>CMakeLists.txt</code>.
</blockquote>

---

## Requirement coverage map

| Requirement | Step | Requirement | Step |
|---|---|---|---|
| REQ-UI-1 band composed by `runTurn` | 10 | REQ-UI-26 ioctl width | 2 |
| REQ-UI-2 read-only, outside tick | 10 | REQ-UI-27 fallback + clamp 20 | 2 |
| REQ-UI-3 every output turn | 10 | REQ-UI-28 re-query at print time | 2, 10 |
| REQ-UI-4 last before prompt | 10 | REQ-UI-29 ASCII rules | 4 |
| REQ-UI-5 startup band | 11 | REQ-UI-30 wrap prose | 3, 10, 11 |
| REQ-UI-6 both appends removed | 10 | REQ-UI-31 count characters | 3 |
| REQ-UI-6a helper survives | 7, 10 | REQ-UI-32 preserve paragraphs | 3 |
| REQ-UI-7 `render()` unchanged | 10 | REQ-UI-33 hanging indent | 4 |
| REQ-UI-7a one test retired | 10 | REQ-UI-33a never truncate | 4, 9 |
| REQ-UI-8 conditional rows | 4 | REQ-UI-33b only rules may drop | 4 |
| REQ-UI-9 room name / no name | 4, 5 | REQ-UI-34 telegraph | 8 |
| REQ-UI-10 exits + latent gate | 5 | REQ-UI-35 states, no magnitude | 8 |
| REQ-UI-11 objects row | 5 | REQ-UI-36 player states | 8 |
| REQ-UI-12 hostile HP | 6 | REQ-UI-37 spell inspection | 12 |
| REQ-UI-13 swarm order | 6 | REQ-UI-38 known spells only | 12 |
| REQ-UI-14 no description | 5 | REQ-UI-39 no-tick | 12 |
| REQ-UI-15 HP always | 7 | REQ-UI-39a bounded exception | 12 |
| REQ-UI-16 compact out of combat | 7 | REQ-UI-39b must not generalize | 12 |
| REQ-UI-17 readiness in combat | 7 | REQ-UI-40 engine-authored numbers | 8 |
| REQ-UI-18 shared predicate | 7 | REQ-UI-41 discovery | 14 |
| REQ-UI-19 basic 16 only | 1, 9 | REQ-UI-42 hidden until discovered | 14 |
| REQ-UI-20 gate precedence | 1 | REQ-UI-42a absence is a fact | 14 |
| REQ-UI-21 `TERM=dumb` | 1 | REQ-UI-43 survives defeat | 14 |
| REQ-UI-22 no stray bytes | 1, 9 | REQ-UI-44 survives restart | 14 |
| REQ-UI-23 bold under `NO_COLOR` | 1 | REQ-UI-44a no backfill | 13 |
| REQ-UI-24 semantic color | 9 | REQ-UI-45 no schema bump | 13 |
| REQ-UI-25 never style prose | 9, 15 | REQ-UI-46 derived from events | 13, 14 |
| | | REQ-UI-47/-48 non-goals | 15 |

All 56 requirement anchors are covered. Checks 1–35 are assigned in the step gates above.

## Out of scope, noted

**The band duplicates the room block on move turns.** `render()`'s `roomBlock`
emits `Exits:` and `You see:`, and `deterministicAppends` emits the same on the AI
path — then the band emits both again. REQ-UI-7 requires `render()`'s output stay
byte-identical, so collapsing this is explicitly *not* this feature's job. Worth its
own decision afterward, once the band has been lived with: the band arguably makes
those two lines redundant on every path, but removing them would touch a large
number of exact-match `render()` assertions and belongs in a change that owns that
cost deliberately.
