# TextWorld

A single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. The engine owns rules, state, and consistency; AI owns prose, character, and new territory. Protected canon — room descriptions, engine-authored refusals — is passed to the model verbatim and mechanically checked, never regenerated or paraphrased.

> **Note:** this project is unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld). The name may change.

## Current state

The **engine foundation**, the **AI prose renderer**, the **AI action resolver** — the input-side mirror of narration — the **AI architect**, which grows the world at its unmapped edges, and a **deterministic combat system** — enemies as locks, spells as keys — that the architect populates as a self-balancing invasion front, over a hand-authored starting world.

- SQLite is the live world store — the world *is* the database file (`world.db`).
- One turn = one tick = one SQLite transaction. No world write ever happens outside a tick.
- Every state change is paired with an event row in the same transaction; the `events` table is a complete transcript of the playthrough.
- All player-visible output is rendered from event rows plus read-only lookups — the renderer never writes.
- State persists across restarts, and copying the database file forks an independent world.

**AI narration** replaces the flat templates with Claude-generated second-person prose, grounded in a facts payload built from the turn's events (never the raw database). It is read-only and additive: the AI path performs SELECTs only, and every failure — no key, HTTP error, timeout, refusal, or a response that fails the mechanical validation gate — falls back silently to the original templates for that turn. The template renderer stays intact as the permanent fallback, so a turn never fails to produce output. Exits, visible items, and inventory lines are always appended deterministically by the engine, never left to the model. See [AI narration](#ai-narration) below to enable it.

**AI input resolution** is the input-side mirror of narration: with AI enabled, a raw input line is lowered to the engine's fixed instruction set by Claude tool-use *before* the parser runs, so natural phrasings like `pick up the candle`, `grab the key`, or `head north` resolve to the same actions the fixed verbs produce. It is read-only and additive on the same terms — SELECTs only, one call per line, an 8-second timeout, no retries — and shares narration's single on/off switch and its silent fallback on any failure. The model recognizes nouns only; whether an action actually applies stays the engine's decision, so a resolved `take` for an item that isn't in the room fails the ordinary way.

