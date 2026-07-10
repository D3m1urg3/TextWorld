---
title: "Implementation plan: blocked-directions-exit-display"
date: 2026-07-10
status: executed
tags: [plan, world-generation, exits, blocked-directions, exit-display, architect, latent-exits, render, mutations, seed]
modules: [world-gen, architect, systems, render, mutations, world]
related: [.lore/work/specs/blocked-directions-exit-display.md, .lore/work/brainstorm/blocked-directions-and-exit-display.md, .lore/work/research/procedural-room-feel.md, .lore/work/plans/ai-resolver.md]
---

# Implementation plan: blocked-directions-exit-display

Makes a room's exits a property **declared at its birth**: a `NULL`-dest `exits`
row is a **latent** open direction (walking it generates the room behind it),
a non-NULL row is **realized** (move), and an absent row is a **wall** (blocked,
not shown). The architect **co-authors** onward exits in the same `create_room`
call as the prose; the engine owns the invariants (return exit, direction legality,
dedup). Once the table holds latent stubs, the existing render query lists the
truthful open set with no new render logic — the display corrects itself.

Source of truth: **[.lore/work/specs/blocked-directions-exit-display.md]**
(11 requirements, prefix `EXITS`). This plan sequences those requirements into
atomic, mostly-deterministic steps that **extend the shipped architect**
(`src/architect.cpp`, `src/mutations.cpp`, `src/systems.cpp`, `tests/tests.cpp`)
rather than reworking its seams — it is the additive follow-on the architect spec
(`story-seed-architect`) deferred as "reference-leak onward exits."

## Guiding constraints (from memory + spec)

- **Deterministic skeleton first, LLM last.** Steps 1–6 are fully unit-testable
  with **no network**; the single live-LLM step (7) is isolated and gated behind
  `TEXTWORLD_AI_LIVE_TEST=1` with **mechanical-only** assertions. This is the
  [[token-risk-estimation]] / [[verification-must-be-bounded]] discipline: the
  token trap is open-ended live verification, not diff size.
