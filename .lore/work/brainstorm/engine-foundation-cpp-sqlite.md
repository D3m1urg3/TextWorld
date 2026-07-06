---
title: Engine foundation — C++, from-scratch ECS-lite, SQLite as the world
date: 2026-07-05
status: open
tags: [architecture, ecs, sqlite, data-oriented, cpp, foundation, turn-loop]
modules: [engine]
related: [.lore/brainstorm/ai-driven-text-adventure.md, .lore/vision.md]
---

# Engine foundation — C++, from-scratch ECS-lite, SQLite as the world

## Context

First technical brainstorm after the vision doc. Goal: settle the base philosophy and code structure for the engine before writing the first line. Constraints set by the user during the session: **C++**, **as much from scratch as possible**, **minimal external dependencies**, exploratory method with **small self-contained atomic steps**. Data-oriented style preferred; ECS liked for its clarity.

## Ideas Explored

### ECS: full vs lite

Classic ECS (archetypes, scheduler, cache locality) solves problems this game doesn't have (~100 entities, one action per turn, no frame loop). But ECS has a second soul that fits perfectly: *state is plain data, logic is functions over data, nothing hides*. Considered EnTT, flecs (relationships + built-in JSON reflection were attractive), and hand-rolled. From-scratch preference settled it: **hand-rolled ECS-lite** — entity = integer id, component = table, system = plain function called by the turn loop. No scheduler.

### SQLite AS the world (Shape B) — chosen

Two shapes considered:

- **Shape A, memory-first:** components in RAM, flushed to SQLite per turn. Cost: serialization layer, two representations to sync.
- **Shape B, store-first:** no in-memory world state. Component = SQL table. Query = SQL. System = function running prepared statements. **Chosen.**

Why B won:

- **Turn = transaction.** Crash mid-turn rolls back to turn start. Atomicity free.
- **No save system.** The world is always saved. Quit = nothing to do.
- **Portability = copy the .db file.** Fork a world before a risky experiment: `cp world.db experiment.db`.
- **Canon principle becomes mechanical:** a `description` row exists → never call the AI again. Vision principle #2 enforced by schema, not policy.
- **Less code than Shape A** — no sync layer, no serializer. Serves the from-scratch/minimal goal by *not writing* code.
- **Debug eyes:** `sqlite3 world.db 'SELECT …'` mid-play.

Cost accepted: systems are SQL-flavored (prepared-statement glue), and "data-oriented" here means *relational*, not cache-lines. Performance irrelevant at this scale.

Sketch schema:

```sql
CREATE TABLE location(entity INTEGER PRIMARY KEY, room INTEGER);
CREATE TABLE portable(entity INTEGER PRIMARY KEY);                -- tag component
CREATE TABLE description(entity INTEGER PRIMARY KEY, prose TEXT); -- canon = row exists
CREATE TABLE exits(room INTEGER, direction TEXT, dest INTEGER);
CREATE TABLE events(turn INTEGER, actor INTEGER, verb TEXT, target INTEGER, detail TEXT);
```

### Mutation discipline: middle path — chosen

Three options for how systems write:

- **B-direct:** systems write SQL immediately inside the turn transaction. Least code; systems see each other's changes.
- **B-changeset:** systems are pure functions emitting changes-as-data; an applier writes the batch at tick end. Payoffs: change-set doubles as event log and as prose-renderer input; systems testable without writes. Trap: systems read stale turn-start state — conflict machinery needed once NPC systems chain.
- **Middle path (chosen): B-direct writes + mandatory event append.** Every mutation goes through a tiny `mutate()` helper that writes the component table AND appends an `events` row — both or neither. Gets the event log's payoffs (narrator food, prose input, debugging) without change-set machinery.

**Revisit trigger for B-changeset:** when NPC systems arrive and same-tick chaining conflicts appear.

### Turn loop skeleton

Time moves only when the player writes a prompt. One prompt = one tick, always ("wait" = no-op action tick). Multi-tick actions = deliberately not designed now.

1. read player line
2. parse → canonical `Action` (hardcoded parser in the prototype; Action Resolver agent later)
3. `BEGIN`
4. `turn++` in meta table
5. systems run in fixed order (prototype: just the action-resolver system)
6. every mutation via `mutate()` → component write + `events` append
7. `COMMIT`
8. render output **from this turn's events**, not from world inspection (prototype: dumb template text; AI prose later reads the same event contract)

Step 8's habit is load-bearing: rendering from events now makes the AI prose swap-in clean later — same input contract, narrow context (vision principle #4).

### Dependency tally

- **sqlite3 amalgamation** — one `.c` + one `.h` vendored in the repo, compiled with the project. The only dependency for the prototype.
- Later, AI calls: libcurl (system lib on macOS) or heresy option — shell out to the `curl` binary (zero link deps). Undecided, not needed yet.
- JSON: none needed until AI arrives; then hand-rolled minimal writer + parser, or nlohmann single header if patience runs out.
- Tests: hand-rolled asserts or doctest single header. Undecided.

### Schema churn risk

Exploration = components change weekly; SQLite `ALTER TABLE` is weak. Mitigation: during exploration **worlds are disposable** — `schema_version` pragma; on mismatch, regenerate the world from the hand-authored seed. No migration machinery until a world worth keeping exists.

## Decisions

1. Language: **C++** (C++20 assumed; standard not explicitly pinned).
2. From scratch, minimal deps; prototype dependency = **sqlite amalgamation only**.
3. **Hand-rolled ECS-lite**, no ECS library.
4. **SQLite is the world** (store-first, Shape B). Component-per-table schema.
5. **Turn = tick = transaction**; time advances only on player prompt.
6. **Middle-path mutation discipline:** direct writes + mandatory paired event rows via `mutate()` helper.
7. Output rendered from events, not world state.
8. Hardcoded verb parser accepted as **disposable test harness** (heresy against the vision's no-command-parser anti-goal; thrown away when the Action Resolver agent arrives).

## Open Questions

1. **AI/engine boundary per turn** — the big one. What the AI handles vs what stays in code each tick. Deliberately deferred: to be explored in running code, not settled here.
2. Seed world: hand-written SQL script vs C++ builder function.
3. Test harness: raw asserts vs doctest single header.
4. HTTP layer when AI arrives: libcurl vs shelling out to `curl`.
5. C++ standard pin (20 vs 23).
6. B-changeset revisit when NPC systems chain within a tick.

## Next Steps

**Prototype (atomic step 0):** two rooms, prove the foundation tech is solid.

- CMake + Ninja, single executable, sqlite amalgamation vendored
- Component tables + `mutate()` helper + events table
- 2 hand-authored rooms, a couple of items, exits
- Hardcoded parser: `go <dir>`, `take <item>`, `drop <item>`, `look`, `wait`
- Turn loop per skeleton above; output from events
- Prove: state mutates, persists across process restarts, world file copyable
