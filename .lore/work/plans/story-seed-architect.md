---
title: "Implementation plan: story-seed + architect"
date: 2026-07-09
status: executed
tags: [plan, ai-integration, story-seed, world-generation, architect, claude-api, tool-use, mutations, fallback, persistence]
modules: [world-gen, architect, systems, mutations, world, prose, render]
related: [.lore/work/specs/story-seed-architect.md, .lore/work/design/story-seed-architect.md, .lore/work/brainstorm/story-seed-and-lod-world.md, .lore/work/plans/ai-resolver.md]
---

# Implementation plan: story-seed + architect

Loads a hand-authored **setting** into canon at world init, then generates one
new room — coherent with the setting and the room being left — when the player
walks an **unmapped** exit, writing it to canon via Claude tool-use with the
existing wall (`"You can't go that way."`) as the permanent deterministic
fallback. Source of truth: **[.lore/work/specs/story-seed-architect.md]** (13
requirements, prefix `ARCH`). This plan sequences those requirements into atomic,
mostly-deterministic steps that **mirror the shipped AI resolver**
(`src/nlresolve.cpp`, `src/prose.hpp`, `tests/tests.cpp`) rather than reinventing
its seams — see [.lore/work/plans/ai-resolver.md].

## Guiding constraints (from memory + spec)

- **Deterministic skeleton first, LLM last.** Steps 1–9 are fully unit-testable
  with a fake transport, no network; the single live-LLM step (10) is isolated and
  gated. This is the [[token-risk-estimation]] / [[verification-must-be-bounded]]
  discipline: the token trap is open-ended live verification, **not** diff size.
  The REQ-ARCH-13 coherence check is **one bounded judge call**, never a loop.
- **Reuse the resolver's proven seams, don't rebuild them:**
  - `HttpResponse` / `HttpTransport` types → included from `prose.hpp` (not redefined).
  - `aiNarrationEnabled()` → **reused verbatim** as the architect's enable switch
    (REQ-ARCH-2: *one* flag governs all AI features).
  - Live-smoke gate `TEXTWORLD_AI_LIVE_TEST=1`, injectable-fake test discipline,
    the 8 s / no-retry transport → copied from the resolver/prose tests.
  - Canned-fixture helper: `cannedToolUse` (`tests.cpp:1234`) → here `cannedCreateRoom`.
- **One seam / one file / one testable behavior per step.** Target ≈ the resolver's
  granularity.

## What's genuinely NEW vs the resolver (call these out)

The resolver was read-only. The architect is the **first read-WRITE AI unit**, so
three things have no analog in the resolver plan and carry the design risk:

1. **The write discipline (REQ-ARCH-6, -9).** `architect.cpp` issues **no raw SQL
   writes** (`grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` must be empty). The
   write lives in **one** sanctioned helper `writeGeneratedRoom` in `mutations.cpp`.
   The model proposes name + description; the engine mints the id, owns the exits +
   reciprocity, and writes canon.
2. **The two-phase catch boundary (REQ-ARCH-4/-5).** Phase 1 (context → request →
   transport → validate) is wholly inside a try/catch → any failure yields `false`
   → the wall. Phase 2 (the write) runs **only** on a validated proposal and is
   **not** inside that catch — a DB fault propagates out to `runTurn`'s existing tick
   rollback (`loop.cpp:51-60`), never downgraded to a wall.
