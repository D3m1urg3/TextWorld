---
title: "Engine foundation design: SQLite-as-world core for the two-room prototype"
date: 2026-07-05
status: draft
tags: [design, engine, sqlite, ecs, turn-loop, cpp]
modules: [engine]
related: [.lore/work/brainstorm/engine-foundation-cpp-sqlite.md, .lore/work/research/similar-projects-and-approaches.md, .lore/vision.md]
---

# Engine foundation design: SQLite-as-world core for the two-room prototype

Scope: the foundation slice only — prove SQLite-as-the-world, turn=tick=transaction, and the mutation/event discipline with two hand-authored rooms and a disposable verb parser. No AI in this slice. Every decision here is sized for the prototype; revisit triggers are listed at the end.

## 1. Module layout

Small fixed set, one executable, headers + implementation split only where it pays.

```
TextWorld/
├── CMakeLists.txt          # CMake + Ninja, C++20, two targets
├── vendor/
│   ├── sqlite3.c           # amalgamation — the only dependency
│   └── sqlite3.h
├── seed/
│   └── base.sql            # hand-authored world seed
├── src/
│   ├── main.cpp            # REPL: readline → tick → render
│   ├── db.hpp / db.cpp     # RAII sqlite wrapper: Db, Stmt (~150 lines)
│   ├── world.hpp / world.cpp   # schema DDL, open-or-create, schema_version check, seed exec
│   ├── action.hpp          # Action struct — canonical action as plain data
│   ├── parser.cpp          # DISPOSABLE hardcoded verb parser → Action
│   ├── mutations.hpp / .cpp    # the mutate() discipline: typed write+event helpers
│   ├── systems.cpp         # resolve(Db&, Action) — game rules
│   └── render.cpp          # render(Db&, turn) — events → text
└── tests/
    └── tests.cpp           # hand-rolled micro-harness, engine functions on temp db
```

Rule that matters more than the file list: **`systems.cpp` never runs raw SQL writes** — all world mutation goes through `mutations.hpp` functions. Reads are free-form. Enforced by convention; the codebase is small enough to see violations.

## 2. Database wrapper (`db.hpp`)

From scratch, thin, RAII. No ORM ambitions.

```cpp
class Db {                       // owns sqlite3*
  void exec(const char* sql);    // for DDL / seed
  Stmt prepare(const char* sql);
  void begin(); void commit(); void rollback();
};
class Stmt {                     // owns sqlite3_stmt*, finalizes in dtor
  Stmt& bind(int idx, ...);      // int64 / text overloads
  bool step();                   // true = row available
  int64_t colInt(int); std::string colText(int);
};
```

