---
title: "Implementation plan: bard-architect-integration"
date: 2026-08-04
status: executed
tags: [plan, bard, architect, materialization, world-gen, nouns-must-exist, catalog, pregen, non-regression]
modules: [architect, bard, mutations, pregen]
related: [.lore/work/specs/bard-architect-integration.md, .lore/work/design/bard-architect-integration.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/bard-catalog-selection.md, .lore/work/specs/bard-overture-and-scheduling.md, .lore/work/specs/background-room-pregeneration.md]
---

# Implementation plan: bard-architect-integration

Brick 4 of 4. The seam where catalog intent becomes world state.
Source of truth: **[.lore/work/specs/bard-architect-integration.md]** (17
requirements, prefix `BARD-ARCH`; 17 AI-validation items).

**This is the only bard brick that modifies existing, working code.** Everything
it consumes was already built by bricks 1–3 and is currently **dead** — the plan
is almost entirely wiring, and its first obligation is not regressing the
architect.

## What already exists (verified in tree)

Every producer this brick needs shipped in bricks 1–3, and three of them have
**zero production call sites** today:

| Seam | File:line | Call sites now | What brick 4 does with it |
|---|---|---|---|
| `eligibleCatalogForNewRoom(db, originRoom)` | `src/bard.cpp:936` | **0** | The prospective-room menu — REQ-BARD-ARCH-2 |
| `catalogForHandle(db, room, handle)` | `src/bard.cpp:921` | **0** | The live commit-time re-check — REQ-BARD-ARCH-11 |
| `placeCatalogEntry(db, catalog, room, description, actor)` | `src/mutations.hpp:232` | **0** | Mint + latch + `materialized` event — REQ-BARD-ARCH-12 |
| `CatalogChoice { handle, blurb, motiveBlurb }` | `src/bard.hpp:35` | context builders | The menu's model-facing shape (no id, no tier) |
| `meta.bard_focus` + `writeBardFocus` | `src/world.cpp:156`, `src/mutations.cpp:739` | bard wake | The one line the architect reads — REQ-BARD-ARCH-1 |
| `catalog.name` | `src/world.cpp:58-`, `writeCatalogEntry` | overture/wake | The minted entity's parser noun |

Both design-1 amendments (`bard_focus` split from the private journal;
`catalog.name` added) and the design-2 amendment (`place_catalog` removed) are
already applied in shipped code — `grep -rn "place_catalog" src/` is empty.

## Seams this brick edits

| Seam | File:line | Change |
|---|---|---|
| `RoomProposal` | `src/architect.hpp:27` | Gains `StoryProposal story` — Step 1 |
| `buildArchitectContext` | `src/architect.cpp:165` | Gains `focus` + `story_options`, 4th param — Step 2 |
| its doc comment ("O(1) in world size") | `src/architect.hpp:57` | Retired — Step 2, REQ-BARD-ARCH-3 |
| `buildArchitectRequestBody` | `src/architect.cpp:179` | Gains optional `story` field — Step 3 |
| `kArchitectPrompt` | `src/architect.cpp:146` | Gains the story clause — Step 4 |
| `validateRoomProposal` | `src/architect.cpp:387-400` | Lenient `story` extraction after the enemy block — Step 5 |
| `architectCommitProposal` | `src/architect.cpp:473-479` | Story placement after enemy placement — Step 6 |
| `architectGenerate` / `architectProposeRoom` | `src/architect.cpp:420`, `:447` | Thread the menu — Step 7 |
| `PregenJob` | `src/pregen.hpp:49` | Gains `storyHandles` — Step 8 |
| `architectQueuePregen` | `src/architect.cpp:509-527` | Hoist the menu out of the loop — Step 8 |
| worker call | `src/pregen.cpp:106` | Pass `job.storyHandles` — Step 8 |

**No new translation unit, no `CMakeLists.txt` change, no DDL.** `architect.hpp`
gains `#include "bard.hpp"` for `CatalogChoice`; that is acyclic (`bard.hpp`
includes only `db.hpp` + `prose.hpp`; `pregen.hpp` already includes
`architect.hpp`).

## Micro-decisions

**Resolved with the author, before drafting:**

