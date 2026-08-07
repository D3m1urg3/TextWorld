---
title: Terminal status band UI
date: 2026-08-02
status: implemented
tags: [ui, terminal, ansi-color, no-color, status-band, prose-wrapping, combat-legibility, resistance-discovery]
modules: [render, loop, combat, main, world]
related: [.lore/work/brainstorm/ui-improvements.md, .lore/work/research/terminal-ui-status-band.md, .lore/work/specs/combat-and-enemies.md]
req-prefix: UI
---

# Terminal status band UI

The player must always know where he is, which exits exist, what objects are
present, whether a monster is present, and what combat is doing — on a clean
screen. This spec defines a **status band**: an engine-authored, ANSI-colored
block printed each turn directly above the prompt, plus the width handling and
prose wrapping it depends on, plus the combat information that makes the
locks-and-keys puzzle legible.

This is deliberately comprehensive. **Atomic sequencing is a plan concern** —
`/prep-plan` will slice it. Group **G** (resistance discovery) is written to be
droppable without unpicking anything else.

### Scope provenance

The brainstorm files prose wrapping (group **E**) under "Adjacent, noted but not
chosen" and combat legibility (group **F**) as an open question. Both were
subsequently **decided into scope** — wrapping because the research established
that terminal-width detection is a shared prerequisite, so building the band
without it means building the same plumbing twice; combat legibility as an
explicit scope choice, accepting that group G reaches past UI into game design.
The brainstorm's headings predate those decisions and were not retro-edited; this
section is the record.

## Context the requirements assume

- **No UI layer exists.** `fputs`/`getline`; no ANSI, `isatty`, wrapping, or
  width detection anywhere in `src/`.
- **The data already exists.** Room `name`, `exits`, `portable`+`location`,
  `hostile`, `health`, `pending_strike`, `status_effects`, `barrier`, and
  `spell_catalog` are all populated and queryable. This feature is a **new
  surface over existing rows**, except group G.
- **Two render paths.** `runTurn()` (`loop.cpp:114-119`) returns either AI prose
  or template output. `combatStatusLine()` is currently appended in *both*, a
  byte-identity duplication asserted by `tests.cpp:1506-1512`.
- **`render()` is read-only by contract** and its output is asserted with exact
  string equality throughout `tests.cpp` (e.g. `:1700`).
- **The schema gate refuses mismatched worlds** (`world.cpp:141`) with no
  migration: bumping `SCHEMA_VERSION` makes every existing `world.db` unopenable.
- **Defeat deletes the `hostile` row** (`mutations.cpp:335`), so an entity's
  archetype is unrecoverable after it dies.
- **The world ticks only on enter.** Nothing changes while the player types.

---

## Requirements

### A. Band composition and placement

<a id="req-ui-1"></a>
**REQ-UI-1** — The status band is composed by `runTurn()` and appended to the
turn's output text, **not** by `render()`. Both the AI-prose path and the
template path receive the identical band by construction rather than by duplicated
calls.

<a id="req-ui-2"></a>
**REQ-UI-2** — Band composition is **read-only**: SELECTs only, no INSERT/UPDATE/
DELETE, and it runs outside the tick transaction. It never appends an `events`
row.

<a id="req-ui-3"></a>
**REQ-UI-3** — The band is printed on **every turn that produces output**,
including no-tick outcomes (unparseable input, a denied cast) and engine errors.
A turn that produces output produces a band.

<a id="req-ui-4"></a>
**REQ-UI-4** — The band is the **last thing written before the prompt**, with the
narration or template text above it.

<a id="req-ui-5"></a>
**REQ-UI-5** — The band is also printed once at startup, after the existing
read-only startup room render (`main.cpp:40`, `renderStartup`) and before the
first prompt is written.

<a id="req-ui-6"></a>
**REQ-UI-6** — **Both** existing status-append call sites are removed: the one
inside `render()` (`render.cpp:222`) and the one on the AI-prose path. After this
change exactly one place in the binary emits status text — the band.

<a id="req-ui-6a"></a>
**REQ-UI-6a** — `combatStatusLine()` **survives as a helper** that the band calls
for spell-readiness content. It is removed as an *appender*, not as a function, so
its readiness semantics remain testable in isolation (see check 20).

<a id="req-ui-7"></a>
**REQ-UI-7** — `render()`'s **event-line output is unchanged**: every verb
template produces byte-identical text to today. Only the trailing status append is
removed.

