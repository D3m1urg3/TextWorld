---
title: "UI improvements: a status band for location, exits, objects, and combat"
date: 2026-08-02
status: resolved
tags: [ui, terminal, ansi-color, rendering, combat-legibility, status-band]
modules: [render, loop, combat, main]
related: [.lore/work/research/terminal-ui-status-band.md, .lore/work/specs/terminal-status-band-ui.md, .lore/work/brainstorm/performance-polish-action-latency.md]
---

# UI improvements: a status band for location, exits, objects, and combat

The player needs to always know: where he is, what the exits are, what objects are
present, whether a monster is present, and what combat is doing. The screen should
stay clean while showing all of it. This brainstorm settled on a **printed status
band above the prompt**, colored with ANSI, composed by the loop.

## The starting condition: there is no UI layer

`fputs` to stdout, `getline` from stdin. A grep for `\033`, `\x1b`, `isatty`,
`readline`, `COLUMNS`, or `ANSI` across `src/` and `CMakeLists.txt` returns
nothing. Whatever the terminal does with raw bytes *is* the UI.

Not a complaint — it means nothing has to be undone.

## The finding that reframed everything

**Every fact the player needs is already in the database and simply never
rendered.** This is not a data problem, it is a missing surface.

| Want to show | Already in schema | Currently rendered? |
|---|---|---|
| Room name | `name(entity, value)` — `'dormitory cell'`, `'corridor'` | **No** — `roomBlock()` prints prose, exits, items; never the name |
| Exits | `exits(room, direction, dest)` | Yes, on move/look only |
| Objects present | `portable` + `location` | Yes, on move/look only |
| Monster present | `hostile(entity, archetype, chip, telegraph_period)` | Only via prose lines |
| **Enemy HP** | `health(entity, current, max)` | **No** — `combatStatusLine()` shows the player's HP only |
| **Telegraph wind-up** | `pending_strike(entity, damage, element)` | One sentence in a prose wall |
| Burning / slowed / stunned | `status_effects(entity, kind, magnitude, remaining)` | Only as one-off event lines |
| Barrier (defense lock) | `barrier(entity)` | Only via `blocked`/`dispelled` lines |
| What a spell *does* | `spell_catalog(spell, element, cooldown, tier, effect)` | **Never surfaced at all** |
| Resistances | `resistance(archetype, element, num, den)` | Never (open question — see below) |

The sharpest one: **you cannot see how close the thing you are fighting is to
dying.** `combat.cpp:477` `combatStatusLine()` reports player HP and per-spell
cooldowns, nothing about the enemy.

## The direction we chose

Prose above, an engine-authored structured band below, printed each turn directly
above the prompt.

```
  The corridor runs black past your candle's reach. Something
  shifts in the dark ahead, and the smell of it reaches you first.

  ── corridor ─────────────────────────────────────────────
   Exits    north  east  south
   You see  cold iron key
   Hostile  goblin grunt   HP 6/10   WINDING UP
  ──────────────────────────────────────────────────────────
   HP 12/20   Ward ready   Fire 3   Stun ready
  >
```

Color does semantic work, not decoration:

- exits **cyan** (navigable)
- items **yellow** (takeable)
- hostiles **red** (danger)
- telegraph **bold + bright red** — a one-turn decision window
- player HP **red when low**
- spells **green when ready**, **dim while cooling**

### Why this beats a pinned split-screen

A full TUI (alternate screen buffer, pinned header/footer, prose scrolling
between) costs **terminal scrollback** — you could no longer scroll up and reread
the room from three moves ago. For a prose-first game whose stated soul is "the
`events` table is a complete transcript," that is a real loss, not an aesthetic
one.

And pinning buys almost nothing here, because of something specific to this game:
**the world only ticks on enter.** Nothing can change while you are typing. A
block printed each turn is therefore **always current by construction**. The only
thing persistence would add is not having to scroll — and you never scroll,
because the band sits directly above your cursor.

Cost accepted: the band repeats down the scrollback. Reading back through a long
session means wading past status blocks. Cheaper than losing scrollback entirely.

## Decisions made

### The band is composed by the loop, not by `render()`

`loop.cpp:114-119` has two return paths — AI prose and template. Today
`combatStatusLine()` is appended inside `render()` *and* again in the AI path,
which is why the README has to promise the two are "byte-identical." That promise
is currently kept by hand.

