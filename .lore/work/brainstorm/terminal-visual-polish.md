---
title: "Terminal visual polish: layout, color, line editing, and ASCII art"
date: 2026-09-07
status: open
tags: [ui, terminal, ansi-color, ascii-art, readability, line-editing, streaming]
modules: [render, loop, band, term, main, architect]
related: [.lore/work/brainstorm/ui-improvements.md, .lore/work/research/terminal-ui-status-band.md, .lore/work/specs/terminal-status-band-ui.md]
---

# Terminal visual polish: layout, color, line editing, and ASCII art

The starting request was "add a UI and visuals." It ended with: stay in the
terminal, fix what is already on screen, add hand-drawn ASCII art where the set
of things is fixed, and defer real images.

## What the screen actually looks like today

Captured from a live run, template mode, 88 columns, `CLICOLOR_FORCE=1`:

```
Here the wall is torn open where the smoke and the deep both climb, and the two darks
are one thing: the black mere pours in through the breach in a slow sheet, and up that
[...eight more lines...]
Exits: north, south.
-- the breach chamber -----------------------------------------------------------
 Exits    north, south
 You      HP: 10/12
> look
Here the wall is torn open where the smoke and the deep both climb, and the two darks
[...the identical ten lines again...]
Exits: north, south.
-- the breach chamber -----------------------------------------------------------
 Exits    north, south
 You      HP: 10/12
> x goblin
You don't see that here.
-- the breach chamber -----------------------------------------------------------
 Exits    north, south
 You      HP: 10/12
>
```

Five problems visible in that capture:

1. Prose, `Exits:`, a refusal, and the band are all the same text at the same
   column. The screen never says what kind of thing it just said.
2. No blank line anywhere. Turns run together.
3. The band repeats identically every turn, four lines at a time.
4. `Exits:` is printed twice, one line apart — once by `roomBlock`, once by the
   band.
5. Out of combat, color does almost nothing: one bold room name, one cyan word.

## Facts found in the code

- All game output leaves through three `fputs` calls (`main.cpp:131`, `:140`,
  `:158`). `runTurn` returns one flat `std::string` with escape codes already
  baked in. Nothing downstream can tell prose from an HP number.
- `loop.cpp:194` wraps prose to `detectWidth()` — the full terminal width. On a
  wide terminal that is 200-character lines.
- `render.cpp:81` and `:86` emit `Exits:` and `You see:`. The band emits both
  again every turn from `exitsRow` and `objectsRow`.
- The band already has a colour vocabulary (`band.cpp:22-30`): cyan exits, green
  objects, red hostiles, bold bright-red telegraph, yellow low HP, bright-blue
  ready spells, grey cooling spells, magenta effects.
- `pregen.cpp` already runs a worker thread that generates the next room while
  the player reads the current one, and it deliberately takes no `Db`.
- The bestiary is a fixed catalog. The generator picks an enemy type from a
  menu; the engine copies every number. The set of enemies is closed.
- The world grows as a tree — every exit leads to a brand-new room, no two
  openings ever meet.

## The surface question, and why it was dropped

Three options were considered: terminal only, terminal plus inline images via
the iTerm2/Kitty protocols, and a browser page reading `world.db` directly.

Sorting the complaints against them showed the decision only mattered for one of
them:

| Complaint | Terminal | Terminal + images | Browser |
|---|---|---|---|
| Wall of text | fixed | fixed | fixed, with far more machinery |
| Feels dead | fixed — same fix on all three | same | same |
| Looks unfinished | mostly fixed; there is a ceiling | better | highest ceiling |
| Can't see the world | impossible | fixed | fixed |

Three of the four are answerable without deciding anything. Only "can't see the
world" needs images, and images were deferred.

**Anthropic has no image generation.** Claude reads images; it does not make
them. Room pictures would need a second vendor (OpenAI `gpt-image-1`, Google,
Stability, Replicate, fal, or local Stable Diffusion) — a second API key, a
second bill, a second failure mode, against a project that currently has one
vendor. That, plus 5-30 second generation times and the problem of keeping 200
rooms in one art style, is why images are parked rather than rejected.

## Agreed direction

Order of work, as agreed:

1. Cap prose width.
2. Delete the duplicated `Exits:` / `You see:` lines.
3. HP bars drawn with background color.
4. Title screen, as the first piece of ASCII art.

### The changes, with the line each one touches

**Cap prose width.** `loop.cpp:194` — `wrapProse(r.output, std::min(w, 72))`.
One line. Books use 60-75 characters; the game currently uses whatever the
terminal is.

**Delete two lines.** `render.cpp:81` and `render.cpp:86`. The band prints both
two lines below, every turn. Deleting them makes the room block pure prose.

**HP as a bar, in background color.** `HP 10/12` is a number to read; a bar is
seen. Box-drawing characters are ambiguous width and would misalign the band
(`band.hpp` already forbids them), but a run of spaces with a colored background
is always one column each and can never misalign. Same treatment for enemy HP
and cooldowns.

**Blank line before the prompt.** `main.cpp:140` — `fputs("\n> ", stdout)`.

**Indent the whole turn output two spaces, leave the band at column 0.**
Narration and the band are one concatenated string by print time, so separating
prose from engine text inside it is more work than it sounds. Indenting
everything and leaving the band flush left gets most of the contrast for almost
nothing.

