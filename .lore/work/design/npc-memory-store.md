---
title: "The NPC memory store: profiles, memory, and where the words live"
date: 2026-08-06
status: implemented
tags: [npc, memory, identity-profile, schema, mutations, append-only, major-npc, caps]
modules: [mutations, world, bard, main]
related: [.lore/work/brainstorm/npcs.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/work/design/bard-fact-store.md, .lore/work/design/examine-perception-verb.md, .lore/vision.md]
---

# The NPC memory store: profiles, memory, and where the words live

## Scope

One element of the NPC feature: **where a character's identity and memory live,
and who may change them.** Everything downstream — the conversation call, the
prompt, major-character arrival — needs these to have a shape, so this is the
gating brick. It is the same position `bard-fact-store.md` held for the bard,
and it follows that design's method: schema plus write helpers, no AI, no
network, testable with pure SQL.

Out of scope, deliberately: the conversation prompt, the `say` action, who reads
what during a turn, movement, arrival, and whether the narrator sees speech.

## Decision 1 — Conversation lines are events. There is no second memory table.

The obvious move is a `npc_memory_entry` table holding what was said. It should
not be built.

The `events` table is already an append-only, turn-stamped, transactionally
consistent transcript, and the prior-art round said plainly that it *is* a
memory stream in the Generative Agents sense — a character's memory is a
filtered fold over it. A parallel log would be a second append-only store with
its own consistency story and its own way to drift from the first.

So a conversation writes two ordinary event rows:

| verb | actor | subject | detail |
|---|---|---|---|
| `said` | the player | the character | the player's line |
| `spoke` | the character | the player | the character's reply |

Both are **no-write verbs** — an `appendEvent` with no paired component write.
`mutations.hpp` currently states that this is legal *only* for `looked`,
`waited`, and `failed`. That contract is amended here to include `said` and
`spoke`, and the amendment must be made in the comment, not just in practice:
that line is the thing a future reader checks a new verb against.

A character's raw memory is then a query, not a table:

```sql
SELECT verb, detail FROM events
 WHERE verb IN ('said','spoke')
   AND (actor = :npc OR subject = :npc)
   AND turn > :summary_turn
 ORDER BY id
 LIMIT :cap;
```

This also settles, without needing to, the deferred question about `events`
having no room column. A character remembers conversations it took part in, and
participation is recorded in `actor`/`subject`. Nothing here needs to know where
anyone was standing.

## Decision 2 — Two new tables: an immutable profile, a rewritable summary

```sql
-- The authored identity of a character: who they are, how they talk, what they
-- know, what they will not say. WRITE-ONCE — there is deliberately no helper
-- that edits a profile, so a character's identity cannot drift.
--
-- Keyed by CATALOG id, not entity, because a hand-authored major character's
-- profile exists from world creation, long before any entity does.
CREATE TABLE catalog_profile(
  catalog INTEGER PRIMARY KEY,        -- catalog.id
  profile TEXT NOT NULL               -- the full character document, model-facing
);

-- What a character remembers, as it remembers it. Freely rewritten, and CAPPED
-- (see decision 4). Keyed by entity: memory only exists once the character does.
CREATE TABLE npc_memory(
  entity       INTEGER PRIMARY KEY,
  summary      TEXT NOT NULL DEFAULT '',
  summary_turn INTEGER NOT NULL DEFAULT 0   -- the turn the summary last covered
);
```

`summary_turn` is the per-character analogue of `meta.bard_last_wake_turn`, and
it exists for the same reason: without it, the raw-line query grows without
bound across a long session and the recurring prompt grows with it.

The two mutabilities are deliberately opposite and that is the whole point of
splitting them:

| | Profile | Memory |
|---|---|---|
| Answers | who this is | what has happened to them |
| Written by | a human (major) or the model, once (minor) | the model, every time it thinks |
| Mutability | **write-once, no update path** | free rewrite, capped |
| If it drifts | the character stops being themselves | the character misremembers, which is fine |

The research round is the reason this split is load-bearing rather than tidy.
Persona self-consistency degrades by more than 30% after eight to twelve turns
when the character definition is diluted by accumulating context, and the
NPC-specific memory work found that generic summarisation strips out
character-defining detail and flattens the character. Both failures are failures
of letting *one* store carry both jobs. Here the profile is immutable and re-sent
in full on every call, so it cannot erode; the summary can be rewritten freely
because nothing about the character's identity depends on it.

## Decision 3 — Major characters are catalog rows with `kind = 'major'`

They need everything the catalog already provides: a handle the model selects
by, a parser noun, a depth gate, a one-way materialisation latch, and the
`materialized` event that the bard's wake trigger reads. Building a parallel
`major_cast` table would duplicate `placeCatalogEntry`,
`materializeCatalogEntry`, the latch, and the event — all of which are built and
tested.