<a id="req-ui-7a"></a>
**REQ-UI-7a** — Consequently, exactly **one** class of existing assertion changes:
the byte-identity test at `tests.cpp:1509-1515`, which asserts `render()`'s output
*ends with* `combatStatusLine()`. That test is **retired**, not rewritten — it
exists to police a duplication this feature eliminates. Every other existing
assertion on `render()` must pass unmodified; any further test edit is a REQ-UI-7
violation.

### B. Band content

<a id="req-ui-8"></a>
**REQ-UI-8** — Rows are **conditional**: a row renders only when it has content.
The band's height varies with what is present.

<a id="req-ui-9"></a>
**REQ-UI-9** — The band's header shows the **current room's name** (from `name`).
A room with no name row renders the header rule without a title rather than
failing.

<a id="req-ui-10"></a>
**REQ-UI-10** — An **Exits** row lists the current room's available directions in
the same order and under the same visibility rule the room block already uses:
realized exits always; latent exits only when the architect is enabled. Latent and
realized exits remain visually indistinguishable.

<a id="req-ui-11"></a>
**REQ-UI-11** — An **objects** row lists the names of portables whose container is
the current room, in entity order. Omitted when the room holds none.

<a id="req-ui-12"></a>
**REQ-UI-12** — A **hostile** row renders for each living hostile sharing the
player's room, showing its name and its **current and maximum HP**.

<a id="req-ui-13"></a>
**REQ-UI-13** — Multiple hostiles each get their own row, in entity order —
matching the swarm ordering combat already uses.

<a id="req-ui-14"></a>
**REQ-UI-14** — The room's **prose description** is not part of the band and
retains its current trigger (move and look only). The band repeats the room's
name, never its description.

### C. Player status rows

<a id="req-ui-15"></a>
**REQ-UI-15** — The player's **HP is shown on every band**, in and out of combat.
This replaces today's behavior where HP is absent outside combat.

<a id="req-ui-16"></a>
**REQ-UI-16** — Out of combat the status row is **compact**: HP only.

<a id="req-ui-17"></a>
**REQ-UI-17** — In combat the status row additionally shows **per-known-spell
readiness**, preserving today's semantics exactly:

- *Ordering*: alphabetical by spell name.
- *Display*: `ready` if the spell is castable as the next action; otherwise the
  integer count of turns remaining.
- *Definition of ready*: `ready_turn - now <= 0`, **or** the spell has no
  `cooldowns` row at all. The absent row is a state meaning ready, not a third
  thing to display.

<a id="req-ui-18"></a>
**REQ-UI-18** — "In combat" uses the **same predicate** the engine already uses —
a living hostile shares the player's room — so the band and combat resolution can
never disagree about whether a fight is happening.

### D. Color

<a id="req-ui-19"></a>
**REQ-UI-19** — Color uses the **basic 16 named ANSI SGR colors** only. No 256-color
or truecolor sequences, so output resolves through the user's terminal theme.

<a id="req-ui-20"></a>
**REQ-UI-20** — Color is gated by this precedence, evaluated in order, with empty
variables treated as unset:

| Order | Condition | Result |
|---|---|---|
| 1 | `NO_COLOR` set, non-empty | no color |
| 2 | `CLICOLOR_FORCE` set, non-`0` | color, regardless of tty |
| 3 | `CLICOLOR` set, non-`0` | color iff stdout is a tty |
| 4 | none set | color iff stdout is a tty |

<a id="req-ui-21"></a>
**REQ-UI-21** — `TERM=dumb` suppresses **all SGR sequences, including bold** —
not merely color. Unlike `NO_COLOR` (REQ-UI-23), `TERM=dumb` signals a terminal
that cannot reliably interpret *any* escape sequence, so nothing styled is
emitted. This is the one case where the telegraph loses its bold and must remain
distinguishable by text alone.

<a id="req-ui-22"></a>
**REQ-UI-22** — Suppressed color means **no escape bytes at all** — not empty
sequences. Byte-for-byte, a colorless run emits exactly the plain text.

<a id="req-ui-23"></a>
**REQ-UI-23** — Per the NO_COLOR spec, suppression governs **color only**. Bold
remains available, so the telegraph stays visually distinct on a colorless
terminal.

<a id="req-ui-24"></a>
**REQ-UI-24** — Color carries **semantic** meaning, one role per color, applied
consistently: exits navigable, objects takeable, hostiles dangerous, player HP
warning-colored when low, spells distinguished ready vs cooling, telegraph
loudest. Color is never the *only* carrier of a fact — every colored fact is also
present in the text.

