---
title: "Spec: two-room engine foundation prototype"
date: 2026-07-05
status: implemented
tags: [spec, engine, prototype, sqlite, turn-loop]
modules: [engine]
related: [.lore/work/design/engine-foundation.md, .lore/work/brainstorm/engine-foundation-cpp-sqlite.md, .lore/vision.md]
req-prefix: PROTO
---

# Spec: two-room engine foundation prototype

## Goal

Prove the foundation tech is solid: SQLite as the live world store, turn = tick = transaction, mutation/event discipline, render-from-events. Two hand-authored rooms, hardcoded disposable parser, no AI. Success = the loop runs, state persists across restarts, the world file can be copied to fork an independent world, and the event log is a complete transcript.

The fixed verb parser knowingly matches the vision's command-parser anti-goal: it is a temporary, disposable test harness that dies when the Action Resolver agent lands (design §7). The `Action` struct is the seam that makes the swap invisible downstream.

Out of scope: any AI/LLM call, NPCs, world generation, natural-language parsing beyond the fixed verb set, migrations, save slots.

**Interface contract:** the engine is invoked with no arguments, reads commands from stdin, writes to stdout, and uses the fixed world file `world.db` in the current working directory.

## Requirements

### Build & dependencies

**REQ-PROTO-1** — The project builds via CMake + Ninja as C++20 with exactly one external dependency: the vendored SQLite amalgamation (`vendor/sqlite3.c`, `vendor/sqlite3.h`) compiled into the build. Two targets exist: `textworld` (the game) and `tests`.

### World store

**REQ-PROTO-2** — On launch with no world file present, the engine creates the database, applies the schema, and executes `seed/base.sql`, producing: 2 rooms, a bidirectional exit pair between them, at least 2 portable items **with at least one in each room**, a player entity located in the first room, a canon `description` row for every room and item (the player entity intentionally carries no prose), and `meta.turn` initialized to 0.

**REQ-PROTO-3** — The database carries a `schema_version` in the `meta` table. On open, a version mismatch causes the engine to refuse to run with a message telling the user to delete the world file; it must not modify the mismatched database.

**REQ-PROTO-4** — World state lives in component-per-table form per the design (§3): `meta`, `entities`, `name`, `room`, `player`, `portable`, `description`, `location`, `exits`, `events`. Containment is uniform — `location.container` references any entity; a carried item's container is the player entity; inventory is a query, not a table. The player is an ordinary entity (tag + location row).

### Turn loop

**REQ-PROTO-5** — One player prompt = at most one tick. Each tick runs inside a single SQLite transaction that increments the turn counter exactly once and commits at tick end. No world write ever occurs outside a tick transaction.

**REQ-PROTO-6** — Failure handling follows three tiers. The parser's noun **vocabulary** is every `name` row in the world; **recognition** (is this word a thing that exists?) is distinct from **resolution** (can the action apply to it here and now?):
  a. **Unparseable input** — unknown verb, a verb missing its required argument (bare `go`, `take`, `drop`), or a noun matching no `name` row anywhere in the world: no transaction, no tick, no turn increment; user gets an error message; world file byte-identical afterward.
  b. **In-world refusal** — the input parses (verb known, noun recognized in vocabulary) but the world says no: `go` with no such exit; `take` of an entity that is non-portable, or not in the player's room, or already carried; `drop` of an item not carried. The tick stands — turn increments, a `failed` event row is written, no component table changes.
  c. **Engine error** (any exception mid-tick): transaction rolls back; turn counter unchanged; world state as if the prompt never happened.

**REQ-PROTO-7** — The verb set is exactly: `look`, `go <direction>`, `take <noun>`, `drop <noun>`, `inventory`, `wait`, `quit`. Behaviors: `go` moves the player through a matching exit; `take` moves a portable item in the player's room into the player; `drop` moves a carried item into the player's room; `look` describes the current room, its exits, and visible items; `inventory` lists carried items; `wait` passes time; `quit` exits the process cleanly. All except `quit` consume a tick, including `look` and `inventory`.

### Events & rendering