Compose the band once in `runTurn()` and append it to whichever text came back.
Both paths get it by construction; the promise stops needing to be kept. Still
read-only, so `render.cpp`'s "READ-ONLY BY CONTRACT" header is untouched — the
band simply does not belong *inside* the thing that has two callers.

### Colors: basic 16, three gates

Use the **basic 16 named ANSI colors**, not 256 and not truecolor. Named colors
resolve through the user's terminal theme, so "red" is *their* red and stays
readable on a light background. Truecolor hardcodes an assumption about the
background and will be unreadable mud for somebody.

Gate on `isatty(fileno(stdout))` **and** `NO_COLOR` **and** `TERM=dumb`. The
isatty gate is not only politeness: `tests/tests.cpp` compares rendered strings,
and escape codes leaking into golden output would break the suite. Gating on a tty
makes tests colorless for free.

### Never colorize inside model prose

Highlighting `goblin grunt` where it appears in a *narrated* paragraph means
substring-matching entity names against generated text — false positives (`key`
inside `keystone`), and it puts the render layer in the business of parsing model
output. That is the boundary this project keeps clean everywhere else.

This is what makes the band load-bearing rather than decorative: **the band
carries the mechanical burden so prose can stay prose.** Monster presence is
guaranteed legible by the band, so narration never has to be mined for it.

### Room name every turn, room prose only on move/look

The band's header repeats one line. The description paragraph keeps its current
trigger. That split is what makes "exits visible at all times" affordable.

### No emoji

Emoji are ambiguous-width and will misalign the rules on some terminals. Box
drawing is safe, and `·` already has precedent in the existing status line. Spend
**bold + bright red** on the telegraph instead of a glyph that breaks alignment.

## Open questions

**Does HP stay visible out of combat?** Today `combatStatusLine()` is self-gating
— returns `""` with no hostile in the room — so player HP *disappears entirely*
outside combat. "At all times" makes that existing behavior something we are
changing, not extending. Related: fixed-height band (steady, costs lines forever)
vs. conditional rows (less noise, screen jumps as rows appear and disappear). The
codebase's house style is self-gating.

**Where does combat legibility live?** Combat is designed as a puzzle — enemies
are locks, spells are keys — and the UI ships no key ring. `spell_catalog` knows
every element, cooldown, and effect, and none of it is reachable in-game. The
puzzle is currently only solvable with the design doc open alongside. Three
candidate homes: in the band, behind an `examine`/`spells` command, or
discovered-and-remembered through play.

**Resistances specifically.** `resistance(archetype, element)` *is* the lock.
Showing it makes combat legible; hiding it makes discovery the game. Middle path:
you learn a resistance permanently once you have hit that archetype with that
element — more machinery, possibly the real answer.

## Adjacent, noted but not chosen

- **Width is a shared prerequisite.** The rules need `ioctl(TIOCGWINSZ)` or
  `COLUMNS`. Once width is known, **wrapping the narration paragraphs is nearly
  free** — and unwrapped prose (currently hard-wrapped mid-word by the terminal)
  was the ugliest thing on the original list. Two features, one prerequisite;
  building them apart means building it twice.
- **Dead air.** A turn is ~99.9% model latency and the terminal is frozen and
  silent for the duration. No spinner, no cursor change. The only item on the
  original list that is not cosmetic. Streaming narration is filed under latency
  in `performance-polish-action-latency.md`, but it changes *perceived* time only
  — it is a presentation feature wearing a performance costume.
- **No line editing.** No history, no arrow keys, no ctrl-a. For a game whose
  input pitch is "type natural phrasings," the input box is the most primitive
  thing in the tree.

## Rejected

- **Full ncurses TUI** — costs scrollback; see above.
- **ASCII auto-map** from the exits graph. The architect literally builds the
  graph, it is right there — and it is the thing that would most kill "walk off
  the edge of the map." The map's job is to have holes.
- **Typewriter effect.** Infuriating for fast readers. Notable because it looks
  identical to real streaming, which is good: the fake version of the good thing
  is the bad thing.
- **Turn separators / timestamps.** Reads as a log viewer, kills the transcript
  feeling. But "a long session is an undifferentiated wall" remains a real
  complaint without a better answer yet.