<a id="req-ui-25"></a>
**REQ-UI-25** — Color is **never applied inside model-generated prose**. Only
engine-composed text is styled. No entity-name matching against narration output.

### E. Width and wrapping

<a id="req-ui-26"></a>
**REQ-UI-26** — Terminal width is detected via `ioctl(TIOCGWINSZ)` on the
**stdout** descriptor, falling back to `COLUMNS`, then to a default of 80.

<a id="req-ui-27"></a>
**REQ-UI-27** — A failed ioctl (non-zero return **or** a zero `ws_col`) must fall
through to the next source. Detected width is clamped to a floor of **20
columns**; a reported width below that is treated as 20. The floor can be this low
because REQ-UI-33 wraps rather than truncates, so only a single word longer than
the floor can overflow.

<a id="req-ui-28"></a>
**REQ-UI-28** — Width is re-queried **at print time** each turn. No `SIGWINCH`
handler; resize is picked up on the next turn.

<a id="req-ui-29"></a>
**REQ-UI-29** — Band rules and separators use **ASCII characters only**.
Rationale: box-drawing `─` (U+2500) and the currently-shipping `·` (U+00B7) are
East Asian **Ambiguous** width, so the terminal — not the program — decides their
column count.

<a id="req-ui-30"></a>
**REQ-UI-30** — Narration and template prose are **wrapped to the detected width**
at word boundaries. No word is broken mid-word by the wrapper.

<a id="req-ui-31"></a>
**REQ-UI-31** — Wrapping counts **characters, not bytes**. Multi-byte UTF-8 in
model prose (em-dashes, curly quotes) must not cause premature wrapping.

<a id="req-ui-32"></a>
**REQ-UI-32** — Wrapping preserves existing paragraph breaks; it never joins
lines the source separated.

<a id="req-ui-33"></a>
**REQ-UI-33** — Rows whose content exceeds the available width **wrap onto
continuation lines with a hanging indent aligned to the row's content column**.
The band never emits a line wider than the detected width.

<a id="req-ui-33a"></a>
**REQ-UI-33a** — The band **never truncates and never elides**. No exit, object,
hostile, or status is dropped or abbreviated away because of width. Rationale:
every fact in the band is *actionable* — an exit the player cannot see is a legal
move they cannot make, and a status they cannot see is a combat decision they
cannot take. The band grows taller; it never grows quieter.

<a id="req-ui-33b"></a>
**REQ-UI-33b** — The only width-driven degradation permitted is dropping the
**ASCII rules and separators** (REQ-UI-29), which carry no information. Content
rows are never sacrificed to fit.

### F. Combat legibility

<a id="req-ui-34"></a>
**REQ-UI-34** — A hostile's **pending telegraph** is shown on its row whenever a
`pending_strike` row exists for it, rendered as the most visually prominent
element in the band.

<a id="req-ui-35"></a>
**REQ-UI-35** — A hostile's **active states** are shown on its row: barrier
present, and each status effect as **kind plus remaining turns**. Magnitude is
**not** shown — the player cannot act differently on it, whereas duration is a
direct decision input (wait out a slow, or burn down a DoT). Rows that grow long
wrap per REQ-UI-33 rather than dropping states.

<a id="req-ui-36"></a>
**REQ-UI-36** — The player's **active states** are shown, notably a held ward,
since it determines whether an incoming telegraphed strike lands.

<a id="req-ui-37"></a>
**REQ-UI-37** — The player can inspect **what a known spell does** in-game —
element, cooldown, and effect from `spell_catalog` — without consulting external
documentation.

<a id="req-ui-38"></a>
**REQ-UI-38** — REQ-UI-37's inspection is available for **every known spell**, and
for known spells only. Unlearned spells are not enumerated; the catalog is not a
spoiler list.

<a id="req-ui-39"></a>
**REQ-UI-39** — Spell inspection is **read-only and consumes no turn**. It returns
the `NoTick` outcome: no transaction opens, `meta.turn` does not change, no
`events` row is written, and no hostile takes a turn.

> **Note:** this deliberately *differs* from `inventory`, which does consume a
> turn (`systems.cpp:206` appends its event inside the tick). Spell rules are
> reference information about the game's mechanics, not an action in the world;
> charging a turn would punish the player precisely when checking matters most —
> mid-fight, while hostiles chip.