**AI world generation** is the first *read-write* AI feature — the world is no longer fixed at two rooms. A room's exits are **declared at its birth**: every direction is either a real opening or a wall, fixed when the room is written. An opening whose room does not exist yet is **latent**, and walking it is what triggers generation. With AI enabled, walking a latent exit generates one new room, coherent with the setting (a hand-authored `seed/setting.txt` loaded into canon at init) and with the room you are leaving, writes it to canon, and moves you in. The model co-authors the room's prose *and* the directions that lead onward from it via Claude tool-use; the engine mints the id, adds the reciprocal exit back, plants each declared direction as its own latent exit, and enforces every invariant (invertible directions only, no duplicates, the return exit is the engine's), so the model never invents structure and never sees an id. A generated room is permanent — walk back and forth and it is the same room, never regenerated. A direction the room never declared is a hard wall; a latent exit under a disabled AI or a failed generation falls back to the original `You can't go that way.` while staying open and retryable — so an edge behaves exactly as it does today whenever generation can't run. The write is confined to one sanctioned engine helper; the architect translation unit itself issues no raw SQL.

**Combat** is a deterministic **puzzle**: enemies are locks, spells are keys, and the engine owns every number — there is no RNG anywhere. It rides the same tick model (one prompt = one tick = one transaction) and the same seams — new verbs, engine-owned mutation helpers, template + AI narration — so a fight is just more events in the same transcript. See [Combat](#combat) below.

There are no NPCs and no dialogue yet, and the setting is static — a hand-authored seed, not a live storyteller evolving the world's facts. The fixed-verb parser is no longer a throwaway harness — it is the **permanent deterministic fallback** for input, the input-side analog of the template renderer: it handles every line when the resolver is disabled, declines, or fails.

## Building

Requirements: CMake ≥ 3.20 and a C++20 compiler. Dependencies are the vendored SQLite and nlohmann/json amalgamations plus system libcurl (`find_package(CURL REQUIRED)`). libcurl ships with macOS, so there is still nothing to install there; on Linux install a libcurl dev package (e.g. `libcurl4-openssl-dev`).

```sh
cmake -B build
cmake --build build
```

This produces two binaries in `build/`: `textworld` (the game) and `tests` (the test suite).

## Playing

Run the game from the directory where you want the world file to live:

```sh
./build/textworld
```

On first launch it creates `world.db` and seeds the starting world: a dormitory cell and a night-dark corridor, a white candle, a cold iron key, an ashwood wand — and a goblin grunt in the corridor, one of the invaders come up from the breached lower halls. On later launches it resumes exactly where you left off.

### Commands

| Command | Effect |
|---|---|
| `look` | Describe the current room, its exits, and visible items |
| `go <direction>` | Move through an exit (e.g. `go north`) |
| `take <item>` | Pick up a portable item in the room |
| `drop <item>` | Drop a carried item |
| `inventory` | List carried items |
| `attack` | Strike the hostile in the room (always available, fixed damage) |
| `cast <spell>` | Cast a known spell that is off cooldown (e.g. `cast ward`) |
| `read <grimoire>` | Study a dropped grimoire to learn its spell, permanently |
| `wait` | Pass time |
| `quit` | Exit the game |

Every command except `quit` consumes a turn — including failed attempts the world understands, like walking into a wall.

With AI enabled (see below), you can type these as natural phrasings too — `pick up the candle`, `head north`, `grab the key`, `swing at the goblin`, `burn it` — and the resolver lowers them to the actions above. Anything it can't map falls through to the fixed verbs, and a line neither can resolve is declined without consuming a turn.

With AI enabled you can also walk *off the edge of the map*: the `Exits:` line lists exactly the directions you can act on, and walking one whose room does not exist yet builds it on the spot and steps you through — see [AI world generation](#ai-world-generation) below. Without AI those not-yet-built exits are hidden, and that same move is the usual `You can't go that way.`

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
| `TEXTWORLD_MODEL` | Overrides the model. Default `claude-opus-4-8`; `claude-haiku-4-5` is a cheaper, faster option. |

With AI enabled, a turn makes up to two synchronous Claude calls — one to resolve the input line, one to narrate the result — and a `go` across an unmapped edge adds one more to generate the room. Each call has an 8-second timeout and no retries; a slow or failed call falls back (to the fixed-verb parser, that turn's template, or the `You can't go that way.` wall, respectively). No network access happens in template mode.

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

### World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game not running, `cp world.db copy.db` — the copy is a fully independent world.
- **Inspect:** the file is a plain SQLite database; `sqlite3 world.db` and poke around. The `events` table holds the full transcript.

## Testing

```sh
./build/tests
```

Runs the engine test suite against temporary world files. Exit code 0 means all tests passed. Coverage includes world seeding, movement, take/drop, world generation (creation, reciprocal exits, architect-declared onward exits as latent stubs, three-state movement across wall/latent/realized exits, truthful exit display, persistence/no-regeneration, and atomic fallback with no orphan rows), the full combat system (the enemy-turn tick, chip clock, telegraph/counter, cooldowns, all four lock categories, status effects, the grimoire→learn economy, the downed model, fleeing, the bestiary catalog, the gated eligible menu, architect enemy spawning, and a determinism replay asserting two identical runs produce byte-identical worlds), all three failure tiers (including mid-tick fault injection and rollback), persistence across reopen, and file-copy portability. All three AI features — the prose renderer, the input resolver, and the architect (including its enemy selection) — are tested with fake HTTP transports, so the default run needs no network and no API key.

Live smoke tests for all three AI features hit the real API and are gated behind `TEXTWORLD_AI_LIVE_TEST=1` (with a real `ANTHROPIC_API_KEY`), skipped otherwise:

```sh
TEXTWORLD_AI_LIVE_TEST=1 ANTHROPIC_API_KEY=sk-ant-... ./build/tests
```

## Project layout

```
src/        engine sources (built into the twcore static library); prose.cpp is the AI renderer, nlresolve.cpp the AI input resolver, architect.cpp the AI world generator, combat.cpp the deterministic combat system
tests/      test suite (hand-rolled micro-harness, no framework)
seed/       base.sql — the hand-authored starting world and bestiary catalog; setting.txt — the freeform setting (including the invasion premise) that guides world generation
vendor/     SQLite and nlohmann/json amalgamations
.lore/      vision, specs, designs, plans, and retros
```

`.lore/vision.md` describes where the project is headed and the design principles that govern it.
