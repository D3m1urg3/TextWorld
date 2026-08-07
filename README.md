# TextWorld

A single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. The engine owns rules, state, and consistency; AI owns prose, character, and new territory. Protected canon — room descriptions, engine-authored refusals — is passed to the model verbatim and mechanically checked, never regenerated or paraphrased.

> **Note:** this project is unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld). The name may change.

## Current state

The **engine foundation**, the **AI prose renderer**, the **AI action resolver** — the input-side mirror of narration — the **AI architect**, which grows the world at its unmapped edges, a **deterministic combat system** — enemies as locks, spells as keys — that the architect populates as a self-balancing invasion front, a **live storyteller** that authors the cast and the beats and brings them into rooms as real entities, an **examine verb** that makes every one of them inspectable, and **characters you can talk to**, who answer in their own voice and remember what you said.

- SQLite is the live world store — the world *is* the database file (`world.db`).
- One turn = one tick = one SQLite transaction. No world write ever happens outside a tick.
- Every state change is paired with an event row in the same transaction; the `events` table is a complete transcript of the playthrough.
- All player-visible output is rendered from event rows plus read-only lookups — the renderer never writes.
- State persists across restarts, and copying the database file forks an independent world.

**AI narration** replaces the flat templates with Claude-generated second-person prose, grounded in a facts payload built from the turn's events (never the raw database). It is read-only and additive: the AI path performs SELECTs only, and every failure — no key, HTTP error, timeout, refusal, or a response that fails the mechanical validation gate — falls back silently to the original templates for that turn. The template renderer stays intact as the permanent fallback, so a turn never fails to produce output. Exits, visible items, and inventory lines are always appended deterministically by the engine, never left to the model. See [AI narration](#ai-narration) below to enable it.

**AI input resolution** is the input-side mirror of narration: with AI enabled, a raw input line is lowered to the engine's fixed instruction set by Claude tool-use *before* the parser runs, so natural phrasings like `pick up the candle`, `grab the key`, or `head north` resolve to the same actions the fixed verbs produce. It is read-only and additive on the same terms — SELECTs only, one call per line, a uniform 20-second timeout, no retries — and shares narration's single on/off switch and its silent fallback on any failure. The model recognizes nouns only; whether an action actually applies stays the engine's decision, so a resolved `take` for an item that isn't in the room fails the ordinary way.

