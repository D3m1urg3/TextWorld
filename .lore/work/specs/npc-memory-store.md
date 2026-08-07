---
title: "The NPC memory store: requirements"
date: 2026-08-06
status: implemented
tags: [npc, memory, identity-profile, schema, mutations, append-only, major-npc, caps, requirements]
modules: [mutations, world, bard, main]
related: [.lore/work/design/npc-memory-store.md, .lore/work/brainstorm/npcs.md, .lore/work/research/llm-npc-dialogue-and-memory.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/examine-perception-verb.md]
req-prefix: NPCSTORE
---

# The NPC memory store: requirements

## Context

Where a character's identity and memory live, and who may change them.
Established in [the design](../design/npc-memory-store.md).

This is the gating brick for NPCs, in the position `bard-fact-store.md` held for
the bard, and it follows that spec's method: schema plus write helpers, **no AI
and no network**, testable with pure SQL.

**Two stores with opposite rules.** A character's *profile* is written once and
never edited, so identity cannot drift. Its *memory* is rewritten freely and
capped, because memory is a reconstruction and a character misremembering costs
nothing mechanical. That split is load-bearing rather than tidy: the research
round found persona self-consistency degrading by more than 30% after 8–12 turns
when one store carries both jobs, and found that generic summarisation strips out
exactly the details that make a character themselves.

**Conversation lines are events.** The `events` table is already an append-only,
turn-stamped, transactional transcript, and the prior-art round identified it as
a memory stream in the Generative Agents sense. A second log would be a second
append-only store with its own way to drift from the first.

**Scope boundary.** The conversation itself — the `say` action, the prompt, the
model call, what the reply does — is specified separately. Movement, arrival, and
major-character placement are a third brick. Nothing here calls a model.

## Requirements

### Speech in the event log