**REQ-PROTO-8** — Every component-table mutation is paired with an event row written in the same transaction (`moved`, `took`, `dropped`). Non-mutating ticks also write their event (`looked`, `waited`, `failed`). Consequence: the `events` table is a complete transcript — every committed turn number appears in it at least once.

**REQ-PROTO-9** — All player-visible output for a turn is produced by the renderer from that turn's event rows plus read-only world lookups (canon prose, names, exits). The renderer performs no writes, and no output describes a state change that lacks an event row.

### Persistence & portability

**REQ-PROTO-10** — Quitting and relaunching against the same world file resumes play with identical state: same player location, same item locations, same turn counter, full event history intact.

**REQ-PROTO-11** — Copying the world file (`cp world.db copy.db`) while the engine is not running yields an independent world: playing the copy does not affect the original, and both remain playable.

### Tests

**REQ-PROTO-12** — The `tests` target exercises engine functions against temp-file worlds using the hand-rolled micro-harness, covering at minimum: world creation + seed; movement (component change + event + turn increment); take/drop round-trip; wall-bump (failed event, turn increments, nothing else changes); `take` of a recognized-but-not-present item (tier b: tick + `failed` event) versus an unknown noun (tier a: no tick) — pinning the REQ-PROTO-6 boundary; missing-argument input (bare `go`) leaving turn count unchanged; a deliberately injected mid-tick exception verifying rollback (turn and state unchanged — tier c); persistence across close/reopen; **file-copy portability** (copy world file, play the copy, original unaffected); unparseable input leaving turn count unchanged. Exit code 0 = all pass, nonzero otherwise.

## AI Validation

How the AI verifies each requirement after implementation. Scripted play = piping commands to the binary's stdin; state checks = `sqlite3` CLI against the world file after the process exits.

1. **Build (REQ-PROTO-1):** `cmake -B build -G Ninja && ninja -C build` succeeds from clean checkout; `otool -L` output and CMakeLists show no external libs beyond system defaults; both binaries exist.
2. **Seed (REQ-PROTO-2):** delete world file, run `echo quit | ./textworld`; then `sqlite3 world.db` — count 2 rows in `room`, ≥2 in `portable` with at least one located in each room, 1 in `player`, exits both directions, `description` row count equals rooms+items, player's `location.container` = first room, `meta.turn` = 0.
3. **Version gate (REQ-PROTO-3):** `sqlite3 world.db "UPDATE meta SET value=999 WHERE key='schema_version'"`; relaunch; expect refusal message, nonzero exit; file hash unchanged before/after (excluding the manual UPDATE itself).
4. **Schema (REQ-PROTO-4):** `.schema` output contains the ten tables and no others; no `inventory` table exists.
5. **Tick discipline (REQ-PROTO-5, REQ-PROTO-7):** pipe a scripted session (`look`, `take <item in room 1>`, `go north`, `drop <item>`, `take <item in room 2>`, `inventory`, `wait`, `go north` bump, `quit`); afterward `meta.turn` equals the count of tick-consuming commands; output shows each verb behaving per REQ-PROTO-7.
6. **Failure tiers (REQ-PROTO-6):** (a) send `frobnicate`, a bare `go`, and `take zeppelin` (noun in no `name` row) — world file hash identical before/after each, turn unchanged; (b) send a wall-bump `go` and a `take` of the item known to be in the other room — each: turn +1, one `failed` event, item/player locations unchanged; (c) verified by the fault-injection case in the tests target (REQ-PROTO-12).
7. **Transcript completeness (REQ-PROTO-8):** `SELECT DISTINCT turn FROM events` covers every integer 1..meta.turn.
8. **Renderer purity (REQ-PROTO-9):** code inspection — render path holds no write statements; every output template keys off an event verb.
9. **Persistence (REQ-PROTO-10):** scripted session, note `meta.turn` and player location; relaunch with `look`+`quit`; state matches, event history row count grew only by the new turns.
10. **Portability (REQ-PROTO-11):** copy world file, play divergent commands in each, verify the two files differ where expected and each remains playable. Also covered automated in the tests target (REQ-PROTO-12).
11. **Tests (REQ-PROTO-12):** `./build/tests` exits 0; deliberately break an assertion to confirm nonzero exit works, then restore.