3. **First runtime entity mint.** Grep confirms nothing in `src/` mints an entity at
   runtime today — every id is a literal in `seed/base.sql`. `writeGeneratedRoom` is
   the first mint (see micro-decision #1).

The seam is `resolveGo`'s **else-branch** (`systems.cpp:53-55`), synchronous
in-tick. And one cross-module touch: the new `generated` event must be filtered out
of **both** payload-feeding SELECTs in `prose.cpp` (REQ-ARCH-10).

## Seams this touches (verified in tree)

| Seam | File:line | What the architect does with it |
|------|-----------|--------------------------------|
| `resolveGo` else-branch | `src/systems.cpp:53-55` | Replace the bare wall with the 3-way branch (REQ-ARCH-3) — Step 8 |
| `exitDest` (private, anon ns) | `src/systems.cpp:33` | Branch (a) — persistence falls out of it (re-crossing never regenerates) |
| `openWorld` / `initialize` | `src/world.cpp:60-99` | Add `settingPath` param + tolerant read + `meta.setting` INSERT — Step 1 |
| `readFile` (throws on absent) | `src/world.cpp:49-58` | **Not reusable for setting** — needs a tolerant reader (micro-decision #2) |
| `appendEvent` / `moveEntity` | `src/mutations.cpp:17-46` | Siblings of the new `writeGeneratedRoom` — Step 6 |
| `entities(id INTEGER PRIMARY KEY)` | `src/world.cpp:13` | Runtime mint via `INSERT … DEFAULT VALUES` (micro-decision #1) — Step 6 |
| `HttpResponse` / `HttpTransport` | `src/prose.hpp:48-53` | Included and reused as-is — Steps 4, 7 |
| `aiNarrationEnabled()` | `src/prose.hpp:81` | Reused as the architect's enable check — Steps 7, 8 |
| `curlTransport` pattern (8 s, no retry) | `src/nlresolve.cpp:121-159` | Copy-adjacent for architect's own transport — Step 7 |
| buildFacts current-turn SELECT | `src/prose.cpp:321` | `AND verb <> 'generated'` — Step 9 |
| buildFacts recent-events SELECT | `src/prose.cpp:~381` (`WHERE turn < ?`) | `AND verb <> 'generated'` — Step 9 |
| `deterministicAppends` SELECT | `src/prose.cpp:123` | **THIRD** `WHERE turn = ?` — branches on known verbs only → **no change** (note in Step 9) |
| "fixed six" comment | `src/render.cpp:128` | Update stale comment; renderer already emits nothing for unknown verbs — Step 9 |
| twcore sources list | `CMakeLists.txt` (near `nlresolve.cpp`) | Add `src/architect.cpp` — Step 2 |
| test runner `main()` | `tests/tests.cpp` | Register each `testArchitect*` |

## Three micro-decisions (flagged, not blocking)

1. **Runtime entity mint.** `Db` (`db.hpp`) exposes no `lastInsertRowid`. Recommend
   doing the mint in SQL inside `writeGeneratedRoom`: `db.exec("INSERT INTO entities
   DEFAULT VALUES")` then `SELECT last_insert_rowid()` via a prepared statement. No
   `db.hpp` change, mint stays localized to the one sanctioned helper. (Alternative:
   add `int64_t Db::lastInsertRowid()` — heavier, unneeded for one call site.)
   Rooms take **no** `location` row (rooms have no container, matching `base.sql`);
   the helper writes the `room` tag, `name`, `description`, two `exits` rows, and the
   `generated` event.
2. **Tolerant setting read.** `readFile` (`world.cpp:49`) throws when the file is
   absent — wrong for the setting, where absent/empty must succeed with an empty
   `meta.setting` (REQ-ARCH-1). Add a small local `readFileOrEmpty(path)` returning
   `""` on open failure. Keep `readFile` as-is for the mandatory `seedPath`.
3. **Production HTTP transport.** Mirror the resolver's own micro-decision: give
   `architect.cpp` its own small `curlTransport` (same URL / headers / **8 s** total
   timeout, no retries — the value the prose renderer already uses for `max_tokens:
   1024`). Only the `HttpTransport` *type* is shared, per REQ-ARCH-11. (Optional
   future cleanup: hoist a shared `http.hpp` transport — out of scope here.)

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> setting seed load ─────────────────────────────┐<br>
<b>2</b> TU scaffold + context builder ──┐&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
<b>3</b> architect system prompt ────────┼─▶ <b>4</b> request body ─┐&nbsp;&nbsp;│<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>7</b> architectGenerate ─▶ <b>8</b> resolveGo seam ─┬─▶ <b>10</b> live smoke <span style="color:#b00">[HIGH]</span><br>
<b>5</b> gate + invertibility table ─────┼─▶ <b>6</b> writeGeneratedRoom ─┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>9</b> generated-event invisibility<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>11</b> final validation vs spec<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification.

Notes on the graph: Step 1 is standalone (no AI) and can land first or in parallel.
Steps 2/3/5 need only the TU to exist (2 creates it, and 2 defines the `RoomProposal`
struct). Step 6 depends on Step 5 for `inverseDirection` (and uses the `RoomProposal`
type from Step 2). Step 7 orchestrates 2+4+5+6. Step 8 wires 7 into the tick. Step 9
needs a `generated` event to exist (Step 6) and is fully integration-tested after
Step 8.

---

### Step 1 — Setting seed load
**Requirements:** REQ-ARCH-1. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Standalone; no AI. In `src/world.cpp`:
- `openWorld` / `world.hpp:37` gain a `settingPath` parameter, default
  `"seed/setting.txt"`, mirroring `seedPath`.
- `initialize` reads the setting with a **tolerant** reader `readFileOrEmpty(path)`
  (micro-decision #2) and, inside the existing init transaction, `INSERT INTO
  meta(key, value) VALUES ('setting', ?)`. **Zero DDL, no `SCHEMA_VERSION` bump** —
  `meta` already exists; this is a new *row*. Absent/empty file → `meta.setting`
  empty/absent, init still succeeds (no separate failure path).
- Commit a **real, modest** `seed/setting.txt`, coherent with the stone hall / garden
  in `seed/base.sql`, so the live smoke has real shared context.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectSettingLoad</code> — a fresh world with
<code>settingPath</code> at a scratch file → <code>meta.setting</code> holds the file
text and <code>SCHEMA_VERSION</code> is unchanged; a fresh world with
<code>settingPath</code> at a non-existent scratch path → init succeeds,
<code>meta.setting</code> empty/absent. Uses scratch paths, never renames the
committed file. Confirm <code>seed/setting.txt</code> is committed and coherent with
<code>seed/base.sql</code> (spec AI-Validation item 3). Existing world tests pass
unchanged (default param → callers unaffected).
</blockquote>

### Step 2 — New architect TU + context builder
**Requirements:** REQ-ARCH-11 (TU), REQ-ARCH-7a, REQ-ARCH-6 (no ids). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/architect.hpp` / `src/architect.cpp`**; add `src/architect.cpp` to the
twcore sources in `CMakeLists.txt` (next to `src/nlresolve.cpp`). The header
`#include "prose.hpp"` (for `HttpResponse`/`HttpTransport`, `aiNarrationEnabled()`)
and declares the `RoomProposal { std::string name; std::string description; }`
struct. Header comment states the contract: **SELECT-only + network egress, and the
ONLY writes go through the sanctioned `mutations.cpp` helper — never raw SQL in this
TU** (mirror `prose.hpp:1-9`, adapted for the read-write unit).

Implement a pure `buildArchitectContext(Db&, int64_t room, const std::string&
direction)` → a payload (JSON string for the user message) containing **exactly** the
REQ-ARCH-7a set and **no ids** (REQ-ARCH-6): the setting text (`meta.setting`); the
origin room's `name`; the origin room's canon `description`; and the `direction`.
Nothing else — no ids, no neighborhood, no history. O(1) in world size. Reuse the SELECT
shapes prose already uses (`nameOf`, `canonProseOf` patterns from `prose.cpp`).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectContext</code> against the seeded world
asserts the payload parses as JSON with exactly the 4 fields (setting, origin name,
origin canon description, direction) and <b>no entity/row ids anywhere</b> (assert the
key set). Empty <code>meta.setting</code> → a thinner-but-well-formed payload. Callable
with no network.
</blockquote>

### Step 3 — The architect system prompt (the "actual work")
**Requirements:** REQ-ARCH-7c. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add a `kArchitectPrompt` constant to `architect.cpp` (git-versioned, like
`nlresolve.cpp`'s `kSystemPrompt`). It instructs the model: generate exactly one room
reachable by travelling `<direction>` from the described origin; **coherent with the
setting and consistent with** the origin room; emit it via `create_room` as a `name`
and a `description`; the description is standalone room prose (as the player reads it
on entry); **do not** describe exits, directions, other rooms, or the player's
arrival; **do not** invent ids. Prompt *quality* is a live concern (Step 10); structure
is pinned mechanically here to keep this step cheap (bounded-verification rule).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> a substring spot-check test (mirroring the resolver prompt
test) asserts the constant mentions "one room", "coherent"/"setting", "create_room",
name + description, and the "no exits/directions", "no arrival", "no ids" prohibitions.
Manual prompt-content inspection = spec AI-Validation item 4.
</blockquote>

### Step 4 — Request body + `create_room` tool schema
**Requirements:** REQ-ARCH-7b. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`buildArchitectRequestBody(const std::string& contextPayload)` → JSON string, built
with nlohmann/json like `nlresolve.cpp`'s request builder. Per REQ-ARCH-7b: a `tools`
array carrying **one** `create_room` tool whose input schema is a schema-enforced
object with **required** `name` (string) and `description` (string) and **no other
fields** (the design's optional `items[]` is superseded — deferred). `tool_choice`
**requires** the tool (there is no "decline" branch — a non-call is a gate failure →
wall). Same as prose/resolver: `POST /v1/messages`, model `claude-opus-4-8`
overridable via `TEXTWORLD_MODEL`, `max_tokens` **1024**, `anthropic-version`
header, **no** `thinking`, **no** `stream`, **no** cache keys. `system` = the Step-3
`kArchitectPrompt`. One user message carrying the context payload.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectRequestBody</code> parses the output back
as JSON and asserts: <code>create_room</code> tool present with required
<code>name</code> + <code>description</code> and no other input properties;
<code>tool_choice</code> requires the tool; <code>max_tokens==1024</code>; model
default + <code>TEXTWORLD_MODEL</code> override; and — mirroring the resolver's
stray-key guard — the <b>exact top-level key set</b> so no
<code>thinking</code>/<code>stream</code>/cache-control key can slip in. Pure
string→string (plus env read).
</blockquote>

### Step 5 — Validation gate + direction-invertibility table
**Requirements:** REQ-ARCH-9 (`validateRoomProposal`), REQ-ARCH-8. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Two pure functions in `architect.cpp`, both no-DB, no-throw:

- `inverseDirection(const std::string&)` → `std::optional<std::string>` — the fixed
  engine table `north↔south`, `east↔west`, `up↔down`, `in↔out`; anything else →
  `nullopt` (not generatable → wall, and no reciprocal to create).
- `validateRoomProposal(const HttpResponse&)` → `std::optional<RoomProposal>` that
  **never throws** and emits one stderr diagnostic naming the first failed clause:
  - **a.** HTTP 200 and the body contains **exactly one** `tool_use` block for
    `create_room` (0 or ≥2 → fail);
  - **b.** `name` present and non-empty after trim;
  - **c.** `description` present and non-empty after trim.
  Anything else → `nullopt` (→ REQ-ARCH-4 fallback).

**Fixture (pin it — do NOT make a live call to discover it).** Add a
`cannedCreateRoom(name, description)` test helper (the architect analog of
`cannedToolUse`, `tests.cpp:1234`) emitting the documented tool-use shape
`{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"create_room",
"input":{"name":...,"description":...}}]}`, so fixtures are built from documented
structure, never a network probe.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectGate</code> feeds
<code>cannedCreateRoom</code> fixtures covering <b>each clause a–c</b> plus 0-block,
≥2-block, empty-name, empty-description → right <code>RoomProposal</code>/<code>nullopt</code>.
<code>testArchitectInvertible</code> asserts the four pairs invert and a non-invertible
direction → <code>nullopt</code>. <b>ids-not-from-model:</b> a
<code>cannedCreateRoom</code> whose <code>input</code> JSON carries a spurious
<code>id</code>/<code>entity</code> key still validates to a
<code>RoomProposal</code> of just name+description — the stray key is ignored at the
gate and never reaches the two-field struct (the model can put no id on the wire). No
network; functions never throw (called outside any try/catch).
</blockquote>

### Step 6 — `writeGeneratedRoom` mutation helper (the write discipline)
**Requirements:** REQ-ARCH-9 (write), REQ-ARCH-6. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Add `writeGeneratedRoom(Db&, int64_t originRoom, const std::string& direction, const
RoomProposal&, int64_t actor)` to `mutations.{hpp,cpp}` — the **sole** sanctioned
write path for generated rooms, a sibling of `moveEntity`. Inside the caller's ambient
transaction it:
- **mints one entity** (`INSERT INTO entities DEFAULT VALUES` + `SELECT
  last_insert_rowid()` — micro-decision #1; the first runtime mint);
- writes its `room` tag, `name` (= proposal name), and `description` (canon = proposal
  description) rows; **no `location` row** (rooms have no container);
- writes the exit `(originRoom, direction) → new` and the reciprocal `(new,
  inverse(direction)) → originRoom` (inverse guaranteed present — the caller only
  reaches here for invertible directions);
- appends one `generated` event: `actor` = player, `subject` = **new room**, `object`
  = **originRoom**, `detail` = direction. (Note this `subject`/`object` reading is
  deliberately unlike `moveEntity`'s.)

Ids are engine-minted; the proposal carries none (REQ-ARCH-6). Update `mutations.hpp`'s
header note: `generated` joins the no-component-write-alone verbs' peers as a
helper-issued event.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testWriteGeneratedRoom</code> calls it directly inside
a transaction and asserts: a new entity id (distinct from seed ids 1–5), its
<code>room</code>/<code>name</code>/<code>description</code> rows carry the canned
values, both reciprocal <code>exits</code> rows exist (north→new AND south new→origin),
and one <code>generated</code> event with the specified subject/object/detail. The minted
id is engine-chosen (the <code>RoomProposal</code> carries none). (The
model-can't-send-an-id half of REQ-ARCH-6 is proven at the gate — Step 5.)
</blockquote>

### Step 7 — `architectGenerate` orchestration + two-phase catch + transport
**Requirements:** REQ-ARCH-4, REQ-ARCH-5, REQ-ARCH-11, REQ-ARCH-6. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Wire the pipeline with the **explicit two-phase boundary** (the load-bearing new
discipline), mirroring `aiResolve`'s two overloads:

```
bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor, const HttpTransport& transport) {
    std::optional<RoomProposal> proposal;
    try {                                         // ── Phase 1 (AI side) ──
        auto ctx  = buildArchitectContext(db, room, direction);
        auto body = buildArchitectRequestBody(ctx);
        auto resp = transport(body);              // at most once, no retries
        proposal  = validateRoomProposal(resp);   // never throws
    } catch (const std::exception& e) {
        std::fprintf(stderr, "architectGenerate: phase 1 failed: %s\n", e.what());
        return false;                             // → wall (REQ-ARCH-3c)
    } catch (...) {                               // mirror aiResolve's catch-all
        std::fprintf(stderr, "architectGenerate: phase 1 failed (non-std)\n");
        return false;
    }
    if (!proposal) return false;                  // gate failure → wall
    writeGeneratedRoom(db, room, direction, *proposal, actor);  // ── Phase 2 ──
    return true;                                  // NOT in the catch: a DB fault
}                                                 //   propagates to tick rollback
```

- **Phase 1** (context → request → transport → validate) wholly inside try/catch:
  HTTP error, timeout, malformed response, no tool call, gate failure, throwing
  transport → `false` → wall. **No DB write happens in Phase 1.** One non-prose stderr
  diagnostic permitted; nothing AI-flavored leaks to player output.
- **Phase 2** (`writeGeneratedRoom`) runs **only** on a validated proposal and is
  **outside** the catch, so a genuine DB fault propagates out of `architectGenerate`
  and `resolveGo` to `runTurn`'s existing try/catch → whole-turn rollback (REQ-ARCH-5).
  Never downgraded to a wall.
- Production overload `architectGenerate(db, room, direction, actor)` binds a local
  `curlTransport` (same URL/headers/**8 s** timeout as resolver; micro-decision #3).
- `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` returns nothing (REQ-ARCH-6) —
  the write is entirely in `writeGeneratedRoom`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectGenerate</code> with fake transports:
canned <code>create_room</code> → returns <code>true</code>, room + reciprocal exits +
<code>generated</code> event exist (creation, end-to-end via the helper);
transportError / malformed body / <b>throwing</b> transport / no-tool-call →
<code>false</code> AND — because no write was attempted — <b>no orphan entity, exit, or
description row</b> exists (Phase-1 atomic fallback); transport call-count == 1. Plus
<code>grep -En "INSERT|UPDATE|DELETE" src/architect.cpp</code> is empty.
</blockquote>

### Step 8 — The `resolveGo` seam
**Requirements:** REQ-ARCH-3, REQ-ARCH-2, REQ-ARCH-8. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Replace `resolveGo`'s bare wall (`systems.cpp:53-55`) with the 3-way branch, evaluated
**in order** (REQ-ARCH-3):
- **(a)** if `exitDest(room, direction)` exists → `moveEntity` (unchanged). Precedes any
  AI call, so re-crossing a generated exit **never regenerates** — persistence falls
  out of `exitDest`.
- **(b)** else if `aiNarrationEnabled()` **and** `inverseDirection(direction)` is present
  **and** `architectGenerate(db, room, direction, player)` returns `true` → `moveEntity`
  through the now-existing exit.
- **(c)** else the existing wall: `appendEvent(..., "failed", ..., "You can't go that
  way.")`.

`#include "architect.hpp"` in `systems.cpp`. Everything else in `resolve()` is
untouched; atomicity rests on the existing per-turn transaction (REQ-PROTO-5).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testResolveGoGenerate</code> drives ticks with a fake
transport (through <code>resolve</code>/the tick, not <code>runTurn</code>): (a) canned
proposal walking an <b>unmapped</b> invertible direction from the hall — <code>east</code>
(<code>seed/base.sql</code> maps only hall↔garden via north/south, so east reaches
branch b) → player moves to a new room, both reciprocal exits (east→new, west
new→hall) exist; (b) <b>persistence/no-regen</b> — go west back, then east again → same
room, transport invoked <b>zero</b> times on the re-crossing (branch a fires first);
(c) non-invertible
direction → wall with <b>no transport call</b>; (d) disabled mode
(<code>aiNarrationEnabled()</code> false) → today's wall, byte-identical, no transport
constructed. Existing <code>testSystems</code>/loop tests pass unchanged.
</blockquote>

### Step 9 — `generated`-event invisibility (cross-module)
**Requirements:** REQ-ARCH-10. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Make the `generated` verb renderer-invisible so a generation turn's player-visible
output stays the `moved` room block:
- `prose.cpp` — add `AND verb <> 'generated'` to **both** payload-feeding SELECTs: the
  **current-turn** query at `:321` (feeds `payload["events"]` — the load-bearing case,
  since `generated` and `moved` share the turn number and would otherwise leak the
  birth into narration) **and** the **recent-events** query at `~:381` (`WHERE turn <
  ?`, feeds `payload["recent_events"]`).
- **Do NOT touch** the third `WHERE turn = ?` SELECT at `prose.cpp:123`
  (`deterministicAppends`): it branches only on `moved`/`looked` verbs, so `generated`
  already produces no output there — noted here so it isn't mistaken for a miss.
- `render.cpp:128` — update the stale "fixed six" comment; the template renderer
  already emits nothing for unrecognized verbs, so `generated` needs no new branch.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testGeneratedEventInvisible</code>: a turn containing a
<code>generated</code> event (via Step 6/8) produces <b>no</b> template output for it,
and the verb is <b>absent from BOTH</b> the <code>events</code> and
<code>recent_events</code> keys of the <code>buildFacts</code> payload. The
generation turn's visible output remains the <code>moved</code> room block.
</blockquote>

### Step 10 — Live end-to-end smoke + bounded coherence judge (isolated, gated)
**Requirements:** REQ-ARCH-13. **Size:** S (code) · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>

> ⚠️ **This is the one high-token-risk step** ([[verification-must-be-bounded]]).
> It is deliberately last, isolated, and its assertions are **mechanical only** so it
> cannot become a tune-retry loop. Do **not** turn it into prompt-tuning iterations
> against live output; if the prompt needs work, that is a bounded, separate effort.

Add `testArchitectLiveSmoke`, structured like `testNlResolveLiveSmoke`: returns
immediately unless `TEXTWORLD_AI_LIVE_TEST=1` (skipped by default, excluded from any
CI), reads env without mutating it.

> **Harness gotcha (non-obvious, load-bearing):** register `testArchitectLiveSmoke()`
> in `main()` **alongside `testProseLiveSmoke()` / `testNlResolveLiveSmoke()`, BEFORE**
> the suite's hermetic `ANTHROPIC_API_KEY` / `TEXTWORLD_AI` unset block (the live-smoke
> calls sit ahead of the `unsetenv` block; every other test is registered after it).
> Registering it with the other `testArchitect*` calls at the bottom runs it after the
> key is cleared → the smoke silently no-ops forever even under
> `TEXTWORLD_AI_LIVE_TEST=1`, with no failure to flag the mistake.

With a real key against the **seeded** world
(setting + base.sql), generate a small chain of rooms through the **production**
transport and assert **mechanical invariants only**: each generated room has a
non-empty name and description, two reciprocal exits, and a valid move — **never**
model-specific wording. Coherence is checked by a **single bounded** judge call: feed
the setting text plus the generated descriptions to one LLM call — "coherent with the
setting and each other? yes/no + one line" — and **observe** the answer. **One call, no
loop**; a human reads the result.

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> default <code>./build/tests</code> run <b>skips</b> it (no
network). Under <code>TEXTWORLD_AI_LIVE_TEST=1</code> with a real key, the mechanical
invariants hold and the single coherence-judge call returns a yes/no + one line that a
human reads — asserted structurally, run rarely, never tuned in a loop.
</blockquote>

### Step 11 — Final validation against the spec checklist
**Requirements:** all (validation sweep). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section (items 1–8) end to end and confirm each:
1. Build check (REQ-ARCH-11) — `cmake -B build && cmake --build build`, no new
   packages; `src/architect.{cpp,hpp}` present, reusing `prose.hpp`'s transport seam +
   `aiNarrationEnabled()`.
2. Write-boundary grep (REQ-ARCH-6) — `grep -En "INSERT|UPDATE|DELETE"
   src/architect.cpp` empty; the room write lives in `mutations.cpp`.
3. Setting load (REQ-ARCH-1) — scratch present/absent cases; `SCHEMA_VERSION`
   unchanged; real `seed/setting.txt` committed and coherent.
4. Prompt inspection (REQ-ARCH-7c) — one room coherent with setting + origin,
   name+description via `create_room`, forbids exits/ids/arrival narration.
5. Disabled-mode run (REQ-ARCH-2, -3) — key unset → walking an unmapped exit yields
   today's byte-identical wall; existing tests unchanged.
6. Unit suite (REQ-ARCH-4/5/7a/7b/8/9/10/12) — `./build/tests` green incl. all new
   `testArchitect*` and generated-event invisibility asserted against **both** payload
   keys.
7. Failure behavior (REQ-ARCH-4) — fake timeout/malformed/throwing → `false`, turn
   walls within the bound, no crash, nothing AI-flavored leaks, no partial room.
8. Live smoke + bounded coherence (REQ-ARCH-13) — manual/optional per Step 10.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every checklist item passes; each maps back to a named
requirement. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-ARCH-1 (setting seed load, zero DDL, `settingPath`) | 1 |
| REQ-ARCH-2 (shared `aiNarrationEnabled()` switch) | 8 (seam gate) |
| REQ-ARCH-3 (resolveGo 3-way branch a/b/c) | 8 |
| REQ-ARCH-4 (Phase-1 catch → false → wall) | 7 |
| REQ-ARCH-5 (Phase-2 write propagates to tick rollback) | 6 (write), 7 (boundary) |
| REQ-ARCH-6 (architect.cpp no raw SQL; write in one helper; no ids) | 2, 6, 7 |
| REQ-ARCH-7a (context builder — exact 4 fields, no ids) | 2 |
| REQ-ARCH-7b (`create_room` tool schema + request params) | 4 |
| REQ-ARCH-7c (architect system prompt) | 3 |
| REQ-ARCH-8 (direction-invertibility table) | 5 (table), 8 (gate) |
| REQ-ARCH-9 (`validateRoomProposal` + `writeGeneratedRoom`) | 5 (validate), 6 (write) |
| REQ-ARCH-10 (generated-event invisibility: both SELECTs + render comment) | 9 |
| REQ-ARCH-11 (new TU, no new deps, transport seam, 8 s/no-retry, injected overload) | 2, 7 |
| REQ-ARCH-12 (unit tests + `cannedCreateRoom`) | 2, 3, 4, 5, 6, 7, 8, 9 |
| REQ-ARCH-13 (gated live smoke + one bounded coherence judge) | 10 |

All 13 ARCH requirements are covered, including the REQ-ARCH-10 renderer-comment
touch (Step 9). No step introduces a new build dependency, a raw SQL write inside
`architect.cpp`, or an id on the wire. The single live-LLM step (10) is isolated,
gated, last, and mechanical-assertions-only.