**Errors should not look like narration.** `renderError` in `render.cpp` is
already its own function. Dim it or color it — a refusal is the game talking,
not the world.

**Ask the narrator for paragraphs.** The captured prose is one solid ten-line
block; nothing in the code forces that. Adding "two or three short paragraphs"
to the narration prompt is a prompt change with no code.

**Line editing.** Input is `std::getline` on `std::cin`: no history, no
up-arrow, no ctrl-a. For a game whose input pitch is "type a natural sentence", a
typo means retyping the sentence. `linenoise` is one C file, ~1000 lines, and
`sqlite3.c` is already vendored. Biggest usability gain on the list and not
visual at all.

**Spinner during the 3.4 second wait.** A thread around `main.cpp:156` that
prints a character, erases it, and stops before the output is written. Gate it on
`currentStyle().attrs` so it never runs into a pipe and the test suite stays
clean.

**Streaming narration.** The real answer to the freeze — text appearing as it is
generated both feels faster and looks alive. The narrate call is not streamed
today. Largest job here, and the only one that changes how the game feels rather
than how it looks.

## ASCII art

The split: ASCII art works where the set of things is closed and fails where it
is open. Model-generated ASCII art is bad in every case.

**Works, hand-drawn, stored in seed data**

- Title screen — the game's name, once, at launch.
- Enemies — the bestiary is a fixed catalog, so one drawing per archetype, keyed
  the same way the numbers already are.
- Items — same argument if items come from a catalog.
- The downed screen — waking in the dormitory cell currently gets a sentence.

**Fails: rooms.** The architect invents rooms freely, so there is no fixed set.

**Unless rooms use the trick the bestiary already uses.** Give the architect a
fixed list of room shapes — corridor, hall, stair, cellar, courtyard, chamber,
cave — and have it pick one from a menu, exactly as it picks an enemy type. Ten
hand-drawn sketches reused across a hundred rooms. No room is unique, but every
room looks good, and it costs one field on the room record.

**Two constraints on all art**

- Plain ASCII only. `─` and `·` are ambiguous width and will misalign the band on
  some terminals; `band.hpp` already forbids them. Art uses `/ \ | _ - . ' ( ) # *`.
- Art costs vertical space. A goblin drawing printed every turn of a fight fills
  the screen in six turns. First sighting only, or on `examine` — which is
  already the verb for looking closely at one thing, and already prints word for
  word.

## Reconsidered: the map

The previous brainstorm rejected an ASCII auto-map because "the map's job is to
have holes." Worth reopening, for one reason: the world grows as a tree, so a map
needs no layout solver. It can print the way `tree(1)` prints a directory, with
unexplored exits shown as stubs. That advertises the holes rather than hiding
them, and it is far less code than a grid map.

## Needs a decision, not just code

- **`look` reprints the whole paragraph you just read.** Ten identical lines back
  to back in the capture. Options: name and exits only, keep the paragraph for
  first visit, or add `look full`. Needs a "have I been here" flag that does not
  exist yet.
- **Collapsing the band when nothing changed.** Cheap to build, but the screen
  jumps between four lines and one as you walk. May be worse than the repetition.
- **Showing what the resolver decided.** You type "swing at the goblin" and never
  learn whether it became `attack`. A dim `(attack)` would build trust in the
  resolver; it also exposes machinery the game is trying to hide. Try it behind a
  flag.

## Rejected

- **Model-generated ASCII art of rooms.** It will be garbage.
- **The model choosing colors.** `composeBand(Db&, int)` takes no strings on
  purpose, so no model-written text can reach a styled span. Any "let the AI
  style the output" idea is already impossible, and correctly so. Room tint
  survives only because the architect would write a room field and the engine
  reads it — the model picks a word, not a color.
- **Background colors behind prose blocks.** Truecolor mud, unreadable on
  someone's light terminal.
- **Full-screen panes.** Costs scrollback, and the transcript is the point.
- **Typewriter effect.** Infuriating for fast readers, and it looks identical to
  real streaming — the fake version of the good thing is the bad thing.
- **Dimming text from earlier turns.** It is already in the scrollback; you would
  have to redraw.
- **Tab completion on room nouns.** Fast to type, but it tells you what is there
  before you look.
- **Terminal bell on damage.** Will annoy someone within five minutes.

## Parked, not rejected

- **Room images**, inline via the iTerm2/Kitty protocols. Needs a second vendor,
  a style that survives 200 separate generations, and a designed empty box for
  when there is no key or the call failed. Generation goes through `pregen.cpp`
  the way room text already does, which means walking into a new room shows an
  empty box for 10-20 seconds before it fills.
- **A browser page reading `world.db` directly.** No engine change at all — the
  band is SELECTs and the transcript is the `events` table. Risk: a game about
  reading prose asking you to look at a second window.
- **Room tint as danger.** The invasion front already knows which rooms are near
  the breach. Coloring the room name by that makes color carry information, the
  rule the band already follows.
- **The playthrough as a book.** The `events` table is a complete transcript;
  rendering it to an HTML page on quit is nearly free and is the one place
  pictures cannot compete with the prose, because you have already read it.