1. **The menu is hoisted, not read inside the builder.** `buildArchitectContext`
   gains a 4th parameter `const std::vector<CatalogChoice>& menu = {}`, and the
   caller reads `eligibleCatalogForNewRoom` **once** and passes it to both the
   context builder and the request-body builder — exactly how `enemyBlurbs` is
   passed today. Three reasons: `architectQueuePregen` calls the context builder
   once **per latent direction** (`src/architect.cpp:512-527`) while the menu is
   per-room, so reading it inside would cost one BFS + neighborhood read per
   direction where the existing code already hoists `eligibleEnemyBlurbs`
   (`:508`); the context blurbs and the schema enum then agree **by
   construction**; and the pregen job needs the handle vector to cross the
   thread anyway. The defaulted parameter keeps all existing call sites and
   tests compiling untouched.
   - *Consequence:* "the enum is the **prospective** menu, not the origin's"
     (spec test 10) is now a property of the **call site**, not the builder. Its
     test must therefore drive `architectGenerate` end-to-end with a
     body-capturing fake transport, not `buildArchitectRequestBody` in
     isolation. Step 7 owns it.

2. **The commit-time re-check is `catalogForHandle(db, newRoom, handle)`** — the
   spec's literal REQ-BARD-ARCH-11, lighting up the shipped API. The room exists
   by then (`writeGeneratedRoom` already ran), so its seed distance is the
   origin's plus one and the tier gate matches what was offered. Where it
   *differs* from the design's origin-menu phrasing is gate (d), the
   knowledge-beat rule: `newRoom` evaluates against the room's real
   neighborhood, **including the enemy placed two lines above**. That is
   strictly more correct — a beat about the goblin is admissible in the room
   with the goblin.

