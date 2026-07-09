# TextWorld

A single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. The engine owns rules, state, and consistency; AI owns prose, character, and new territory. Protected canon — room descriptions, engine-authored refusals — is passed to the model verbatim and mechanically checked, never regenerated or paraphrased.

> **Note:** this project is unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld). The name may change.

## Current state

The **engine foundation**, the **AI prose renderer**, the **AI action resolver** — the input-side mirror of narration — and the **AI architect**, which grows the world at its unmapped edges, over a hand-authored starting world.

- SQLite is the live world store — the world *is* the database file (`world.db`).
- One turn = one tick = one SQLite transaction. No world write ever happens outside a tick.
- Every state change is paired with an event row in the same transaction; the `events` table is a complete transcript of the playthrough.
- All player-visible output is rendered from event rows plus read-only lookups — the renderer never writes.
- State persists across restarts, and copying the database file forks an independent world.

**AI narration** replaces the flat templates with Claude-generated second-person prose, grounded in a facts payload built from the turn's events (never the raw database). It is read-only and additive: the AI path performs SELECTs only, and every failure — no key, HTTP error, timeout, refusal, or a response that fails the mechanical validation gate — falls back silently to the original templates for that turn. The template renderer stays intact as the permanent fallback, so a turn never fails to produce output. Exits, visible items, and inventory lines are always appended deterministically by the engine, never left to the model. See [AI narration](#ai-narration) below to enable it.

**AI input resolution** is the input-side mirror of narration: with AI enabled, a raw input line is lowered to the engine's fixed instruction set by Claude tool-use *before* the parser runs, so natural phrasings like `pick up the candle`, `grab the key`, or `head north` resolve to the same actions the fixed verbs produce. It is read-only and additive on the same terms — SELECTs only, one call per line, an 8-second timeout, no retries — and shares narration's single on/off switch and its silent fallback on any failure. The model recognizes nouns only; whether an action actually applies stays the engine's decision, so a resolved `take` for an item that isn't in the room fails the ordinary way.

**AI world generation** is the first *read-write* AI feature — the world is no longer fixed at two rooms. With AI enabled, walking an exit that leads nowhere yet no longer just fails: an **architect** generates one new room, coherent with the setting (a hand-authored `seed/setting.txt` loaded into canon at init) and with the room you are leaving, writes it to canon, and moves you in. The model proposes only a name and a description via Claude tool-use; the engine mints the id, owns the exit and its mechanical reciprocal, and enforces every invariant, so the model never invents structure and never sees an id. A generated room is permanent — walk back and forth and it is the same room, never regenerated, because the exit now simply exists. Directions with no mechanical opposite (`northeast`, `widdershins`), a disabled AI, or any generation failure fall back to the original wall, `You can't go that way.` — so an unmapped edge behaves exactly as it does today whenever generation can't run. The write is confined to one sanctioned engine helper; the architect translation unit itself issues no raw SQL.

There are no NPCs and no puzzles yet, and the setting is static — a hand-authored seed, not a live storyteller evolving the world's facts. The fixed-verb parser is no longer a throwaway harness — it is the **permanent deterministic fallback** for input, the input-side analog of the template renderer: it handles every line when the resolver is disabled, declines, or fails.

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

On first launch it creates `world.db` and seeds the starting world: a dormitory cell and a night-dark corridor, a white candle, a cold iron key, and an ashwood wand. On later launches it resumes exactly where you left off.

### Commands

| Command | Effect |
|---|---|
| `look` | Describe the current room, its exits, and visible items |
| `go <direction>` | Move through an exit (e.g. `go north`) |
| `take <item>` | Pick up a portable item in the room |
| `drop <item>` | Drop a carried item |
| `inventory` | List carried items |
| `wait` | Pass time |
| `quit` | Exit the game |

Every command except `quit` consumes a turn — including failed attempts the world understands, like walking into a wall.

With AI enabled (see below), you can type these as natural phrasings too — `pick up the candle`, `head north`, `grab the key` — and the resolver lowers them to the actions above. Anything it can't map falls through to the fixed verbs, and a line neither can resolve is declined without consuming a turn.

With AI enabled you can also walk *off the edge of the map*: a `go` in a direction that leads nowhere yet builds a new room on the spot and steps you through it — see [AI world generation](#ai-world-generation) below. Without AI, that same move is the usual `You can't go that way.`

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

When AI is on and you walk an exit that has no room beyond it yet, the game generates that room synchronously and moves you in. The context sent to the model is deliberately small and fixed-size: the setting text, the name and canon description of the room you are leaving, and the direction — no ids, no map, no history. The model returns just a room name and description; the engine assigns the id, creates the exit and its reciprocal (`north↔south`, `east↔west`, `up↔down`, `in↔out`), and commits it all inside the same turn transaction, so a generation either lands whole or not at all. Because the new exit then exists in canon, re-crossing it never calls the model again.

The setting lives in `seed/setting.txt` — a freeform prose document describing the world's tone, premise, and scale, loaded into the world once at creation. Edit it before first launch (or delete `world.db` and relaunch) to grow a different kind of world; an absent or empty file just yields plainer rooms. The shipped setting is Thornmere Hall, a manor–castle school of magic explored at night, coherent with the dormitory cell and corridor of the starting world.

Deferred for now: a live storyteller that evolves the setting, coarse-to-fine level-of-detail with background prefetch (generation currently stalls the turn for one round trip), a world-size cap, and de-duplicating rooms that should be the same place. The world grows as a tree — each new room advertises only the way back.

### World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game not running, `cp world.db copy.db` — the copy is a fully independent world.
- **Inspect:** the file is a plain SQLite database; `sqlite3 world.db` and poke around. The `events` table holds the full transcript.

## Testing

```sh
./build/tests
```

Runs the engine test suite against temporary world files. Exit code 0 means all tests passed. Coverage includes world seeding, movement, take/drop, world generation (creation, reciprocal exits, persistence/no-regeneration, and atomic fallback with no orphan rows), all three failure tiers (including mid-tick fault injection and rollback), persistence across reopen, and file-copy portability. All three AI features — the prose renderer, the input resolver, and the architect — are tested with fake HTTP transports, so the default run needs no network and no API key.

Live smoke tests for all three AI features hit the real API and are gated behind `TEXTWORLD_AI_LIVE_TEST=1` (with a real `ANTHROPIC_API_KEY`), skipped otherwise:

```sh
TEXTWORLD_AI_LIVE_TEST=1 ANTHROPIC_API_KEY=sk-ant-... ./build/tests
```

## Project layout

```
src/        engine sources (built into the twcore static library); prose.cpp is the AI renderer, nlresolve.cpp the AI input resolver, architect.cpp the AI world generator
tests/      test suite (hand-rolled micro-harness, no framework)
seed/       base.sql — the hand-authored starting world; setting.txt — the freeform setting that guides world generation
vendor/     SQLite and nlohmann/json amalgamations
.lore/      vision, specs, designs, plans, and retros
```

`.lore/vision.md` describes where the project is headed and the design principles that govern it.
