---
title: "Implementation plan: two-room engine foundation prototype"
date: 2026-07-05
status: executed
tags: [plan, engine, prototype, sqlite, turn-loop, cpp]
modules: [engine]
related: [.lore/work/specs/engine-foundation-prototype.md, .lore/work/design/engine-foundation.md, .lore/work/brainstorm/engine-foundation-cpp-sqlite.md, .lore/vision.md]
---

# Implementation plan: two-room engine foundation prototype

**Source spec:** `.lore/work/specs/engine-foundation-prototype.md` (REQ-PROTO-1 … REQ-PROTO-12)
**Source design:** `.lore/work/design/engine-foundation.md` (module layout §1, schema §3, failure tiers §4, mutation discipline §5)

Greenfield: the repo contains only `.lore/`. Every file below is new. The plan follows the design's module layout exactly; where the design and the older brainstorm sketch disagree (e.g. `location.container` vs `location.room`), the design wins.

## Step sequence

<svg viewBox="0 0 760 210" xmlns="http://www.w3.org/2000/svg" style="max-width:100%">
  <style>
    .box{fill:#eef2f7;stroke:#5b7a9d;rx:8}
    .gate{fill:#e7f2e7;stroke:#4a8a4a;rx:8}
    .t{font:12px monospace;fill:#1a2733}
    .n{font:10px sans-serif;fill:#666}
    .a{stroke:#5b7a9d;stroke-width:1.5;marker-end:url(#ar)}
  </style>
  <defs><marker id="ar" markerWidth="8" markerHeight="8" refX="7" refY="3" orient="auto"><path d="M0,0 L8,3 L0,6 z" fill="#5b7a9d"/></marker></defs>
  <rect class="box" x="8" y="20" width="110" height="34"/><text class="t" x="18" y="41">1 scaffold</text>
  <rect class="box" x="150" y="20" width="110" height="34"/><text class="t" x="160" y="41">2 db wrapper</text>
  <rect class="box" x="292" y="20" width="130" height="34"/><text class="t" x="302" y="41">3 schema+seed</text>
  <rect class="box" x="454" y="20" width="140" height="34"/><text class="t" x="464" y="41">4 action+parser</text>
  <rect class="box" x="8" y="90" width="120" height="34"/><text class="t" x="18" y="111">5 mutations</text>
  <rect class="box" x="160" y="90" width="110" height="34"/><text class="t" x="170" y="111">6 systems</text>
  <rect class="box" x="302" y="90" width="110" height="34"/><text class="t" x="312" y="111">7 renderer</text>
  <rect class="box" x="444" y="90" width="120" height="34"/><text class="t" x="454" y="111">8 turn loop</text>
  <rect class="box" x="596" y="90" width="100" height="34"/><text class="t" x="606" y="111">9 tests</text>
  <rect class="gate" x="240" y="160" width="290" height="34"/><text class="t" x="252" y="181">10 VALIDATE against spec</text>
  <line class="a" x1="118" y1="37" x2="150" y2="37"/>
  <line class="a" x1="260" y1="37" x2="292" y2="37"/>
  <line class="a" x1="422" y1="37" x2="454" y2="37"/>
  <line class="a" x1="357" y1="54" x2="68" y2="90"/>
  <line class="a" x1="128" y1="107" x2="160" y2="107"/>
  <line class="a" x1="270" y1="107" x2="302" y2="107"/>
  <line class="a" x1="412" y1="107" x2="444" y2="107"/>
  <line class="a" x1="564" y1="107" x2="596" y2="107"/>
  <line class="a" x1="646" y1="124" x2="500" y2="160"/>
  <text class="n" x="454" y="70">4 depends on 3 (parser vocabulary = name rows)</text>
  <text class="n" x="8" y="150">5–8 strictly sequential; 9 grows alongside 5–8, final pass after 8</text>
</svg>

Steps 1–3 are the substrate; 4 can proceed in parallel with 5 in principle, but the whole slice is small enough that sequential order is simpler and safe. Step 9's test cases should be written as each engine capability lands (the harness exists from step 1), with a completeness pass at the end.

---

### Step 1 — Build scaffold (REQ-PROTO-1)

**Files:** `CMakeLists.txt`, `vendor/sqlite3.c`, `vendor/sqlite3.h`, stub `src/main.cpp`, stub `tests/tests.cpp`.

- Download the SQLite amalgamation (current release from sqlite.org/download.html) and commit `sqlite3.c` + `sqlite3.h` into `vendor/`. This is the only network-dependent moment in the plan; if the implementing session has no network egress, check for a system-local copy (`/usr/local`, Homebrew cellar, another checkout) or ask the user to supply the two files — do not substitute a linked system libsqlite3, which would violate REQ-PROTO-1's vendored-amalgamation requirement.
- `CMakeLists.txt`: C++20, Ninja generator assumed but not required by the file. Three targets:
  - `sqlite3` — static library from `vendor/sqlite3.c`, compiled as C, with `-DSQLITE_OMIT_LOAD_EXTENSION`.
  - `textworld` — game executable, links `sqlite3`.
  - `tests` — test executable, links `sqlite3`.
- Stub `main.cpp` prints a line and exits; stub `tests.cpp` contains the micro-harness skeleton (~30 lines: check counter, `CHECK` macro, `main` returning nonzero on any failure) and one trivial passing check.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 1:</strong> <code>cmake -B build -G Ninja && ninja -C build</code> succeeds from clean; <code>./build/textworld</code> and <code>./build/tests</code> both run; <code>tests</code> exits 0. No external libraries beyond system defaults (<code>otool -L</code>).</div>

### Step 2 — Database wrapper (design §2)

**Files:** `src/db.hpp`, `src/db.cpp` (~150 lines total).

- `Db` class: owns `sqlite3*`; constructor opens (creates file if absent), destructor closes. Methods: `exec(const char*)` for DDL/seed, `prepare(const char*) → Stmt`, `begin()`, `commit()`, `rollback()`.
- `Stmt` class: owns `sqlite3_stmt*`, finalizes in destructor, move-only. Methods: `bind(int, int64_t)`, `bind(int, const std::string&)`, `step() → bool` (true = row), `colInt(int) → int64_t`, `colText(int) → std::string`.
- Every sqlite error throws `std::runtime_error` carrying `sqlite3_errmsg`. No statement cache. No FK pragma (design: no FK enforcement this phase).

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 2:</strong> tests: open temp db, create table, insert, read back typed values, verify error on bad SQL throws. <code>tests</code> exits 0.</div>

### Step 3 — Schema, world open/create, seed (REQ-PROTO-2, 3, 4)

**Files:** `src/world.hpp`, `src/world.cpp`, `seed/base.sql`.

- `world.cpp` holds the schema DDL exactly as design §3: `meta`, `entities`, `name`, `room`, `player`, `portable`, `description`, `location`, `exits`, `events` — ten tables, no `inventory` table. A `SCHEMA_VERSION` integer constant lives here.
- `openWorld(path) → Db`:
  - File absent (or present but empty/uninitialized): create, apply DDL, execute `seed/base.sql`, write `schema_version` and `turn=0` into `meta`.
  - File present: read `meta.schema_version`; on mismatch, print refusal telling the user to delete the world file and exit nonzero **without any write** to the db.
- `seed/base.sql`: literal ids with comment convention (`-- 1: room 'stone hall'`). Content per design §8: rooms `stone hall` (id 1) and `garden` (id 2); exits north (1→2) and south (2→1); player entity (id 3) in stone hall; portable items — fixed names so later gates can reference them: `lantern` (id 4, in stone hall) and `key` (id 5, in garden), satisfying "≥2 portable with at least one in each room" (REQ-PROTO-2); `name` rows for every referencable entity; `description` rows for both rooms and both items — none for the player.
- Seed-file location: resolve `seed/base.sql` relative to the working directory for the prototype (interface contract already fixes cwd; the binary is always launched from the repo root, e.g. `./build/textworld`); embed-in-binary is a non-goal now.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 3:</strong> tests: fresh temp-path open creates world — 2 room rows, ≥2 portable with one per room, 1 player row located in room 1, bidirectional exits, description count = rooms+items, <code>meta.turn</code>=0; reopen succeeds; bumping <code>schema_version</code> then reopening refuses without modifying the file.</div>

### Step 4 — Action struct and disposable parser (REQ-PROTO-6a, 7)

**Files:** `src/action.hpp`, `src/parser.cpp` (+ small header or declaration in `action.hpp`).

- `action.hpp`: `enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit };` and `struct Action { Verb verb; int64_t subject = 0; std::string direction; };` This struct is the seam for the future Action Resolver agent — keep it free of parser types.
- `parser.cpp`: `std::optional<Action> parse(Db&, const std::string& line)`. Header comment marks it **DISPOSABLE**. Logic: lowercase, first token = verb, remainder = argument.
  - Unknown verb → `nullopt`.
  - `go`/`take`/`drop` with empty argument → `nullopt` (REQ-PROTO-6a: bare verb is unparseable).
  - `take`/`drop` noun: match remainder against **all** `name` rows in the world (recognition). No match anywhere → `nullopt`. Match → `Action.subject` = entity id, first match wins.
  - `go`: direction kept as string; direction validity is resolution, not recognition — any non-empty direction parses.
- Parse returning `nullopt` means: caller prints an error, **no transaction, no tick**.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 4:</strong> tests: <code>frobnicate</code>, bare <code>go</code>, bare <code>take</code>, <code>take zeppelin</code> all → nullopt; <code>take lantern</code> (seeded name) → Action with correct subject id; <code>go north</code> → Action with direction "north"; verbs parse case-insensitively.</div>

### Step 5 — Mutation helpers (REQ-PROTO-8, design §5)

**Files:** `src/mutations.hpp`, `src/mutations.cpp`.

- `appendEvent(Db&, int64_t actor, const char* verb, int64_t subj, int64_t obj, const char* detail)` — inserts one `events` row with the current turn number (read from `meta.turn` inside the ambient transaction).
- `moveEntity(Db&, int64_t what, int64_t toContainer, int64_t actor, const char* verb)` — updates `location.container` for `what` AND appends the event row. One call = component write + event, both inside the caller's transaction, so "both or neither" is guaranteed by transactionality.
- Convention documented in the header: `appendEvent` alone is legal only for the no-write verbs `looked`, `waited`, `failed`. All other world mutation in the codebase goes through these helpers — `systems.cpp` never runs raw SQL writes.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 5:</strong> tests: <code>moveEntity</code> inside a transaction changes exactly one <code>location</code> row and adds exactly one <code>events</code> row with matching turn/verb/subject/object; rollback after the call leaves both tables untouched.</div>

### Step 6 — Systems / action resolution (REQ-PROTO-6b, 7)

**Files:** `src/systems.hpp` (declaration), `src/systems.cpp`.

- `resolve(Db&, const Action&, int64_t player)` — runs inside the tick transaction, after `turn++`. Fixed dispatch on `Action.verb`:
  - **go:** look up `exits` for (player's current room, direction). Exit exists → `moveEntity(player, dest, player, "moved")`. No exit → `appendEvent(failed)` with detail (in-world refusal; tick stands).
  - **take:** subject portable AND `location.container` == player's room → `moveEntity(subject, player, player, "took")`. Otherwise (non-portable, elsewhere, already carried) → `failed` event.
  - **drop:** subject's container == player → `moveEntity(subject, playerRoom, player, "dropped")`. Not carried → `failed` event.
  - **look** → `appendEvent(looked)`; **inventory** → `appendEvent(looked, detail="inventory")` — reuses the design's fixed six-verb set (`moved`,`took`,`dropped`,`looked`,`waited`,`failed`); the renderer branches on the `detail` column. No seventh verb string is introduced.
  - **wait** → `appendEvent(waited)`.
  - **quit** never reaches `resolve` (handled pre-transaction in the loop, no tick).
- All reads are free-form SQL; all writes go through mutations helpers.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 6:</strong> tests: movement changes location + <code>moved</code> event + turn increment; take/drop round-trip; wall bump → <code>failed</code> event, turn incremented, no component change; <code>take</code> of the item in the other room → <code>failed</code> event tick (tier b), contrasted with unknown noun → no tick (tier a, from step 4). This pins the REQ-PROTO-6 boundary.</div>

### Step 7 — Renderer (REQ-PROTO-9)

**Files:** `src/render.hpp` (declaration), `src/render.cpp`.

- `render(Db&, int64_t turn) → std::string` — selects `events WHERE turn = :t`, maps each verb to a dumb template: `moved` → room description block (canon prose + exits + visible items, all read-only lookups), `took`/`dropped` → "You take/drop the %s.", `looked` → room or inventory listing, `waited` → "Time passes.", `failed` → the detail text.
- Contract enforced by structure: the function takes no non-const path to mutation — it only ever calls `SELECT`s. No output line exists without a sourcing event row.
- Also a tiny `renderError(msg)` path for tier-a parse failures (not event-sourced — no tick happened; this output is outside the turn contract, which the spec's interface allows since no state change is described).

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 7:</strong> tests: after a scripted <code>moved</code> event, render output contains the destination room's canon prose and its exits; render performs no writes (verify db file hash / change counter unchanged across a render call).</div>

### Step 8 — Turn loop / REPL (REQ-PROTO-5, 6c, 10, 11)

**Files:** `src/loop.hpp`, `src/loop.cpp`, `src/main.cpp` (replace stub).

- The per-line dispatch lives in a shared translation unit so `tests` exercises the production path rather than a reimplementation:
  ```cpp
  // loop.hpp
  enum class TurnOutcome { NoTick, Ticked, EngineError, Quit };
  struct TurnResult { TurnOutcome outcome; std::string output; };
  TurnResult runTurn(Db&, const std::string& line);
  ```
  `runTurn` implements: `parse` → `nullopt` → error message, no transaction (tier a); `Verb::Quit` → `Quit`, no tick; otherwise `begin(); turn++; resolve(...); commit();` then `render(db, turn)`. Any exception between begin and commit → `rollback()`, engine-error message — turn counter and world state as if the prompt never happened (tier c).
- `main.cpp` shrinks to: open world, print startup description, `std::getline` loop calling `runTurn` and printing `result.output` until `Quit` or EOF.
- `turn++` implemented as one `UPDATE meta SET value = value+1 WHERE key='turn'` at tick start — exactly once per tick (REQ-PROTO-5).
- On startup: `openWorld("world.db")` (fixed name, cwd — the spec's interface contract), then print the initial room description via a courtesy non-tick `look`-style render or a plain read; **must not** consume a turn or write an event before the first prompt.
- EOF on stdin behaves as `quit`.

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 8:</strong> manual scripted session — pipe the spec's validation script #5 (<code>look</code>, <code>take</code>, <code>go north</code>, <code>drop</code>, <code>take</code>, <code>inventory</code>, <code>wait</code>, wall-bump <code>go north</code>, <code>quit</code>) into <code>./textworld</code>; afterwards <code>meta.turn</code> = 8 and output shows each verb behaving per REQ-PROTO-7. Relaunch resumes identical state (REQ-PROTO-10).</div>

### Step 9 — Test completeness pass (REQ-PROTO-12)

**Files:** `tests/tests.cpp` (grown throughout steps 2–8; this step closes gaps).

Checklist against REQ-PROTO-12 — each item is one or more `CHECK` groups against a temp-file world:

1. World creation + seed (from gate 3).
2. Movement: component change + event + turn increment (gate 6).
3. Take/drop round-trip (gate 6).
4. Wall bump: `failed` event, turn increments, nothing else changes (gate 6).
5. Tier boundary: `take` of recognized-but-absent item (tick + `failed`) vs unknown noun (no tick) — both driven through `runTurn` (gates 4+6).
6. Missing argument: `runTurn(db, "go")` returns `NoTick` and leaves turn count unchanged.
7. **Fault injection:** drive `runTurn` into a mid-tick throw (e.g. drop a component table inside an open transaction via a test hook, or a test-only throwing mutation); verify `EngineError` outcome and rollback — turn and all component tables unchanged (tier c).
8. Persistence: close db, reopen, state identical, event history intact.
9. File-copy portability: copy world file, play the copy (run a movement through engine functions), original file unchanged; both remain openable.
10. Unparseable input: turn count unchanged.

Exit code 0 = all pass; any failure → nonzero (harness from step 1).

<div style="border-left:4px solid #4a8a4a;background:#f0f7f0;padding:6px 12px;margin:8px 0"><strong>Gate 9:</strong> <code>./build/tests</code> exits 0. Deliberately break one assertion, confirm nonzero exit, restore.</div>

### Step 10 — Final validation against the spec

Run the spec's **AI Validation** section (items 1–11) end to end, in order, from a clean checkout: build, seed inspection via `sqlite3` CLI, version-gate tamper test, `.schema` table audit, scripted-session tick accounting, all three failure-tier probes, transcript completeness query (`SELECT DISTINCT turn FROM events` covers 1..meta.turn), renderer purity code inspection, persistence relaunch, file-copy divergence, and the tests-target break/restore check. Record any deviation against its REQ-PROTO number; every requirement must either pass or have an explicit, user-approved deviation note before the plan is marked executed.

<div style="border-left:4px solid #a05252;background:#f9efef;padding:6px 12px;margin:8px 0"><strong>Final gate:</strong> all 12 requirements verified per the spec's own validation script. This gate blocks marking the spec <code>implemented</code>.</div>

---

## Notes for the implementer

- **Write-path discipline is the one convention to police:** `systems.cpp` and `main.cpp` never run raw SQL writes; only `mutations.cpp` and `world.cpp` (DDL/seed) do. Reads are free-form anywhere.
- **Do not pre-solve deferred items:** no FK pragmas, no statement cache, no migrations, no doctest, no name-ambiguity handling beyond first-match-wins, no JSON, no HTTP. Each has a revisit trigger in the design.
- **Parser is disposable** — keep everything parser-specific inside `parser.cpp`; nothing downstream of the `Action` struct may know the parser exists.
- **Intentional additions to the design's §1 file list:** `systems.hpp`, `render.hpp`, and `loop.hpp`/`loop.cpp`. The first two are plain declaration headers; `loop.cpp` factors the per-line tick dispatch out of `main.cpp` so the `tests` target can exercise the production turn path (tier a/c behavior) instead of reimplementing it. Elaboration, not deviation.
- The project-name collision with Microsoft TextWorld (research doc, finding 6) is out of scope here; nothing in this slice needs to embed the name anywhere expensive to change (binary name `textworld` is cheap to rename).
- No specialized expertise flagged: plain C++20, SQLite C API, CMake. All steps are within a single generalist implementer's range.
