---
title: "Error logging: internal messages leave the game screen"
date: 2026-08-05
status: resolved
tags: [logging, diagnostics, terminal, log-levels, session-log]
modules: [profile, main, bard, architect, prose, nlresolve, pregen, mutations, world]
related: [.lore/work/specs/session-logging.md, .lore/work/brainstorm/ui-improvements.md, .lore/work/brainstorm/performance-polish-action-latency.md]
---

# Error logging: internal messages leave the game screen

The terminal is the game screen. Anything printed on it that isn't the game is a
bug. Right now about 40 places in the code print internal messages straight to
the screen, in the middle of play. They should go to a log file instead — a
classic logging system, with levels, timestamps, and one file per session.

## The starting condition

Every internal message is a direct `std::fprintf(stderr, …)`. There is no
logging layer at all. The sites are spread across `bard.cpp`, `architect.cpp`,
`prose.cpp`, `nlresolve.cpp`, `pregen.cpp`, `mutations.cpp`, `world.cpp`, and
`main.cpp`. Game text goes to stdout via `fputs`.

So the messages are already on a separate channel from the game text. The
problem is that both channels land on the same terminal. Which means
`./textworld 2> some.log` already "works" — worth naming, because it sets the
bar. A real logging system has to earn its keep with levels, timestamps,
per-session files, and working by default without anyone remembering a shell
trick.

## What already exists and should be copied

`profile.cpp` is two-thirds of a logger already:

- an on/off switch read once at startup and cached
- a swappable output target, so tests can listen in without touching a stream
- writes kept from tangling, so that one record is always one whole line
- the guarantee that a record is written in a single call, never split in two

That last pair matters because **three parts of the game write at the same
time**: the main game, the background room-builder, and the storyteller. Any
logger that doesn't copy this discipline will produce braided, half-written
lines the first time two threads speak at once.

There is also a turn counter that any part of the program can read instantly,
with no database access. That is a gift — every log entry can be stamped with
the turn it happened on, for free.

## The shape

Six fields per entry:

```
2026-08-05 14:30:22.184  INFO   main    turn=0   world  opened world.db (existing)
2026-08-05 14:30:41.902  DEBUG  bard    turn=3   bard   rejected: motive not in vocabulary
2026-08-05 14:30:44.310  WARN   main    turn=3   prose  render failed, using template text
2026-08-05 14:31:02.771  ERROR  pregen  turn=7   pregen job failed: timeout after 20s
```

Time, level, which part of the program was running, which turn, which area of
the engine, then the message.

The "which part was running" column is not decoration. Without it the three
threads' messages land jumbled and you can't tell whose is whose; with it you
can read one thread of activity at a time.

The turn number isn't standard in a logging system, but it is the single most
useful key for connecting a message to what the player was doing.

### The source label is already there

Every message already begins with the name of the part that wrote it —
`aiRender:`, `bard:`, `architect:`, `writeGeneratedRoom:`. That's a structured
field wearing a prose costume. Lift it out of the format string into its own
column and the file becomes filterable by subsystem.

## Where the existing messages land

| Level | What goes there |
|-------|-----------------|
| ERROR | The caught crashes: storyteller wake failure, background job death, room-builder failure, the save-file-too-old refusal, the final crash message |
| WARN | The quiet downgrades: AI description failed → canned text; AI command understanding failed → plain parser. Plus the "this shouldn't be possible" missing-exit case in `mutations.cpp:516` |
| DEBUG | Every "the AI sent back something malformed, threw it away" message. There are many, they're routine, and they are the reason levels are needed at all |
| INFO | Mostly doesn't exist yet — session started, world created or resumed, AI on or off |

**The INFO gap is worth fixing as part of this.** Today the code only speaks
when something goes wrong, so a healthy session produces an empty file — and an
empty file can't be told apart from broken logging. A few routine entries at
startup fix that.

## Decisions taken

- **All internal records go to the log.** Including the performance timing
  records, which today go to the screen behind their own switch.
- **The player is never told anything.** No on-screen notice, not even when the
  AI degrades. The log is where you look.
- **One file per session**, named with year-first date and time
  (`textworld-20260805-143022.log`) so files sort chronologically by name.
- **Best-effort.** Try to open the file once at startup. If it fails, the game
  plays normally and nothing is recorded. No retry, no re-opening mid-session —
  a game that quietly starts logging on turn 40 is worse than one that never
  logs.

### The one carve-out

Two messages fire when there is no game on screen: the save-file-version refusal
in `world.cpp:178` and the crash message in `main.cpp:114`. The binary is
refusing to start, or is already dead. Those aren't interrupting anything, and
the terminal is the only channel left.

## Two places to catch the messages

This is the fork worth thinking hardest about, and the answer is probably both.

**At the call sites.** A logging module shaped like `profile.hpp`, plus ~40
mechanical edits. Gives structure: level, source, turn, and a way for tests to
listen. Gives no *guarantee* — the next print statement someone adds lands on
the game screen, and so does anything libcurl or SQLite decides to say.

**On the way out.** Three lines at startup that point the whole error channel at
the log file. No call-site edits, and airtight: nothing from any library at any
level can reach the screen. But the file is just relocated noise — no levels, no
turn numbers, no labels.

Together they're cheap, and "the game screen stays clean" becomes something the
program enforces rather than something every future commit has to remember.

**Consequence of best-effort opening:** if the file can't be created, the
catch-all has nowhere to point. Leaving it aimed at the terminal is exactly the
thing being prevented, so it must point at nothing and discard. Odd to write
down, but it's the only version consistent with the rule.

## Open

**Do the timing records stay behind their switch?** They're off by default today
because they were built to cost nothing when nobody's watching. If everything
goes to the log, either they stay switched off (and the log is complete only
when you remember) or they become always-on (and every session carries full
timing, at the cost of a clock reading per turn). Leaning always-on, but it's a
behavior change and should be decided on purpose. Their format is strict and
pinned byte-for-byte by tests, so they'd keep their exact wording and simply sit
after the normal timestamp and level.

**Log files pile up forever.** One per session with no cleanup means a full
folder a year out. Usual answers: keep the newest N, or delete anything older
than a couple of weeks. Doing nothing is legitimate for a game only one person
runs.

**Level threshold at runtime.** An environment variable would match the
conventions the game already uses for its AI and timing settings — run at WARN
normally, DEBUG when hunting something.

**The tests.** `tests.cpp:10320` has a helper that captures these messages the
old way, and `tests.cpp:10332` asserts a particular message is *absent*. A
proper logging module makes that test cleaner, but the catch-all redirect would
need to stay switched off in the test binary.

## The thing being hidden on purpose

Worth recording plainly, since it's a deliberate cost rather than an oversight.
When the AI fails to write a description, the game silently falls back to canned
text. Today the only sign is the message about to be hidden. After this change
the player gets duller writing and no explanation — and neither does the
developer, until someone opens the log.

That is the accepted trade: the game screen is for the game.
