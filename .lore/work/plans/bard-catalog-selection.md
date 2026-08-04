---
title: "Implementation plan: bard-catalog-selection"
date: 2026-08-03
status: approved
tags: [plan, bard, dungeon-master, eligibility, tool-use, validation-gate, wire-format, degradation, determinism]
modules: [bard, combat, mutations, architect]
related: [.lore/work/specs/bard-catalog-selection.md, .lore/work/design/bard-catalog-selection.md, .lore/work/specs/bard-fact-store.md, .lore/work/plans/bard-fact-store.md, .lore/work/specs/bard-overture-and-scheduling.md, .lore/work/specs/bard-architect-integration.md]
---

# Implementation plan: bard-catalog-selection

Brick 2 of the bard feature: **what the model sees and what the engine accepts back.**
Source of truth: **[.lore/work/specs/bard-catalog-selection.md]** (24 requirements,
prefix `BARD-SEL`), as amended by
[design 4](../design/bard-architect-integration.md). Depends on Brick 1
([bard-fact-store](bard-fact-store.md), `executed`) for the schema and the six write
helpers.

**Still no network call anywhere in this brick.** Context builders are SELECT-only,
request bodies are pure string→string, validation gates are pure functions over a
canned `HttpResponse`, and admission calls the Brick-1 helpers inside a transaction
the *caller* (Brick 3) owns. Nothing here spawns a thread, constructs a transport, or
touches `main.cpp` — the same offline shape as `buildArchitectContext` /
`buildArchitectRequestBody` / `validateRoomProposal`, all tested today without a key.

The work lands in **one new translation unit** (`src/bard.hpp`, `src/bard.cpp`), a
one-line `CMakeLists.txt` change, a small public-surface change in `src/combat.hpp` /
`src/combat.cpp`, and `tests/tests.cpp`. No schema change, no seed change, no
`SCHEMA_VERSION` bump.

## Guiding constraints

- **Skeleton first.** Step 1 creates the unit, wires it into `twcore`, and exports the
  two combat readers this brick needs — before any behavior exists. Every later step
  is then a compile-and-test cycle against a unit that already builds.
- **Read side, then wire side, then gate side, then admission.** Steps 2–5 are pure
  SELECTs; 6–7 build payloads; 8–10 build request bodies; 11–12 are the pure gates;
  13 is the only step that writes anything, and it writes only through Brick 1's
  helpers.
- **Zero live-LLM verification.** Every gate below is a canned `HttpResponse` and a
  seeded world. [[verification-must-be-bounded]] has nothing to bite on: there is no
  step where "check whether the model did the right thing" is the validation.
  Token-risk is LOW throughout except Step 8 (authored prompt prose), which is MEDIUM
  only because prompt wording invites rewrite churn — not because it costs calls.
- **The bard's TU is read-only by grep.** `grep -En "INSERT|UPDATE|DELETE" src/bard.cpp`
  must stay empty for the life of the brick (REQ-BARD-SEL-21). Calling
  `writeCatalogEntry` from `bard.cpp` is fine and intended — the helper contains the
  SQL, the caller does not.
- **Empty is the normal answer.** An empty catalog, an empty menu, an empty motive
  list, and a response with no tool call are all valid, common outcomes. Any step that
  makes one of them an error has been implemented wrong.

## Seams this touches (verified in tree)

| Seam | File:line | What this brick does with it |
|---|---|---|
| `distanceFromSeed` | `src/combat.cpp:164` (anon ns) | **exported** to `combat.hpp` — Step 1 |
| `eligibleArchetypesForNewRoom` | `src/combat.cpp:301` (anon ns) | **exported** to `combat.hpp` — Step 1 |
| anon-namespace boundary | `src/combat.cpp:13`–`317` | the two above move below it |
| `eligibleArchetypes` | `src/combat.hpp:113`, `combat.cpp:606` | **called** by gate (d) — Step 3 |
| `kFrontRadius` | `src/combat.hpp:56` | the front dial gate (d) inherits, unchanged |
| `twcore` source list | `CMakeLists.txt:17` | `src/bard.cpp` appended — Step 1 |
| `settingText` / `nameOf` | `src/architect.cpp:88`, `:95` (anon ns) | the shape `bard.cpp`'s file-local readers copy |
| `buildArchitectContext` | `src/architect.cpp:165` | the payload-shape precedent (nlohmann `json`, `.dump()`) |
| `buildArchitectRequestBody` | `src/architect.cpp:179` | the tool-schema + schema-enforced-enum precedent |
| `kArchitectPrompt` | `src/architect.cpp:146` | the git-versioned raw-string prompt precedent — Step 8 |
| `failClause` / `blankAfterTrim` | `src/architect.cpp:37`, `:45` | copied file-local into `bard.cpp` — Steps 11–12 |
| `validateRoomProposal` | `src/architect.cpp:266` | the exception-free `json::parse` + block-scan precedent |
| the narrator's tag shield | `src/prose.cpp:314` | the rule Step 7 reuses for wake event lines |
| `writeCatalogEntry` (+ truth gate) | `src/mutations.hpp:201` | throws on a false fact — caught per-entry in Step 13 |
| `writeBardJournal` / `writeBardFocus` | `src/mutations.hpp:253`, `:262` | called by admission — Step 13 |
| `markCatalogSeeded` | `src/mutations.hpp:240` | called by `mark_seeded` — Step 13 |
| `readFileBytes` source guards | `tests/tests.cpp:135`, `:1320` | the precedent for encoding the spec's greps as tests — Step 14 |
| test registration | `tests/tests.cpp:8549-8556` | one `testBardSel*` call per new test, beside the `testBardStore*` block |

## Micro-decisions pinned before drafting

Nine points the spec leaves open or under-determines. All are decided here so
implementation does not re-litigate them.

**1. The motive enum is passed in, not read inside the builder.** REQ-BARD-SEL-11 says
`buildOvertureRequestBody(contextPayload)` is pure string→string; REQ-BARD-SEL-12 says
the motive enum is "read from the database rather than hardcoded in the request
builder." Both cannot be literally true of one function. Resolved exactly the way the
architect already resolved the identical tension for the `enemy` enum
(`architect.hpp:80`): the vocabulary is a **second parameter**, and a one-line DB
reader supplies it.