Two code changes fall out, and the second is a trap worth naming:

1. `writeCatalogEntry` refuses any `kind` other than `'character'` or `'beat'`
   (`mutations.cpp:577`), and `bard.cpp:796` repeats the check on the model's
   side. The helper's guard gains `'major'`. **The model-facing check must
   not** — the bard authors characters and beats, never majors, so a `major`
   arriving from the model is still a refusal.
2. `eligibleCatalogForNewRoom` calls `offerable(db, /*kind=*/"", …)`
   (`bard.cpp:952`), and an empty kind means *every* kind. As written, adding
   `'major'` would silently start offering major characters to the room
   generator, which is exactly the arrival mechanism this design is trying to
   avoid. **That call must become an explicit pair**, `character` and `beat`, so
   the generator's menu is a stated list rather than a default.

That second one is the argument for putting majors in the catalog rather than
beside it, inverted: a shared table means every query has to remember to exclude
them, and we have already found one that would not have.

The bard *should* see majors in its wake context — it is the thing that decides
when one arrives — and that already works, because the wake context carries the
full catalog rather than the eligible menu.

## Decision 4 — Caps are the only mechanical brake, and they are honest about it

Three caps, all enforced in the write helpers, all truncating in **code points**
(the `writeBardFocus` precedent, which has a test for exactly this):

| What | Cap | Why this number |
|---|---|---|
| `catalog_profile.profile` | 4000 code points | generous for a hand-written character (~700 words is well under), and a hard stop on a model-written one that runs away |
| `npc_memory.summary` | 800 code points | short enough that it cannot hold a personality essay |
| raw lines per read | 40 | bounds the recurring prompt |

The summary cap is the mechanical form of the research finding that a rewritable
summary erodes personality. The rule we actually want — *the summary carries
facts, never voice* — is a prompt rule, and a prompt rule is not a guarantee.
The cap is a brake, not a proof: a determined model can still write 800
characters of voice. It is worth having anyway, because the failure mode is
gradual and a hard ceiling stops it from compounding.

State that plainly rather than implying the schema enforces it.

## Decision 5 — Profile files: header for the engine, body for the model

Major characters are hand-written files, read at world creation and loaded into
the database, following the `seed/setting.txt` precedent exactly — `main.cpp`
reads them, `openWorld` stores them, and after that the world owns them.

The brainstorm found that a profile is really two documents: how the character
talks, which is for the model, and how they behave, which is for the engine. The
file format is that split:

```
handle: ilse_rooke
name: ilse
motive: rivalry
tier: 2

Fifth-year student. She/her. Out of bed tonight, same as you, and much
better at it.
...
```

`key: value` lines, one blank line, then the body verbatim. Hand-writable, no
escaping, and about thirty lines of parsing — as against JSON, where a
thousand-word profile becomes an unreadable escaped string, or YAML, which we
have no parser for.

The header fields are exactly the catalog columns. The body is the profile, and
it is model-facing only. **The engine-owned rules — never explain mechanics,
never volunteer background, never name what does not exist — are not in the
file.** They live in the prompt the engine controls, applied to every character,
for the reason the research round sharpened: a rule that a player will actively
attack cannot live in a file an author can edit or forget.

Two things this format does not carry, on purpose:

- **A goal field.** Movement is the third brick and nothing here reads a goal.
  The bard fact store already set the precedent — it specified `catalog_binding`
  and then declined to ship it, on the grounds that an inert table through a
  schema bump buys nothing. Same reasoning: the goal arrives with the thing that
  moves.
- **A blurb.** The catalog's `blurb` is selection text for the model, and a
  major character is not selected from a menu. `writeCatalogEntry` requires one,
  so the loader passes the profile's **first non-empty** body line — enough to
  satisfy the column without inventing a second authoring surface. "Non-empty"
  rather than "first" so a body that opens with a blank line still yields a
  usable blurb.

## Decision 6 — Ordering: majors are written before the overture runs

The brainstorm's requirement is that the bard's story is built *around* the
hand-written cast, not in ignorance of it. That is an ordering constraint on
initialisation:

1. `openWorld` creates the world, seeds it, and writes the major-character
   catalog rows and their profiles — all inside `initialize()`'s existing
   transaction.
2. `main.cpp` runs the overture immediately after, as it already does.
3. `buildOvertureContext` gains a third element: the major cast, as name plus
   profile.

Today that context carries "exactly two things — `meta.setting` and the motive
vocabulary," and the comment says so. Adding a third is a real change to a
documented contract and should be made in the comment as well as the code.

Note what this does *not* do: the overture still cannot place anyone, because
the map does not exist yet. It reads the cast so the story accommodates them.