- Errors throw `std::runtime_error` with the sqlite message. The turn loop catches, rolls back, prints. A bug mid-turn therefore cannot half-write the world — the transaction is the safety net, by design not by care.
- Statements prepared at point of use, no cache. At ~100 entities and one action per turn, prepare cost is noise. Revisit trigger: profiling says otherwise (it won't).
- No foreign-key enforcement during the exploration phase — schema churns weekly, loose keys keep churn cheap. The `schema_version` gate (below) is the real guard.

## 3. Schema

Component-per-table; entity = integer id; tag component = table with only the entity column.

```sql
CREATE TABLE meta(key TEXT PRIMARY KEY, value);          -- 'schema_version', 'turn'
CREATE TABLE entities(id INTEGER PRIMARY KEY);           -- id mint: INSERT → rowid

-- components
CREATE TABLE name(entity INTEGER PRIMARY KEY, value TEXT);        -- parser handle, unique enough for now
CREATE TABLE room(entity INTEGER PRIMARY KEY);                    -- tag
CREATE TABLE player(entity INTEGER PRIMARY KEY);                  -- tag, singleton by convention
CREATE TABLE portable(entity INTEGER PRIMARY KEY);                -- tag
CREATE TABLE description(entity INTEGER PRIMARY KEY, prose TEXT); -- canon: row exists = never regenerate
CREATE TABLE location(entity INTEGER PRIMARY KEY, container INTEGER);
CREATE TABLE exits(room INTEGER, direction TEXT, dest INTEGER, PRIMARY KEY(room, direction));

-- the event log (append-only)
CREATE TABLE events(
  id INTEGER PRIMARY KEY,
  turn INTEGER NOT NULL,
  actor INTEGER,            -- who did it (player entity for now)
  verb TEXT NOT NULL,       -- 'moved','took','dropped','looked','waited','failed'
  subject INTEGER,          -- primary entity acted on
  object INTEGER,           -- secondary entity (destination room, container…)
  detail TEXT               -- human-readable fragment or NULL
);
```

**Design call — uniform containment.** `location.container` points at *any* entity: item in room → container = room; item carried → container = player. Inventory is not a concept, it's a query (`WHERE container = :player`). One table, no special cases, and later containers (chests, NPCs holding things) come free.

**Design call — player is an ordinary entity** with `player` tag + `location` row. Systems treat it like anything else; "where is the player" is a join, not a global.

**Canon principle, mechanical:** `description` row exists → the engine reads it and never asks anyone (human or AI) again. Vision principle #2 as schema.

## 4. Turn loop

<svg viewBox="0 0 720 150" xmlns="http://www.w3.org/2000/svg" style="max-width:100%">
  <style>.b{fill:#eef2f7;stroke:#5b7a9d;rx:8}.t{font:13px monospace;fill:#1a2733}.a{stroke:#5b7a9d;stroke-width:1.5;marker-end:url(#ar)}.n{font:11px sans-serif;fill:#666}</style>
  <defs><marker id="ar" markerWidth="8" markerHeight="8" refX="7" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6 z" fill="#5b7a9d"/></marker></defs>
  <rect class="b" x="8"   y="40" width="90"  height="38"/><text class="t" x="20"  y="63">readline</text>
  <rect class="b" x="126" y="40" width="80"  height="38"/><text class="t" x="140" y="63">parse</text>
  <rect class="b" x="234" y="40" width="330" height="38" style="fill:#e7f2e7;stroke:#4a8a4a"/>
  <text class="t" x="248" y="63">BEGIN · turn++ · resolve(action) · COMMIT</text>
  <rect class="b" x="592" y="40" width="120" height="38"/><text class="t" x="604" y="63">render(events)</text>
  <line class="a" x1="98"  y1="59" x2="126" y2="59"/>
  <line class="a" x1="206" y1="59" x2="234" y2="59"/>
  <line class="a" x1="564" y1="59" x2="592" y2="59"/>
  <text class="n" x="128" y="30">parse fail → msg, NO tick</text>
  <text class="n" x="250" y="105">throw → ROLLBACK, msg, NO tick (engine bug ≠ world time)</text>
  <text class="n" x="250" y="122">in-world refusal → 'failed' event, tick STANDS</text>
  <text class="n" x="592" y="105">reads events of this turn +</text>
  <text class="n" x="592" y="122">canon prose (read-only)</text>
</svg>

**Design call — three failure tiers.** This is the sharpest decision in the slice:

| Failure | Example | Tick? | Mechanism |
|---|---|---|---|
| Not understood | `xyzzy frobnicate` | no | parser returns no Action; world untouched |
| World refuses | `go north` into a wall | **yes** | systems emit `failed` event; turn commits — bumping a wall is something that *happened* |
| Engine error | SQL bug, constraint hit | no | exception → rollback; the turn never existed |

Non-mutating verbs (`look`, `inventory`) tick too — one prompt = one tick, always. The event log is therefore a complete play transcript, which the future Narrator agent reads (research: memory-stream pattern).

## 5. Mutation discipline (`mutations.hpp`)

Generic `mutate(table, entity, …)` in C++ without reflection = string-typed mush. Instead: **a small set of typed helpers, each performing exactly one component write + one event append**, both inside the ambient transaction.

```cpp
// each: writes component table AND appends event row — both or neither
void moveEntity(Db&, int64_t what, int64_t toContainer, int64_t actor, const char* verb);
void appendEvent(Db&, int64_t actor, const char* verb, int64_t subj, int64_t obj, const char* detail);
// (appendEvent alone is legal only for no-write verbs: looked, waited, failed)
```

Systems compose these. New component → new helper pair. The B-changeset alternative (pure systems emitting change lists) stays parked; revisit trigger unchanged: same-tick NPC chaining conflicts.

## 6. Renderer contract (`render.cpp`)

Honest version of "render from events": the renderer's *input* is this turn's event rows; it may additionally **read** world state to flesh out descriptions (reading canon prose after a `moved` event). It must never infer a state change that isn't in an event, and never writes.

```
render(db, turn):  f(events WHERE turn = :t, read-only world) → text
```

Prototype renders with dumb templates (`"You take the %s."`). The contract — events in, prose out, no writes — is exactly the contract the future AI prose layer gets. Swap-in stays clean because the interface was never "inspect the world and say something."

## 7. Parser (`parser.cpp`) — disposable

Verbs: `look`, `go <dir>`, `take <name>`, `drop <name>`, `inventory`, `wait`, `quit`. Lowercase, first-token verb, remainder = noun matched against **all** `name` rows (the world's vocabulary — recognition), while systems decide whether the action can apply here and now (resolution). A recognized noun that fails resolution is an in-world refusal (tick + `failed`); an unrecognized noun is unparseable (no tick). Ambiguity: first match wins, prototype-grade. Marked disposable in the header comment; dies when the Action Resolver agent lands. `Action` struct is the seam:

```cpp
struct Action { Verb verb; int64_t subject = 0; std::string direction; };
```

The parser and the future agent both produce `Action`; nothing downstream knows which existed.

## 8. Seed (`seed/base.sql`)

**Design call — SQL script, not C++ builder.** Declarative, diffable, data-not-code, and later hand-authored areas are just more SQL files. Explicit literal ids with a comment convention (`-- 1: room 'stone hall'`); brittle beyond ~50 entities but fine at 2 rooms, and worlds are disposable this phase. `world.cpp` executes it on first open; on `schema_version` mismatch it refuses and tells the user to delete the world file — no migrations until a world worth keeping exists.

Prototype content: 2 rooms (`stone hall`, `garden`), one exit pair (north/south), 2–3 portable items, player in room 1, canon descriptions for everything.

## 9. Tests (`tests/tests.cpp`)

Hand-rolled micro-harness (~30 lines: counter + `CHECK` macro + exit code), honoring minimal-deps. Engine functions all take `Db&`, so tests run them against a temp-file world. Coverage for this slice:

1. create world → schema present, seed loaded
2. `go north` → location row changed, `moved` event row exists, turn incremented
3. `take` / `drop` round-trip through containment
4. wall bump → `failed` event, turn incremented, nothing else changed
5. **persistence proof:** close db, reopen, state identical; copy file, open copy, play on
6. unparseable input → turn NOT incremented

Revisit trigger for doctest: when test count or assertion ergonomics start hurting (~50+ assertions).

## 10. Build

CMake + Ninja, C++20 (`-std=c++20`, clang/macOS; no modules — support still uneven). `sqlite3.c` compiled as C into a static lib target; `-DSQLITE_OMIT_LOAD_EXTENSION` to keep it lean. Targets: `textworld`, `tests`.

## Decisions

1. Module layout per §1; systems never raw-write — mutations.hpp is the only write path.
2. Thin RAII Db/Stmt wrapper, exceptions → turn rollback; no stmt cache; no FK enforcement this phase.
3. Schema per §3 with **uniform containment** (`location.container`, inventory = query) and **player as ordinary entity**.
4. **Three failure tiers:** parse fail = no tick; in-world refusal = tick + `failed` event; engine error = rollback, no tick. Non-mutating verbs tick.
5. Typed mutation helpers (write + event, both or neither); generic mutate() rejected; B-changeset stays parked.
6. Renderer contract: input = turn's events, world readable read-only, never writes, never infers unlogged changes.
7. Parser: hardcoded, disposable, produces `Action` — the seam for the future Action Resolver agent.
8. Seed = SQL script with literal ids; schema_version mismatch = delete world, no migrations.
9. Tests = hand-rolled micro-harness on temp-file worlds; six cases in §9, persistence proof included.
10. C++20, CMake + Ninja, sqlite amalgamation as sole dependency.

## Revisit triggers

- Stmt cache → measured prepare cost (unlikely ever).
- B-changeset → NPC systems chaining within one tick.
- doctest → assertion count pain.
- Migrations → first world worth keeping.
- Name matching / ambiguity ("which key?") → more than one plausible noun match in play; likely lands with the Action Resolver agent.
- FK enforcement → schema stabilizes post-exploration.
