# TextWorld

A single-player text adventure. A normal game engine owns the rules, the state,
and the numbers. AI writes the prose, plays the characters, and builds new rooms
when you walk off the edge of the map.

The engine never hands the rules to the model. Room descriptions and
engine-written messages are printed word for word, never rewritten by AI.

> **Note:** unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld).
> The name may change.

## What works today

- **The engine.** Rooms, items, movement, inventory, and a full event log.
- **AI prose.** Turn results are described by Claude instead of flat templates.
- **AI input.** You can type normal English instead of fixed verbs.
- **AI world building.** Walking an unbuilt exit generates a new room on the spot.
- **Combat.** Fully deterministic. Enemies are locks, spells are keys, no dice.
- **A storyteller.** Writes the cast and the story beats, then places them in
  rooms as real objects you can look at.
- **`examine`.** Every one of those things can be inspected.
- **Characters you can talk to.** They answer in their own voice and remember
  what you said.

Every AI feature is optional. Without an API key the game still runs, using
templates and fixed verbs.

### How the engine is put together

- The world is a SQLite file (`world.db`). There is no separate save format.
- One command = one turn = one database transaction. Nothing is written outside
  a turn.
- Every change also writes a row to the `events` table, in the same
  transaction. That table is the full transcript of your playthrough.
- Everything printed on screen is built from event rows plus read-only lookups.
  The part that prints never writes.
- State survives restarts. Copying the database file gives you an independent
  world.

### What is missing

- **Major characters have no way into the world.** The storage, the profile
  loader, and the conversation code all handle them, but nothing places one in a
  room yet. They are meant to arrive, and arrival is not built.
- Characters don't move, act, or leave.
- The setting is still a hand-written file, not something the game evolves.
- **The story arc is stored and walked, but invisible.** Its steps are written by
  hand in the seed file rather than by the storyteller, and reaching one changes
  nothing you can see. Both halves are next.

## Building

You need CMake 3.20+ and a C++20 compiler. SQLite and nlohmann/json are
vendored. libcurl comes from the system (`find_package(CURL REQUIRED)`) — macOS
already has it; on Linux install a dev package such as `libcurl4-openssl-dev`.

```sh
cmake -B build
cmake --build build
```

You get two binaries in `build/`: `textworld` (the game) and `tests`.

## How to play

Run the game from wherever you want the world file to live:

```sh
./build/textworld
```

On first launch it creates `world.db` and builds the starting world: a dormitory
cell and a dark corridor, a white candle, a cold iron key, an ashwood wand, and
a goblin grunt in the corridor. Later launches pick up where you left off.

You are a student at Thornmere Hall, a manor-castle school of magic, on a night
when something has come up from the breached lower halls. There is no quest log
and no objective text. You get a place to be in, things to look at, people to
talk to, and invaders to get past.

### Commands

| Command | Effect |
|---|---|
| `look` | Describe the room, its exits, and what you can see |
| `examine <noun>` / `x <noun>` | Look closely at one thing — an item, an enemy, a person, scenery |
| `go <direction>` | Move through an exit (e.g. `go north`) |
| `take <item>` | Pick up an item in the room |
| `drop <item>` | Drop something you're carrying |
| `inventory` | List what you're carrying |
| `say <something>` | Talk to the character in the room (e.g. `say who are you`) |
| `attack` | Hit the enemy in the room. Always available, fixed damage |
| `cast <spell>` | Cast a spell you know that is off cooldown (e.g. `cast ward`) |
| `read <grimoire>` | Study a dropped grimoire to learn its spell, permanently |
| `spells` | List your spells and what they do. Free, costs no turn |
| `wait` | Pass time |
| `quit` | Exit |

Directions: `north`, `south`, `east`, `west`, `up`, `down`, `in`, `out`.
`attack` also answers to `hit`, `kill`, and `fight`. A bare spell name (`ward`,
`fire`) casts it.

### Turns

Every command except `quit` and `spells` uses up a turn. That includes failed
attempts the game understands, like walking into a wall or talking to an empty
room.

`spells` is free because it tells you about the rules rather than doing
something in the world. Checking it mid-fight costs nothing and no enemy moves.
It lists only spells you have actually learned.