<a id="req-ui-39a"></a>
**REQ-UI-39a** — REQ-UI-39 is a **bounded exception** to the project's rule that
all player-visible output renders from `events` rows plus read-only lookups. The
exception is confined to spell inspection and is permitted because the output
describes *the rules*, not *the world* — no world state is read that a turn could
have changed, and nothing about the exchange is worth preserving in the
transcript. Precedent: the engine already declines actions without a tick (an
unparseable line, a spell on cooldown — `loop.cpp:64`, `loop.cpp:78`).

<a id="req-ui-39b"></a>
**REQ-UI-39b** — This exception **must not generalize**. No other command may
produce output outside the events model on its authority. Any future addition
wanting the same treatment requires its own decision, not an appeal to this one.

<a id="req-ui-40"></a>
**REQ-UI-40** — All combat numbers shown remain **engine-authored**. The band
never displays a model-supplied quantity.

### G. Resistance discovery — separable

> This group is the only part of the spec requiring new persistent state. It can
> be dropped or staged independently; **A–F must not depend on it.**

<a id="req-ui-41"></a>
**REQ-UI-41** — Once the player has damaged a given archetype with a given
element, that archetype's resistance to that element becomes **permanently known**
and is thereafter shown when facing that archetype.

<a id="req-ui-42"></a>
**REQ-UI-42** — Undiscovered resistances are **not shown**. Discovery through play
is the mechanic; the resistance table is never surfaced wholesale.

<a id="req-ui-42a"></a>
**REQ-UI-42a** — A discovered **absence** of resistance is displayed as a positive
fact, textually distinct from "not yet tested". Hitting an archetype with an
element it does not resist is a successful experiment and must read as one —
otherwise the player cannot tell a tested element from an untested one, and the
discovery mechanic teaches nothing.

<a id="req-ui-43"></a>
**REQ-UI-43** — Discovery **survives the enemy's defeat**, which deletes the
`hostile` row and with it the entity→archetype link. Discovery is keyed to the
**archetype**, not the instance.

<a id="req-ui-44"></a>
**REQ-UI-44** — Discovery **persists across restarts**: reopening a world file
preserves everything discovered in earlier sessions.

<a id="req-ui-44a"></a>
**REQ-UI-44a** — Discovery accrues **only from combat that occurs after this
feature ships**. A world file created by an earlier build carries damage events
whose `detail` is `NULL` (`mutations.cpp:95`), so pre-existing fights are
unrecoverable and such a save legitimately starts with nothing discovered. No
backfill is attempted — one would require reconstructing the entity→archetype
link that defeat already destroyed (REQ-UI-43). This is a deliberate accepted
limitation, and it is why REQ-UI-44 is scoped to restarts rather than justified by
the project's "knowledge is permanent" rule, which it does not fully satisfy.

<a id="req-ui-45"></a>
**REQ-UI-45** — Discovery **must not require a `SCHEMA_VERSION` bump**. The gate
at `world.cpp:141` has no migration path, so a bump makes every existing world
file unopenable. If the chosen mechanism cannot avoid a bump, this group is
deferred rather than shipped.

<a id="req-ui-46"></a>
**REQ-UI-46** — Discovery is **derived from the existing transcript**, not from a
parallel write path. The `events` table remains the single source of truth: no new
table stores discovery state. Falsifiable consequence — deleting the `events` rows
for a fight erases what that fight taught.

*Feasibility, cited: `events.detail` is declared free TEXT, "human-readable
fragment or NULL" (`world.cpp:60`), and `damageEntity` currently passes `nullptr`
for it on every damage event (`mutations.cpp:95`), so the column is unused on
exactly the events that would need to carry an archetype. Whether to use it, and
how, is a plan concern; this requirement fixes only the property.*

### H. Non-goals

<a id="req-ui-47"></a>
**REQ-UI-47** — Explicitly out of scope: alternate-screen or pinned split-screen
layouts; DECSTBM scroll regions; line editing and command history; streaming
narration; an ASCII map; turn separators or timestamps; any third-party UI
dependency.

<a id="req-ui-48"></a>
**REQ-UI-48** — The band's repetition in terminal scrollback is an **accepted
cost**, deliberately traded for preserving scrollback intact.

---

## AI Validation

Behavioral checks. Each is runnable or observable; none requires a live model.

### Determinism and purity

1. **No writes.** Assert the band-composition translation unit contains no
   `INSERT`, `UPDATE`, or `DELETE` — the same static check `render.cpp` already
   lives under. Confirm `meta.turn` and the `events` row count are unchanged
   across N band compositions.