**AI world generation** is the first *read-write* AI feature — the world is no longer fixed at two rooms. A room's exits are **declared at its birth**: every direction is either a real opening or a wall, fixed when the room is written. An opening whose room does not exist yet is **latent**, and walking it is what triggers generation. With AI enabled, walking a latent exit generates one new room, coherent with the setting (a hand-authored `seed/setting.txt` loaded into canon at init) and with the room you are leaving, writes it to canon, and moves you in. The model co-authors the room's prose *and* the directions that lead onward from it via Claude tool-use; the engine mints the id, adds the reciprocal exit back, plants each declared direction as its own latent exit, and enforces every invariant (invertible directions only, no duplicates, the return exit is the engine's), so the model never invents structure and never sees an id. A generated room is permanent — walk back and forth and it is the same room, never regenerated. A direction the room never declared is a hard wall; a latent exit under a disabled AI or a failed generation falls back to the original `You can't go that way.` while staying open and retryable — so an edge behaves exactly as it does today whenever generation can't run. The write is confined to one sanctioned engine helper; the architect translation unit itself issues no raw SQL.

**Combat** is a deterministic **puzzle**: enemies are locks, spells are keys, and the engine owns every number — there is no RNG anywhere. It rides the same tick model (one prompt = one tick = one transaction) and the same seams — new verbs, engine-owned mutation helpers, template + AI narration — so a fight is just more events in the same transcript. See [Combat](#combat) below.

**Turn latency** is now measurable, and a little cheaper. All three AI roles share one persistent libcurl handle owned by the process, so only the first call of a session pays DNS, TCP, and TLS — every later call, of any role and across idle gaps, reuses the connection. Input resolution runs on a fast model while narration and generation keep the prose model. Gated profiling (`TEXTWORLD_LOG_LEVEL=debug`) reports per-phase wall-clock and a per-call setup-vs-TTFB split — into the session log, not onto the screen — which is how the honest accounting below got made: a turn is ~99.9% model latency, so connection reuse buys back well under 1% of it. It is free and permanent; the levers that would actually move a turn — streaming narration, pre-generating neighbor rooms — are deferred, and profiling now exists to measure them.

**The status band** is the engine's answer to "where am I and what is happening": an ANSI-colored block printed above the prompt on *every* turn — including turns the world declines — showing the room, its exits, the objects in it, every living hostile with its HP, telegraphed strikes and active states, and your own HP and spell readiness. It is composed once, in the turn loop, from read-only queries, so the AI and template paths get identical bytes and no model-supplied number can ever reach it. Prose is wrapped to the terminal width, color follows the `NO_COLOR` / `CLICOLOR` conventions and vanishes entirely when output is piped, and nothing is ever truncated to fit — a narrow terminal makes the band taller, never quieter. See [The status band](#the-status-band) below.

**The bard's fact store** is the first piece of the live storyteller, and it is the layer the other two stand on. It adds where the story's facts will live and, more importantly, who may change them: a `catalog` table the storyteller appends entries to, a closed vocabulary of eight character motives, and two freeform notes it keeps for itself. The load-bearing property is **append-only, enforced by the absence of a code path** — there is no helper that edits an entry's text, so a correction has to be appended as a new entry and the contradiction stays visible in the table instead of being absorbed into it. Two fields move, both one-way latches guarded in SQL. Entries that claim something about combat ("the rime-touched fear fire") are checked against the real resistance table at admission and refused if the claim is not mechanically true, so the world can teach you a weakness through fiction without ever lying about the rules. The tables ship empty; what fills them is the overture described two paragraphs down.

**The bard's catalog selection** is the second piece, and it settles what the storyteller may be offered and what the engine will accept back. Eligibility is **spatial**, like everything else here: each entry records how deep from the start it belongs, and it is offered only once you are that far in — measured with the same graph distance the invasion front uses, so story and danger escalate on one dial rather than two. An entry that teaches something about combat is offered only where its subject is actually live nearby, decided by *calling* combat's own eligible-archetype menu rather than by keeping a second copy of its rules. There are no per-entry unlock flags and no column to hold one: what is offerable follows from where you are and what you can already solve, never from a hand-wired prerequisite. What crosses the wire is a handle, a blurb, and a motive's meaning — never an id, a depth, or an internal tag, and the event log the storyteller reads is passed through the same shield that keeps machine tokens out of narration. Coming back, a malformed entry inside a batch is dropped and its siblings still land, while only a transport or parse failure rejects a response outright; a waking that calls no tool at all is a **success**, because declining to act is a legitimate turn. Every entry is re-checked at admission against the same rules the write helper enforces — so a claim that isn't mechanically true costs the whole entry, while a genuine database fault still propagates and rolls the batch back instead of being mistaken for a bad entry. Two prompts ship with it, both forbidding deadlines and countdowns: escalation here is distance, and a ticking clock would undercut the one mechanic the whole system rests on.

**The bard now runs.** The two pieces above stop being groundwork: the storyteller writes, and the engine schedules it. Once — on the very first launch of a world file, and never again — the game blocks and says so while the whole cast and the beats are authored in one call. Blocking is the point rather than a compromise: generated rooms are canon forever, so an opening area written before the story existed would be permanently story-less, and the sixty seconds this is allowed to take is the one deliberate exception to the engine's uniform call budget. After that the storyteller **wakes only on irreversible change** — a room generated, an enemy defeated, a spell learned, a beat made real — never on movement, never on a look, and never more than once every five turns however frantically you play. A waking runs on its own background thread with its own connection, so the turn that triggered it is already on your screen before the call begins; a burst of three kills across three turns is one waking, not three, and a trigger that arrives mid-waking earns exactly one more look afterwards rather than a queue of them. Everything it proposes is re-checked against the world as it is at the moment it commits, not as it was when the request was sent, so a waking that took a while cannot write something that has since become false. And the whole thing is built to be **skippable**: the overture failing, the thread never starting, every waking failing, or the commit itself faulting all leave you with byte-for-byte the game you would have had with the storyteller switched off — asserted by running one scripted session seven ways and comparing the output byte for byte.

**The cast now enters the world.** This is the seam the other three bricks were built toward: when the world generator writes a new room, it is handed the story entries eligible for that room alongside the enemy menu it already had, and it may bring **at most one** of them in. The division is strict — the catalog supplies *who or what it is*, the generator supplies *how it looks here*. The blurb the model chose from is selection text and is never written into the world; the prose that lands is written fresh for that room. What materializes is a **real entity with a real parser noun**, not a sentence in a description: the whole point of the storyteller was to stop the world's facts from living only in prose, and an entry that materialized as flavor text would have defeated it. The handle is re-checked against the world **as it is at the moment of commit**, not as it was when the request was sent — a room pre-generated fifty turns ago that names an entry since placed elsewhere places nothing, and the room is still made. That leniency runs all the way down: a malformed selection is dropped and never rejects a room, and an entry arriving without its prose is dropped whole rather than half-written. The generator itself still cannot write — it performs only reads and one network call, with every write going through the same sanctioned helper as before, asserted by grep. And the placement sits deliberately **outside** the failure boundary that turns a bad AI call into a wall: a genuine database fault while placing rolls the turn back instead of being quietly downgraded, which is a distinction only a fault-injection test can see and so has one.

**You can look at any of it.** `examine <noun>` — or `x` — prints an entity's canon prose **verbatim**, the treatment room descriptions already get, with an engine-authored fallback for the few things that have none. Scope is the room and your hands, with no portability check: that is the whole difference from `take`, and it is what makes enemies, characters, and fixed scenery inspectable where the narrator's facts and the resolver's noun list previously saw only what could be picked up. Nothing mechanical is appended — health, hostility, and resistance stay the status band's business — so examining is a way to read the world's prose, never a way to read its numbers. It costs a turn like any other action the world understands, which means examining mid-fight ticks the chip clock.

**Characters remember, and characters answer.** A character's *identity* is written once and never edited; its *memory* is rewritten freely and capped. That split is load-bearing rather than tidy — the research round found persona consistency degrading sharply when one store carries both jobs, and found that generic summarisation strips out exactly the details that make a character themselves. Conversation lines are ordinary event rows, so the transcript that already records every mutation is also the raw memory, and there is no second log to drift from the first. Type `say <something>` — or, with AI on, just talk — and the character in the room answers in its own voice, printed **verbatim**, with no narrator paraphrasing it. On first contact the same call that produces the reply also writes that character's identity document, permanently; once the conversation runs long the same call folds it into a memory summary, so the prompt stays bounded across a session instead of growing with it. See [Talking to characters](#talking-to-characters) below.

The load-bearing property is that **a conversation cannot write to the world**. A talk turn produces two event rows, at most one profile, and at most one memory summary — a character cannot open a door, hand over an item, or change a number, because no code path exists for it. The research round named the failure this prevents: players social-engineer NPCs into surrendering quest-critical items, which decouples persuasion from the game's own systems. Prompt rules exist too, but they are defence in depth; the structure is the defence, and it is asserted by counting component-table rows across a turn in which the character cheerfully agrees to hand something over.

What is still missing: **major characters have an identity but no way in.** The store, the hand-authored profile loader, and the conversation all handle them — but nothing places one in a room, deliberately, on three separate guards. They are meant to *arrive* rather than spawn, and arrival is unbuilt. Characters also do not move, act, or leave. The setting itself remains a hand-authored seed. The fixed-verb parser is no longer a throwaway harness — it is the **permanent deterministic fallback** for input, the input-side analog of the template renderer: it handles every line when the resolver is disabled, declines, or fails.

## Building

Requirements: CMake ≥ 3.20 and a C++20 compiler. Dependencies are the vendored SQLite and nlohmann/json amalgamations plus system libcurl (`find_package(CURL REQUIRED)`). libcurl ships with macOS, so there is still nothing to install there; on Linux install a libcurl dev package (e.g. `libcurl4-openssl-dev`).

```sh
cmake -B build
cmake --build build
```

This produces two binaries in `build/`: `textworld` (the game) and `tests` (the test suite).

## How to play

Run the game from the directory where you want the world file to live:

```sh
./build/textworld
```

On first launch it creates `world.db` and seeds the starting world: a dormitory cell and a night-dark corridor, a white candle, a cold iron key, an ashwood wand — and a goblin grunt in the corridor, one of the invaders come up from the breached lower halls. On later launches it resumes exactly where you left off.

You are a student at Thornmere Hall, a manor-castle school of magic, on a night when something has come up from the breached lower halls. There is no quest log and no objective text. What you have is a place to be in, things to look at, people to talk to, and invaders to get past.

### Commands

| Command | Effect |
|---|---|
| `look` | Describe the current room, its exits, and visible items |
| `examine <noun>` / `x <noun>` | Look closely at one thing present — an item, an enemy, a person, the scenery |
| `go <direction>` | Move through an exit (e.g. `go north`) |
| `take <item>` | Pick up a portable item in the room |
| `drop <item>` | Drop a carried item |
| `inventory` | List carried items |
| `say <something>` | Speak to the character in the room (e.g. `say who are you`) |
| `attack` | Strike the hostile in the room (always available, fixed damage) |
| `cast <spell>` | Cast a known spell that is off cooldown (e.g. `cast ward`) |
| `read <grimoire>` | Study a dropped grimoire to learn its spell, permanently |
| `spells` | List what your known spells do — element, cooldown, effect. Costs no turn |
| `wait` | Pass time |
| `quit` | Exit the game |

Directions are `north`, `south`, `east`, `west`, `up`, `down`, `in`, `out`. `attack` also answers to `hit`, `kill`, and `fight`, and a bare spell name (`ward`, `fire`) is shorthand for casting it.

### How turns work

Every command except `quit` and `spells` consumes a turn — **including failed attempts the world understands**, like walking into a wall or talking to an empty room. `spells` is reference information about the rules rather than an action in the world, so checking it mid-fight is free: the turn counter does not move and no enemy acts. It lists only the spells you have actually learned; the catalog is not a spoiler list.

A turn that consumes time also gives every hostile in the room its one turn, in the same transaction. That is why examining a goblin mid-fight costs you health: it is an ordinary action, and the chip clock does not care what you spent the tick on.

With AI enabled (see below), you can type all of the above as natural phrasings — `pick up the candle`, `head north`, `grab the key`, `swing at the goblin`, `burn it`, `look at the desk` — and the resolver lowers them to the actions above. Anything it can't map falls through to the fixed verbs, and a line neither can resolve is declined **without consuming a turn**, so a typo is never punished.

With AI enabled you can also walk *off the edge of the map*: the `Exits:` line lists exactly the directions you can act on, and walking one whose room does not exist yet builds it on the spot and steps you through — see [AI world generation](#ai-world-generation) below. Without AI those not-yet-built exits are hidden, and that same move is the usual `You can't go that way.`

### Tips

**Read the band, not the prose.** The block above the prompt carries every number that matters — your HP, each spell's cooldown, every enemy's HP, what is winding up, and what states are running. Prose is written by a model; the band is written by the engine, and no model-supplied number can reach it. When the two seem to disagree, the band is right.

**`[WINDING UP]` is a question with three right answers.** An enemy winding up lands a heavy blow next turn. Kill it, `cast stun` to cancel the strike outright, or `cast ward` to negate the damage. Doing anything else that turn is choosing to take the hit.

**Fights are puzzles, not damage races.** There is no RNG anywhere. If an enemy is shrugging off everything, you are using the wrong key: try a different element, or `dispel` if attacks are breaking against a barrier. `attack` always does its floor damage, so nothing is ever a hard deadlock — but grinding is rarely the intended answer.

**Discovery is permanent, and it is the mechanic.** Hit an archetype with an element once and that resistance is shown on its row forever after, across deaths and restarts. It is derived from the event transcript, so it cannot be lost. Experimenting costs a turn and buys knowledge you keep.

**Power is keys, never numbers.** There are no levels and no XP. Defeated enemies drop grimoires; `read` one to learn its spell permanently. That is the entire progression, and a learned spell survives being downed.

**Being downed is not dying.** At zero HP you wake in the dormitory cell at full health, having dropped what you carried where you fell. Your spellbook is never lost. The enemy resets too, so a bad fight costs you the walk back and the items, not the run.

**Examine everything, especially people.** Canon prose is printed verbatim and is the only place some of the world's detail exists. It is also how you find out whether the figure in the room is worth talking to.

**Talk to characters as if they were people.** With AI on you rarely need the `say` verb — just type what you want to say. Ask questions, be rude, lie. They answer in character and remember it. What they will not do is act: no character can open a door or hand you an item, so treat them as a source of voice and rumour rather than a lock to be picked.

**You cannot break the world by playing badly.** Every failure — a bad model response, a timeout, no API key at all — falls back to something playable rather than stopping the game. And the world file is just SQLite: copy `world.db` before trying something reckless and you have a save.

### AI narration

By default the game runs in template mode and prints `AI narration off — template mode` once at startup. Setting `ANTHROPIC_API_KEY` enables both AI features — Claude-generated prose *and* natural-language input resolution — under a single switch:

```sh
ANTHROPIC_API_KEY=sk-ant-... ./build/textworld
```

Environment variables:

| Variable | Effect |
|---|---|
| `ANTHROPIC_API_KEY` | Enables both AI features (narration and input resolution) when set and non-empty. The key is sent only in the request's `x-api-key` header — never logged, stored, or written to the world file. |
| `TEXTWORLD_AI` | Kill switch for both AI features. Set to exactly `0` to force template + fixed-verb mode even with a key present. Any other value (or unset) leaves AI on. |
| `TEXTWORLD_MODEL` | Overrides the model **for every role**. Without it each role uses its own default: input resolution runs on `claude-haiku-4-5` (a constrained, schema-gated classification — a wrong answer fails the same validation gate and falls back to the fixed-verb parser), while narration, world generation, the storyteller, and dialogue stay on `claude-opus-4-8` for prose quality. Set and non-empty, this variable replaces all five. |
| `TEXTWORLD_LOG_LEVEL` | How much the engine records about itself: `error`, `warn`, `info`, or `debug`, case-insensitive. **Defaults to `info`**; unset, empty, or unrecognized values all mean `info`, silently. Everything lands in `logs/textworld-*.log` and **nothing ever reaches the terminal** — see [The session log](#the-session-log) below. `debug` is also the profiling switch: at that level each turn writes one `twprof key=value` line per phase (`resolve`, `tick`, `narrate`, `generate`, `total`) and per network call (curl's namelookup/connect/appconnect/starttransfer/total split, plus role, model, and token counts). Whatever the level, a turn's output and behavior on screen are byte-for-byte identical. |

With AI enabled, a turn makes up to two synchronous Claude calls — one to resolve the input line, one to narrate the result — and a `go` across an unmapped edge adds one more to generate the room. A **talk** turn is still two: the narrator does not run, so the reply takes the slot narration would have used. Each call has a 20-second timeout and no retries; a slow or failed call falls back (to the fixed-verb parser, that turn's template, the `You can't go that way.` wall, or `You get no reply.` respectively). No network access happens in template mode.

Measured on a live session, a talk turn costs about **3.4 s** wall clock, the same as an ordinary narrated turn. That is the honest number behind the deferred streaming work: waiting three and a half seconds for a room description is tolerable, and waiting it for a person to answer you is not.

All three roles go out through one shared client holding a single persistent libcurl handle, initialized and torn down at the process boundaries. The first call of a session pays the full DNS + TCP + TLS handshake; every later one reuses that connection and TLS session, so its setup time is effectively zero. The handle is main-thread-only by contract — nothing here starts a thread — which is recorded in `src/aihttp.hpp` for the deferred background pre-generation work.

Setting `TEXTWORLD_LOG_LEVEL=debug` makes all of this visible. Each turn writes `twprof` lines into the session log — stage durations, plus one record per network call carrying curl's timing split, the role, the model, and the token counts. The `twprof key=value` payload is unchanged from when these records went to stderr; it now follows the log's standard six-field prefix. Failed calls are marked and carry no token counts rather than fabricated zeros, and a stage that did not run is absent rather than reported as zero. A measured live run is written up in `.lore/work/validation/turn-latency-polish/findings.md`.

### The session log

The terminal is the game screen, and only game text reaches it. Everything the
engine has to say about itself — a rejected AI response, a fallback to template
prose, a failed background job — goes to a log file instead.

One file per session, created at startup in a `logs/` directory beside
`world.db`, named `textworld-YYYYMMDD-HHMMSS.log` so a directory listing sorts
chronologically. The 20 most recent are kept, the session's own file included;
older ones are deleted at startup, and a file in `logs/` that does not match
that pattern is never touched. Logging is **best effort**: if the file cannot be
created the game plays normally and records nothing — no retry, no message, no
non-zero exit.

Every entry is one line of six fields — timestamp, level, thread, turn, source,
message:

```
2026-08-05 14:30:44.310  WARN   main    turn=3   prose   aiRender: failed, falling back to templates: timeout
```

The turn number is what correlates a message with what the player was doing, and
the thread field (`main`, `pregen`, or `bard`) says which part of the program was
speaking. Entries are flushed as they are written, so a session killed mid-turn
still has everything up to that point.

Two messages are exempt and still reach the terminal, because they fire when
there is no game on screen: the schema-mismatch refusal and the fatal-exception
message. Both are written to the log as well.

No log entry at any level contains an API key, and none at `info` or above
contains a prompt, a response body, or the player's typed input.

### AI world generation

When AI is on and you walk a **latent** exit — an opening whose room has not been generated yet — the game generates that room synchronously and moves you in. The context sent to the model is deliberately small and fixed-size: the setting text, the name and canon description of the room you are leaving, and the direction — no ids, no map, no history. The model returns a room name, its description, and the directions that lead onward from it; the engine assigns the id, adds the reciprocal exit back (`north↔south`, `east↔west`, `up↔down`, `in↔out`), plants each declared onward direction as a new latent exit, and commits it all inside the same turn transaction, so a generation either lands whole or not at all. Because the new exit then points at a real room, re-crossing it never calls the model again.

The `Exits:` line is **truthful**: it lists every direction you can act on — rooms already reached and latent openings alike, rendered indistinguishably — and nothing else. A direction not listed is a wall. So rooms can now be dead ends or narrow passages rather than silent eight-way junctions; how many ways lead onward is the architect's call, room by room. With AI off, latent exits are hidden (walking one would only wall) and the world is a frozen tree of whatever was already generated. Because generation now fires only on latent exits, the shipped seed plants a small frontier — a couple of latent openings off the starting corridor — so a fresh world has somewhere to grow.

The setting lives in `seed/setting.txt` — a freeform prose document describing the world's tone, premise, and scale, loaded into the world once at creation. Edit it before first launch (or delete `world.db` and relaunch) to grow a different kind of world; an absent or empty file just yields plainer rooms. The shipped setting is Thornmere Hall, a manor–castle school of magic explored at night, coherent with the dormitory cell and corridor of the starting world.

Deferred for now: a live storyteller that evolves the setting, coarse-to-fine level-of-detail with background prefetch (generation currently stalls the turn for one round trip), a world-size cap, and de-duplicating rooms that should be the same place. The world grows as a **tree** — every declared exit spawns a brand-new room, so no two openings ever lead to the same place.

### Combat

Combat is a **deterministic puzzle**, not a dice game. Enemies are locks and spells are keys; the engine owns every quantity — damage, health, cooldowns, resistances — and **no RNG** is involved anywhere, so the same inputs always produce the same fight. It needs no AI: the whole system runs in template + fixed-verb mode.

You are in combat implicitly whenever a hostile shares your room. Each tick you take **one** action — `attack`, or `cast` a spell (never both) — and then every hostile present takes its single turn, all inside the one transaction. A **chip** of fixed damage lands every tick regardless, so health is a **clock**: even flawless play costs something, and a fight you can't solve is a fight you'll lose. `attack` always deals a fixed, non-zero amount to any enemy, so no encounter is ever a hard deadlock — but grinding through the wrong way is rarely enough.

Enemies express four kinds of **lock**, each answered by the right key:

- **Telegraph** — the enemy winds up a heavy blow one tick before it lands. Your action in that window is the counter: **Ward** negates the strike, **Stun** cancels it outright and interrupts the enemy.
- **Element** — a resistance/weakness table (exact integer ratios, no floats). The right element hits for extra; the wrong one is shrugged off. A basic attack ignores the table and always deals its floor.
- **Defense** — a **barrier** negates all damage until **Dispel** strips it: a two-key sequence.
- **Multiplicity** — a swarm of low-health bodies answered by area damage or a damage-over-time that reaches each of them.

A defeated enemy **drops a grimoire**; `read` it to add its spell to your book — **permanently**, surviving death and restart. That is the *only* progression: power is **keys known, never numbers grown** — there are no levels, no XP, no growable stat anywhere in the schema. Falling to zero health leaves you **downed, not dead**: you wake in the dormitory cell at full health, having dropped what you carried where you fell (your spellbook is never lost), and the enemy resets to its opening state. You can **flee** through an already-generated exit — the enemy takes one parting turn as you go — but never into an ungenerated one. An engine-authored status line reports your health and each spell's cooldown; it is never left to the model.

The shipped world hand-places one goblin so combat is exercisable immediately, and the **architect grows the rest**. A `bestiary` catalog (seed data, like the rooms) is the mold every enemy is cast from: when the architect generates a room it may place at most one enemy, **selecting** an archetype from an engine-computed eligible menu — the model sees only each archetype's short blurb and picks a costume; the engine mints every number by copying the catalog. The menu is gated so the economy can't deadlock (only locks you can already solve, or easy foes that teach a key you lack), seeded by a bootstrap rule (the first spawn is basic-soluble and drops a starter spell), and shaped by an **invasion front**: rooms near the breached core are contested, the far edges are safe. With AI off or on any generation failure, no enemy is placed — the same silent boundary as room generation.

### Talking to characters

When a character shares your room you can speak to it. `say <something>` always works; with AI on you can usually just type what you mean, and the resolver decides whether a line is an action or speech — preferring the action whenever the line clearly means one, so `take the key` stays a `take` even with someone standing there.

A talk turn has exactly one shape. Your line is recorded, one model call produces the reply, and the reply prints **verbatim** — the narrator does not run at all, because the reply *is* the AI output for that turn and a narrator would paraphrase the one thing this feature refuses to paraphrase. That also means dialogue costs no more per turn than an ordinary narrated one.

The prompt is split by **stability, not tidiness**: the engine's rules for every character, that character's identity document, and the setting are stable and go in the system block; the memory summary, the recent lines, and what you just said are volatile and go in the user message. That boundary is the prompt-cache boundary, and it is asserted as a property — two conversations with the same character whose memory differs produce byte-identical system blocks.

Three rules are engine-owned rather than authored into any character file, because a rule players will actively attack cannot live somewhere an author can edit or forget. No character will explain a mechanic — a weakness, a cooldown, a resistance, a number — however you ask; none volunteers background you did not ask for; and none names a place, person, or object the world has not already established. On top of that, no character can act at all: agreeing to open a door changes nothing, because there is no code path from a conversation to a component write.

Two refusals, both consuming the turn like every refusal the world understands:

| Situation | What you get |
|---|---|
| Nobody here | `There is no one here to talk to.` |
| A hostile in the room | `There is no time for talk in a fight.` |

The no-one-here check runs **first**, so an empty room that happens to contain a goblin tells you there is nobody to talk to rather than implying there was.

Every AI failure — no key, a timeout, a transport error, a malformed response — produces the same engine-authored `You get no reply.` and still ticks the turn. The fallback line is a constant precisely so a failure can never surface as a fabricated line of dialogue. A **database** fault is the one thing treated differently: it rolls the whole turn back rather than degrading to the no-reply line, because that line would report a world that did not change as one that did.

**Memory.** A character's identity document is written on first contact and never edited, so it cannot drift. Its memory of your conversation is rewritten freely and capped — it is a reconstruction, and a character misremembering costs nothing mechanical. Once twenty unsummarised lines accumulate, the same call that answers you also folds the conversation into a fresh summary, so the prompt stays bounded however long you talk.

**Major characters** exist as a shape — hand-authored profile files load from `seed/majors/` at world creation, and the conversation machinery already accepts them — but nothing places one in a room yet. They are meant to arrive rather than spawn, and arrival is not built. `seed/majors/` does not ship, so a fresh world has none.

### The status band

Every turn ends with a band printed directly above the prompt:

```
-- corridor -------------------------------------------------
 Exits    east, south, up
 Objects  key
 Enemy    goblin grunt  HP: 8/8  [WINDING UP]  slow 1
 You      HP: 11/12  Stun: ready  Ward: 2
```

Rows appear only when they have content, so an empty room collapses to a header and your HP. Every number in it is engine-authored and read straight from the world — the model never supplies one, and the band never sees narration text.

`[WINDING UP]` means that enemy has a heavy blow landing next turn: strike it down, stun it, or raise a ward. Enemy and player states show as kind plus **turns remaining** (`slow 1`, `dot 2`, `ward 1`) — duration is what you can act on. Spell readiness is either `ready` or the number of turns left.

Once you have hit an archetype with an element, that archetype's resistance to it is permanently known and shown on its row from then on — `fire x1/2` for a resistance, `fire x2` for a weakness, `fire x1` for an element it simply does not resist. Untested elements show nothing; discovery is the mechanic. It survives the enemy's death and reopening the world, because it is derived from the event transcript rather than stored anywhere.

**Color** uses the basic 16 ANSI colors only, so it resolves through your terminal theme, and it is suppressed by any of the usual signals:

| Variable | Effect |
|---|---|
| `NO_COLOR` (set, non-empty) | No color. Bold survives, so telegraphs stay distinct |
| `CLICOLOR_FORCE` (set, non-`0`) | Color even when output is not a terminal |
| `CLICOLOR=0` | No color |
| `TERM=dumb` | No escape sequences at all, bold included |

Piping or redirecting output emits no escape bytes of any kind, so `./build/textworld > log.txt` is clean. Every colored fact is also carried by its text, so nothing is lost without color.

**Width** comes from `ioctl(TIOCGWINSZ)` on stdout, falling back to `COLUMNS`, then to 80, re-checked each turn — resize is picked up on the next turn. Long rows wrap onto continuation lines aligned to the content column; the band never truncates or elides, and never drops an exit, object, or status to fit.

### World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game not running, `cp world.db copy.db` — the copy is a fully independent world.
- **Inspect:** the file is a plain SQLite database; `sqlite3 world.db` and poke around. The `events` table holds the full transcript.
- **Schema changes:** the world file records the schema version it was built with, and the game **refuses to open a file from an older one** rather than migrating it — there are no migrations until there is a world worth keeping. It prints what it found, what it expected, and what to do: delete `world.db` and relaunch. The NPC memory store took the schema to version 7, so a world file created before it needs exactly that.

## Testing

```sh
./build/tests
```

Runs the engine test suite against temporary world files. Exit code 0 means all tests passed. Coverage includes world seeding, movement, take/drop, world generation (creation, reciprocal exits, architect-declared onward exits as latent stubs, three-state movement across wall/latent/realized exits, truthful exit display, persistence/no-regeneration, and atomic fallback with no orphan rows), the full combat system (the enemy-turn tick, chip clock, telegraph/counter, cooldowns, all four lock categories, status effects, the grimoire→learn economy, the downed model, fleeing, the bestiary catalog, the gated eligible menu, architect enemy spawning, and a determinism replay asserting two identical runs produce byte-identical worlds), all three failure tiers (including mid-tick fault injection and rollback), persistence across reopen, and file-copy portability. All three AI features — the prose renderer, the input resolver, and the architect (including its enemy and story selection) — are tested with fake HTTP transports, so the default run needs no network and no API key. The latency work is covered the same offline way: the profiling gate and record format, the per-stage timers (including `generate` nested inside `tick`, and stages being absent rather than zero), the per-role model rule and its `TEXTWORLD_MODEL` override, and the pure `usage`/`model` body parsers. The bard fact store is covered the same way and needs nothing but SQL: the schema shape and seeded motive vocabulary, every argument refusal, the truth gate driven from the real resistance rows, the two one-way latches and their idempotence, the code-point-safe truncation of the storyteller's public note, and rollback behavior inside a caller-owned transaction. Its two append-only guarantees — that no code path edits a catalog entry, and that one translation unit is the only writer — are asserted against the **source text** so they survive as regression guards rather than being grepped once by hand. Catalog selection is offline too, and needs no key: the four composing eligibility gates and their ordering, the knowledge-beat gate driven from one world by moving only the room argument, determinism across a close and reopen, both context payloads and both request bodies (including a sweep asserting no id, depth, or internal tag reaches the wire, and a ninth motive row proving the tool schema's vocabulary is read from the database rather than hardcoded), both validation gates over canned responses — every drop reason, every rejection clause, and the no-tool-call waking asserted to emit no failure diagnostic at all — and admission, where a false claim is shown to leave zero rows while its siblings commit, duplicate handles inside one batch admit exactly one without throwing, and a caller's rollback discards the lot. The pre-flight that makes that possible is fenced by an equivalence test: it refuses a set of arguments **if and only if** the write helper throws on the same ones. The scheduling brick is offline too, and every one of its threads is driven by a fake transport with no sleeps anywhere: the worker's lifecycle matrix (asserted through a thread-existence hook rather than through an absent log line, because a thread never created and a thread sitting idle log identically), one waking in flight at a time with concurrent entry flagged, the coalescing flag proven to be a flag and not a counter, the rate ceiling and the case where it masks the flag — the common path, where the events are shown to survive anyway because the wake marker is stamped when a request is sent rather than when it returns — the marker asserted mid-call to pin exactly that, the commit path leaving the world's clock untouched and rolling back every one of its writes together on a fault, and a stale waking whose handles have since been taken applying the rest of itself and dropping only what no longer holds. The degradation claim is a test rather than a promise: one scripted session run seven ways — the storyteller off, the overture failed, the thread never started, the thread throwing, every waking unparseable, the commit faulting, and the evaluation itself throwing — with all six failing runs compared byte-for-byte against the first, along with the turn counter and the whole event log. Two orderings that are undefined behavior if broken, rather than merely wrong, are pinned as source-text regression guards: the declaration order in `main()` that joins both worker threads before libcurl is torn down, and the flush that puts the player's text on screen before the storyteller is given the turn. Materialization is covered offline too, and its first obligation is **not regressing the world generator**: every existing generator, pre-generation, and combat test passes **unmodified**, which is checked as a property of the diff rather than claimed — the whole brick adds test code and edits none. On top of that: the context and tool schema on an empty catalog asserted to be exactly what they were before the storyteller existed, with both new fields omitted rather than present-and-empty; the offered menu proven to be the **prospective** room's rather than the origin's, which after the read was hoisted into the caller is a property of the call site and so is driven end-to-end through a body-capturing transport; all eight ways a malformed selection is dropped, each asserting the room survived intact; an entry that was materialized between snapshot and commit resolving to nothing rather than minting a second copy; and a path-equivalence proof committing the same proposal through the pre-generated and synchronous paths into two worlds and comparing the rows and events. The claim that a database fault during placement rolls the turn back rather than degrading to a wall is pinned by a surgical fault injection with a **control arm** proving the injection touches nothing else — and the arm was itself checked by mutation: swallowing the exception, the exact downgrade the requirement forbids, turns the suite red.

The perception verb is pinned by a **golden session**: one scripted playthrough exercising every other verb, captured as a byte-exact literal before `examine` existed, so the claim that the new verb changed nothing else is a property of the diff rather than a promise. The NPC work is offline too, and needs no key. The memory store is pure SQL: both tables' shapes, the write-once profile latch and its refusal of orphans, the free-rewrite memory, the code-point-safe caps, and the bounded raw-line read — including the case where the cap would hand back a reply without its question, told apart from a legitimately leading reply by probing one row past the cap rather than guessing from the row count. Conversation is driven with fake transports throughout: both refusals and their fixed ordering, the character-versus-beat target rule, the exact top-level shape of the request body and its three-property tool, all eight enumerated AI failures asserted to produce **byte-identical output across every one of them**, and the leniency that drops a malformed profile or summary without costing the reply. Three properties get sharper treatment than a test each. The prompt-cache boundary is asserted as byte-identity between two system blocks built either side of a memory write. The id shield is a sweep over both prompt builders for a character deliberately given catalog id 317, entity id 419, tier 3, a `seeded` flag, and a distinctive handle — none of which may appear anywhere in the request. And the rule that a conversation cannot write to the world is checked by counting every component table's rows across a turn in which the character agrees to hand over a key, with a full-row checksum on `health` so a value change with a stable count is caught too.

Two guarantees are verified by **mutation**, run by hand and reverted: that the memory summary is stamped with the turn *before* the current one — an off-by-one that would silently lose the last exchange of every conversation, forever, with nothing failing — and that a database fault during a talk turn rolls the turn back rather than degrading to the no-reply line. Both were observed turning the suite red before being restored, because a guard never seen to fail is not a guard.

Live smoke tests for all AI features hit the real API and are gated behind `TEXTWORLD_AI_LIVE_TEST=1` (with a real `ANTHROPIC_API_KEY`), skipped otherwise:

```sh
TEXTWORLD_AI_LIVE_TEST=1 ANTHROPIC_API_KEY=sk-ant-... ./build/tests
```

## Project layout

```
src/        engine sources (built into the twcore static library); prose.cpp is the AI renderer, nlresolve.cpp the AI input resolver, architect.cpp the AI world generator (which also offers the storyteller's eligible entries and places the one chosen), combat.cpp the deterministic combat system, band.cpp the status band and term.cpp its terminal services (color gating, width detection, wrapping), aihttp.cpp the shared persistent-connection HTTP client and per-role model rule, log.cpp the session log (levels, the six-field entry format, the one file per session, and the redirect that keeps internal messages off the game screen), profile.cpp the turn profiling that rides on it, bard.cpp the storyteller's eligibility, wire format, validation gates, overture, and post-turn scheduling (read-only: every write goes through mutations.cpp), bardworker.cpp its background waking thread (no database access of any kind, by construction), npc.cpp the conversation verb — the character lookup, the two refusals, the dialogue prompt and its engine-owned rules, the reply validation gate, and the one model call per talk turn (read-only apart from mutations.cpp calls: the translation unit contains no raw write statement, asserted by grep)
tests/      test suite (hand-rolled micro-harness, no framework)
seed/       base.sql — the hand-authored starting world, the bestiary catalog, and the closed motive vocabulary; setting.txt — the freeform setting (including the invasion premise) that guides world generation; majors/ — optional hand-authored major-character profiles, read at world creation (does not ship; a world with none is valid)
logs/       one session log per run, 20 kept (created at runtime, git-ignored)
vendor/     SQLite and nlohmann/json amalgamations
.lore/      vision, specs, designs, plans, and retros
```

`.lore/vision.md` describes where the project is headed and the design principles that govern it.