```cpp
std::vector<std::string> motiveKeys(Db& db);                    // SELECT motive FROM motive_catalog ORDER BY motive
std::string buildOvertureRequestBody(const std::string& contextPayload,
                                     const std::vector<std::string>& motives);
std::string buildWakeRequestBody(const std::string& contextPayload,
                                 const std::vector<std::string>& motives);
```

Spec test 11 is satisfied unchanged — a ninth `motive_catalog` row changes what
`motiveKeys` returns, which changes the enum — while the builders stay testable with
no `Db` at all. The alternative (a `Db&` in the request builder) would make the bard
the only unit whose request body is not a pure function, for no gain.

**2. The two combat readers are exported, not reimplemented.** Gate (b) needs
`distanceFromSeed`; REQ-BARD-SEL-5 needs the origin+1 variant. Both live in
`combat.cpp`'s anonymous namespace today. REQ-BARD-SEL-3's rule ("call it, don't
reimplement it") is about drift, and it applies just as much to the distance metric as
to the archetype menu — a second BFS in `bard.cpp` is exactly the drift the requirement
forbids. Both move below `}  // namespace` (`combat.cpp:317`) and are declared in
`combat.hpp` beside `eligibleArchetypes`, comments carried over verbatim.

**3. An unreachable room offers nothing.** `distanceFromSeed` returns `INT64_MAX` for a
room BFS cannot reach (`combat.cpp:187`). Read literally, gate (b)'s
`tier <= distanceFromSeed(room)` then admits *every* entry at *every* tier — the story
menu would be at its most permissive exactly where combat's is empty. Pinned: an
`INT64_MAX` distance yields an **empty** menu, guarded before the comparison, so story
and combat agree on what "off the map" means. Mirror `combat.cpp:302-305`'s overflow
guard in `eligibleCatalogForNewRoom` for the same reason.

**4. Gate (d) reads *every* neighbor, and a prospective room's neighborhood is its
origin.** Spec and design both say "a room one hop away" in the singular; implemented
as the **union over every realized neighbor** (`SELECT dest FROM exits WHERE room = ?
AND dest IS NOT NULL`), because a room with three exits has three adjacent menus and
picking one of them would be arbitrary. For the prospective room of REQ-BARD-SEL-5 the
room does not exist yet, so "eligibleArchetypes for this room" is
`eligibleArchetypesForNewRoom(originRoom)` and the one-hop set is
`eligibleArchetypes(originRoom)` — the origin is its only neighbor by construction
(`combat.cpp:298-300` states exactly this about its own link), so no exits read happens
on that path.

**5. `eligibleCatalogForNewRoom` spans both kinds.** REQ-BARD-SEL-5 takes no `kind`
argument, and REQ-BARD-ARCH-6 wants "the eligible handles" as one enum. Characters and
beats both materialize as entities (design 4, decision 1), so the architect's menu is
the union, ordered by `catalog.id` like every other menu here.

**6. Admission lives in this brick; the transaction does not.** REQ-BARD-SEL-19 says
the fact drop is enforced "at the call site," and spec tests 12/14 assert *rows*, not
proposals — so the loop that calls `writeCatalogEntry` per entry belongs here. Brick 3
(REQ-BARD-WAKE-7) owns opening the transaction around it. Two functions, both taking a
`Db&` and neither beginning nor committing:

```cpp
int admitOvertureProposal(Db& db, const OvertureProposal& proposal);  // returns entries admitted
int applyWakeProposal(Db& db, const WakeProposal& proposal);          // returns writes applied
```

**6a. Admission pre-flights each entry and does NOT catch.** REQ-BARD-SEL-19 names its
mechanism — "catching the helper's throw at the call site" — and that mechanism cannot
work in this codebase. `db.hpp:1-2`: *"All errors throw `std::runtime_error`."*
`writeCatalogEntry` throws the **same type** for a false fact as SQLite does for a disk
fault, a lock, or a `UNIQUE` violation on `catalog.handle`. A `catch (const
std::runtime_error&)` therefore cannot tell "drop this entry" from "the database is
broken," and would silently swallow the second — while REQ-BARD-WAKE-7 and -23 require
exactly the opposite (*"a throw from any helper rolls back the whole overture, leaving
zero catalog rows"*). As written the two specs cannot both be satisfied by a catch.

Both are satisfiable without one. Admission runs a **read-only pre-flight** that mirrors
every refusal `writeCatalogEntry` makes — `kind` in vocabulary, a `motive_catalog` row
exists, `handle`/`name`/`blurb` non-empty after trim, `tier >= 0`, `fact` both-or-
neither, and the three truth-gate clauses (`bestiary` row, `spell_catalog.element`,
`resistance` row for the pair) — **plus** handle uniqueness against `catalog` and
against handles already admitted in this same call, which the helper enforces only via
the `UNIQUE` constraint and which a bulk overture can plausibly violate on its own. An
entry the pre-flight refuses is dropped with one diagnostic and **never reaches the
helper**; the observable outcome of REQ-BARD-SEL-19 and its spec test 14 is unchanged
(zero rows for that entry, siblings admitted). Any throw that still escapes is by
construction a genuine engine fault and **propagates** to the caller's transaction —
the `architectCommitProposal` discipline (`architect.cpp:438-443`: Phase 2 sits outside
the catch so a real DB fault reaches `runTurn`'s rollback rather than being downgraded).

All SELECTs, so REQ-BARD-SEL-21 holds. The cost is a duplicated predicate, and the
duplication is fenced by an equivalence test in Step 13 rather than by hope.

**6b. One spec sentence needs amending, and it is not this plan's to change.**
REQ-BARD-SEL-19's clause *"This is enforced by catching the helper's throw at the call
site and continuing with the remaining entries"* describes a mechanism that 6a replaces;
its **requirement** — a false fact drops the entire entry, not merely the fact — is
implemented exactly and its test passes verbatim. Flag it to the spec owner when this
plan is approved. Nothing else in either spec moves.

**7. The wake event budget is `kBardWakeEventLimit = 120`.** Confirmed with the author.
REQ-BARD-SEL-10 bounds neither the event span nor the catalog. The catalog stays whole
(authored once, small, and the spec is explicit). Events are capped at the most recent
120 rows since `meta.bard_last_wake_turn`, rendered **oldest-first** so the narrative
order is preserved. Without the cap, a session in which every wake fails grows the
payload without bound — `meta.bard_last_wake_turn` only advances when a wake is
*queued* (REQ-BARD-WAKE-11), which is Brick 3's write, not this brick's.

**8. Event lines carry names, never ids or internal tags.** The wake context renders
each event as one line of the form `turn 12: defeated rime-touched acolyte`. The
`subject` is name-resolved (0 → omitted); the `object` column is **dropped entirely**
(it carries room/container ids — `prose.cpp:279-281` refuses it for the same reason);
and `detail` is withheld on `burned`, `froze`, and `materialized`, the three
engine-internal tags named at `prose.cpp:312-315` and `mutations.hpp:51-62`. This is
the same shield, applied to a second model-facing surface, and it is why REQ-BARD-SEL-8's
"no ids on the wire" survives the event lines.

**9. No `maxLength` on the journal in the tool schema.** Design open question 3 asks
for one and answers "the number is a guess until there is real output." Deferred
deliberately: REQ-BARD-STORE-16 stores the journal verbatim and uncapped, nothing else
reads it, and adding the constraint later is one line in the schema. Recorded here so
it is a decision rather than an omission. (`focus` needs none — REQ-BARD-STORE-16a
truncates it at the write side.)

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<b>1</b> unit skeleton + CMake + combat exports<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>2</b> eligibleCatalog gates a–c ──▶ <b>3</b> gate (d) ──▶ <b>4</b> eligibleCatalogForNewRoom<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>5</b> catalogForHandle / catalogIdForHandle<br>
&nbsp;&nbsp;│<br>
&nbsp;&nbsp;├─▶ <b>6</b> buildOvertureContext ─┐<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>9</b> buildOvertureRequestBody ──▶ <b>10</b> buildWakeRequestBody<br>
&nbsp;&nbsp;├─▶ <b>7</b> buildWakeContext ─────┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▲<br>
&nbsp;&nbsp;│&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;│<br>
&nbsp;&nbsp;└─▶ <b>8</b> the two system prompts ───────────────┘<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>11</b> validateOvertureResponse ──▶ <b>12</b> validateWakeResponse<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>13</b> admission (13a overture · 13b wake)<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;▼<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;<b>14</b> contract guards ──▶ <b>15</b> final validation<br>
</div>

Risk legend:
<span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, mechanically verified ·
<span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span> authored content, invites rewrite churn.

Step 1 gates everything. After it, the read branch (2→3→4, 5) and the prompt/wire
branch (6, 7, 8 → 9 → 10) are independent and can be reordered freely. Steps 11–15 are
strictly terminal in the order shown: 13 consumes the proposal types 11 and 12 define,
and 14 inspects source lines every prior step wrote.

---

### Step 1 — The unit skeleton, the CMake wire-in, and the two combat exports
**Requirements:** REQ-BARD-SEL-21 (enabling). **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Create **`src/bard.hpp`** with the unit's contract comment — the `architect.hpp:1-13`
header block is the model, restated for a unit that is read-only *and* has no write
counterpart of its own:

> The bard: the world's live author. This TU performs ONLY SELECTs and (in Brick 3)
> network egress — no INSERT, UPDATE, or DELETE may ever appear in `bard.cpp`
> (REQ-BARD-SEL-21). Every write goes through the `mutations.cpp` helpers. Catalog
> ids, entity ids, `tier` values, and the `seeded` flag are never sent to or read
> from the model (REQ-BARD-SEL-8).

Declare the three value types the later steps fill in — `CatalogChoice`,
`OvertureProposal`, `WakeProposal` — and nothing else yet:

```cpp
// One offerable catalog entry, as the model sees it. Carries NO id, tier, name,
// seeded flag, or raw fact column (REQ-BARD-SEL-1, -8).
struct CatalogChoice {
    std::string handle;       // the selection token, returned verbatim
    std::string blurb;        // the only prose the model selects on
    std::string motiveBlurb;  // motive_catalog.blurb, never the key
};
```

Create **`src/bard.cpp`** including `bard.hpp`, `combat.hpp`, `json.hpp`,
`mutations.hpp`, and an anonymous namespace holding file-local `settingText`,
`nameOf`, `trim`/`blankAfterTrim`, and `failClause` copies — the same duplication
`architect.cpp:88-105` and `nlresolve.cpp:49` already accept for one-line readers
rather than growing a shared header.

Append `src/bard.cpp` to the `twcore` source list at **`CMakeLists.txt:17`**.

Then **export the two combat readers** (micro-decision 2): move `distanceFromSeed`
(`combat.cpp:164-188`) and `eligibleArchetypesForNewRoom` (`combat.cpp:298-306`) below
`}  // namespace` at `combat.cpp:317`, and declare both in `combat.hpp` beside
`eligibleArchetypes` (`combat.hpp:113`), carrying their comments verbatim and adding
one sentence each naming the bard as the second caller.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean (the new TU compiles
and links with no symbols yet); the <b>whole</b> existing suite green — the combat
export is a linkage change only, so any combat/architect/pregen failure here means
the move changed behavior and must be undone. New <code>testBardSelExports</code>:
<code>distanceFromSeed(db, kDormitoryCell) == 0</code>, a one-hop room is 1, and a
room with no realized path from the seed returns <code>INT64_MAX</code>;
<code>eligibleArchetypesForNewRoom</code> called from the test binary returns the same
vector <code>eligibleEnemyBlurbs</code>'s archetypes map to. <code>grep -En
"INSERT|UPDATE|DELETE" src/bard.cpp</code> empty (trivially, and it stays that way).
</blockquote>

### Step 2 — `eligibleCatalog`: gates (a), (b), (c), ordering
**Requirements:** REQ-BARD-SEL-1, -2a/b/c, -4, -7. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

```cpp
// The catalog entries offerable in `room`, each as handle + blurb + motive blurb.
// Read-only, deterministic, O(catalog size). Empty is a NORMAL answer, never an
// error — the same contract eligibleEnemyBlurbs has on a safe-edge room.
std::vector<CatalogChoice> eligibleCatalog(Db& db, int64_t room,
                                           const std::string& kind);
```

One SELECT joining `catalog` to `motive_catalog` on `motive`, filtered by
`entity IS NULL` (a), `kind = ?` (c), and ordered by `catalog.id` (REQ-BARD-SEL-4).
Gate (b) is applied in code, not SQL, because of micro-decision 3: read
`distanceFromSeed(db, room)` once, return `{}` immediately if it is `INT64_MAX`, and
otherwise keep entries with `tier <= distance`.

Use a **LEFT JOIN** on `motive_catalog` so an entry whose motive row is somehow absent
still offers with an empty motive blurb rather than vanishing — `writeCatalogEntry`
makes that unreachable today, and a silent disappearance would be the harder failure
to diagnose if it ever becomes reachable.

No per-entry unlock condition is read and none exists (REQ-BARD-SEL-7) — this is the
storylet/time-cave line from the design, and the enforcement is that Step 14's grep
finds no such column reference.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelEligible</code> against
<code>tests/combat_fixture.sql</code> with a catalog seeded across tiers 0–3 via
<code>writeCatalogEntry</code> (spec tests 5, 7, 8): at a seed-adjacent room only
tier ≤ distance entries are offered; the same call at a deeper room offers strictly
more; an entry materialized with <code>materializeCatalogEntry</code> disappears from
the menu; <code>kind = "beat"</code> excludes every <code>character</code>; results
carry <b>no</b> id/tier/name/seeded field (assert by struct shape and by string search
on the blurbs); the returned order equals ascending <code>catalog.id</code> across two
calls and across a close+reopen of the world (determinism, spec test 7); a room whose
catalog offers nothing returns empty <b>without throwing</b> (spec test 8).
</blockquote>

### Step 3 — Gate (d): knowledge beats need a live subject
**Requirements:** REQ-BARD-SEL-2d, -3. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 2. An entry with a non-empty `fact_archetype` passes only if that
archetype appears in `eligibleArchetypes(db, room)` **or** in
`eligibleArchetypes(db, neighbor)` for any neighbor reachable through a realized exit
(`SELECT dest FROM exits WHERE room = ? AND dest IS NOT NULL`).

Call `eligibleArchetypes` — never reimplement its three gates (REQ-BARD-SEL-3). Compute
the union once per `eligibleCatalog` call, not once per entry, and only when at least
one surviving entry actually carries a fact, so the common (factless) catalog pays
nothing.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardSelEligible</code> gains spec test 6: a
knowledge beat on an archetype the local and adjacent menus do not offer is
<b>withheld</b>; the identical beat becomes offerable when the room argument moves to
a room whose own or whose neighbor's <code>eligibleArchetypes</code> includes it —
<b>drive both states from one world by changing only the room argument</b>, so the
test cannot pass by accident of fixture divergence. A factless entry in the same
catalog is unaffected in both states. Plus the union case (micro-decision 4): a room
with <b>two</b> realized neighbors, only one of which offers the archetype, still
offers the beat — the assertion that catches a first-neighbor-only implementation.
</blockquote>

### Step 4 — `eligibleCatalogForNewRoom`
**Requirements:** REQ-BARD-SEL-5. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 2–3.

```cpp
// The choices for the room about to be created one hop beyond `originRoom` —
// its front distance is the origin's plus one. Spans BOTH kinds (design 4).
// Mirrors eligibleEnemyBlurbs' relationship to eligibleArchetypes.
std::vector<CatalogChoice> eligibleCatalogForNewRoom(Db& db, int64_t originRoom);
```

Distance is `originDist + 1`, with `combat.cpp:302-305`'s `INT64_MAX` guard copied so
the increment cannot overflow. Gate (d) uses micro-decision 4's neighborhood:
`eligibleArchetypesForNewRoom(db, originRoom)` ∪ `eligibleArchetypes(db, originRoom)`.
No `kind` filter (micro-decision 5).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardSelEligible</code> gains: for an origin at
distance <i>d</i>, the prospective menu equals <code>eligibleCatalog</code>'s at a real
room of distance <i>d+1</i> for the same catalog — <b>construct a world where the
origin's own menu and the prospective menu differ by exactly one tier</b>, so a test
passing against the wrong distance is impossible; both kinds appear in one call; an
origin whose distance is <code>INT64_MAX</code> yields empty and does not overflow.
</blockquote>

### Step 5 — `catalogForHandle` and `catalogIdForHandle`
**Requirements:** REQ-BARD-SEL-6, -24. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 2–3 — `catalogForHandle` re-runs `eligibleCatalog`, so it is only
correct once gate (d) exists. Two deliberately different lookups — the header comment must say
why, because the next reader will otherwise "simplify" one into the other:

```cpp
// LIVE re-check (REQ-BARD-SEL-6): resolve a model-supplied handle to a catalog id
// ONLY if it is eligible in `room` right now. Unknown, stale, already-materialized,
// or empty -> 0. Verbatim the archetypeForEnemyBlurb contract, and it matters MORE
// here: a bard proposal may have been snapshotted many turns before it commits.
int64_t catalogForHandle(Db& db, int64_t room, const std::string& handle);

// ROOM-FREE, GATE-FREE lookup (REQ-BARD-SEL-24), for mark_seeded ONLY. A wake is
// not scoped to a room, so there is no room to supply, and the gates ask the wrong
// question: seeding records that an entry was HINTED, which can be true of an entry
// that is not offerable anywhere. Unknown handle -> 0.
int64_t catalogIdForHandle(Db& db, const std::string& handle);
```

`catalogForHandle` re-runs `eligibleCatalog` for both kinds and matches on `handle`;
it never trusts a menu snapshot. `catalogIdForHandle` is one `SELECT id FROM catalog
WHERE handle = ?`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelHandle</code> (spec tests 9, 11b):
<code>catalogForHandle</code> returns the id for a currently-eligible handle; returns
<b>0</b> for an unknown handle, for <code>""</code>, for a handle whose entry was
materialized <i>between</i> menu construction and the call (materialize it mid-test —
this is the stale-snapshot case), and for one whose tier exceeds the room's distance.
<code>catalogIdForHandle</code> returns <b>non-zero</b> for both of the handles
<code>catalogForHandle</code> just refused (ineligible-by-tier and already-materialized)
and 0 for an unknown one — the assertion that catches <code>mark_seeded</code> being
wired to the wrong lookup.
</blockquote>

### Step 6 — `buildOvertureContext`
**Requirements:** REQ-BARD-SEL-9, -8. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Pure function of the database, `buildArchitectContext`'s shape (`architect.cpp:165-177`):
an nlohmann `json` object, `.dump()`ed, with **exactly two** keys — `setting`
(`meta.setting`) and `motives` (an array of `{key, blurb}` from `motive_catalog`,
ordered by key).

The blurbs are not optional and the comment must say so: the tool schema constrains
`motive` to eight bare keys, and without their meanings the model is choosing between
opaque tokens — it cannot tell `obligation` from `homesickness`. Nothing else about
world state goes in; at overture time none exists to describe.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelContext</code> (spec tests 10, 11a):
the payload parses as a JSON object with exactly the keys
<code>{setting, motives}</code>; it contains all <b>eight</b> motive keys <b>and</b>
their eight blurb strings verbatim; it contains the setting text; an empty
<code>meta.setting</code> still yields a well-formed object. No id/tier assertions are
possible yet (no catalog is carried) — those land in Step 7.
</blockquote>

### Step 7 — `buildWakeContext`, the event lines, and `kBardWakeEventLimit`
**Requirements:** REQ-BARD-SEL-10, -8. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Pure function of the database, carrying exactly five keys: `setting`, `motives` (same
builder as Step 6, factored into one file-local helper), `journal`
(`meta.bard_journal`), `events`, and `catalog`.

`events` is the rows with `turn > meta.bard_last_wake_turn`, rendered per
micro-decision 8 as human-readable lines, most-recent-`kBardWakeEventLimit` selected
by `ORDER BY id DESC LIMIT ?` then **reversed to oldest-first** before rendering. Put
the constant in `bard.hpp` with the reason:

```cpp
// The most recent N events a wake may see. meta.bard_last_wake_turn advances when a
// wake is QUEUED (REQ-BARD-WAKE-11, Brick 3), so a session where every wake fails
// would otherwise grow this payload without bound. Tunable = this one line.
inline constexpr int64_t kBardWakeEventLimit = 120;
```

`catalog` is the **full** table rendered as handle + blurb + motive blurb +
materialized-or-not (REQ-BARD-SEL-10) — no ids, no tier, no `seeded`. The wake needs
the whole table, not the eligible menu, because `mark_seeded` and the journal reason
about entries that are not currently offerable anywhere.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardSelContext</code> gains spec tests 10, 11a
against a seeded world with a materialized entry, a defeated enemy, and a generated
room: the payload carries exactly the five keys; every motive key <b>and blurb</b>
appears; a known handle and its blurb appear; a materialized entry is marked as such.
Then the <b>no-ids sweep</b> — assert the string forms of every <code>catalog.id</code>,
every entity id in the world, and every distinct <code>tier</code> value appear
<b>nowhere</b> in the payload (skip the degenerate single-digit trap by seeding ids
above 9, or assert on delimited forms). Assert the <code>materialized</code> event's
<b>handle detail is absent</b> (micro-decision 8's shield) and that no event line
contains the object column's room id. Cap test: with 200 qualifying events, exactly
120 lines are rendered, they are the <b>most recent</b> 120, and they are ordered
oldest-first.
</blockquote>

### Step 8 — The two system prompts
**Requirements:** REQ-BARD-SEL-22, -23. **Size:** M · **Token-risk:** <span style="background:#fff4e5;color:#8a5300;padding:1px 6px;border-radius:3px;">MED</span>

`extern const char* const kBardOverturePrompt;` and `kBardWakePrompt;` in `bard.hpp`,
defined as raw string literals in `bard.cpp` — `kArchitectPrompt`
(`architect.cpp:146-163`) is the shape to follow, including the paragraph that names
each context key so the model knows what it is reading.

Both must contain the **situations-not-urgency** clause (REQ-BARD-SEL-23), stated
plainly enough to survive a substring test: no deadlines, no countdowns, nothing that
makes standing still or talking at length feel expensive. Escalation in this game is
**spatial** — distance from the seed is the only intensity dial — and a prompt that
introduces time pressure undercuts the one mechanic the whole eligibility system is
built on.

The overture prompt additionally states: author the cast and the beats for a world
that does not exist yet; `tier` is how deep from the start a thing belongs, not how
dangerous it is; a `fact` must be true of the rules, and a false one costs you the
whole entry. The wake prompt states: you are reading what has happened since you last
looked; `write_focus` is the one line the room-writer will see, so make it usable;
declining to act is a legitimate turn.

**Token-risk note.** MED is about *drafting churn*, not calls — prompt wording is the
one thing in this brick with no mechanical right answer. Write both once, pin their
structure by substring, and resist tuning them here: quality tuning belongs to Brick 3,
where a real call can be observed. Do not build a rewrite-and-rerun loop.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelPrompt</code>, modeled on
<code>testArchitectPrompt</code> (tests.cpp:6422): each prompt is non-empty; each
names its own tools by name; each contains the urgency-prohibition clause (spec
mechanical check 4 — assert on a distinctive phrase, not a whole sentence); the
overture prompt mentions <code>write_catalog</code> and the wake prompt mentions all
four wake tools; neither contains the words "id" or "tier" as a thing to emit. Both
are string constants in the unit's header surface (REQ-BARD-SEL-22).
</blockquote>

### Step 9 — `buildOvertureRequestBody` and the `write_catalog` schema
**Requirements:** REQ-BARD-SEL-11, -12, -14, -8. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 6 and 8. Signature per micro-decision 1, with `motiveKeys(Db&)` added
beside it as the DB reader that feeds the enum.

Body: `model = modelForRole(AiRole::Generate)` (story authoring is quality work, same
role the architect uses — do not restate the precedence rule, it lives in
`aihttp.hpp`), `max_tokens` large enough for a bulk write (**4096**, stated as the one
deliberate difference from the architect's 1024 — an overture writes a whole cast, not
one room), `system = kBardOverturePrompt`, one user message carrying
`contextPayload`, one `write_catalog` tool, and `tool_choice = {"type":"auto"}`
(REQ-BARD-SEL-14 — a response with no tool call is a valid outcome, unlike the
architect's required tool).

The tool's input schema: required `entries` array and required `journal` string. Each
entry object has required `kind`, `handle`, `name`, `blurb`, `motive`, `tier`, and
optional `fact` of `{archetype, element}` with **both** required inside it. `kind` is
`enum: ["character","beat"]`; `motive` is `enum:` the passed-in keys — the same
schema-enforced-enum treatment `enemy` gets at `architect.cpp:238-247`. Belt and
braces: the schema stops a ninth motive, and `writeCatalogEntry` throws if one arrives
anyway.

No `maxLength` on `journal` (micro-decision 9). No `place_catalog` tool, here or
anywhere (REQ-BARD-SEL-15).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelRequestBody</code>, modeled on
<code>testArchitectRequestBody</code> (tests.cpp:4289): the body parses; its top-level
key set is exactly <code>{model, max_tokens, system, messages, tools, tool_choice}</code>
— no <code>thinking</code>, no <code>stream</code>, no cache key;
<code>tool_choice.type == "auto"</code>; the single tool is named
<code>write_catalog</code>; <code>required</code> is exactly
<code>["entries","journal"]</code>; the entry schema's <code>required</code> is exactly
the six non-fact fields; <code>fact</code> requires both of its fields. Spec test 11:
insert a <b>ninth</b> <code>motive_catalog</code> row in the test world, pass
<code>motiveKeys(db)</code>'s nine-element return into
<code>buildOvertureRequestBody</code>, and assert the <b>emitted body's</b>
<code>motive.enum</code> array has nine members including the new key — the artifact
under test is the request body, not <code>motiveKeys</code> in isolation.
Spec test 10: no <code>catalog.id</code>, entity id, or <code>tier</code> value string
appears in the body.
</blockquote>

### Step 10 — `buildWakeRequestBody` and the four wake tools
**Requirements:** REQ-BARD-SEL-13, -14, -15. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 9. Four tools: `write_focus {text}`, `write_journal {text}`,
`append_catalog {entry}`, `mark_seeded {handle}`. Small tools, not one large one, so a
partial response is still useful — a wake that writes focus but fails to journal has
still influenced the world.

`append_catalog`'s `entry` is **one element of** Step 9's `entries` array — an object
of `kind`/`handle`/`name`/`blurb`/`motive`/`tier`/optional `fact` — **not** the whole
`write_catalog` input. It carries no `entries` array and no `journal`. Factor the
entry schema into one file-local builder shared by both bodies so the two cannot drift;
that shared builder is the requirement's real enforcement.

`tool_choice` is `auto`, `max_tokens` back to 1024 (a wake writes a line and a
paragraph, not a cast).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testBardSelRequestBody</code> gains: the wake body
carries exactly four tools with exactly those names; <code>append_catalog</code>'s
input schema has <b>no</b> <code>entries</code> and <b>no</b> <code>journal</code> key,
and its <code>entry</code> object's <code>required</code> list is byte-equal to the
overture entry schema's (assert by comparing the two serialized sub-objects — this is
what catches drift); <code>tool_choice.type == "auto"</code>;
<code>grep -n "place_catalog" src/</code> is empty (spec mechanical check 3, encoded in
Step 14).
</blockquote>

### Step 11 — `validateOvertureResponse`: the pure gate, lenient per entry
**Requirements:** REQ-BARD-SEL-16, -17, -18. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

```cpp
struct CatalogEntryProposal {
    std::string kind, handle, name, blurb, motive;
    int64_t tier = 0;
    std::string factArchetype, factElement;  // both empty or both set
};
struct OvertureProposal {
    std::vector<CatalogEntryProposal> entries;
    std::string journal;
};
std::optional<OvertureProposal> validateOvertureResponse(const HttpResponse& response);
```

Pure function of the `HttpResponse`: no DB, **never throws**, one stderr diagnostic
naming the first failed clause — `validateRoomProposal` (`architect.cpp:266-300`) is
the shape, including `json::parse(..., allow_exceptions=false)` and scanning
`content[]` for the tool_use block by name rather than by position.

**Strict per response (REQ-BARD-SEL-18)** — reject in full, returning `nullopt`, only
when: status ≠ 200 (a transport error carries status 0, so it fails here), the body is
unparseable or not an object, there is no `content` array, or there is no
`write_catalog` tool_use block. A rejected overture yields an empty catalog, which is
a supported state everywhere.

**Lenient per entry (REQ-BARD-SEL-17)** — drop the entry, one diagnostic, keep the
rest, when: `handle`/`name`/`blurb` is empty after trim; `kind` or `motive` is outside
its vocabulary; `tier` is not a non-negative integer; or `fact` carries one field
without the other. **Dropping never rejects the response** — rejecting a whole overture
over one malformed entry would leave the game with no story at all, strictly worse than
a story with nine entries instead of ten.

The `motive` vocabulary check needs the eight keys without a DB. Pass them in
(`validateOvertureResponse(response, motives)`) rather than hardcoding — the enum is
data-driven at the schema, and a hardcoded copy in the gate would be the exact drift
REQ-BARD-SEL-12 exists to prevent. An empty `motives` argument means "vocabulary
unchecked here," and the truth of it is then enforced by `writeCatalogEntry`'s throw at
admission.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelGateOverture</code> (spec tests 12, 13),
modeled on <code>testArchitectGate</code> (tests.cpp:4397). Lenient: a canned response
with three entries, one malformed, yields <b>two</b> entries and does not return
nullopt — repeat once per REQ-BARD-SEL-17 failure mode (blank handle, blank name, blank
blurb, bad kind, bad motive, negative tier, non-integer tier, fact with archetype only,
fact with element only). Strict: non-200, transport-error, unparseable body, JSON array
instead of object, missing content, and a response whose only tool_use is some other
tool each yield <code>nullopt</code>. <b>The gate never throws on any of these</b> —
call every case outside a try/catch, as the architect's gate tests do.
</blockquote>

### Step 12 — `validateWakeResponse`
**Requirements:** REQ-BARD-SEL-16, -14, -20. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Step 11 (it reuses the per-entry validator verbatim).

```cpp
struct WakeProposal {
    bool hasFocus = false;   std::string focus;
    bool hasJournal = false; std::string journal;
    std::vector<CatalogEntryProposal> appended;  // append_catalog, 0..n
    std::vector<std::string> seededHandles;      // mark_seeded, 0..n
};
std::optional<WakeProposal> validateWakeResponse(const HttpResponse& response,
                                                 const std::vector<std::string>& motives);
```

Scan `content[]` for **all four** tool names, accumulating. The `has*` flags exist
because "wrote an empty focus" and "did not call write_focus" are different facts and
admission treats them differently.

Only the transport/parse clauses reject (status ≠ 200, unparseable, no content array).
**A response with zero tool calls is a successful, empty wake** (REQ-BARD-SEL-14) — it
returns an empty `WakeProposal`, not `nullopt`, and emits **no** diagnostic claiming
failure. The bard declining to act is normal; treating it as an error is the single
most likely misimplementation in this step.

An `append_catalog` entry that fails the per-entry checks is dropped with a diagnostic,
like the overture's. A `mark_seeded` with a blank handle is dropped here; an
*unknown* handle is not detectable without a DB and is handled at admission
(REQ-BARD-SEL-20, Step 13).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelGateWake</code> (spec test 15): a canned
response with all four tools populates all four fields; one with only
<code>write_journal</code> sets <code>hasJournal</code> and leaves
<code>hasFocus</code> false; <b>a canned response with no tool call at all returns a
WakeProposal (not nullopt) with every field empty</b>, and stderr carries no failure
line — capture stderr for this one assertion, the way the existing gate tests check
diagnostics. Two <code>append_catalog</code> calls in one response accumulate to two
entries; a malformed one of the two drops to one. Non-200 and unparseable yield
nullopt without throwing.
</blockquote>

### Step 13 — Admission: the only step that writes
**Requirements:** REQ-BARD-SEL-19, -20, -24, -21. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on Steps 5, 11, 12. Two functions (micro-decision 6), neither beginning,
committing, nor rolling back — Brick 3 owns the transaction (REQ-BARD-WAKE-7).

First, the shared pre-flight from micro-decision 6a — one read-only function, used by
both:

```cpp
// "" if this entry would be admitted, else the reason it is refused. Mirrors every
// refusal writeCatalogEntry makes (mutations.hpp:180-206) plus handle uniqueness,
// so the helper is only ever called with arguments it cannot refuse — and any throw
// that DOES escape is a genuine fault the caller must roll back on. SELECTs only.
std::string catalogEntryRefusal(Db& db, const CatalogEntryProposal& entry,
                                const std::set<std::string>& admittedThisCall);
```

**13a — `admitOvertureProposal(db, proposal) -> int`.** For each entry: run
`catalogEntryRefusal`; on a non-empty reason, emit one diagnostic naming the handle and
the reason and **continue with the remaining entries** — a `fact` that fails the truth
gate drops the **entire entry**, not merely the fact, because a knowledge beat whose
knowledge is false has no remaining purpose (REQ-BARD-SEL-19). Otherwise call
`writeCatalogEntry` **outside any catch**, and record the handle as admitted. Then
`writeBardJournal(db, proposal.journal)`. Return the number admitted.

No try/catch appears anywhere in either admission function — see micro-decision 6a for
why a catch here would be actively wrong.

**13b — `applyWakeProposal(db, proposal) -> int`.** In order: `writeBardFocus` if
`hasFocus`; `writeBardJournal` if `hasJournal`; each appended entry through the same
pre-flight-then-`writeCatalogEntry` path as 13a; each `mark_seeded` handle resolved
by **`catalogIdForHandle`** (REQ-BARD-SEL-24 — room-free and gate-free, *not*
`catalogForHandle`) and, when non-zero, passed to `markCatalogSeeded`. A zero
resolution is ignored with one diagnostic and **the rest of the wake still applies**
(REQ-BARD-SEL-20).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelAdmit</code> (spec tests 12, 14):
a three-entry overture whose middle entry asserts a weakness contradicted by
<code>resistance</code> (reuse Brick 1's <code>('goblin_grunt','fire')</code> neutral
case) writes <b>exactly two</b> catalog rows — <b>zero</b> rows for the false entry,
not a row minus its fact — and the journal is still written. <b>The equivalence
guard</b> (micro-decision 6a's anti-drift fence): for a table covering every refusal
— bad kind, unknown motive, blank handle/name/blurb, negative tier, fact with one
field, each of the three truth-gate clauses, and a duplicate handle —
<code>catalogEntryRefusal</code> returns non-empty <b>iff</b> a direct
<code>writeCatalogEntry</code> with the same arguments throws (drive both from one
loop; for the duplicate-handle case, write the first entry, then assert both paths
refuse the second). <b>Duplicate handles within one overture</b>: two entries sharing
a handle admit exactly one, and admission <b>does not throw</b> — the case that would
otherwise surface as a <code>UNIQUE</code> violation and, under a catch, be
indistinguishable from a disk fault. A wake with a
<code>mark_seeded</code> naming an unknown handle leaves <code>seeded</code> untouched
while its <code>write_focus</code> and <code>append_catalog</code> both land. A
<code>mark_seeded</code> naming an entry that is <b>ineligible everywhere</b> (tier
above any reachable room) <b>does</b> set <code>seeded = 1</code> — the assertion that
catches <code>catalogForHandle</code> being used here. Wrap one admission in
<code>db.begin()</code>/<code>db.rollback()</code> and assert zero rows survive, the
Brick-1 transaction shape — this is also the shape Brick 3 relies on for
REQ-BARD-WAKE-7. Finally, <code>grep -n "catch" src/bard.cpp</code> finds nothing in
either admission function (micro-decision 6a).
</blockquote>

### Step 14 — The unit-contract guards, encoded as tests
**Requirements:** REQ-BARD-SEL-21, -15, -7, -23. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Depends on every prior step — it inspects the source they wrote. Encode the spec's
greps as source-text assertions via `readFileBytes` (tests.cpp:135), the
`testCombatFinalSweep` precedent, so they survive as regression guards instead of being
run once by hand:

- **REQ-BARD-SEL-21:** `src/bard.cpp` contains no `INSERT`, `UPDATE`, or `DELETE`
  (mechanical check 2).
- **REQ-BARD-SEL-15:** no file under `src/` contains `place_catalog` (mechanical
  check 3).
- **REQ-BARD-SEL-7:** `src/bard.cpp` references no `unlock` column and `src/world.cpp`
  declares none — the storylet line, asserted rather than remembered.
- **REQ-BARD-SEL-23:** both prompts contain the urgency-prohibition clause (mechanical
  check 4) — assert here as well as in Step 8, since this is the sweep a future
  reworder will run.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> new <code>testBardSelContract</code> holds all four and
passes. Then run the spec's greps <b>by hand once</b> and confirm they agree with the
encoded tests — a guard that disagrees with the grep it encodes is worse than no guard.
</blockquote>

### Step 15 — Final validation against the spec
**Requirements:** all. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Walk the spec's **AI Validation** section top to bottom (4 mechanical, 15 behavioral)
and confirm each item, then walk the coverage map below and confirm every requirement
has a passing assertion.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> (1) <code>cmake --build build</code> clean and the
<b>whole</b> suite green — zero regressions in combat, architect, pregen, band, or the
Brick-1 <code>testBardStore*</code> block, which the combat export in Step 1 is the
only plausible cause of; (2) every new <code>testBardSel*</code> is registered in
<code>main()</code> beside the <code>testBardStore*</code> calls (tests.cpp:8549-8556);
(3) the four mechanical greps return what the spec says; (4) the game still launches
and plays several turns unchanged — <b>this brick is invisible at runtime</b>, since
nothing calls into <code>bard.cpp</code> yet, and any user-visible change means scope
leaked in from Brick 3 or 4; (5) <code>git diff --stat</code> touches only
<code>src/bard.*</code>, <code>src/combat.*</code>, <code>CMakeLists.txt</code>, and
<code>tests/tests.cpp</code>.
</blockquote>

---

## Coverage map

| Requirement | Step | Spec test |
|---|---|---|
| REQ-BARD-SEL-1 — `eligibleCatalog` / `CatalogChoice` | 2 | 5 |
| REQ-BARD-SEL-2a/b/c — unmaterialized, tier, kind | 2 | 5 |
| REQ-BARD-SEL-2d — knowledge beats need a live subject | 3 | 6 |
| REQ-BARD-SEL-3 — gate (d) calls `eligibleArchetypes` | 1, 3 | 6 |
| REQ-BARD-SEL-4 — id order; empty is normal | 2 | 7, 8 |
| REQ-BARD-SEL-5 — `eligibleCatalogForNewRoom` | 4 | — (new) |
| REQ-BARD-SEL-6 — `catalogForHandle` live re-check | 5 | 9 |
| REQ-BARD-SEL-7 — no per-entry unlock condition | 2, 14 | — (guard) |
| REQ-BARD-SEL-8 — no ids/tier/seeded on the wire | 6, 7, 9 | 10 |
| REQ-BARD-SEL-9 — `buildOvertureContext` + motive blurbs | 6 | 11a |
| REQ-BARD-SEL-10 — `buildWakeContext`'s five parts | 7 | 10, 11a |
| REQ-BARD-SEL-11 — `write_catalog` schema | 9 | — (body test) |
| REQ-BARD-SEL-12 — `kind` / `motive` enums, DB-driven | 9 | 11 |
| REQ-BARD-SEL-13 — four wake tools; `entry` ≠ `entries` | 10 | — (body test) |
| REQ-BARD-SEL-14 — `tool_choice: auto` both shapes | 9, 10, 12 | 15 |
| REQ-BARD-SEL-15 — no `place_catalog` | 10, 14 | mech. 3 |
| REQ-BARD-SEL-16 — pure gates, never throw | 11, 12 | 13 |
| REQ-BARD-SEL-17 — lenient per entry | 11 | 12 |
| REQ-BARD-SEL-18 — strict per response | 11 | 13 |
| REQ-BARD-SEL-19 — a false fact drops its whole entry | 13a | 14 |
| REQ-BARD-SEL-20 — unknown `mark_seeded` handle ignored | 13b | — (new) |
| REQ-BARD-SEL-21 — `bard.cpp` is read-only | 1, 14 | mech. 2 |
| REQ-BARD-SEL-22 — prompts are versioned constants | 8 | — (prompt test) |
| REQ-BARD-SEL-23 — situations, not urgency | 8, 14 | mech. 4 |
| REQ-BARD-SEL-24 — `catalogIdForHandle` is room-free | 5, 13b | 11b |
| *(micro-decision 7)* — `kBardWakeEventLimit` | 7 | — (new) |

## Explicitly not built here

- **Any transport, thread, or `main.cpp` change** — the overture call, the 60 s
  timeout, `BardGuard`, coalescing, and `meta.bard_last_wake_turn` **writes** all
  belong to [specs/bard-overture-and-scheduling.md](../specs/bard-overture-and-scheduling.md).
  This brick only *reads* that meta row. If implementation reaches for an
  `HttpTransport`, the brick has been scoped wrong.
- **`buildArchitectContext` carrying `bard_focus` or the catalog menu**, the `story`
  field on `create_room`, and `placeCatalogEntry` call sites — owned by
  [specs/bard-architect-integration.md](../specs/bard-architect-integration.md). Step 4
  builds the menu function that spec will call; it wires it to nothing.
- **Prompt quality tuning** — structure is pinned by substring here; wording is judged
  against real output in Brick 3.
- **`catalog_binding`, `catalog.pending`, the L1 sketch lane** — still deferred, as in
  Brick 1.
- **Placement into already-explored rooms** — deferred by design 4.

## Notes for whoever implements this

- **Every new test opens `tests/combat_fixture.sql`.** It is the only fixture carrying
  `bestiary`, `spell_catalog`, `resistance`, and the eight `motive_catalog` rows — gate
  (d) and the truth gate both read them.
- **Seed catalog rows through `writeCatalogEntry`, never raw SQL**, so the tests
  exercise the admission path Brick 1 shipped and cannot drift from it.
- **The combat export in Step 1 is the one place a regression can hide.** If the
  existing suite goes red after it, the move changed behavior — revert and redo it as a
  pure declaration move with the bodies untouched.
- **No CMake change beyond the one source line**, no schema change, no
  `SCHEMA_VERSION` bump, no `world.db` delete needed.
- **`README.md` needs no change** — this brick adds no player-visible surface.
- **One spec sentence to amend on approval** (micro-decision 6b): REQ-BARD-SEL-19's
  clause naming *"catching the helper's throw at the call site"* as the mechanism. The
  requirement's outcome is implemented exactly and its test passes verbatim; only the
  mechanism sentence is superseded, by the pre-flight that keeps
  REQ-BARD-WAKE-7/-23's rollback guarantee true. No other requirement in any bard spec
  moves.
- The eight motives are **approved as final** (author, 2026-08-03), so the enum and the
  blurbs may be treated as settled; the enum stays data-driven regardless, so a later
  change remains a seed edit plus a world-file delete rather than a code change.