2. **Both paths identical.** With narration stubbed to succeed and to fail on the
   same world state, assert the appended band bytes are identical. Then assert
   there is exactly **one** call site appending status text (REQ-UI-6).
3. **`render()` untouched, with one sanctioned exception.** The existing suite
   passes unmodified **except** the byte-identity test at `tests.cpp:1509-1515`,
   which is deleted (REQ-UI-7a). Any *other* required edit to a `render()`
   assertion is a REQ-UI-7 violation. Verify by diffing the test file: exactly one
   deleted block, no modified assertions.

### Color gating

4. **Truth table.** For each combination of `NO_COLOR`, `CLICOLOR_FORCE`,
   `CLICOLOR`, `TERM=dumb`, and tty/not-tty, assert color present or absent per
   REQ-UI-20/21. Include empty-string cases — they must behave as unset.
5. **No stray bytes.** In every suppressed case, assert output contains no `0x1b`
   byte at all (REQ-UI-22).
6. **Bold survives `NO_COLOR`.** With a telegraph pending, assert color bytes are
   absent but the bold sequence is present (REQ-UI-23).
6a. **`TERM=dumb` strips everything.** Same state under `TERM=dumb`: assert **no**
   `0x1b` byte at all, bold included, and that the telegraph is still identifiable
   from its text alone (REQ-UI-21).
7. **Piped output is clean.** Run the binary with stdout to a pipe and assert
   zero escape bytes; repeat with `CLICOLOR_FORCE=1` and assert they return.
8. **Prose is unstyled.** With narration returning text containing an entity name,
   assert no escape byte appears within the narration segment (REQ-UI-25).

### Width and wrapping

9. **The failing ioctl.** With stdout not a tty (the CI/pipe case, which
   reproduces `rc=-1, cols=0`), assert a valid non-zero band width and no
   zero-width or divide-by-zero behavior.
10. **Fallback order.** ioctl fails + `COLUMNS=52` → width 52; ioctl fails +
    `COLUMNS` unset → 80; `COLUMNS` set to garbage → 80.
11. **No overflow.** Across widths 20 (the clamp floor), 40, 80, and 200, assert
    **no** emitted line exceeds the width (REQ-UI-33), including a room stuffed
    with many objects and a long exit list.
11a. **Clamp floor.** A reported width of 10, 1, and 0 all yield a band built at
    width 20 (REQ-UI-27).
11b. **Nothing lost.** Render the same world state at width 200 and width 20;
    assert the **set** of exits, object names, hostile names, and status kinds is
    *identical* between the two — only line breaks differ. This is the executable
    form of REQ-UI-33a and the check most likely to catch a truncation shortcut.
11c. **Hanging indent.** At a width forcing a wrap, assert continuation lines are
    indented to the row's content column, not to column 0 (REQ-UI-33).
12. **ASCII only.** Assert every band-framing byte is ASCII (REQ-UI-29).
13. **UTF-8 wrapping.** Wrap a paragraph containing em-dashes and curly quotes;
    assert lines reach near the width rather than wrapping early, and that no
    multi-byte sequence is split.
14. **Word integrity.** Assert no output line ends mid-word for any input word
    shorter than the width.
15. **Paragraph preservation.** A two-paragraph input yields two paragraphs
    (REQ-UI-32).

### Band content

16. **Golden bands.** For fixed world states — empty room; room with objects;
    room with one hostile; room with three hostiles; hostile mid-telegraph;
    hostile with barrier; player warded — assert exact expected band text with
    color disabled.
17. **Conditional rows.** Moving from a populated room to an empty one drops the
    object and hostile rows entirely (REQ-UI-8).
18. **Latent exits.** With the architect disabled, latent exits are absent from
    the Exits row; enabled, they appear and are textually indistinguishable from
    realized ones (REQ-UI-10).
19. **HP always.** Out of combat, HP present and no spell readiness (REQ-UI-16);
    in combat, both (REQ-UI-17).
20. **Readiness parity.** Assert the band's in-combat readiness values equal what
    `combatStatusLine()` returns for the same state. This is viable precisely
    because REQ-UI-6a keeps the function alive as a helper — it is removed as an
    *appender* only, so it remains available as a comparison oracle.
21. **No-tick turns.** An unparseable line and a denied cast each still emit a
    band, and neither increments `meta.turn` (REQ-UI-3).