When a turn passes, every enemy in the room also gets its move, in the same
transaction. That's why examining a goblin mid-fight costs you health — it is a
normal action, and the damage clock doesn't care what you spent the turn on.

With AI on, you can type normally: `pick up the candle`, `head north`, `grab the
key`, `swing at the goblin`, `burn it`, `look at the desk`. Anything the AI
can't map falls through to the fixed verbs. A line neither can understand is
refused **without using a turn**, so typos aren't punished.

With AI on you can also walk off the edge of the map. The `Exits:` line lists
exactly the directions you can use, and walking one that has no room yet builds
it and steps you through. See [AI world generation](#ai-world-generation).
Without AI those exits are hidden and that move gives the usual
`You can't go that way.`

### Tips

**Read the band, not the prose.** The block above the prompt has every number
that matters: your HP, spell cooldowns, enemy HP, what is winding up, and what
effects are running. The prose comes from a model; the band comes from the
engine. If they disagree, the band is right.

**`[WINDING UP]` has three right answers.** That enemy lands a heavy blow next
turn. Kill it, `cast stun` to cancel the strike, or `cast ward` to block the
damage. Doing anything else is choosing to take the hit.

**Fights are puzzles, not damage races.** There is no randomness anywhere. If an
enemy shrugs off everything, you're using the wrong key: try another element, or
`dispel` if your attacks are breaking against a barrier. `attack` always does
its floor damage, so nothing is ever unwinnable — but grinding is rarely the
answer.

**Discovery is permanent.** Hit an enemy type with an element once and that
resistance shows on its row forever, across deaths and restarts. It's worked out
from the event log, so it can't be lost. Experimenting costs a turn and buys
knowledge you keep.

**Power is spells, not stats.** No levels, no XP. Defeated enemies drop
grimoires; `read` one to learn its spell for good. That's the whole progression,
and learned spells survive being downed.

**Being downed is not dying.** At zero HP you wake in the dormitory cell at full
health, having dropped what you were carrying where you fell. Your spellbook is
never lost. The enemy resets too, so a bad fight costs you the walk back and
your items, not the run.

**Examine everything, especially people.** The prose is printed word for word
and is the only place some details exist. It's also how you find out whether the
figure in the room is worth talking to.

**Talk to characters like people.** With AI on you rarely need the `say` verb —
just type what you want to say. Ask questions, be rude, lie. They answer in
character and remember it. What they won't do is act: no character can open a
door or hand you an item.

**You can't break the world by playing badly.** Every failure — a bad model
response, a timeout, no API key at all — falls back to something playable. And
the world file is just SQLite: copy `world.db` before trying something reckless
and you have a save.

## AI

By default the game runs on templates and prints `AI narration off — template
mode` at startup. Setting `ANTHROPIC_API_KEY` turns on all AI features at once:

```sh
ANTHROPIC_API_KEY=sk-ant-... ./build/textworld
```

### Environment variables

| Variable | Effect |
|---|---|
| `ANTHROPIC_API_KEY` | Turns AI on when set and non-empty. The key is sent only in the request's `x-api-key` header. It is never logged, stored, or written to the world file. |
| `TEXTWORLD_AI` | Off switch. Set to exactly `0` to force template mode even with a key. Any other value, or unset, leaves AI on. |
| `TEXTWORLD_MODEL` | Overrides the model for every role. Without it, input parsing uses `claude-haiku-4-5` and narration, world generation, the storyteller, and dialogue use `claude-opus-4-8`. |
| `TEXTWORLD_LOG_LEVEL` | `error`, `warn`, `info`, or `debug`, case-insensitive. Defaults to `info`; anything unrecognized also means `info`. Everything goes to `logs/textworld-*.log` and nothing reaches the terminal. `debug` also turns on profiling. The level never changes what you see on screen. |

### What AI does

**Narration.** Claude writes second-person prose instead of the flat templates.
It gets a summary built from the turn's events, never the raw database. It only
reads. Exits, visible items, and inventory lines are always added by the engine.

**Input.** Before the parser runs, Claude turns your typed line into one of the
engine's fixed commands. It only recognizes nouns — whether an action actually
works stays the engine's call, so `take` for an item that isn't in the room
fails normally.

**World generation.** See [AI world generation](#ai-world-generation).

**Dialogue.** See [Talking to characters](#talking-to-characters).

### When AI fails

Any failure — no key, an HTTP error, a timeout, a refusal, or a response that
doesn't pass validation — silently falls back for that turn:

| Feature | Fallback |
|---|---|
| Narration | The original template text |
| Input | The fixed-verb parser |
| World generation | `You can't go that way.` (the exit stays open, try again) |
| Dialogue | `You get no reply.` |

The template renderer and the fixed-verb parser are permanent. They are not
scaffolding to be removed — they are what the game runs on when AI is off or
fails, so a turn always produces output.

### Cost per turn

With AI on, a turn makes up to two calls: one to read your input, one to narrate
the result. Walking into an unbuilt room adds a third. A talk turn is still two —
the narrator doesn't run, so the reply takes its slot.

Each call times out after 20 seconds and is never retried. Template mode makes
no network calls at all.

Measured live, a talk turn takes about **3.4 s**, the same as a narrated turn.
That's the honest number behind the deferred streaming work: waiting three and a
half seconds for a room description is fine, waiting it for a person to answer
you is not.

All roles share one libcurl handle owned by the process, so only the first call
of a session pays for DNS, TCP, and TLS. Every later call reuses the connection.
This is free, but small: a turn is about 99.9% model latency, so connection
reuse buys back well under 1% of it. The changes that would actually help —
streaming narration, building neighbour rooms ahead of time — are deferred.
Profiling now exists to measure them.

Set `TEXTWORLD_LOG_LEVEL=debug` and each turn writes `twprof key=value` lines to
the session log: one per stage (`resolve`, `tick`, `narrate`, `generate`,
`total`) and one per network call, with curl's timing breakdown, the role, the
model, and token counts. Failed calls are marked and carry no token counts. A
stage that didn't run is left out rather than reported as zero. A measured run
is written up in `.lore/work/validation/turn-latency-polish/findings.md`.

## The session log

The terminal is the game screen. Only game text reaches it. Everything the
engine has to say about itself — a rejected AI response, a fallback to
templates, a failed background job — goes to a file.

One file per session, created at startup in a `logs/` directory next to
`world.db`, named `textworld-YYYYMMDD-HHMMSS.log` so listings sort by time. The
20 most recent are kept; older ones are deleted at startup. Files that don't
match that pattern are never touched.

Logging is best effort. If the file can't be created the game plays normally and
records nothing — no retry, no message, no error exit.

Each entry is one line with six fields: timestamp, level, thread, turn, source,
message.

```
2026-08-05 14:30:44.310  WARN   main    turn=3   prose   aiRender: failed, falling back to templates: timeout
```

The turn number tells you what the player was doing. The thread field (`main`,
`pregen`, or `bard`) tells you which part of the program was talking. Entries
are flushed as they're written, so a session killed mid-turn still has
everything up to that point.

Two messages still reach the terminal, because they happen when there is no game
on screen: the schema-mismatch refusal and the fatal-error message. Both are
also written to the log.

No log entry at any level contains an API key. Nothing at `info` or above
contains a prompt, a response body, or what the player typed.

## AI world generation

Each room's exits are decided when the room is created. Every direction is
either a real opening or a wall. An opening whose room doesn't exist yet is
**latent**, and walking it is what triggers generation.

With AI on, walking a latent exit generates one room, writes it, and moves you
in. What the model gets is small and fixed: the setting text, the name and
description of the room you're leaving, and the direction. No ids, no map, no
history. It returns a room name, a description, and the directions leading
onward.

The engine does the rest: it assigns the id, adds the exit back the way you came
(`north↔south`, `east↔west`, `up↔down`, `in↔out`), turns each declared onward
direction into a new latent exit, and commits it all inside the same turn. A
generation either lands whole or not at all. The model never invents structure
and never sees an id.

Generated rooms are permanent. Walk back and forth and it's the same room.

The `Exits:` line is truthful: it lists every direction you can act on, whether
the room behind it exists yet or not, and nothing else. A direction not listed
is a wall. So rooms can be dead ends or corridors rather than eight-way
junctions — how many ways lead on is the generator's choice, room by room.

With AI off, latent exits are hidden and the world is frozen at whatever was
already built. Because generation only fires on latent exits, the starting world
ships with a few of them off the corridor so a fresh world has somewhere to
grow.

The setting lives in `seed/setting.txt` — freeform prose about the world's tone,
premise, and scale, loaded into the world once at creation. Edit it before first
launch (or delete `world.db` and relaunch) to grow a different kind of world. An
empty or missing file just gives you plainer rooms. What ships is Thornmere
Hall.

Deferred: coarse-to-fine detail with background prefetch (generation currently
stalls the turn for one round trip), a world-size cap, and merging rooms that
should be the same place. The world currently grows as a tree — every exit leads
to a brand-new room, so no two openings ever meet.

## Combat

Combat is a deterministic puzzle, not a dice game. Enemies are locks and spells
are keys. The engine owns every number — damage, health, cooldowns, resistances
— and there is no randomness anywhere, so the same inputs always give the same
fight. It needs no AI and works fully in template mode.

You're in combat whenever an enemy shares your room. Each turn you take one
action — `attack` or `cast`, never both — and then every enemy takes its turn,
all in one transaction.

A fixed **chip** of damage lands every turn no matter what, so health is a
clock. Even perfect play costs something, and a fight you can't solve is a fight
you'll lose. `attack` always deals a fixed non-zero amount to anything, so no
fight is ever a dead end — but grinding is rarely enough.

Enemies have four kinds of lock:

- **Telegraph** — the enemy winds up a heavy blow one turn before it lands. Your
  move in that window is the answer: **ward** blocks the strike, **stun**
  cancels it and interrupts the enemy.
- **Element** — a resistance table of exact whole-number ratios, no floats. The
  right element hits harder; the wrong one is shrugged off. A basic `attack`
  ignores the table and always deals its floor.
- **Defense** — a **barrier** blocks all damage until **dispel** strips it. Two
  keys in sequence.
- **Multiplicity** — a swarm of low-health bodies, answered by area damage or a
  damage-over-time effect that reaches all of them.

A defeated enemy drops a grimoire. `read` it to learn its spell permanently,
surviving death and restart. That is the only progression — there are no levels,
no XP, and no growable stat anywhere in the database.

At zero health you are **downed, not dead**. You wake in the dormitory cell at
full health, having dropped what you carried where you fell. Your spellbook is
never lost, and the enemy resets to its starting state.

You can **flee** through an exit that already exists — the enemy gets one
parting shot as you go — but never into an unbuilt one.

The starting world places one goblin by hand so combat works immediately. The
generator grows the rest. A `bestiary` catalog in the seed data is the mold every
enemy is cast from: when the generator makes a room it may place at most one
enemy, picking a type from a menu the engine computes. The model sees only a
short blurb for each type and picks a costume. The engine copies every number
from the catalog.

That menu is filtered so the game can't deadlock: it only offers locks you can
already solve, or easy enemies that teach you a key you're missing. The first
spawn is always beatable with basic attacks and drops a starter spell. And it's
shaped by an **invasion front** — rooms near the breached core are dangerous,
the far edges are safe.

With AI off, or on any generation failure, no enemy is placed.

## Talking to characters

When a character is in your room you can talk to it. `say <something>` always
works. With AI on you can usually just type what you mean, and the resolver
decides whether a line is an action or speech. It prefers the action when the
line clearly is one, so `take the key` stays a `take` even with someone
standing there.

A talk turn has one shape: your line is recorded, one model call produces the
reply, and the reply prints **word for word**. The narrator doesn't run at all —
the reply *is* the AI output for that turn, and a narrator would paraphrase the
one thing this feature refuses to paraphrase. Dialogue therefore costs no more
per turn than ordinary narration.

### What characters won't do

Three rules are built into the engine rather than written into any character
file, because a rule players will attack can't live somewhere an author might
edit or forget:

- No character explains a mechanic. Not a weakness, a cooldown, a resistance, or
  a number, however you ask.
- No character volunteers background you didn't ask for.
- No character names a place, person, or object the world hasn't established.

On top of that, **no character can act at all**. Agreeing to open a door changes
nothing, because there is no code path from a conversation to a world change. A
talk turn writes exactly two event rows, at most one character profile, and at
most one memory summary. Nothing else.

This is deliberate. The common failure in AI-driven games is players talking an
NPC into handing over quest items, which disconnects persuasion from the game's
own systems. Prompt rules exist too, but they're a backup. The structure is the
real defence, and a test checks it by counting database rows across a turn where
the character happily agrees to hand something over.

### Refusals

Both use up the turn, like every refusal the game understands.

| Situation | What you get |
|---|---|
| Nobody here | `There is no one here to talk to.` |
| An enemy in the room | `There is no time for talk in a fight.` |

The nobody-here check runs first, so an empty room that happens to contain a
goblin tells you there's nobody to talk to rather than implying there was.

Every AI failure — no key, timeout, transport error, malformed response — gives
the same `You get no reply.` and still uses the turn. That line is a fixed
constant precisely so a failure can never show up as invented dialogue.

A database error is handled differently: it rolls the whole turn back rather
than printing the no-reply line, because that line would report an unchanged
world as a changed one.

### Memory

A character's identity is written once, on first contact, and never edited, so
it can't drift. Its memory of your conversation is rewritten freely and capped —
it's a reconstruction, and a character misremembering costs you nothing
mechanically.

These are two separate jobs on purpose. Research found that personas degrade
badly when one store does both, and that generic summarising strips out exactly
the details that make a character themselves.

Conversation lines are ordinary event rows, so the transcript that already
records every change is also the raw memory. There is no second log to drift
from the first.

Once twenty unsummarised lines pile up, the same call that answers you also
folds the conversation into a fresh summary, so the prompt stays bounded however
long you talk.

The prompt is split by how often things change. The engine's rules, the
character's identity, and the setting go in the system block. The memory
summary, recent lines, and what you just said go in the user message. That's the
prompt-cache boundary, and it's tested: two conversations with the same
character but different memory produce byte-identical system blocks.

### Major characters

They exist as a shape. Hand-written profiles load from `seed/majors/` at world
creation and the conversation code accepts them, but nothing places one in a
room. They're meant to arrive rather than spawn, and arrival isn't built.
`seed/majors/` doesn't ship, so a fresh world has none.

## The storyteller

The storyteller writes the world's cast and story beats, and the world generator
brings them into rooms.

### Where the facts live

A `catalog` table holds story entries. It is **append-only** — there is no code
that edits an entry's text, so a correction has to be added as a new entry and
the contradiction stays visible instead of being quietly absorbed. Two fields
can change, both one-way switches guarded in SQL.

Entries pick a motive from a closed list of eight, and the storyteller keeps two
freeform notes for itself.

An entry that claims something about combat ("the rime-touched fear fire") is
checked against the real resistance table before it's accepted, and refused if
the claim isn't true. So the world can teach you a weakness through fiction
without ever lying about the rules.

### The story arc

Underneath the cast sits the arc: three sentences saying what is happening, what
the invaders want, and what would settle it — plus a short ordered list of steps
walking toward it.

Each step names a condition the engine can check and a line of prose describing
the world once that step is reached. There are four kinds of condition: enemies
defeated, rooms discovered, a spell learned, and how deep from the start you have
gone. The list of kinds is closed, and a step naming a condition the engine
cannot check is refused when it is written — a step may not promise something
nothing can verify.

The engine walks the list; the storyteller never advances anything and is never
asked to. Once a turn, after everything else that turn has resolved and inside
the same transaction, the engine looks at the lowest step you have not reached —
only that one, never the ones after it — and if its condition holds, marks it
reached. At most one step per turn. The mark is one-way: a step counts as reached
because its condition was true at least once, so walking back the way you came
does not undo it.

**There is no clock in any of this.** No condition reads the turn counter, and
waiting cannot advance the story — fifty turns of `wait` leave the arc exactly
where it was. Escalation here is distance and what you have done, like everything
else; a timer would turn the invasion into a flood, which is the one thing the
setting rules out.

Today this is storage and a rule and nothing more. **Nothing tells you about
it.** Reaching a step writes a row to the event log and is deliberately invisible
to both the AI narrator and the template renderer, so the game reads exactly as
it did before. The parts that would make it visible — new rooms knowing which
step you are on, the narrator saying the world has shifted — are not built.

### What gets offered

Eligibility is based on distance, like everything else here. Each entry records
how deep from the start it belongs, and it's only offered once you're that far
in — measured with the same graph distance the invasion front uses, so story and
danger escalate together rather than on two separate dials.

An entry that teaches something about combat is only offered where its subject
is actually nearby, decided by calling combat's own eligible-enemy menu rather
than keeping a second copy of its rules.

There are no per-entry unlock flags, and no column to hold one. What's available
follows from where you are and what you can already solve.

What crosses the wire is a handle, a blurb, and a motive's meaning. Never an id,
a depth, or an internal tag. The event log the storyteller reads goes through
the same filter that keeps machine tokens out of narration.

### When it runs

**Once, at world creation**, the game blocks and says so while the whole cast and
the story beats are written in a single call. Blocking here is the point, not a
compromise: generated rooms are permanent, so an opening area written before the
story existed would be story-less forever. This one call is allowed sixty
seconds — the only exception to the engine's uniform timeout.

**After that it wakes only on irreversible change**: a room generated, an enemy
defeated, a spell learned, a beat made real, a story step reached. Never on
movement, never on a `look`, and never more than once every five turns however
fast you play.

Each waking runs on a background thread with its own database connection, so the
turn that triggered it is already on your screen before the call starts. Three
kills across three turns is one waking, not three. A trigger arriving mid-waking
earns exactly one more look afterwards, not a queue.

Everything the storyteller proposes is re-checked against the world as it is
when it commits, not as it was when the request was sent. A waking that took a
while can't write something that has since become false.

Inside a batch, a malformed entry is dropped and its siblings still land. Only a
transport or parse failure rejects a whole response. A waking that calls no tool
is a **success** — declining to act is a legitimate turn.

The two prompts both forbid deadlines and countdowns. Escalation here is
distance, and a ticking clock would undercut the mechanic the whole system rests
on.

### It is skippable by design

The overture failing, the thread never starting, every waking failing, or the
commit itself faulting all leave you with byte-for-byte the game you'd have had
with the storyteller switched off. This is checked by running one scripted
session seven ways and comparing the output.

### Getting into rooms

When the generator writes a new room, it's handed the story entries eligible for
that room alongside the enemy menu it already had. It may bring in **at most
one**.

The division is strict: the catalog says *who or what it is*, the generator says
*how it looks here*. The blurb the model chose from is selection text and is
never written into the world — the prose that lands is written fresh for that
room.

What appears is a **real object with a real parser noun**, not a sentence in a
description. The whole point of the storyteller was to stop the world's facts
from living only in prose.

The handle is re-checked at commit time. A room pre-generated fifty turns ago
that names an entry since placed elsewhere places nothing, and the room is still
made. Same for malformed input: a bad selection is dropped and never rejects a
room, and an entry arriving without its prose is dropped whole rather than
half-written.

The generator still can't write. It does reads and one network call, with every
write going through the same sanctioned helper as before.

One thing is deliberately *not* forgiving: a real database error while placing
an entry rolls the turn back instead of quietly becoming a wall.

## Examine

`examine <noun>` — or `x` — prints an object's prose **word for word**, the same
treatment room descriptions get, with an engine-written fallback for the few
things that have none.

Scope is the room and your hands, with no portability check. That's the whole
difference from `take`, and it's what makes enemies, characters, and fixed
scenery inspectable — previously the narrator and the input resolver only saw
things that could be picked up.

Nothing mechanical is added. Health, hostility, and resistance are the status
band's job, so examining reads the world's prose, never its numbers.

It costs a turn like any other action, which means examining mid-fight advances
the damage clock.

## The status band

Every turn ends with a band printed just above the prompt:

```
-- corridor -------------------------------------------------
 Exits    east, south, up
 Objects  key
 Enemy    goblin grunt  HP: 8/8  [WINDING UP]  slow 1
 You      HP: 11/12  Stun: ready  Ward: 2
```

It prints on *every* turn, including turns the game refuses. Rows only appear
when they have content, so an empty room collapses to a header and your HP.

Every number is written by the engine and read straight from the world. It's
composed once, in the turn loop, from read-only queries, so the AI and template
paths produce identical bytes. The band never sees narration text and no
model-supplied number can reach it.

`[WINDING UP]` means that enemy has a heavy blow landing next turn: kill it,
stun it, or ward.

Effects show as kind plus **turns remaining** (`slow 1`, `dot 2`, `ward 1`) —
duration is what you can act on. Spell readiness is either `ready` or the number
of turns left.

Once you've hit an enemy type with an element, its resistance shows on that row
from then on: `fire x1/2` for a resistance, `fire x2` for a weakness, `fire x1`
for no effect. Untested elements show nothing. This survives the enemy's death
and reopening the world, because it's worked out from the event log rather than
stored.

### Color and width

Color uses the basic 16 ANSI colors only, so it goes through your terminal
theme. It's suppressed by the usual signals:

| Variable | Effect |
|---|---|
| `NO_COLOR` (set, non-empty) | No color. Bold survives, so telegraphs stay visible |
| `CLICOLOR_FORCE` (set, non-`0`) | Color even when piped |
| `CLICOLOR=0` | No color |
| `TERM=dumb` | No escape sequences at all, bold included |

Piping or redirecting emits no escape bytes, so `./build/textworld > log.txt` is
clean. Every colored fact is also carried by its text, so nothing is lost
without color.

Width comes from `ioctl(TIOCGWINSZ)` on stdout, then `COLUMNS`, then 80. It's
re-checked each turn, so resizing is picked up on the next one. Long rows wrap
onto continuation lines aligned to the content column. The band never truncates
and never drops an exit, object, or status to fit — a narrow terminal makes it
taller, not quieter.

## World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game closed, `cp world.db copy.db`. The copy is a fully
  independent world.
- **Inspect:** it's a plain SQLite database. Run `sqlite3 world.db` and look
  around. The `events` table has the full transcript.
- **Schema changes:** the file records the schema version it was built with, and
  the game **refuses to open an older one** rather than migrating it. There are
  no migrations until there's a world worth keeping. It prints what it found,
  what it expected, and what to do — delete `world.db` and relaunch. The current
  version is 8.

## Testing

```sh
./build/tests
```

Runs the suite against temporary world files. Exit code 0 means everything
passed. No network and no API key needed — all AI features are tested with fake
HTTP transports.

Covered:

- **Engine:** world seeding, movement, take/drop, persistence across reopen,
  file-copy portability, and all three failure tiers including mid-turn fault
  injection and rollback.
- **World generation:** room creation, reciprocal exits, declared exits becoming
  latent stubs, movement across wall/latent/real exits, truthful exit display,
  no regeneration on revisit, and atomic fallback leaving no orphan rows.
- **Combat:** the enemy turn, the chip clock, telegraph and counters, cooldowns,
  all four lock types, status effects, the grimoire economy, being downed,
  fleeing, the bestiary catalog, the filtered enemy menu, generator spawning,
  and a replay proving two identical runs produce byte-identical worlds.
- **Latency:** the profiling gate and record format, per-stage timers, the
  per-role model rule and its override, and the response body parsers.
- **The storyteller's fact store:** schema shape, the motive vocabulary, every
  argument refusal, the truth gate against real resistance rows, the two one-way
  switches, safe truncation, and rollback inside a caller's transaction.
- **Selection:** the four eligibility gates and their order, determinism across
  close and reopen, both request bodies (including a sweep proving no id, depth,
  or internal tag reaches the wire), every validation drop and rejection, and
  admission — where a false claim leaves zero rows while its siblings commit.
- **Scheduling:** the worker's lifecycle, one waking at a time, coalescing, the
  rate ceiling, commit leaving the world clock untouched, and a stale waking
  applying what still holds and dropping what doesn't. Every thread is driven by
  a fake transport with no sleeps anywhere.
- **Conversation:** both refusals and their order, the character-versus-beat
  target rule, the request body shape, all eight AI failures producing
  byte-identical output, and leniency dropping a malformed profile or summary
  without costing the reply.
- **Memory:** both table shapes, the write-once profile latch and its refusal of
  orphans, the free-rewrite memory, safe caps, and the bounded line read —
  including the case where the cap would return a reply without its question.
- **The story arc:** both table shapes and the version gate, the closed
  condition vocabulary in the shipped seed and the test fixture alike, every
  admission refusal, all four conditions false-then-true through the sanctioned
  path, the one-way mark, exhaustion, an empty list, one step per turn, the rule
  refusing to look past the lowest step you have not reached, and a fault on the
  advance path rolling the whole turn back — the mark with it.

Some claims get sharper treatment than a normal test:

- **Append-only is asserted against the source text**, not just behavior — that
  no code edits a catalog entry, and that one file is the only writer. Same for
  two orderings that would be undefined behavior if broken: joining both worker
  threads before libcurl is torn down, and flushing the player's text to screen
  before the storyteller gets the turn.
- **The storyteller is skippable** is a test, not a promise: one scripted
  session run seven ways, with all six failing runs compared byte-for-byte
  against the first, including the turn counter and the whole event log.
- **Materialization doesn't regress the generator** is checked as a property of
  the diff: every existing generator, pre-generation, and combat test passes
  unmodified. The whole feature adds test code and edits none.
- **The prompt-cache boundary** is asserted as byte-identity between two system
  blocks built either side of a memory write.
- **The id shield** is a sweep over both prompt builders for a character given a
  deliberately distinctive id, tier, flag, and handle — none of which may appear
  anywhere in the request.
- **A conversation can't write to the world** is checked by counting every
  component table's rows across a turn where the character agrees to hand over a
  key, with a full-row checksum on `health` so a value change with a stable
  count is caught too.
- **`examine` changed nothing else** is pinned by a golden session: a scripted
  playthrough of every other verb, captured byte-exact before `examine` existed.
- **The story arc changes no narrated byte** is pinned by a second golden
  session, captured before the feature existed and compared byte-for-byte after
  — once with the five steps in place, where the run really does reach two of
  them, and once with the list emptied. The same transcript from both
  directions, so neither an advance nor its absence can move a character.
- **Nothing in the arc reads a clock** is asserted twice: against the source
  text of the condition check alone — not the whole file, where reading the turn
  counter to stamp a mark is correct — and behaviourally, by waiting fifty turns
  and finding every step still unreached.
- **Two guarantees were verified by mutation**, run by hand and reverted: that
  the memory summary is stamped with the previous turn (an off-by-one would
  silently lose the last exchange of every conversation, with nothing failing),
  and that a database fault during a talk turn rolls back rather than printing
  the no-reply line. Both were seen turning the suite red before being restored,
  because a guard never seen to fail is not a guard.

Live tests hit the real API and are gated behind `TEXTWORLD_AI_LIVE_TEST=1`,
skipped otherwise:

```sh
TEXTWORLD_AI_LIVE_TEST=1 ANTHROPIC_API_KEY=sk-ant-... ./build/tests
```

## Project layout

```
src/        engine sources, built into the twcore static library
tests/      test suite (hand-rolled, no framework)
seed/       starting world, bestiary, setting text, the story arc and its steps,
            optional major-character profiles
logs/       one session log per run, 20 kept (runtime, git-ignored)
vendor/     SQLite and nlohmann/json amalgamations
.lore/      vision, specs, designs, plans, and retros
```

Inside `src/`:

| File | What it does |
|---|---|
| `prose.cpp` | AI narration |
| `nlresolve.cpp` | AI input resolution |
| `architect.cpp` | AI world generation, plus offering and placing story entries |
| `combat.cpp` | The deterministic combat system |
| `band.cpp` | The status band |
| `term.cpp` | Terminal services — color gating, width, wrapping |
| `aihttp.cpp` | Shared HTTP client and the per-role model rule |
| `log.cpp` | The session log |
| `profile.cpp` | Turn profiling |
| `bard.cpp` | Storyteller eligibility, wire format, validation, overture, scheduling |
| `bardworker.cpp` | The storyteller's background thread (no database access at all) |
| `npc.cpp` | Conversation — lookup, refusals, prompt, validation, one call per turn |
| `systems.cpp` | Turn resolution, and the rule that walks the story arc |
| `mutations.cpp` | The only place world writes happen |

`prose.cpp`, `nlresolve.cpp`, `architect.cpp`, `bard.cpp`, and `npc.cpp` are
read-only apart from calls into `mutations.cpp`. None of them contains a raw
write statement, which is checked by grep.

`.lore/vision.md` describes where the project is headed and the design
principles behind it.