- **No schema change, no `SCHEMA_VERSION` bump.** `exits.dest` is already nullable
  (`src/world.cpp:22`); "latent = NULL dest" needs no DDL. The db.hpp `Stmt` API
  (`colInt`/`colText` only) also stays frozen — the three-state read is done in SQL
  (micro-decision #1), not by extending the C++ API.
- **Reuse proven architect seams, don't rebuild them:** `cannedCreateRoom` fixture,
  `HttpTransport` fake-transport discipline, `tickT` injected-transport tick, the
  db-byte / no-egress contracts, and the `TEXTWORLD_AI_LIVE_TEST=1` gate all carry
  over from the architect tests.
- **One seam / one file / one testable behavior per step.** Target the architect
  plan's granularity; every deterministic step ships its own validation gate.

## Seams this touches (verified in tree)

| Seam | File:line | What this feature does with it |
|------|-----------|-------------------------------|
| `kArchitectPrompt` | `src/architect.cpp:138` | Flip the exit **prohibition** into a **requirement** — Step 1 |
| `buildArchitectRequestBody` create_room schema | `src/architect.cpp:179-197` | Add optional string-array `exits` property — Step 1 |
| `RoomProposal` struct | `src/architect.hpp:26-29` | Gains `std::vector<std::string> exits` — Step 2 |
| `validateRoomProposal(HttpResponse&)` | `src/architect.cpp:228` / `architect.hpp:75` | **Signature +body change**: takes direction-of-travel, sanitizes exits — Step 2 |
| `writeGeneratedRoom` origin-exit `INSERT` | `src/mutations.cpp:96-103` | Origin row now pre-exists latent → **upsert**; plant latent stubs — Step 3 |
| `resolveGo` case (b) | `src/systems.cpp:53-82` | Three-case split; free-generation + invertibility check **removed** — Step 4 |
| `exitDest` (anon ns) | `src/systems.cpp:34-40` | Replaced by a **three-state** read (no-row/latent/realized) — Step 4 |
| `roomBlock` exits query | `src/render.cpp:67-74` | Filter: realized always, latent iff `architectEnabled()` — Step 5 |
| `aiNarrationEnabled()` | `src/prose.cpp:416` | Reused verbatim as the **generation** switch (resolveGo); a new purpose-named `architectEnabled()` gates **display** — Steps 4, 5 |
| `seed/base.sql` exits | `seed/base.sql:36-38` | Add 2–3 `dest = NULL` frontier rows on the start cell/corridor — Step 6 |
| `world.db` (**gitignored, local**) | repo root | Delete so `openWorld` rebuilds from the new seed — **not** committed — Step 6 |
| `testShippedSeedShape` | `tests/tests.cpp` (`COUNT(*) FROM exits == 2`) | Update for the latent frontier rows — Step 6 |
| `cannedCreateRoom` fixture | `tests/tests.cpp:2509` | Gains an optional `exits` argument — Step 2 |

## Three micro-decisions (flagged, not blocking)

1. **Three-state exit read without a db.hpp change (Step 4).** `colInt` on a NULL
   column returns `0`, indistinguishable from "no row" via the current `exitDest`.
   **Recommend** a single SQL read that carries the null bit:
   `SELECT dest, dest IS NULL FROM exits WHERE room = ? AND direction = ?` —
   `!step()` ⇒ **no row (wall)**; `colInt(1)==1` ⇒ **latent**; else **realized**
   with `dest = colInt(0)`. Wrap it in an `enum class ExitState { Wall, Latent,
   Realized }` + optional dest helper in `systems.cpp`'s anon namespace, replacing
   `exitDest`. Alternative (rejected): add `bool colIsNull(int)` to `db.hpp` — a
   wider API change for no gain, and the spec pins "no db API change."

2. **`architectEnabled()` placement (Step 5).** A **distinct, purpose-named**
   predicate is mandated by REQ-EXITS-4 (display of a latent exit is *ontological*;
   prose narration is *cosmetic* — do not overload `aiNarrationEnabled()`).
   **Recommend** declaring/defining it in `architect.hpp`/`architect.cpp`, body
   returning `aiNarrationEnabled()` today (both env-backed, read at process start),
   documented as intentionally separable. `render.cpp` `#include "architect.hpp"`.
   Alternative: put it in `prose.hpp` beside `aiNarrationEnabled()` — rejected, the
   map concern belongs with the architect.

3. **Origin-exit realize = upsert with a precondition diagnostic (Step 3).** The
   production path always reaches `writeGeneratedRoom` via a **pre-existing latent
   row** (resolveGo 2b), so realizing it is an **UPDATE**, but a fresh `INSERT`
   would hit the `(room,direction)` PK. **Recommend**
   `INSERT INTO exits(room,direction,dest) VALUES(?,?,?) ON CONFLICT(room,direction)
   DO UPDATE SET dest = excluded.dest`, preceded by an existence check that emits
   **one stderr diagnostic** if the latent row was absent (a REQ-EXITS-2b
   precondition violation, never a normal path — never relied on by callers).

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> tool schema + prompt flip ─────────────┐<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>7</b> live smoke <span style="color:#b00">[HIGH token-risk — isolated]</span><br>
<b>2</b> RoomProposal.exits + gate ─▶ <b>3</b> writeGeneratedRoom ─▶ <b>4</b> resolveGo 3-case ─┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
<b>5</b> render filter + architectEnabled() ─────┼──────────────┤<br>
<b>6</b> seed frontier + world.db regen ─────────┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>8</b> final validation vs spec checklist<br>
</div>

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified · <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH</span> live-LLM verification.

Dependency notes: **2 → 3 → 4** is a hard chain (the `exits` field feeds the write
helper feeds the caller). **1**, **5**, and **6** are independent of that chain and
of each other — orderable freely, drawn in a natural reading order. **7** and **8**
follow everything.

---

### Step 1 — `create_room` `exits` field + prompt flip
**Requirements:** REQ-EXITS-5, REQ-EXITS-6. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Two edits in `src/architect.cpp`, both pure string / schema:

- **Tool schema** (`buildArchitectRequestBody`, ~`:179-197`): add one **optional**
  property `exits` to the `create_room` input schema — `{"type":"array",
  "items":{"type":"string"}}` with a description ("the invertible directions that
  lead onward from this room, excluding the way the player entered"). `name` and
  `description` **stay required**; `exits` is **not** added to `required`. Update the
  header comment in `architect.hpp:49-57` (the "no other fields" clause) accordingly.
- **Prompt** (`kArchitectPrompt`, ~`:149-151`): replace the absolute prohibition
  ("Do NOT describe exits, directions, doorways leading onward…") with the
  requirement — declare in `exits` the directions that lead onward, chosen to fit
  the setting and this room (density is the model's call); **exclude the entry-return
  direction** (the engine adds it); **describe the declared exits in the prose** and
  **name no opening not declared**. Keep the unchanged clauses (no ids, no arrival
  narration, description is of THIS room only). Sync the `architect.hpp:31-38`
  prompt-contract comment.

Prompt *quality* is verified live (Step 7); here we pin only structure.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> succeeds.
<code>testArchitectRequestBody</code> is updated: assert <code>properties</code>
now has <b>3</b> keys, <code>exits</code> is <code>type:"array"</code> with string
<code>items</code>, and <code>required == ["name","description"]</code> (exits
<b>absent</b> from required). <code>testArchitectPrompt</code> substring-asserts the
prompt now <b>requires</b> declaring onward exits, <b>excludes</b> the return
direction, and <b>requires</b> the prose to describe them (and no longer forbids
exits). Verifies spec AI-Validation item 4.
</blockquote>

### Step 2 — `RoomProposal.exits` + validation-gate sanitization
**Requirements:** REQ-EXITS-7 (and REQ-EXITS-1 invertible-only invariant). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- **Struct** (`architect.hpp:26-29`): add `std::vector<std::string> exits;` to
  `RoomProposal` (empty = dead end).
- **Signature** (`architect.hpp:75`, `architect.cpp:228`): `validateRoomProposal`
  gains the **direction of travel** so it can drop the return direction —
  `std::optional<RoomProposal> validateRoomProposal(const HttpResponse&, const
  std::string& directionOfTravel = "")`. `architectGenerate` (`:301`) passes its
  `direction` through. **Default it to `""`** (which `inverseDirection("")` maps to
  nullopt ⇒ no return-direction to drop) so the **11** existing
  `validateRoomProposal` call sites in `testArchitectGate` (`tests.cpp:2534-2627`),
  most of which don't exercise exits, compile **unchanged** — only the new
  exit-dropping cases pass a direction explicitly.
- **Body**: after the existing **fatal** name/description clauses (a–c, unchanged —
  a bad exit **never** rejects a room), read `input["exits"]` if present. For each
  entry: skip if not a string (one stderr diagnostic per drop); **normalize** (trim
  + lowercase); drop if not one of the eight invertible directions
  (`inverseDirection(norm)` non-nullopt), if a duplicate of an already-kept entry,
  or if it equals `inverse(directionOfTravel)` (the return direction the model was
  told to omit). Survivors (naturally ≤7) → `proposal.exits`. Absent/empty ⇒ empty
  vector. **Never fails on an exit** — lenient-for-exits / strict-for-prose.
- **Fixture**: `cannedCreateRoom` (`tests/tests.cpp:2509`) gains an optional
  `std::vector<std::string> exits = {}` argument that, when non-empty, adds an
  `"exits"` array to the tool `input` — the analog of how `cannedToolUse` carries
  optional fields.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectGate</code> updated to the new
signature and extended: a proposal with <code>exits:["north","East"," up "]</code>
travelling <code>"west"</code> yields the cleaned set <code>{north,east,up}</code>
(normalized); a proposal mixing valid dirs with a non-string, <code>"northeast"</code>,
a duplicate, and the <b>return direction</b> (<code>inverse(travel)</code>) drops
exactly those and <b>still creates the room</b>; blank name/description remain
<b>fatal</b>. No network. Function never throws. Verifies spec AI-Validation item 5.
</blockquote>

### Step 3 — `writeGeneratedRoom`: realize-origin upsert + latent stubs
**Requirements:** REQ-EXITS-8. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In `src/mutations.cpp:51-119`, inside the caller's transaction (order preserved):

1. **Realize the origin exit** — replace the plain `INSERT` (`:96-103`) with the
   **upsert** of micro-decision #3, preceded by an existence check emitting one
   stderr diagnostic when the `(originRoom, direction)` latent row is **absent**
   (precondition violation of REQ-EXITS-2b). Post-condition: `(originRoom, direction)`
   realized to `newRoom`.
2. **Realized return** `(newRoom, inverse(direction)) → originRoom` — **unchanged**
   (`:104-111`), still `INSERT` (a fresh room has no prior rows).
3. **Plant latent stubs**: for each `dir` in `proposal.exits`, `INSERT INTO
   exits(room, direction, dest) VALUES(newRoom, dir, NULL)`. The set is already
   deduped and already excludes the return direction (Step 2), so no collision with
   step 2's return row — no further filtering.
4. **`generated` event** — **unchanged** (`:116`).

Latent stubs emit **no** events; the `generated` event stays renderer-invisible
(`testGeneratedEventInvisible` still holds).

Pre-existing test to keep green: `testArchitectGenerate` (`tests.cpp:2687+`) calls
`architectGenerate(db, 1, "east", …)` with **no** pre-existing `east` row — i.e. it
already exercises the **absent-latent-row** branch. It must stay **unmodified and
passing**: an absent row is **log-diagnostic-and-proceed** (upsert with no conflict =
plain insert), **not** a hard failure. Do not tighten the precondition into a throw —
that would silently break this test.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testWriteGeneratedRoom</code> updated: seed a
<b>latent</b> <code>origin→north</code> row, then realize a proposal
<code>exits:["east","down"]</code> travelling <code>"north"</code> and assert —
origin north <b>realized</b> to the new room (upsert, not a second row),
<code>new→south→origin</code> realized, <code>new→east</code> and
<code>new→down</code> present with <b>NULL</b> dest, exactly <b>one</b>
<code>generated</code> event, and (new sub-case) an <b>absent</b> origin latent row
still realizes but logs the diagnostic. Verifies spec AI-Validation item 6.
</blockquote>

### Step 4 — `resolveGo` three-case split + three-state exit read
**Requirements:** REQ-EXITS-2, REQ-EXITS-3 (and REQ-EXITS-10 movement/persistence cases). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In `src/systems.cpp`:

- Replace `exitDest` (`:34-40`) with the **three-state** read of micro-decision #1
  (`ExitState { Wall, Latent, Realized }` + dest).
- Rewrite `resolveGo` (`:53-82`) to the exhaustive table, **in order**:
  **(a)** Realized → `moveEntity` (unchanged); **(b)** Latent **and**
  `aiNarrationEnabled()` → `architectGenerate` to realize, then move through the
  now-realized exit (on Phase-1 failure, wall for the turn — the latent row is
  untouched, retryable next turn, REQ-EXITS-3); **(c)** otherwise (Wall, or Latent
  with generation disabled, or generation failed) → `appendEvent(… "failed" …
  "You can't go that way.")`, byte-identical text.
- **Remove** the free-generation path and the `inverseDirection(action.direction)`
  guard from `resolveGo` — a direction with **no row** is now always a hard wall,
  and every latent row is invertible by construction (Step 2 / seed), so
  invertibility is checked at the gate/seed, never here.

Test harness note: `fixture.sql` maps only **realized** north/south, so the
generation cases must **seed a latent row in-test**
(`INSERT INTO exits(1,'east',NULL)` etc.) before ticking — free generation on a
bare direction is gone. `testResolveGoGenerate` and `testGeneratedEventInvisible`
are updated to seed the latent origin exit they walk.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testResolveGoGenerate</code> rewritten to the
three-case contract with the injected fake transport (via <code>tickT</code>):
<b>realized</b> → move; <b>latent + AI-on</b> → generates, moves, origin row now
non-NULL; <b>undeclared direction (no row)</b> → wall, <b>zero</b> transport calls;
<b>latent + AI-off</b> → wall; <b>failed generation</b> (Phase-1 forced to fail
twice at the same latent exit) → walls both times with the latent row <b>still
present</b> (provably retryable); <b>persistence</b> — re-crossing a realized exit
takes case (a), no regeneration. Verifies spec AI-Validation items 2 &amp; part of 8.
</blockquote>

### Step 5 — Render exit-display filter + `architectEnabled()`
**Requirements:** REQ-EXITS-4. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- **New predicate** `architectEnabled()` (micro-decision #2): declare in
  `architect.hpp`, define in `architect.cpp` returning `aiNarrationEnabled()` today,
  with a comment documenting it as the **display/ontology** gate, deliberately named
  apart from the prose/cosmetic switch so the two concerns can diverge.
- **Render filter** (`render.cpp:67-74`, `roomBlock`): `#include "architect.hpp"`;
  change the query to list **realized always, latent iff enabled** —
  `SELECT direction FROM exits WHERE room = ? AND (dest IS NOT NULL OR ?)
  ORDER BY direction`, binding `architectEnabled() ? 1 : 0`. A latent exit renders
  **identically** to a realized one (no marker). The `Exits:` line stays absent only
  when the row set is empty (a generated room always has ≥1 realized return, so it
  is never empty).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> a new <code>testExitDisplayInvariant</code> builds a room
with <b>one realized and one latent</b> exit: with AI enabled the <code>Exits:</code>
line names <b>both</b>; with AI disabled (<code>ANTHROPIC_API_KEY</code> unset) only
the <b>realized</b> one; the two exits render with <b>identical wording</b>. Verifies
spec AI-Validation item 3.
</blockquote>

### Step 6 — Seed frontier (+ local `world.db` refresh)
**Requirements:** REQ-EXITS-9 (+ migration note). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

- **`seed/base.sql`** (the deliverable): add a **fixed** frontier — **2** `INSERT
  INTO exits(room, direction, dest) VALUES(…, NULL)` latent rows on the start cell /
  corridor, coherent with Thornmere Hall (`seed/setting.txt`): e.g. the corridor
  (room 2) leading on to an unseen stair/landing (`north`), and a way `up` to the
  floor above. The realized cell↔corridor `north/south` pair is **unchanged**.
  Hand-authored SQL, invertible directions only (guards the displayed≠walkable hole
  per REQ-EXITS-8).
- **`world.db` is gitignored and untracked** (`.gitignore:3`); `openWorld`
  (`world.cpp:102`) auto-initializes it from `seed/base.sql` on first run when the
  meta table is absent. So there is **nothing to commit** — the repo has **no**
  frozen-tree risk (the spec's migration note applies only to a *player's own local
  save*). Action: **delete the local `world.db`** so the next run rebuilds it from the
  new seed. Do **not** `git add` it and do **not** override `.gitignore`.
- **`testShippedSeedShape`**: change `COUNT(*) FROM exits == 2` to the **exact** new
  total `== 4` (2 realized + 2 latent), and **add** an assertion that exactly 2
  latent (`dest IS NULL`) exits exist on start rooms. The realized bidirectional-pair
  join (which ignores NULL dests) **stays == 2**. This fresh-from-seed
  `TempDbFile` open **is** the authoritative check that the seed carries the frontier
  — it is the same code path `openWorld` runs, so no separate "did the engineer
  regenerate the file" test is needed.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testShippedSeedShape</code> passes with
<code>COUNT(*) FROM exits == 4</code>, exactly 2 <code>dest IS NULL</code> start
exits, and the realized cell↔corridor pair unchanged (join still == 2). A manual run
of a freshly-rebuilt world (with AI enabled) shows the latent exits on the
<code>Exits:</code> line and walking one generates a room. <code>world.db</code>
stays untracked (<code>git check-ignore world.db</code> still matches). Verifies spec
AI-Validation item 7.
</blockquote>

### Step 7 — Live end-to-end smoke (isolated, gated)
**Requirements:** REQ-EXITS-11. **Size:** S (code) · **Token-risk:** <span style="background:#fce8e6;color:#b00;padding:1px 6px;border-radius:3px;">HIGH — live LLM</span>

> ⚠️ **This is the one high-token-risk step** ([[verification-must-be-bounded]]).
> Deliberately last, isolated, gated behind `TEXTWORLD_AI_LIVE_TEST=1`, and its
> assertions are **mechanical only** — it must never become a prompt-tune-retry
> loop against live output. If the prompt needs work, that is a bounded, separate
> effort.

Extend `testArchitectLiveSmoke` (`tests/tests.cpp:2117`, already gated + registered
first in `main()`) to assert, over a short generated chain, **mechanically only**:

- a generated room's declared exits become **latent rows**; walking one continues
  generation;
- **displayed == walkable, failure-tolerant** (the controlling invariant): every
  invertible direction **not** on the exits line **walls** when walked; every
  direction **on** the line, when walked, either moves/generates **or**
  walls-while-leaving-its-row-intact (a transient Phase-1 failure per REQ-EXITS-3 is
  a **conforming** outcome, not a violation). The only failures: a shown direction
  that walls **and drops its row**, or an unshown direction that moves.
- **anti-degeneration**: across the chain, a **nonzero** fraction of rooms declare
  ≥1 surviving onward exit (guards the silent all-dead-ends collapse — a run where
  *every* room declares zero onward exits fails).

No wording assertions; coherence remains the single bounded judge call already in
this test.

<blockquote style="border-left:4px solid #b00;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> default <code>./build/tests</code> <b>skips</b> it (no
network). Under <code>TEXTWORLD_AI_LIVE_TEST=1</code> with a real key: declared exits
become latent rows, walking them continues generation, the displayed==walkable
invariant holds (failure-tolerant), and the anti-degeneration fraction is nonzero —
asserted structurally, run rarely. Verifies spec AI-Validation item 9.
</blockquote>

### Step 8 — Final validation against the spec checklist
**Requirements:** all (validation sweep). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section (items 1–9) end to end and confirm each:

1. **Build, no schema change** — `cmake --build build` succeeds; `grep -n
   "SCHEMA_VERSION" src/world.hpp` still shows `1`; no `ALTER`/new column in the DDL.
2. **Three-case movement** (Step 4) — realized moves; latent generates + realizes;
   no-row walls with unchanged text; forced Phase-1 failure walls + retryable.
3. **Display invariant** (Step 5) — AI-on lists realized + latent; AI-off only
   realized; identical wording.
4. **Tool + prompt** (Step 1) — schema has optional string-array `exits`; prompt
   requires declaring onward exits, excludes the return, requires prose to describe.
5. **Gate leniency** (Step 2) — junk exits dropped, room still created; blank
   name/description still fatal.
6. **Write helper** (Step 3) — origin realized, return realized, declared exits
   latent, one `generated` event.
7. **Seed frontier** (Step 6) — fresh world has ≥1 latent start exit; cell↔corridor
   unchanged.
8. **Regression + disabled parity** — `./build/tests` green incl. updated REQ-ARCH
   cases; `ANTHROPIC_API_KEY` unset ⇒ walls byte-identical, latent exits hidden.
9. **Live smoke** (Step 7) — manual/optional.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> every checklist item passes; each maps back to a named
requirement. This step is the plan's contract with the spec.
</blockquote>

---

## Requirement coverage map

| Requirement | Step(s) |
|-------------|---------|
| REQ-EXITS-1 (three-state exit model, no DDL) | 3, 4 (states realized), 2/6 (invertible-only) |
| REQ-EXITS-2 (resolveGo three-case) | 4 |
| REQ-EXITS-3 (failed realization retryable) | 4 |
| REQ-EXITS-4 (truthful display + `architectEnabled()`) | 5 |
| REQ-EXITS-5 (`create_room` `exits` field) | 1 |
| REQ-EXITS-6 (prompt flip) | 1 |
| REQ-EXITS-7 (gate sanitization) | 2 |
| REQ-EXITS-8 (write helper: realize + stubs) | 3 |
| REQ-EXITS-9 (seed frontier + migration) | 6 |
| REQ-EXITS-10 (unit tests) | 2, 3, 4, 5, 6 |
| REQ-EXITS-11 (gated live smoke) | 7 |

All 11 EXITS requirements are covered. No step bumps `SCHEMA_VERSION`, adds a DDL
column, extends the db.hpp `Stmt` API, or introduces a new build dependency. The
three-state distinction is done in SQL; display and generation are gated by two
deliberately separate predicates.
