---
title: "Architect integration: how story reaches the world"
date: 2026-08-03
status: approved
tags: [bard, dungeon-master, architect, materialization, world-gen, nouns-must-exist, catalog, integration, amendments]
modules: [architect, bard, mutations, combat]
related: [.lore/work/design/bard-fact-store.md, .lore/work/design/bard-catalog-selection.md, .lore/work/design/bard-overture-and-scheduling.md, .lore/work/design/story-seed-architect.md, .lore/work/specs/ai-prose-renderer.md]
---

# Architect integration: how story reaches the world

## Scope

Design 4 of 4. Covers the seam where catalog intent becomes world state: what the architect reads, what its tool gains, how an entry materializes, and who is allowed to place.

**This design amends designs 1 and 2.** Working through the consumer exposed two errors in the producer, both recorded below rather than quietly patched.

## The two findings, up front

**Finding A — the bard cannot place, because it cannot name a room.** Design 2 gives the micro wake a `place_catalog { handle }` tool. But placement needs a *room*, and entity ids never go on the wire — that discipline is absolute in this codebase. The bard has no way to say "put her in the library." Room names are not unique and are not identifiers.

The resolution is not to relax the id rule. It is that **placement belongs to the architect**, which always has a room in hand and is already writing prose for it. The bard maintains the catalog and its own state; the world is changed by the unit that was already changing it.

**Finding B — which leaves the wake with no external effect.** If the architect selects from the eligible menu on its own, and the journal is private (design 1, decision 2), then a micro wake produces *nothing anyone reads*. Its only outward effect would be occasional catalog growth. That is not enough to justify waking at all.

So design 1 over-collapsed. The pressures lane was folded into the journal on the grounds that both are freeform and rewritten each wake — but the distinction that matters is **who reads them**, not how they mutate. Restored, in a more defensible form:

| Lane | Home | Read by |
|---|---|---|
| Private journal | `meta.bard_journal` | the bard only |
| **Public focus** | `meta.bard_focus` | **the architect**, via `buildArchitectContext` |

`meta.bard_focus` is a short string (cap it — 300 chars is a starting guess), rewritten each wake, and it is the entire channel through which a wake influences the world. Still a `meta` row, still zero DDL.

## Decision 1 — Both kinds materialize as entities

A `character` obviously becomes an entity. A `beat` — a scorched study, a torn bestiary page — is more tempting to treat as prose folded into the room description. That would be wrong, and the prose renderer design already says why: **nouns must exist.**

If a room's canon prose mentions a scorched lectern and no lectern entity exists, the player types `examine lectern` and gets refused. That is the invented-affordance failure the renderer was designed to prevent, and materializing beats as prose would reintroduce it deliberately.

So both kinds mint an entity with `name`, `description`, and `location` rows — scenery you can look at, in the `beat` case. This also keeps the design-1 latch honest: `catalog.entity` is non-NULL because there genuinely is an entity.

*Consequence:* the knowledge in a knowledge-beat is discovered by **examining** something, which is a real interaction rather than a wall of room text.

## Decision 2 — The catalog supplies *who*; the architect supplies *how it looks here*

`placeEnemy` writes "a description row = the archetype blurb," reusing the selection text as prose. That is acceptable for a goblin cast from a mold. It is poor for story: a blurb written to help a model *choose* ("a student who keeps something hidden in the deep stacks") reads oddly as room prose.

The architect is already generating prose for this room in this call. So it writes the instance description too, and the catalog contributes identity:

```
tool: create_room
  name:        string                       -- existing
  description: string                       -- existing
  exits:       optional [string]            -- existing
  enemy:       optional enum[<blurbs>]      -- existing
  story:       optional {                   -- NEW
    handle:      enum[<eligible handles>]   -- schema-enforced, like `enemy`
    description: string                     -- how this entry appears in THIS room
  }
```

This is the macro/micro split one level down: the catalog says who and why, the architect says how they appear *here*. It costs no extra call — the prose is generated in the room's own request.

`handle` is a schema-enforced enum over the eligible menu, exactly as `enemy` is constrained to eligible blurbs. The model cannot name an entry that is not on offer.

### Amendment to design 1: the catalog needs a `name`

`handle` is the unique, model-facing selection token. The entity also needs a parser noun — `bestiary` carries both (`archetype` the key, `name` the instance handle, `blurb` the model-facing field), and the catalog should mirror that rather than overload one column.

```sql
ALTER: catalog gains  name TEXT NOT NULL   -- the in-world noun, e.g. 'scorched lectern'
```

Written by the bard at authoring time, validated non-empty at admission, and used as the minted entity's `name` row.

## Decision 3 — At most one story entry per generated room

Mirrors the enemy rule: `enemy` is at most one, and `story` is at most one. Keeps density legible, keeps the tool schema flat, and keeps the commit path a single re-check rather than a loop.

A room may contain both an enemy and a story entry — they are independent gates, and "a goblin rifling through the scorched study" is a better room than either alone.

## Decision 4 — Context grows by one menu and one line

`buildArchitectContext` today is deliberately O(1): setting + origin room name + canon description + direction. It gains:

- `meta.bard_focus` — one short line;
- the eligible catalog menu for the **prospective** room, as handle + blurb + motive blurb.

The prospective-room detail matters and has precedent: `eligibleEnemyBlurbs(db, originRoom)` notes that "the prospective room is one hop past the origin, so its front distance is the origin's plus one." The catalog menu needs the same treatment, and should mirror the naming:

```cpp
// Existing rooms.
std::vector<CatalogChoice> eligibleCatalog(Db& db, int64_t room, const std::string& kind);

// The room the architect is about to create beyond `originRoom` — distance is
// the origin's plus one. Mirrors eligibleEnemyBlurbs' relationship to
// eligibleArchetypes.
std::vector<CatalogChoice> eligibleCatalogForNewRoom(Db& db, int64_t originRoom);
```

**Context is no longer O(1)** — it is now O(eligible catalog), bounded by a catalog authored once at the overture. That is a real change to a stated property of `buildArchitectContext` and should be documented there rather than allowed to erode silently. It is also a large, stable, recurring prefix, which is exactly the shape prompt caching rewards at Opus 4.8's 1024-token minimum; worth measuring `cache_read_input_tokens` once this lands.

## Decision 5 — Materialization happens in Phase 2, beside the enemy

`architectCommitProposal` is already the place where a proposal becomes canon: mint the room, realize the origin exit, plant the reciprocal and latent stubs, append `generated`, then **re-check the enemy blurb against the live menu** and `placeEnemy` + `recordArchitectSpawn`.

Story placement slots in immediately after, with the same shape:

1. re-check `story.handle` against the **live** `eligibleCatalogForNewRoom` menu → `catalogForHandle`; a stale or hallucinated handle resolves to 0 and places nothing;
2. mint the entity; write `name` (from `catalog.name`), `description` (from the architect's `story.description`), and `location` (the new room);
3. `materializeCatalogEntry` — latch `catalog.entity`, append the `materialized` event.

Steps 2 and 3 belong in one new mutation helper:

```cpp
// Materialize one catalog entry into `room`: mint the entity, write its
// name/description/location rows, then latch catalog.entity and append the
// 'materialized' event. `description` is the ARCHITECT's instance prose, not
// the catalog blurb. Returns the minted entity id, or 0 if the entry was
// already materialized (the latch lost a race). Never begins/commits.
int64_t placeCatalogEntry(Db& db, int64_t catalog, int64_t room,
                          const std::string& description, int64_t actor);
```

Placing it in Phase 2 inherits the property that makes the architect safe: Phase 2 is **outside the catch**, so a genuine DB fault propagates to the tick's rollback rather than being downgraded to a silent wall. And the same code runs for a pre-generated candidate and a synchronously generated one, so a story entry cannot behave differently depending on whether pregen hit.

**The live re-check is not optional here.** A pregen candidate may have been snapshotted many turns before it commits, and its chosen entry may have been materialized elsewhere since. The latch (`WHERE entity IS NULL`) is the second line of defence; the menu re-check is the first.

## Decision 6 — The bard never places; placement into existing rooms is deferred

Following from Finding A. In v1, an entry materializes **only** when a room is generated. The bard's wake tools reduce to:

- `append_catalog` — grow the catalog;
- `write_journal` — private state;
- `write_focus` — the one line the architect reads;
- `mark_seeded` — record that an entry has been hinted in prose.

`place_catalog` from design 2 is **removed**.

**The limitation this creates, stated plainly:** once the map is fully explored, no new story materializes. Room generation is the only door, and it eventually stops.

The deferred fix, designed in shape only: a `catalog.pending` flag the bard sets, plus an engine rule that places the highest-priority pending entry when the player *enters* a room satisfying the eligibility gates. That gives placement into existing rooms with no room ids on the wire — the engine chooses the room, the bard chose the entry. Not built now; the shape is recorded so the schema does not have to change to accommodate it later.

## Amendments this design makes

| Design | Was | Now |
|---|---|---|
| 1 — fact store | pressures folded into the private journal | split: `meta.bard_journal` (private) + `meta.bard_focus` (read by the architect) |
| 1 — fact store | `handle` only | add `catalog.name` — the in-world parser noun, mirroring `bestiary.name` |
| 2 — selection | `place_catalog` tool on the micro wake | removed; replaced by `write_focus` + deferred `pending` flag |

Designs 1 and 2 should be edited to match before either becomes a spec.

## Open questions

- **Should `meta.bard_focus` also reach the narrator?** A line of current story tone would let room prose carry mood. But the narrator's contract is "no claim without a sourcing event row," and a focus line is not an event — it is exactly the kind of input that produces invented affordances. Leaning no, and it is a one-line change to reverse if prose feels flat.
- **What happens to a catalog entry that is never eligible anywhere?** Written at the overture, gated by `tier`, and possibly never offered because the player never goes deep enough. It sits latent forever. Harmless, but if it is common the overture is writing at the wrong tiers, and only a profile count will reveal that.
- **Does `mark_seeded` need to be a tool at all?** The narrator, not the bard, is what actually mentions a thing in prose — so seeding arguably ought to be detected rather than declared. Detecting it means matching prose against catalog handles, which is fuzzy and fragile. A declared flag is cruder and honest.

## Decision summary

1. **Both `character` and `beat` materialize as entities** with name/description/location — nouns must exist, or `examine` gaslights the player.
2. **The catalog supplies identity; the architect writes the instance prose**, via a new optional `story { handle, description }` field on `create_room`, with `handle` a schema-enforced enum over the eligible menu.
3. **At most one story entry per generated room**, mirroring `enemy`; both may appear together.
4. **`buildArchitectContext` gains `meta.bard_focus` and the prospective-room catalog menu**, and is no longer O(1) — documented, not eroded.
5. **Materialization happens in `architectCommitProposal` Phase 2**, beside enemy placement, with a live menu re-check plus the `WHERE entity IS NULL` latch, through a new `placeCatalogEntry` helper.
6. **The bard never places.** Its wake tools are `append_catalog`, `write_journal`, `write_focus`, `mark_seeded`. Placement into existing rooms is deferred with its shape recorded.
7. **Two amendments to design 1 and one to design 2**, listed above, to be applied before specs are written.