3. **`kArchitectPrompt` gains a story clause.** The spec has no prompt
   requirement, but `enemy` has a dedicated clause and design Decision 2 ("the
   catalog supplies *who*; the architect supplies *how it looks here*") has no
   home unless the prompt states it. Purely additive — every substring
   `testArchitectPrompt` asserts (`tests/tests.cpp:6469`) survives verbatim.

4. **Spec test 17 is descoped to what brick 4 can own** — see
   [What this plan deliberately does not do](#what-this-plan-deliberately-does-not-do).

**Decided here, flagged for review:**

5. **Context keys are `focus` and `story_options`**, and both are **omitted
   entirely when empty**, never present-and-empty. Forced, not chosen:
   `testArchitectContext` asserts `j.size() == 4` exactly
   (`tests/tests.cpp:4291`), and REQ-BARD-ARCH-4/-15 require the all-empty
   payload to be equivalent to today's. Omission makes the byte-identity claim
   free rather than argued.

6. **`story` is an empty-handle sentinel struct, not `std::optional`** —
   mirroring `RoomProposal::enemyBlurb`'s `""`-means-none precedent
   (`src/architect.hpp:37`). REQ-BARD-ARCH-9 drops any story missing either
   field, so a surviving story always has both non-empty and the sentinel is
   unambiguous.

7. **The pregen job snapshots handles only** (`std::vector<std::string>`), not
   the full `CatalogChoice` vector. The blurbs are already inside
   `job.contextPayload`; the worker needs the handles solely to build the
   schema enum. Carrying both would put the same strings on the job twice and
   invite them to disagree.

8. **Every test is a NEW function; no existing test is edited.** This is what
   makes the spec's mechanical check 2 auditable rather than asserted — after
   this brick, `git diff tests/tests.cpp` must show **additions only** in the
   architect/pregen/combat regions. If a step finds itself wanting to edit an
   existing test, that is the regression signal, and it stops the step.

9. **Factor the duplicated trim.** `validateRoomProposal`'s enemy block inlines
   a trim lambda (`src/architect.cpp:392-397`); the story block needs the same
   thing twice more. Lift it to a file-local `trimmed(const std::string&)` in
   the anonymous namespace and have the enemy block use it. Behavior-preserving,
   fenced by the existing `testArchitectGate` passing unmodified. If it does not
   pass unmodified, revert the factoring and duplicate.

---

## Step sequence & dependencies

<div style="font-family: ui-monospace, monospace; line-height: 1.6; padding: 8px 0;">
<span style="color:#666;">&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;┌────── any order, four disjoint functions ──────┐</span><br>
<b>1</b> StoryProposal + include ─┬─▶ <b>2</b> context: focus + menu ────────────────┐<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>3</b> story tool schema ──────────────────┤<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>5</b> lenient story gate ─▶ <b>6</b> commit ──┼─▶ <b>7</b> sync wiring ─┬─▶ <b>8</b> pregen path<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>4</b> prompt clause ─────────────────────┘&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;├─▶ <b>9</b> nouns-must-exist<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;└─▶ <b>10</b> final audit<br>
</div>

**The arrows out of Step 1 are a fan-out, not a chain.** Steps 2, 3, 4 and 5 are
mutually independent — they edit four disjoint functions in `architect.cpp`
(`buildArchitectContext`, `buildArchitectRequestBody`, `kArchitectPrompt`,
`validateRoomProposal`) and depend on nothing but Step 1's type. The only real
edge inside that group is **5 → 6**: commit places what the gate extracted. The
listed order is data-flow order (what the model sees → what it may say → what we
accept → what we write) because that is the order the spec reads in, not because
one blocks the next.

Risk legend: <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span> deterministic, offline, mechanically verified.

**Every step in this brick is LOW token-risk.** The spec requires no live-LLM
verification at all — its 4 mechanical checks are a build, a suite run, and two
greps, and all 17 validation items run offline against canned responses. Steps
1–6 are testable before any call site changes; steps 7–8 are the wiring. Nothing
here is a tune-and-retry loop.

Steps 7 and 8 are the wiring, and are the only steps that must come last: they
are what turn four independently-tested functions into one path.

---

### Step 1 — `StoryProposal` on `RoomProposal`
**Requirements:** REQ-BARD-ARCH-5 (shape), REQ-BARD-ARCH-17. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Skeleton only, no behavior. In **`src/architect.hpp`**: add
`#include "bard.hpp"` (for `CatalogChoice`, used by Step 2's signature), then
beside `RoomProposal`:

```cpp
// The story entry the model optionally selected (REQ-BARD-ARCH-5): the catalog
// HANDLE it chose from the schema-enforced enum, and the instance prose it wrote
// for THIS room. Empty handle = no story, mirroring enemyBlurb's ""-means-none.
// Extracted LENIENTLY at the gate (REQ-BARD-ARCH-8); the engine re-checks the
// handle against the LIVE menu before placing, so a hallucinated or stale one
// places nothing. Carries no id — the handle and the prose are the whole of it
// (REQ-BARD-ARCH-17).
struct StoryProposal {
    std::string handle;
    std::string description;
};
```

and `StoryProposal story;` as a `RoomProposal` member, after `enemyBlurb`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>cmake --build build</code> clean and
<code>./build/tests</code> fully green, <b>with no test file edited</b>. A
default-constructed <code>RoomProposal</code> has an empty story, so every one of
the ~40 existing sites that brace-initializes one is unaffected. No new test —
this step adds no behavior to test.
</blockquote>

### Step 2 — Context: `focus` + `story_options`, and the retired O(1) claim
**Requirements:** REQ-BARD-ARCH-1, -2, -3, -4. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In **`src/architect.cpp`**: add a file-local `bardFocusText(Db&)` beside
`settingText` (`:88`) reading `meta.bard_focus`, returning `""` if absent. Change
`buildArchitectContext` to take a 4th parameter
`const std::vector<CatalogChoice>& menu` (declared `= {}` in the header) and,
**after** the four existing keys:

```cpp
// Both keys are OMITTED when empty, never present-and-empty (REQ-BARD-ARCH-4):
// with no catalog and no focus the payload is EXACTLY today's four fields, which
// is what makes REQ-BARD-ARCH-15's non-regression claim free rather than argued.
const std::string focus = bardFocusText(db);
if (!focus.empty()) payload["focus"] = focus;
if (!menu.empty()) {
    json options = json::array();
    for (const CatalogChoice& c : menu) {
        options.push_back({{"handle", c.handle},
                           {"blurb", c.blurb},
                           {"motive", c.motiveBlurb}});
    }
    payload["story_options"] = std::move(options);
}
```

No id, no `catalog.id`, no `tier`, no `seeded` — `CatalogChoice` carries none of
them by construction (`src/bard.hpp:35`), and `motive` carries the motive
**blurb**, never the key.

In **`src/architect.hpp`**: rewrite the `buildArchitectContext` doc comment. It
currently claims *"Nothing else — no ids, no neighborhood, no history. O(1) in
world size"* (`:56-57`). Replace with the two new fields, the omit-when-empty
rule, and the honest cost — **O(eligible catalog), and its `distanceFromSeed` BFS
is O(world)** — plus the note that the menu is supplied by the caller, not read
here, and why (micro-decision 1). Add the prompt-caching observation from design
Decision 4 as a comment, not as work.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectContext</code>
(<code>tests.cpp:4278</code>) passes <b>unmodified</b> — its
<code>j.size() == 4</code> assertion on a catalog-free fixture world <b>is</b>
spec test 5's byte-identity proof, which is why omit-when-empty is not optional.
New <code>testArchitectStoryContext</code>: seed a catalog and a focus, then
assert (a) <code>story_options</code> lists exactly the eligible handles with
their blurbs and <b>motive blurbs</b>, in <code>catalog.id</code> order;
(b) <code>focus</code> is verbatim; (c) <code>checkNoIdKeys</code> passes and the
dumped string contains no <code>tier</code>, no <code>catalog</code> id, and no
entity id in string form (spec test 9); (d) empty catalog + non-empty focus, and
non-empty catalog + empty focus, each yield 5 keys, and both empty yields 4
(REQ-BARD-ARCH-4). Mechanical check 4: <code>grep -n "O(1) in world size"
src/architect.hpp</code> is empty.
</blockquote>

### Step 3 — The optional `story` tool field
**Requirements:** REQ-BARD-ARCH-5, -6, -7. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`buildArchitectRequestBody` gains
`const std::vector<std::string>& storyHandles = {}` after `enemyBlurbs`, and —
mirroring the `enemy` block at `src/architect.cpp:219-227` — when it is non-empty:

```cpp
properties["story"] = {
    {"type", "object"},
    {"properties",
     {{"handle", {{"type", "string"}, {"enum", storyHandles},
                  {"description", "..."}}},
      {"description", {{"type", "string"},
                       {"description", "How this entry appears in THIS room."}}}}},
    {"required", json::array({"handle", "description"})},
    {"description",
     "Optional. One story entry to bring into this room, or omit for none."}};
```

`story` stays out of the tool's **top-level** `required` array (`:234`), so at
most one and possibly none (REQ-BARD-ARCH-7). Empty handles → the key is never
constructed, so the body is byte-identical to a no-bard one (REQ-BARD-ARCH-6).
`enemy` and `story` are written independently, so a room may carry both.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectRequestBody</code>
(<code>tests.cpp:4329</code>) passes <b>unmodified</b> — including its
<code>schema["properties"].size() == 3</code> assertion, which holds because the
default is empty. New <code>testArchitectStoryRequestBody</code>: with handles
supplied, <code>story.properties.handle.enum</code> equals the handle vector
exactly, <code>story.required</code> is <code>[handle, description]</code>,
top-level <code>required</code> is still <code>[name, description]</code>, and
<code>properties.size() == 4</code>; with enemy blurbs AND story handles both
supplied, <code>properties.size() == 5</code> (REQ-BARD-ARCH-7); with empty
handles, <code>body.find("story") == std::string::npos</code> — <b>substring
absence</b>, exactly as spec test 6 words it.
</blockquote>

### Step 4 — The story clause in `kArchitectPrompt`
**Requirements:** design Decision 2 (no spec requirement — micro-decision 3). **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Append one bullet to the "Rules, absolute" block, after the existing `enemy`
clause (`src/architect.cpp:163`), in its exact voice:

> - If (and only if) the create_room tool offers a "story" field, you may bring
>   one of the entries listed there into this room: set story.handle to exactly
>   one of its listed values, and story.description to how that entry appears
>   HERE, in this room's own words - or omit story entirely to leave the room
>   without one. Never invent a handle; the entries offered are the only ones.
>   The listed blurb tells you who or what the entry is; it is not the prose - you
>   write the prose. When you bring one in, let it show in the room description.

The last two sentences are the whole of design Decision 2, and the reason this
step exists: without them the model has a schema field and no statement that the
blurb is selection text rather than room text.

**This scopes REQ-BARD-ARCH-6's "byte-identical" claim, and the plan says so
rather than letting it slide.** `kArchitectPrompt` is one global constant that
`buildArchitectRequestBody` sets unconditionally (`src/architect.cpp:240`), so
after this step an empty-catalog body carries the story instructions in its
`system` field regardless. Byte-identity therefore holds for the **context and
the tool schema** — the two things the menu actually gates — and not for the
static prompt. That is precedent, not drift: the `enemy` clause has been
unconditionally baked into the same constant since the combat brick
(`src/architect.cpp:158-163`), and no test breaks, because both
`testArchitectRequestBody` (`:4350`) and `testArchitectPrompt` (`:6470`) compare
against the live constant rather than a hardcoded string.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectPrompt</code>
(<code>tests.cpp:6469</code>) passes <b>unmodified</b> — every substring it
asserts, and its one negative assertion, is untouched by an append. New
<code>testArchitectStoryPrompt</code> spot-checks the clause by substring:
<code>"story"</code>, <code>"exactly one of its listed values"</code>,
<code>"Never invent a handle"</code>, <code>"it is not the prose"</code>. Prompt
STRUCTURE only — quality is not verified here, and this step must never become a
live retry loop.
</blockquote>

### Step 5 — Lenient `story` extraction at the gate
**Requirements:** REQ-BARD-ARCH-8, -9, -17. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

First, micro-decision 9: lift the enemy block's inline trim lambda
(`src/architect.cpp:392-397`) into a file-local `trimmed()` and have the enemy
block call it. Then, immediately after the enemy block, extract `story` in the
manner of `exits` — **drop, never reject**:

```cpp
// --- story selection (REQ-BARD-ARCH-8): LENIENT, like exits and enemy. `name`
// and `description` remain the ONLY strict clauses. A malformed, empty, or
// absent story is dropped with ONE diagnostic and NEVER rejects the room, and
// a story missing EITHER field is dropped IN FULL (REQ-BARD-ARCH-9) — an entry
// without instance prose has nothing to write into the world. No id is read.
```

Drop, with one `fprintf` naming the reason, on **eight** conditions: `story` is
absent (silently — absence is the common case, not a fault); is not an object;
`handle` is missing, is not a string, or is blank after trim; `description` is
missing, is not a string, or is blank after trim. Otherwise store both trimmed.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectGate</code>
(<code>tests.cpp:4437</code>) passes <b>unmodified</b> — this is spec test 7, and
it is also the fence on the trim factoring. New
<code>testArchitectStoryGate</code> over canned responses: a well-formed story
survives with both fields trimmed; each of the seven drop reasons above yields a
proposal that is <b>non-null</b> with an empty <code>story.handle</code> and the
room's <code>name</code>/<code>description</code>/<code>exits</code> intact
(REQ-BARD-ARCH-8, spec test 15); a story carrying a spurious <code>id</code> or
<code>catalog</code> field has it ignored, not read (REQ-BARD-ARCH-17); and a
story present alongside an <code>enemy</code> leaves both extracted. That is
<b>eight</b> arms — absent, not-an-object, and three each for
<code>handle</code> and <code>description</code> — plus the three positive ones.
</blockquote>

### Step 6 — Materialization in `architectCommitProposal`
**Requirements:** REQ-BARD-ARCH-10, -11, -12, -13, -16. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

In `architectCommitProposal`, **after** the enemy block's closing brace
(`src/architect.cpp:478`) and **before** the `return newRoom;` at `:479`:

```cpp
// Story placement (REQ-BARD-ARCH-10), beside the enemy and with the same shape:
// re-check, place, record. The menu is re-read LIVE against the room that now
// EXISTS (REQ-BARD-ARCH-11) — a pregen candidate may have been snapshotted many
// turns before it commits, and an entry materialized elsewhere since must place
// nothing. An unknown, stale, or already-materialized handle resolves to 0; the
// room is still created normally. placeCatalogEntry writes the ARCHITECT's
// instance prose as the description; the catalog supplies only the name
// (REQ-BARD-ARCH-12). The blurb is never written into the world.
const int64_t entry = catalogForHandle(db, newRoom, proposal.story.handle);
if (entry != 0) {
    placeCatalogEntry(db, entry, newRoom, proposal.story.description, actor);
}
```

`catalogForHandle` already returns 0 on an empty handle (`src/bard.cpp:923`), so
the no-story case needs no guard. This sits in Phase 2, **outside the Phase-1
catch** by construction (REQ-BARD-ARCH-13) — a DB fault propagates to the tick's
rollback rather than being downgraded to a wall. Update the
`architectCommitProposal` doc comment (`src/architect.hpp:162-176`) to name the
third half. `architect.cpp` writes nothing itself: `placeCatalogEntry` is the
`mutations.cpp` helper (REQ-BARD-ARCH-16).

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectCommitProposal</code>
(<code>tests.cpp:5498</code>) and <code>testArchitectSpawn</code>
(<code>:5539</code>) pass <b>unmodified</b>. <code>grep -En
"INSERT|UPDATE|DELETE" src/architect.cpp</code> is <b>empty</b> (mechanical check
3, REQ-BARD-ARCH-16). New <code>testArchitectStoryPlacement</code>, one arm per
spec test: <b>11</b> — an eligible handle mints an entity whose <code>name</code>
is <code>catalog.name</code> and whose <code>description</code> is the canned
story prose, <b>explicitly asserted not equal to the blurb</b>, located in the
new room, with <code>catalog.entity</code> latched and <b>exactly one</b>
<code>materialized</code> event; <b>12</b> — an off-menu handle creates the room
and places nothing (no entity, no event, <code>catalog.entity</code> still NULL);
<b>13</b> — a handle materialized between snapshot and commit resolves to 0, the
room is created, and no second entity is minted; <b>14</b> — a proposal carrying
both an enemy and a story places both; <b>15</b> — a malformed story still
creates the room (inherited from Step 5, re-asserted end-to-end here).<br>
<b>Plus one arm the spec does not list, for REQ-BARD-ARCH-13.</b> Nothing in the
spec's 17 items would fail if story placement were moved <i>inside</i>
<code>architectGenerate</code>'s Phase-1 catch (<code>src/architect.cpp:423-435</code>)
— the same structural-only gap the shipped enemy half of REQ-ARCH-5 has today.
Close it with the loop suite's own fault-injection convention
(<code>tests.cpp:1831-1858</code>): <code>db.exec("DROP TABLE location")</code>,
which is <b>surgical</b> here — <code>writeGeneratedRoom</code> writes no
location row (rooms have no container, <code>mutations.hpp:163</code>), so with
an enemy-free, story-carrying proposal the <b>only</b> writer that touches it is
<code>placeCatalogEntry</code>. Then walk the latent exit through
<code>runTurn</code> and assert the outcome is <code>EngineError</code> with
<code>meta.turn</code>, the room count, and the event count <b>all unchanged</b>
— not <code>Ticked</code> with a wall. A downgrade to a silent wall fails this
arm loudly.
</blockquote>

### Step 7 — Sync path: thread the menu through `architectGenerate`
**Requirements:** REQ-BARD-ARCH-1, -2, -6, -15. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`architectProposeRoom` gains `const std::vector<std::string>& storyHandles`
**after `enemyBlurbs`**, mirroring Step 3's placement in
`buildArchitectRequestBody`, and **with no default** — both call sites are
in-tree and must be explicit about what they snapshot:

```cpp
std::optional<RoomProposal> architectProposeRoom(
    const std::string& contextPayload,
    const std::vector<std::string>& enemyBlurbs,
    const std::vector<std::string>& storyHandles,
    const std::string& direction, const HttpTransport& transport);
```

It forwards `storyHandles` to `buildArchitectRequestBody`. In
`architectGenerate`, beside the existing `eligibleEnemyBlurbs` read (`:420`),
read the menu once and derive the handles; pass the menu to
`buildArchitectContext` **inside** the Phase-1 catch (it is a DB read, and a
fault there must wall exactly as today) and the handles to `architectProposeRoom`.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> existing <code>testArchitectGenerate</code>
(<code>tests.cpp:4675</code>) passes <b>unmodified</b> — with no catalog rows the
menu is empty and the transport sees the same body it always saw
(REQ-BARD-ARCH-15). New <code>testArchitectStoryGenerate</code>, driving
<code>architectGenerate</code> with a body-capturing fake transport: <b>spec test
10</b> — construct a world where the origin's own menu and the prospective room's
menu <b>differ</b> (an entry at <code>tier == originDistance + 1</code> is on one
and not the other), and assert the captured <code>story.handle</code> enum is the
<b>prospective</b> set, is not the whole catalog, and is not the origin's; plus
one end-to-end arm where a canned story response produces a room and a placed
entity in a single call.
</blockquote>

### Step 8 — Pregen path: snapshot the handles, hoist the menu
**Requirements:** REQ-BARD-ARCH-14, -15. **Size:** M · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

`PregenJob` (`src/pregen.hpp:49`) gains
`std::vector<std::string> storyHandles;  // eligible catalog handles, snapshotted`
beside `enemyBlurbs`, with the same comment discipline. `src/pregen.cpp:106`
passes `job.storyHandles` to `architectProposeRoom`. **No SQL enters
`pregen.cpp`** — the snapshot rule (REQ-PREGEN-5/-7) is why the handles are a
job field at all.

In `architectQueuePregen`, hoist beside the existing hoisted reads (`:509-510`):

```cpp
const std::vector<CatalogChoice> menu = eligibleCatalogForNewRoom(db, room);
```

then inside the loop pass `menu` to `buildArchitectContext` and the derived
handles to `job.storyHandles`. Per-room, not per-direction — that is the whole
point of micro-decision 1, and it is why this read sits with `enemyBlurbs`
rather than inside the builder.

Commit needs no change: `architectCommitProposal` is already the single Phase 2
both paths run (`src/systems.cpp:116` and `src/architect.cpp:443`), so Step 6
made story placement path-identical **structurally**. This step proves it.

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> the <b>entire existing pregen suite</b> —
<code>testPregenStore</code>, <code>testPregenWorker</code>,
<code>testPregenWait</code>, <code>testArchitectQueuePregen</code>,
<code>testPregenCommit</code>, <code>testPregenOutcomeRecords</code> — passes
<b>unmodified</b>. New <code>testArchitectStoryPregen</code>: (a) a queued job's
<code>contextPayload</code> carries <code>story_options</code> and its
<code>storyHandles</code> equals the prospective menu; (b) <b>spec test 16</b>,
the path-equivalence proof — commit the <b>same canned proposal</b> once through
<code>pregenInjectReadyForTest</code> + a latent-exit walk and once through a
direct synchronous <code>architectGenerate</code>, into two worlds built from the
same fixture, and assert <b>identical rows</b> (entity, name, description,
location, <code>catalog.entity</code>) and <b>identical events</b> (verb, subject,
object, detail).
</blockquote>

### Step 9 — Nouns must exist, as provable today
**Requirements:** REQ-BARD-ARCH-12 (the noun half); spec test 17, **descoped** — see below. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

Test-only; no production change. New `testArchitectStoryNoun`, asserting the
part of "nouns must exist" that brick 4 owns: after a story materializes,
`lookupNoun(db, catalog.name)` (`src/lookup.hpp:16`) returns the minted entity;
its `location.container` is the new room; its `description.prose` is the
architect's instance prose; and the room's canon description — which the prompt
clause from Step 4 asks the model to write the entry into — is the surface the
player reads it on.

Then record the interaction gap **in the test's own comment**, so the next
reader finds it where they need it rather than only in this plan:

```cpp
// WHAT THIS DOES NOT ASSERT, AND WHY. Spec test 17 words this as "the player can
// examine it." That is not implementable today: there is no examine verb (the
// ISA is Look/Go/Take/Drop/Inventory/Wait/Quit/Attack/Cast/Read/Spells,
// action.hpp:11, and Look takes no subject), and BOTH the narrator's facts
// (prose.cpp:105) and the resolver's scope payload (nlresolve.cpp:166) enumerate
// only PORTABLE entities in a room — a story entity is neither portable nor
// hostile, so it reaches neither. The noun genuinely exists and resolves; it
// cannot yet be acted on. Closing that is a separate brick (scenery-in-scope +
// an examine verb), deliberately out of this brick's modules.
//
// A SECOND caveat on the same claim: lookupNoun matches EXACTLY, and its header
// notes names are stored lowercase (lookup.hpp:16). writeCatalogEntry only TRIMS
// `name` (mutations.cpp:581) — it does not lowercase it. A bard that authors
// "Scorched Lectern" therefore mints a noun the player cannot type. This fixture
// controls the case, so this test passes either way; the exposure is real bard
// output, and the fix belongs in brick 1's writeCatalogEntry, not here.
```

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate:</b> <code>testArchitectStoryNoun</code> passes, and its
comment names both blockers with file:line. This step's <b>real</b> gate is
honesty: the plan does not get to claim spec test 17 in the coverage map, and
does not.
</blockquote>

### Step 10 — Final validation & contract audit
**Requirements:** all 17, read back. **Size:** S · **Token-risk:** <span style="background:#e6f4ea;color:#1e7e34;padding:1px 6px;border-radius:3px;">LOW</span>

<blockquote style="border-left:4px solid #1e7e34;padding-left:12px;margin-left:0;">
<b>✅ Validation gate (the spec's own mechanical list, 1–4):</b><br>
<b>1.</b> <code>cmake --build build</code> clean.<br>
<b>2.</b> <code>./build/tests</code> green, and <b>the entire existing architect,
pregen, and combat suite passed unmodified</b> — audited as
<code>git diff tests/tests.cpp</code> showing <b>additions only</b> (micro-decision
8). Any edit to an existing test is a regression until proven otherwise.<br>
<b>3.</b> <code>grep -En "INSERT|UPDATE|DELETE" src/architect.cpp</code> →
<b>empty</b> (REQ-BARD-ARCH-16).<br>
<b>4.</b> <code>grep -n "O(1) in world size" src/architect.hpp</code> →
<b>empty</b>, and the replacement comment states O(eligible catalog)
(REQ-BARD-ARCH-3).<br>
<b>Plus the inherited guards:</b> <code>grep -En "INSERT|UPDATE|DELETE"
src/bard.cpp</code> still empty; <code>grep -rn "place_catalog" src/</code> still
empty; brick 1's two append-only source-text tests still pass. And a read of this
plan against the spec: every REQ-BARD-ARCH-N appears in the coverage map below
with a step and a test that fails if it is removed.
</blockquote>

---

## Coverage map — requirement → step

| Requirement | Step | Pinned by |
|---|---|---|
| ARCH-1 context carries `bard_focus` + menu | 2, 7, 8 | `testArchitectStoryContext` (focus verbatim, handles/blurbs/motive blurbs) |
| ARCH-2 `eligibleCatalogForNewRoom` supplies it | 7, 8 | `testArchitectStoryGenerate` spec-test-10 arm (prospective ≠ origin) |
| ARCH-3 O(1) claim retired | 2 | mechanical check 4 |
| ARCH-4 empty catalog/menu/focus → thinner, well-formed | 2 | `testArchitectStoryContext` 4-key/5-key arms |
| ARCH-5 optional `story {handle, description}` | 1, 3 | `testArchitectStoryRequestBody` schema assertions |
| ARCH-6 handle is a schema enum; empty menu → field omitted | 3 | enum equality + `find("story") == npos` (spec test 6) |
| ARCH-7 at most one story; enemy + story independent | 3, 6 | `properties.size() == 5` arm; placement spec-test-14 arm |
| ARCH-8 lenient extraction, never rejects | 5 | `testArchitectStoryGate`, all seven drop reasons |
| ARCH-9 blank handle or description → dropped in full | 5 | `testArchitectStoryGate` blank arms |
| ARCH-10 placed in Phase 2, after the enemy | 6 | `testArchitectStoryPlacement` ordering + spec-test-14 arm |
| ARCH-11 live re-check; unknown/stale/materialized → 0 | 6 | spec-test-12 and spec-test-13 arms |
| ARCH-12 architect prose is the description, catalog the name | 6 | spec-test-11 arm, asserting description ≠ blurb |
| ARCH-13 outside the Phase-1 catch | 6 | `testArchitectStoryPlacement` fault-injection arm — `DROP TABLE location` → `EngineError` + full rollback, never a wall |
| ARCH-14 pregen and sync place through the same code | 8 | `testArchitectStoryPregen` path-equivalence arm (spec test 16) |
| ARCH-15 empty catalog → every existing behavior unchanged | 2, 3, 7, 8 | mechanical check 2: existing suite unmodified |
| ARCH-16 architect stays read-only | 6, 10 | mechanical check 3 |
| ARCH-17 no ids on the wire | 1, 2, 5 | `checkNoIdKeys` + spurious-field arm |

## Spec test → step

| Spec test | Step | Spec test | Step |
|---|---|---|---|
| 1–4 (mechanical) | 10 | 11, 12, 13, 14, 15 (materialization) | 6 |
| 5, 6, 7 (non-regression) | 2, 3, 5 | 16 (path equivalence) | 8 |
| 8, 9 (context) | 2 | 17 (nouns must exist) | 9 — **partial, see below** |
| 10 (schema) | 7 | | |

## What this plan deliberately does not do

- **Spec test 17 is not met as written, and the plan says so.** A materialized
  story entry cannot be examined: there is no `examine` verb (`src/action.hpp:11`),
  and both the narrator's facts (`src/prose.cpp:105`) and the AI resolver's scope
  payload (`src/nlresolve.cpp:166`) enumerate only `portable` entities in a room.
  The noun exists, resolves via `lookupNoun`, and is named in the room's canon
  prose — Step 9 proves all three — but it cannot be acted on. Closing this needs
  a scenery-in-room read plumbed into the resolver's scope and the narrator's
  facts, plus an `Examine` verb (or `Look` with a subject) through `action.hpp`,
  `parser.cpp`, `nlresolve.cpp`, `systems.cpp` and `render.cpp`. **That is five
  more shipped subsystems and is outside this spec's modules**, so it is a
  follow-up brick, not a step here. Worth raising as the next spec: until it
  lands, materialized story is scenery the player can read about but not touch.
- **No lowercasing of `catalog.name`, though it looks like a live bug in brick 1.**
  `lookupNoun` matches exactly and expects lowercase (`src/lookup.hpp:16`), but
  `writeCatalogEntry` only trims `name` (`src/mutations.cpp:581`). A bard that
  authors "Scorched Lectern" mints a noun the player cannot type — which would
  defeat the nouns-must-exist guarantee this whole brick exists to serve. The fix
  is one `tolower` in **brick 1's** helper, outside this brick's diff and its
  non-regression promise. **Raise it as the next spec's first line.**
- **No placement into already-explored rooms.** Deferred by design Decision 6;
  the `catalog.pending` shape is recorded there and nothing here implements it.
  The stated consequence stands: once the map is fully explored, no new story
  materializes.
- **No `bard_focus` to the narrator.** Design open question, leaning no — a focus
  line is not an event, and the narrator's contract is no claim without a sourcing
  event row.
- **No live-LLM verification.** The spec requires none, and adding a story arm to
  the gated `testArchitectLiveSmoke` (`tests.cpp:3861`) is an optional follow-up,
  not a gate. Prompt quality is judged against real play, not in the suite.
- **No `cache_read_input_tokens` measurement.** Design Decision 4 notes the menu
  is a large stable recurring prefix and is worth measuring once this lands. That
  is an observation to act on after a real session, not work in this brick.
- **No catalog-tier profiling.** The design's open question about entries that are
  never eligible anywhere needs a profile count over real play, not a test.
