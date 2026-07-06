# TextWorld

A single-player conversational text adventure where a deterministic game engine is extended — not replaced — by AI. The engine owns rules, state, and consistency; AI (eventually) owns prose, character, and new territory. Everything AI generates is written to canon and never regenerated.

> **Note:** this project is unrelated to [Microsoft TextWorld](https://github.com/microsoft/TextWorld). The name may change.

## Current state

The **engine foundation prototype**: a two-room, hand-authored world proving the core tech before any AI is involved.

- SQLite is the live world store — the world *is* the database file (`world.db`).
- One turn = one tick = one SQLite transaction. No world write ever happens outside a tick.
- Every state change is paired with an event row in the same transaction; the `events` table is a complete transcript of the playthrough.
- All player-visible output is rendered from event rows plus read-only lookups — the renderer never writes.
- State persists across restarts, and copying the database file forks an independent world.

There is no AI, no NPCs, no puzzles, and no world generation yet. The fixed-verb parser is a deliberately disposable test harness; natural-language input is the intended baseline once the AI action resolver lands.

## Building

Requirements: CMake ≥ 3.20 and a C++20 compiler. The only dependency is the vendored SQLite amalgamation — nothing to install.

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

### World files

- **Reset:** delete `world.db` and relaunch.
- **Fork:** with the game not running, `cp world.db copy.db` — the copy is a fully independent world.
- **Inspect:** the file is a plain SQLite database; `sqlite3 world.db` and poke around. The `events` table holds the full transcript.

## Testing

```sh
./build/tests
```

Runs the engine test suite against temporary world files. Exit code 0 means all tests passed. Coverage includes world seeding, movement, take/drop, all three failure tiers (including mid-tick fault injection and rollback), persistence across reopen, and file-copy portability.

## Project layout

```
src/        engine sources (built into the twcore static library)
tests/      test suite (hand-rolled micro-harness, no framework)
seed/       base.sql — the hand-authored starting world
vendor/     SQLite amalgamation
.lore/      vision, specs, designs, plans, and retros
```

`.lore/vision.md` describes where the project is headed and the design principles that govern it.