## Helpers to add to `mutations.hpp`

```cpp
// Write a character's authored profile. WRITE-ONCE: guarded in SQL, so a second
// call for the same catalog entry changes nothing and returns false — the
// learnSpell / materializeCatalogEntry idempotence shape. There is deliberately
// NO helper that edits an existing profile: a character's identity cannot drift
// because no code path exists to drift it.
//
// Truncated to kProfileCap code points. Event-free — an authored profile has not
// happened; the character's arrival in the world is what produces an event.
bool writeCatalogProfile(Db& db, int64_t catalog, const std::string& profile);

// Upsert a character's memory summary and stamp summary_turn with the CURRENT
// turn, both inside the caller's ambient transaction. Free rewrite: memory is a
// reconstruction, and a character misremembering costs nothing mechanical.
//
// Truncated to kSummaryCap code points. See the design's decision 4: this cap is
// a brake on personality erosion, not a proof against it.
void writeNpcMemory(Db& db, int64_t entity, const std::string& summary);
```

Both are event-free, and both follow `writeCatalogEntry`'s precedent for why:
neither is a thing that *happened* in the world.

Speech needs no new helper. `said` and `spoke` are `appendEvent` calls, which is
the point of decision 1 — but the contract comment listing the legal no-write
verbs must be amended, or the next person to read it will conclude speech is
breaking the rule.

## Read helpers (specified here, called by the conversation brick)

```cpp
// The profile of the character `entity` is, via its catalog row. Empty if the
// entity is not a catalog character or has no profile yet — which is the normal
// state of a minor character before its first conversation.
std::string npcProfile(Db& db, int64_t entity);

// The character's memory summary, and the turn it covers to.
NpcMemory npcMemory(Db& db, int64_t entity);

// The speech events this character took part in since its summary was written,
// oldest first, capped at kLineCap. The bounded read is what stops the
// recurring prompt growing across a session — see decision 4, and the persona
// drift finding behind it.
std::vector<SpeechLine> npcLinesSince(Db& db, int64_t entity);
```

## Grep-checkable invariants

Following the fact store, which asserts its two append-only guarantees against
the **source text** so they survive as regression guards:

1. **No code path edits a profile.** `grep -En "UPDATE catalog_profile"
   src/*.cpp` must be empty.
2. **One translation unit writes these tables.** `INSERT|UPDATE|DELETE` against
   `catalog_profile` or `npc_memory` appears only in `mutations.cpp`.

## Schema impact

Two new tables and one new `kind` value. **`SCHEMA_VERSION` 6 → 7.** Migration
story unchanged: delete `world.db` and relaunch. Content is still disposable.

`npc_memory` rows are not pre-created at materialisation — the helper upserts,
so there is no row to branch on. This differs from the bard's `meta` rows, which
are seeded empty at init precisely so their helpers can `UPDATE` unconditionally;
here the row count is unbounded, so an upsert is the right shape.

## What this design does not settle

- **The conversation itself** — the `say` action, the prompt, the model call,
  and what the reply does. Next brick.
- **Whether the narrator sees `said` / `spoke` events.** They exist in the log
  either way; who reads them is the conversation brick's call.
- **Movement, goals, and arrival.** Third brick. The catalog `kind = 'major'`
  and the one-way materialisation latch are what it will build on.
- **Whether a minor character's profile should ever be regenerated.** It is
  write-once here, which means a bad first profile is permanent. That matches
  how everything else in this project treats canon, and it is the right default,
  but it is the first thing to complain about if minor characters turn out
  bland.

## Decision summary

1. **Conversation lines are `said` / `spoke` event rows.** No second memory
   table; a character's raw memory is a bounded query over `events`. The
   no-write-verb contract in `mutations.hpp` is amended to include them.
2. **Two tables:** `catalog_profile`, write-once and keyed by catalog id;
   `npc_memory`, freely rewritten and keyed by entity, carrying a
   `summary_turn` watermark.
3. **Major characters are catalog rows with `kind = 'major'`**, which reuses
   materialisation wholesale — and requires making
   `eligibleCatalogForNewRoom`'s "all kinds" call explicit, or majors leak into
   the room generator's menu.
4. **Three code-point caps** — 4000 for a profile, 800 for a summary, 40 lines
   per read. The summary cap is a brake on personality erosion, stated honestly
   as a brake rather than a guarantee.
5. **Profile files are `key: value` header plus freeform body** — header for the
   engine, body for the model. Engine-owned rules live in the prompt, never in
   the file.
6. **Majors are written during `initialize()`, before the overture**, and
   `buildOvertureContext` gains the cast as a third element so the story is
   authored around them.
7. **Write-once and single-writer are asserted against the source text**, not
   trusted.
8. **`SCHEMA_VERSION` 6 → 7.**
