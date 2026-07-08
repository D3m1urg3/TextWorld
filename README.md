# TextWorld

A single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. The engine owns rules, state, and consistency; AI owns prose, character, and (eventually) new territory. Protected canon — room descriptions, engine-authored refusals — is passed to the model verbatim and mechanically checked, never regenerated or paraphrased.

> **Note:** this project is unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld). The name may change.

## Current state

The **engine foundation**, the **AI prose renderer**, and the **AI action resolver** — the input-side mirror of narration — over a two-room, hand-authored world.

- SQLite is the live world store — the world *is* the database file (`world.db`).
- One turn = one tick = one SQLite transaction. No world write ever happens outside a tick.
- Every state change is paired with an event row in the same transaction; the `events` table is a complete transcript of the playthrough.
- All player-visible output is rendered from event rows plus read-only lookups — the renderer never writes.
- State persists across restarts, and copying the database file forks an independent world.

**AI narration** replaces the flat templates with Claude-generated second-person prose, grounded in a facts payload built from the turn's events (never the raw database). It is read-only and additive: the AI path performs SELECTs only, and every failure — no key, HTTP error, timeout, refusal, or a response that fails the mechanical validation gate — falls back silently to the original templates for that turn. The template renderer stays intact as the permanent fallback, so a turn never fails to produce output. Exits, visible items, and inventory lines are always appended deterministically by the engine, never left to the model. See [AI narration](#ai-narration) below to enable it.

**AI input resolution** is the input-side mirror of narration: with AI enabled, a raw input line is lowered to the engine's fixed instruction set by Claude tool-use *before* the parser runs, so natural phrasings like `pick up the lantern`, `grab the key`, or `head north` resolve to the same actions the fixed verbs produce. It is read-only and additive on the same terms — SELECTs only, one call per line, an 8-second timeout, no retries — and shares narration's single on/off switch and its silent fallback on any failure. The model recognizes nouns only; whether an action actually applies stays the engine's decision, so a resolved `take` for an item that isn't in the room fails the ordinary way.

There are no NPCs, no puzzles, and no world generation yet. The fixed-verb parser is no longer a throwaway harness — it is the **permanent deterministic fallback** for input, the input-side analog of the template renderer: it handles every line when the resolver is disabled, declines, or fails.

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

On first launch it creates `world.db` and seeds the starting world: a stone hall and a walled garden, a brass lantern, and a rusty iron key. On later launches it resumes exactly where you left off.

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

With AI enabled (see below), you can type these as natural phrasings too — `pick up the lantern`, `head north`, `grab the key` — and the resolver lowers them to the actions above. Anything it can't map falls through to the fixed verbs, and a line neither can resolve is declined without consuming a turn.

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

With AI enabled, a turn makes up to two synchronous Claude calls — one to resolve the input line, one to narrate the result — each with an 8-second timeout and no retries; a slow or failed call falls back (to the fixed-verb parser or that turn's template, respectively). No network access happens in template mode.

### World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game not running, `cp world.db copy.db` — the copy is a fully independent world.
- **Inspect:** the file is a plain SQLite database; `sqlite3 world.db` and poke around. The `events` table holds the full transcript.

## Testing

```sh
./build/tests
```

Runs the engine test suite against temporary world files. Exit code 0 means all tests passed. Coverage includes world seeding, movement, take/drop, all three failure tiers (including mid-tick fault injection and rollback), persistence across reopen, and file-copy portability. Both AI features — the prose renderer and the input resolver — are tested with fake HTTP transports, so the default run needs no network and no API key.

Live smoke tests for both AI features hit the real API and are gated behind `TEXTWORLD_AI_LIVE_TEST=1` (with a real `ANTHROPIC_API_KEY`), skipped otherwise:

```sh
TEXTWORLD_AI_LIVE_TEST=1 ANTHROPIC_API_KEY=sk-ant-... ./build/tests
```

## Project layout

```
src/        engine sources (built into the twcore static library); prose.cpp is the AI renderer, nlresolve.cpp the AI input resolver
tests/      test suite (hand-rolled micro-harness, no framework)
seed/       base.sql — the hand-authored starting world
vendor/     SQLite and nlohmann/json amalgamations
.lore/      vision, specs, designs, plans, and retros
```

`.lore/vision.md` describes where the project is headed and the design principles that govern it.