**REQ-NPCSTORE-1** — Two new event verbs exist: `said` (actor = the player,
subject = the character, detail = the player's line) and `spoke` (actor = the
character, subject = the player, detail = the character's reply). No other event
verb is added by this spec.

**REQ-NPCSTORE-2** — Both are **no-write verbs**: an `appendEvent` with no paired
component write. `mutations.hpp`'s contract comment, which today states this is
legal *only* for `looked`, `waited`, and `failed`, is amended to name `said` and
`spoke`. The amendment is in the comment, not only in practice.

**REQ-NPCSTORE-3** — No table is created to hold conversation lines. A
character's raw memory is a query over `events`, filtered to `said`/`spoke` rows
where the character is `actor` or `subject`.

**REQ-NPCSTORE-4** — `said` and `spoke` are **not** added to the bard's wake
predicate, which stays `generated`, `defeated`, `learned`, `materialized`.
Speech is not irreversible.

**REQ-NPCSTORE-5** — `said` rows contain the player's typed input, so `world.db`
now holds what the player wrote. This is intended — it is the memory — and is
stated because the session log deliberately does the opposite and records typed
input at no level above debug.

### The two tables

**REQ-NPCSTORE-6** — `catalog_profile(catalog INTEGER PRIMARY KEY, profile TEXT
NOT NULL)`. Keyed by `catalog.id`, not by entity, because a hand-authored major
character's profile exists from world creation and its entity does not.

**REQ-NPCSTORE-7** — `npc_memory(entity INTEGER PRIMARY KEY, summary TEXT NOT
NULL DEFAULT '', summary_turn INTEGER NOT NULL DEFAULT 0)`. Keyed by entity:
memory exists only once the character does.

**REQ-NPCSTORE-8** — `summary_turn` records the turn the summary last covered. It
is the per-character analogue of `meta.bard_last_wake_turn` and exists for the
same reason: without it the raw-line read grows without bound across a session
and the recurring prompt grows with it.

> **Amended by [npc-conversation](npc-conversation.md) (REQ-NPCTALK-29a).**
> `writeNpcMemory` stamps the turn **before** the current one, not the current
> turn. A summary is composed by the model from the lines it was handed, in the
> same call that produces this turn's reply, so it can never cover this turn's
> own exchange — and `npcLinesSince` filters on `turn > summary_turn`, so
> stamping the current turn would hide that exchange from every future read.
> The stamp is still computed **inside** the helper, which is what this
> requirement is actually protecting: no caller gets to choose it.

**REQ-NPCSTORE-9** — `npc_memory` rows are **not** pre-created at
materialisation. The write helper upserts, so no row exists to branch on. This
differs deliberately from the bard's `meta` rows, which are seeded empty at init
because there is a fixed number of them.

**REQ-NPCSTORE-10** — `SCHEMA_VERSION` 6 → 7. Migration story unchanged: the
engine refuses an older world file and the player deletes `world.db`.

### Profiles are write-once

**REQ-NPCSTORE-11** — `writeCatalogProfile(Db&, int64_t catalog, const
std::string& profile) -> bool` inserts one profile row. The write-once guard is
**in the SQL**, so a second call for the same catalog entry changes nothing and
returns `false` — the `learnSpell` / `materializeCatalogEntry` idempotence shape,
never a prior read.

**REQ-NPCSTORE-12** — There is **no code path that edits an existing profile**.
No helper updates `catalog_profile.profile`, and none is added later without
revisiting this requirement. A character's identity cannot drift because nothing
can drift it.

**REQ-NPCSTORE-13** — `writeCatalogProfile` is event-free. An authored profile
has not *happened*; the character's arrival in the world is what produces an
event. Same reasoning `writeCatalogEntry`, `dropGrimoire`, and `placeEnemy`
already carry.

**REQ-NPCSTORE-14** — A profile for a catalog id that does not exist is refused
and returns `false`. No orphan row is created.

### Memory is rewritable and capped

**REQ-NPCSTORE-15** — `writeNpcMemory(Db&, int64_t entity, const std::string&
summary)` upserts the summary and stamps `summary_turn`, read from `meta.turn`
inside the caller's ambient transaction. Free rewrite: no latch, no append
semantics. *(The stamp is the turn **before** the current one — see the
amendment under REQ-NPCSTORE-8.)*

**REQ-NPCSTORE-16** — Both helpers run inside the caller's ambient transaction
and **never** begin, commit, or roll back. The caller owns the boundary — the
rule every mutation helper here already follows.

**REQ-NPCSTORE-17** — Three caps, all enforced in the write helpers, all
truncating in **code points** (the `writeBardFocus` precedent, which has a test
for exactly this):

| Constant | Value | Applies to |
|---|---|---|
| `kProfileCap` | 4000 code points | `catalog_profile.profile` |
| `kSummaryCap` | 800 code points | `npc_memory.summary` |
| `kLineCap` | 40 | rows returned by the raw-line read |

**REQ-NPCSTORE-18** — The summary cap is a **brake, not a guarantee**. The rule
it stands in for — *the summary carries facts, never voice* — is a prompt rule
and cannot be mechanically enforced on free text. The cap stops erosion
compounding; it does not prevent it. This is stated so no later reader mistakes
the cap for enforcement.

### Read helpers

**REQ-NPCSTORE-19** — `npcProfile(Db&, int64_t entity) -> std::string` returns
the profile via the entity's catalog row, or `""` when the entity is not a
catalog character or has no profile yet. Empty is a **normal, common** answer —
it is the state of every minor character before its first conversation — and is
never an error.

**REQ-NPCSTORE-20** — `npcMemory(Db&, int64_t entity)` returns the summary and
its `summary_turn`. A character with no `npc_memory` row yields `""` and `0`,
not an error.

**REQ-NPCSTORE-21** — `npcLinesSince(Db&, int64_t entity)` returns the
`said`/`spoke` events the character took part in with `turn > summary_turn`,
**oldest first**, capped at `kLineCap`. When more than `kLineCap` qualify, the
**most recent** `kLineCap` are returned — a character forgets the middle of a
long conversation, never the end of it.

**REQ-NPCSTORE-21a** — The cap must never hand back a reply without its
question. `said` and `spoke` are written as a pair in one turn, so a row-granular
cut can leave a leading orphan `spoke`. After taking the most recent `kLineCap`
rows, a **leading `spoke` whose paired `said` was cut is dropped**, returning
`kLineCap - 1` rows. A **trailing** `said` with no `spoke` is legitimate and is
kept — that is what a failed reply looks like, and hiding it would make the
character unaware it was spoken to.

**REQ-NPCSTORE-22** — All three read helpers perform `SELECT`s only and are
deterministic functions of the database.

### Major characters in the catalog

**REQ-NPCSTORE-23** — `catalog.kind` gains the value `'major'`.
`writeCatalogEntry`'s guard (`mutations.cpp:577`) accepts it.

**REQ-NPCSTORE-24** — The **model-facing** kind check (`bard.cpp:796`) does
**not** accept `'major'`. The bard authors characters and beats; a `major`
arriving from the model stays a refusal. The two checks diverge on purpose and
the divergence is commented at both sites.

**REQ-NPCSTORE-25** — `eligibleCatalogForNewRoom`'s call to `offerable` with an
empty `kind` (`bard.cpp:952`) becomes an **explicit pair**, `character` and
`beat`. As written, an empty kind spans every kind, so adding `'major'` would
silently start offering major characters to the room generator — the exact
arrival mechanism this design avoids. The generator's menu must be a stated list,
not a default.

**REQ-NPCSTORE-26** — The bard's wake context continues to carry the **full**
catalog, so majors are visible to it. The bard is what decides when a major
arrives, and this already works without change.

### Hand-authored profile files

**REQ-NPCSTORE-27** — A major character is authored as one file: `key: value`
header lines, one blank line, then the body verbatim. Header keys are `handle`,
`name`, `motive`, `tier`. The body is the profile.

**REQ-NPCSTORE-28** — The header supplies the catalog columns; the body supplies
`catalog_profile.profile` and is **model-facing only**. Engine-owned rules —
never explain a mechanic, never volunteer background, never name what does not
exist — are **not** in the file. They live in the prompt the engine controls, for
the reason the research round sharpened: a rule a player will actively attack
cannot live in a file an author can edit or forget.

**REQ-NPCSTORE-29** — `writeCatalogEntry` requires a `blurb`, and a major
character is never selected from a menu, so the loader supplies the profile's
**first non-empty body line** as the blurb. No second authoring surface is
invented for a field nothing reads.

**REQ-NPCSTORE-30** — The file format carries **no goal field**. Movement is a
later brick and nothing here reads a goal; the bard fact store set the precedent
by specifying `catalog_binding` and declining to ship it inert.

**REQ-NPCSTORE-31** — A malformed profile file **fails world creation loudly**
rather than being skipped, with a message naming the file. These are
hand-authored seed files like `base.sql`; a silently dropped major character is a
world missing its most expensive content with nothing to show for it. Malformed
means any of: a missing required header key; an **unrecognised** header key; a
motive absent from `motive_catalog`; a non-integer `tier`; a handle duplicating
another **profile file's** handle; or a header that does not parse.

**REQ-NPCSTORE-31a** — An unrecognised header key is a failure, not a silent
skip. The concrete case is an author writing `goal:` — a field this spec
deliberately does not carry (REQ-NPCSTORE-30) — and getting a world where the
line quietly did nothing.

**REQ-NPCSTORE-31b** — The duplicate check at creation is **among the profile
files only**, because the catalog is empty when they are written
(REQ-NPCSTORE-33). Collision with a *later* handle is a separate case: the
overture may propose a handle a major already owns, and `catalog.handle` is
`UNIQUE`, so an unguarded insert would throw and roll back the whole batch.
That entry is **dropped and its siblings still admit**, which is exactly the
existing behaviour for a duplicate handle inside one batch — the same path, now
reachable against a pre-existing row rather than only against a sibling.

**REQ-NPCSTORE-32** — Zero profile files is valid and yields a world with no
major characters, no error, and no diagnostic. The absence of a cast is not a
fault.

### Initialisation order

**REQ-NPCSTORE-33** — Major catalog rows and their profiles are written during
`initialize()`, inside its existing transaction, before `openWorld` returns.

**REQ-NPCSTORE-34** — Profile files are read by `main.cpp` and passed to
`openWorld` as data, exactly as `setting.txt` already is. `world.cpp` performs no
file I/O of its own.

**REQ-NPCSTORE-35** — `buildOvertureContext` gains a **third** element: the major
cast, as name plus profile. Its comment today states it carries "exactly two
things"; that comment is updated with the code. The overture still cannot place
anyone — the map does not exist yet — it reads the cast so the story is authored
around them.

### Invariants asserted against the source

**REQ-NPCSTORE-36** — `grep -En "UPDATE catalog_profile" src/*.cpp` is empty.
Write-once is a property of the source text, not a convention.

**REQ-NPCSTORE-37** — `INSERT`, `UPDATE`, or `DELETE` against `catalog_profile`
or `npc_memory` appears in `mutations.cpp` and nowhere else.

**REQ-NPCSTORE-38** — Both invariants are asserted **by the test suite reading
the source text**, following the bard fact store's two append-only guards, so
they survive as regressions rather than being checked once by hand.

## AI Validation

Entirely offline. This spec introduces no model call, so every check below runs
with no network and no `ANTHROPIC_API_KEY`.

### Schema

1. A fresh world has `catalog_profile` and `npc_memory` with the exact column
   names, types, and defaults of REQ-NPCSTORE-6 and -7.
2. `SCHEMA_VERSION` is 7, and a version-6 world file is refused with the existing
   message.
3. `catalog.kind = 'major'` is accepted by `writeCatalogEntry`; `'wanderer'` is
   still refused.

### Profiles

4. `writeCatalogProfile` on a fresh catalog entry returns `true` and stores the
   text.
5. A second call with **different** text returns `false` and leaves the stored
   text **byte-identical**. Assert the stored value, not just the return.
6. A call for a catalog id that does not exist returns `false` and adds no row.
7. A 5000-code-point profile is stored truncated to exactly 4000 **code
   points** — driven with multi-byte characters, so a byte-truncating
   implementation fails and a code-point one passes.
8. No event row is appended by any of the above.
9. Called inside a transaction the caller then rolls back, nothing persists.

### Memory

10. `writeNpcMemory` on an entity with no row creates one; a second call
    replaces the summary outright.
11. `summary_turn` equals `meta.turn` at the moment of the write, asserted after
    advancing the turn between two writes.
12. An 900-code-point summary truncates to exactly 800 code points, multi-byte
    driven.
13. `npcMemory` on an entity with no row returns `""` and `0` without throwing.

### Raw lines

14. Six `said`/`spoke` rows, a summary written after the third, then
    `npcLinesSince` returns exactly the last three, oldest first.
15. With `kLineCap` + 10 qualifying rows, exactly `kLineCap` return and they are
    the **most recent** ones — assert the first returned row is not the oldest
    qualifying row.
16. Lines from a conversation with a **different** character are excluded.
17. A `said` row where the character is the `subject` and a `spoke` row where it
    is the `actor` are both included.
18. `looked`, `moved`, and `failed` rows in the same turn range are excluded.
18a. The returned `detail` values are **byte-equal** to the text originally
    appended — the player's line on `said`, the character's reply on `spoke`.
    Filtering and ordering are worthless if the content is not what was stored
    (REQ-NPCSTORE-1, -5).
18b. `kLineCap` + 1 qualifying rows whose oldest surviving row would be a
    `spoke`: exactly `kLineCap - 1` rows return and the first is a `said`
    (REQ-NPCSTORE-21a).
18c. A trailing `said` with no `spoke` — a failed reply — is present in the
    result and is not dropped.

### Catalog and the generator

19. **The regression that matters:** a world with one `'major'` entry and one
    `'character'` entry, both eligible by tier — `eligibleCatalogForNewRoom`
    returns the character and **not** the major. Written to fail if
    REQ-NPCSTORE-25's explicit pair is ever reverted to an empty kind.
20. `eligibleCatalog(db, room, "character")` and `(…, "beat")` are unchanged
    from before this spec, asserted against the existing catalog-selection
    tests run unmodified.
21. The bard's wake context contains the major entry — majors are visible to the
    bard even though they are invisible to the generator.
22. A model response proposing `kind: "major"` is refused by the validation gate
    with the existing wording, and the sibling entries in the same batch still
    admit.

### Profile files

23. A well-formed file yields one catalog row with the header's handle, name,
    motive, and tier, one profile row with the body byte-exact, and a blurb
    equal to the body's first non-empty line.
24. A file whose body contains a line matching `key: value` **after** the blank
    line is stored verbatim in the body — the header ends at the first blank
    line and never resumes.
25. Missing header key, unknown motive, duplicate handle, and unparseable header
    each fail world creation with a message naming the file.
26. Zero profile files creates a world successfully with no major rows and no
    diagnostic emitted at any log level.
27. A body of 5000 code points is stored truncated to 4000 (REQ-NPCSTORE-17
    applies to hand-authored files too).

### Ordering and the overture

28. Major catalog rows exist when `openWorld` returns, before any overture call
    is possible.
29. `buildOvertureContext` on a world with two majors contains both names and
    both profiles; on a world with none it is byte-identical to what it produced
    before this spec — the third element absent, not present-and-empty.
30. Everything written by `initialize()` lands in one transaction: a fault
    injected during major-row writing leaves **no** world file rows at all, not
    a half-seeded world.

### Source-text invariants

30a. An unrecognised header key (`goal: deeper`) fails world creation with a
    message naming the file (REQ-NPCSTORE-31a).
30b. An overture batch proposing a handle a major already owns: that entry is
    dropped, its siblings admit, the major's row is untouched, and nothing
    throws (REQ-NPCSTORE-31b).
31. `grep -En "UPDATE catalog_profile" src/*.cpp` is empty, asserted by the
    suite.
31a. `mutations.hpp`'s no-write-verb comment names `said` and `spoke`, asserted
    by substring against the source text — the amendment in REQ-NPCSTORE-2 is
    the thing a future reader checks a new verb against, so it is verified like
    the other source-text invariants rather than trusted.
32. `INSERT|UPDATE|DELETE` against either new table appears only in
    `mutations.cpp`, asserted by the suite.
33. Both assertions are checked by **mutation**: adding a profile-editing helper
    in a scratch copy turns the suite red.

### Whole-spec gates

34. No new translation unit performs network I/O; this spec adds no model call.
35. The full suite passes with no network and no API key.
36. Every existing bard, catalog-selection, materialisation, and combat test
    passes **unmodified** — this spec adds test code and edits none, which is
    checked as a property of the diff.

## Out of scope

- The `say` action, the conversation prompt, and the model call.
- Whether the narrator sees `said` / `spoke` events.
- Movement, goals, and major-character arrival.
- Regenerating a minor character's profile. It is write-once here, so a bland
  first profile is permanent — the same bet this project already makes about
  room descriptions, and the first thing to revisit if characters come out flat.
- Promoting anything a character said into the catalog. The bard sees speech in
  its wake context for free; nothing here acts on it.