22. **Startup.** A fresh world emits a band before the first prompt (REQ-UI-5).
23. **Swarm order.** Three hostiles render in entity order, matching combat's
    ordering (REQ-UI-13).
24. **Missing name.** A room with no `name` row renders without throwing
    (REQ-UI-9).

### Combat legibility

25. **Telegraph visibility.** With `pending_strike` present, assert the telegraph
    marker appears; absent, it does not (REQ-UI-34).
26. **Spell inspection.** Learn one spell; assert inspection shows its element,
    cooldown, and effect; assert an unlearned spell is not listed (REQ-UI-38).
26a. **Genuinely no-tick.** Inspect spells with a hostile in the room and assert
    **all** of: `meta.turn` unchanged, `events` row count unchanged, player HP
    unchanged (no chip damage), and no enemy telegraph advanced. Turn cost is not
    observable by the turn counter alone — the enemy must also not have acted
    (REQ-UI-39).
26b. **Exception stays bounded.** Assert every *other* command that produces
    output still writes an `events` row — spell inspection is the only path
    emitting player-visible text without one (REQ-UI-39b).
26c. **State detail.** With a hostile carrying a DoT and a slow, assert the row
    shows each kind with its remaining turns and **no** magnitude (REQ-UI-35).

### Resistance discovery (group G)

27. **Not shown before discovery.** A fresh world facing an archetype shows no
    resistance information (REQ-UI-42).
28. **Shown after.** Hit that archetype with an element; assert its resistance to
    that element is thereafter shown, and that resistances to *other* elements are
    still hidden.
29. **Survives defeat.** Discover, kill the enemy, meet a **new instance** of the
    same archetype; assert the resistance is still known (REQ-UI-43).
30. **Survives restart.** Reopen the world file; assert discovery persists
    (REQ-UI-44).
31. **No schema bump.** Assert `SCHEMA_VERSION` is unchanged, and that a
    `world.db` created by the **previous** build still opens without
    `SchemaMismatch` (REQ-UI-45). This check is the gate on whether group G ships
    at all.
32. **No shadow store.** Assert the schema contains no table added by this
    feature — enumerate `sqlite_master` and diff against the pre-feature table
    list. A shadow discovery table would satisfy checks 27-31 while violating
    REQ-UI-46; this is what makes that requirement falsifiable.
33. **Transcript is the source of truth.** Discover a resistance, then delete the
    `events` rows for that fight and reopen. Assert the discovery is **gone**. If
    it survives, state is being stored somewhere other than the transcript
    (REQ-UI-46).
34. **Zero resistance reads as tested.** Hit an archetype with an element it does
    not resist; assert the result is displayed as an explicit no-resistance fact,
    and that it is textually distinguishable from an element never tried
    (REQ-UI-42a).
35. **Legacy save starts empty.** Open a `world.db` carrying pre-feature damage
    events; assert zero discovered resistances and no crash on the `NULL` details
    (REQ-UI-44a).

## Resolved during specification

All three questions raised in the first draft are now decided and folded into
requirements:

| Question | Decision | Requirements |
|---|---|---|
| Minimum width floor and degradation | Clamp at **20**; never truncate, wrap with hanging indent; rules are the only thing width may drop | REQ-UI-27, -33, -33a, -33b |
| Spell inspection turn cost | **No-tick query**, no `events` row — a bounded, non-generalizing exception to the events-render rule | REQ-UI-39, -39a, -39b |
| Enemy state verbosity | **Kind + remaining turns**, no magnitude; long rows wrap | REQ-UI-35 |

## Remaining for the plan

Genuinely implementation-level, deliberately not fixed here:

- **The command word and its resolver wiring.** REQ-UI-37/-39 fix the capability
  and its turn cost, not the spelling. Adding a verb touches `Verb` in
  `action.hpp`, `parser.cpp`, and the model-facing tool enum plus prompt text in
  `nlresolve.cpp` (`:93`, `:97`, `:189`) — a known shape, since `Inventory` has
  exactly it, but the no-tick routing has no existing verb to copy: every current
  verb resolves inside the tick, and the two `NoTick` returns are both refusals
  (`loop.cpp:64`, `:78`), not successful output. That routing is the one novel
  piece of plumbing in this spec.
- **Exact column layout and label widths** — a golden-file concern (check 16).
- **Which of the 16 colors maps to which role.** REQ-UI-24 fixes that the mapping
  is semantic and consistent, not what it is.
