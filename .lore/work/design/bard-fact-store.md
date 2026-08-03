---
title: "The bard's fact store: lanes, catalog schema, and write rules"
date: 2026-08-03
status: approved
tags: [bard, dungeon-master, schema, fact-store, catalog, storylets, level-of-detail, placeholders, mutations, append-only, retcon]
modules: [bard, mutations, world, architect, combat]
related: [.lore/work/brainstorm/dungeon-master.md, .lore/work/design/story-seed-architect.md, .lore/work/research/drama-manager-prior-art.md, .lore/work/specs/combat-and-enemies.md, .lore/vision.md]
---

# The bard's fact store: lanes, catalog schema, and write rules

## Scope

One element of the bard feature: **where its facts live and who may change them.** Everything else downstream — the eligibility predicate, the overture hook, the wake hook, the architect wire format — depends on the catalog having a shape, so this is the gating decision.

Out of scope, deliberately: prompt text, cadence implementation, the pregen-style worker, NPC readership.

Inputs: [dungeon-master.md](../brainstorm/dungeon-master.md) (decisions 7, 11–16), [story-seed-architect.md](story-seed-architect.md) Decision 5 (the LOD ladder), [drama-manager-prior-art.md](../research/drama-manager-prior-art.md) (storylets, placeholders), and the live schema in `world.cpp`.

## Decision 1 — Two writable lanes, not three. The setting is human-authored and read-only to the bard.

The brainstorm proposed three lanes with the setting lane **append-only rather than frozen**, so the world could accumulate facts ("the east wing burned in the third week"). Testing that against the code, it should be dropped.

`seed/setting.txt` is a **design document written by a human**, not bard output. It is loaded once into `meta.setting` and read by `buildArchitectContext`. It states premise and tone — *"hushed, warm, quietly wondrous"*, the spatial front, the goblins' motive. Premises do not change; that is what makes them premises. And the disruption is already *in* it: the seed text already describes the invasion, so the staleness scenario that motivated an append path does not arise.

Dropping it removes the hardest enforcement problem in the feature. "Append but never edit" on a single text blob is not mechanically checkable — `UPDATE meta SET value = value || ?` and a full rewrite are the same statement shape. Any guarantee would have been prompt-level, i.e. not a guarantee.

**So: `meta.setting` stays immutable after `initialize()`, and the bard never writes it.** World facts that become true are already recorded in the append-only `events` log, which is the correct home for them.

*Revisit when:* a generated room needs to contradict the seed premise (an explicitly authored world-altering event), at which point the addendum lane arrives with a real consumer instead of a speculative one.

That leaves:

| Lane | Home | Written by | Mutability |
|---|---|---|---|
| **Setting** | `meta.setting` | human, via `seed/setting.txt` | immutable after init |
| **Catalog** | new `catalog` + `catalog_binding` tables | bard, via helpers | **append-only** |
| **Journal** (private) | `meta.bard_journal` | bard, via helper | free rewrite |
| **Focus** (public) | `meta.bard_focus` | bard, via helper | free rewrite |

## Decision 2 — Two freeform rows, split by who reads them. Zero DDL.

**Amended 2026-08-03 by [design 4](bard-architect-integration.md).** This section originally collapsed pressures into the journal on the grounds that both are freeform and rewritten each wake. Working through the consumer showed that was wrong: the distinction that matters is **who reads a lane**, not how it mutates. With a private journal and nothing else, a micro wake produces nothing anyone reads — its only outward effect would be occasional catalog growth, which does not justify waking at all.

Two rows, both following the `meta.setting` precedent (a new **row**, not a new **shape** — zero DDL, no bump on their account):

| Row | Contents | Read by | Cap |
|---|---|---|---|
| `meta.bard_journal` | working memory — what the bard has noticed, what it is weighing | the bard only | generous |
| `meta.bard_focus` | one short line of current story focus | **the architect**, via `buildArchitectContext` | ~300 chars |

`meta.bard_focus` is the entire channel through which a wake influences the world, and it is deliberately narrow. A short cap is a feature: it forces the bard to say one thing rather than paste its reasoning into room-generation context, and it bounds the recurring prefix the architect pays for on every generation.

The journal stays private for the reason the original text gave, which still holds: a rambling journal must never be able to leak into room prose.

## Decision 3 — The catalog is a `bestiary` for story

The catalog must be *selectable*: the engine computes an eligible menu, the model picks by name, the engine re-checks the pick. That is `bestiary` + `eligibleEnemyBlurbs` + `archetypeForEnemyBlurb`, and the parallel should be exact rather than approximate.

```sql
-- The story catalog: entries authored by the overture, materialized one at a
-- time. Mirrors `bestiary` — a frozen record the world is cast from — except
-- that rows are minted at runtime by the bard rather than seeded, and each is
-- cast AT MOST ONCE.
CREATE TABLE catalog(
  id      INTEGER PRIMARY KEY,          -- engine-minted; NEVER on the wire
  kind    TEXT NOT NULL,                -- 'character' | 'beat'
  handle  TEXT NOT NULL UNIQUE,         -- the model-facing SELECTION token
  name    TEXT NOT NULL,                -- the in-world parser noun ('scorched
                                        -- lectern') — mirrors bestiary.name;
                                        -- becomes the minted entity's name row
  blurb   TEXT NOT NULL,                -- the ONLY prose the model sees to select
  motive  TEXT NOT NULL,                -- FK motive_catalog.motive (closed vocab)
  tier    INTEGER NOT NULL,             -- placement gate vs distanceFromSeed
  seeded  INTEGER NOT NULL DEFAULT 0,   -- 1 = hinted in prose, not yet materialized
  entity  INTEGER,                      -- NULL = latent; non-NULL = MATERIALIZED
  -- A KNOWLEDGE beat asserts something TRUE about combat. Both NULL on every
  -- other entry; both non-NULL together, never one. Validated at admission
  -- against `resistance` — see below.
  fact_archetype TEXT,                  -- bestiary.archetype the fact concerns
  fact_element   TEXT                   -- element whose resistance it reveals
);

-- StoryVerse-style typed placeholders, bound at materialization and frozen.
-- INSERT-only: there is no helper that updates a binding.
--
-- DEFERRED at spec time (REQ-BARD-STORE-3): no spec in the first build ever
-- writes to this table — nothing binds a placeholder, and materialization does
-- not either — so shipping it inert through a SCHEMA_VERSION bump buys nothing.
-- It arrives with the placeholder machinery that needs it. Shape kept here so
-- that work does not have to re-derive it.
CREATE TABLE catalog_binding(
  catalog INTEGER, name TEXT,           -- 'X', 'Y' — as written in the blurb
  entity  INTEGER NOT NULL,
  PRIMARY KEY(catalog, name)
);

-- The closed motive vocabulary, authored for Thornmere. Engine-owned constants,
-- like spell_catalog: the model sees `blurb`, never the key.
CREATE TABLE motive_catalog(motive TEXT PRIMARY KEY, blurb TEXT);
```

Notes on each column:

- **`handle` selects; `name` is the noun.** `bestiary` carries the same split — `archetype` the key, `name` the instance handle, `blurb` the model-facing field — and overloading one column here would force a selection token to double as parser vocabulary. `name` becomes the minted entity's `name` row at materialization (design 4).
- **`handle` is the wire identity.** Ids never leave the process — the same discipline as `architect.hpp:12` ("Entity ids are never sent to or read from the model"). The model selects a handle; `catalogForHandle` resolves it against the *live* menu and returns 0 on a hallucinated or stale pick, exactly as `archetypeForEnemyBlurb` does.
- **`blurb` is the only prose the model sees when selecting.** `bestiary`'s comment already calls blurb "the ONLY model-facing field"; same rule here.
- **`tier` gates placement against `distanceFromSeed`**, reusing the existing spatial metric rather than inventing a second one. This is where decision 16 lands mechanically: escalation is spatial, so the catalog's only intensity dial is a distance gate. There is no clock column and no urgency column, and that absence is deliberate.
- **`seeded`** is the research finding on foreshadowing: an entry hinted in prose is costlier to retcon than one the player has never heard of. Set once, never cleared.
- **`entity`** is both the L2 marker and the binding. `NULL` → latent; non-NULL → this catalog entry *is* that world entity.

### Knowledge beats — fiction gated on mechanical truth

`discoveredResistances` (`band.cpp:472`) derives everything from the transcript: *"no cache and no shadow table"*, only elements the player has actually cast at that archetype are known, and deleting a fight's events erases what it taught. Mechanically clean — and it means the **only** way to learn a weakness is to stand in front of the thing spending turns and taking chip damage while experimenting. In a school of restless books and doors with opinions, nobody warns you and nothing is written down.

A `beat` with `fact_archetype` / `fact_element` set is the fix: a scorched study where someone clearly tried the wrong element, a portrait that mutters about the rime-touched, a page torn from a bestiary. It grants **nothing mechanical** — `discoveredResistances` still derives from the transcript, and the player still has to cast the spell — but they walk in knowing which one to try.

The reason this is worth schema rather than prose: **the engine can verify the fiction is true.** A bard that writes *"the rime-touched fear fire"* when fire is resisted is lying to the player about the rules. `writeCatalogEntry` refuses the row — the same shape as `placeEnemy` throwing on an unknown archetype, and the same discipline as the narrator's canon-verbatim gate. A catalog entry cannot promise a falsehood about the mechanics.

Three admission rules, all mechanical:

1. `fact_archetype` and `fact_element` are both NULL or both non-NULL — never one.
2. `fact_archetype` must have a `bestiary` row; `fact_element` must appear in `spell_catalog.element`.
3. The pair must be **materially true**: either a `resistance` row exists for it, or its absence means neutral (`x1`) and the blurb must not claim a weakness. The engine knows which, because `resistance` is engine-owned constants.

This also gives `seeded` real teeth. A weakness the player has *heard about* is a promise the world made out loud; breaking it is detectably worse than breaking one they never encountered.

### The LOD ladder, without a level column

Story-seed Decision 5 is explicit: *"LOD levels are derived from which rows exist — no `level` column."* Honored:

| Tier | Rows present | Meaning |
|---|---|---|
| **L0 catalog** | `catalog` row, `entity IS NULL`, no `sketch` row | exists in potential only |
| **L1 sketched** | + a `sketch` row keyed by an entity the bard reserved | fleshed out, not yet met |
| **L2 materialized** | `catalog.entity` non-NULL | in the world; canon |

L1 requires the reserved `sketch` lane, which is still deferred. **This design does not build L1** — the catalog goes L0 → L2 in one step, the way the architect's rooms currently go latent-exit → realized in one step. When `sketch` lands, L1 slots in with no change to this schema.

## Decision 4 — Append-only is enforced by the absence of a code path

The retcon brake in the brainstorm's *Unresolved* section was stated as a rule ("append-only, records its own breakage"). A rule the bard is asked to follow is not a guarantee. The mechanical form:

**There is no helper that updates `kind`, `handle`, `blurb`, `motive`, or `tier`.** The bard cannot rewrite a catalog entry because no code path exists to do it. Corrections are expressed the only way the schema permits — by appending a *new* entry, leaving the original visible and un-materialized. Drift becomes detectable in the table rather than absorbed into it, which is the error-accumulation brake the AI Dungeon research argues for.

Two fields do change, and both are **one-way latches** with the guard in the SQL:

```sql
UPDATE catalog SET entity = ? WHERE id = ? AND entity IS NULL;   -- materialize
UPDATE catalog SET seeded = 1 WHERE id = ? AND seeded = 0;       -- seed
```

Each transitions once and can never transition back. A second call is a silent no-op by construction, not by discipline — the same shape as `learnSpell`'s idempotence.

### The write path

<svg viewBox="0 0 640 250" width="100%" style="max-width:640px;font-family:system-ui,sans-serif;font-size:11px">
  <rect x="8" y="14" width="150" height="54" rx="6" fill="none" stroke="#555"/>
  <text x="83" y="34" text-anchor="middle" fill="#333">bard.cpp</text>
  <text x="83" y="49" text-anchor="middle" fill="#777">SELECT + network only</text>
  <text x="83" y="62" text-anchor="middle" fill="#777">grep INSERT|UPDATE = ∅</text>

  <rect x="230" y="14" width="150" height="54" rx="6" fill="none" stroke="#555"/>
  <text x="305" y="36" text-anchor="middle" fill="#333">mutations.cpp</text>
  <text x="305" y="52" text-anchor="middle" fill="#777">the only write path</text>

  <line x1="158" y1="41" x2="228" y2="41" stroke="#555"/>
  <polygon points="228,41 220,37 220,45" fill="#555"/>
  <text x="193" y="33" text-anchor="middle" fill="#777">proposes</text>

  <rect x="452" y="0" width="180" height="34" rx="6" fill="none" stroke="#555"/>
  <text x="542" y="21" text-anchor="middle" fill="#333">catalog — append-only</text>

  <rect x="452" y="44" width="180" height="34" rx="6" fill="none" stroke="#555"/>
  <text x="542" y="65" text-anchor="middle" fill="#333">bard_journal + bard_focus — rewrite</text>

  <rect x="452" y="88" width="180" height="34" rx="6" fill="none" stroke="#555"/>
  <text x="542" y="109" text-anchor="middle" fill="#333">events — append-only</text>

  <line x1="380" y1="35" x2="450" y2="18" stroke="#555"/><polygon points="450,18 441,18 444,25" fill="#555"/>
  <line x1="380" y1="45" x2="450" y2="60" stroke="#555"/><polygon points="450,60 441,54 441,61" fill="#555"/>
  <line x1="380" y1="55" x2="450" y2="102" stroke="#555"/><polygon points="450,102 441,95 438,102" fill="#555"/>

  <rect x="452" y="140" width="180" height="34" rx="6" fill="none" stroke="#999" stroke-dasharray="4 3"/>
  <text x="542" y="161" text-anchor="middle" fill="#777">meta.setting — immutable</text>

  <path d="M452 105 C 300 150, 200 130, 83 78" fill="none" stroke="#999" stroke-dasharray="4 3"/>
  <polygon points="83,78 90,86 95,79" fill="#999"/>
  <text x="270" y="127" text-anchor="middle" fill="#777">reads back (the feedback loop)</text>

  <path d="M452 157 C 320 200, 190 130, 83 74" fill="none" stroke="#999" stroke-dasharray="4 3"/>

  <text x="8" y="205" fill="#777">Solid = writes. Dashed = reads. The bard never touches world state — only catalog, journal, and its own event rows.</text>
  <text x="8" y="222" fill="#777">Corrections append a new catalog row; nothing rewrites an old one.</text>
</svg>

### Helpers to add to `mutations.hpp`

```cpp
// Mint one catalog entry (the overture's bulk write, and the micro wake's
// append path). INSERT-only: there is deliberately NO helper that edits an
// existing entry's kind/handle/blurb/motive/tier — appending a corrected entry
// is the only way to change the bard's mind, so drift stays visible.
// Throws if `motive` has no motive_catalog row (engine fault: the menu is closed).
//
// THE TRUTH GATE. `factArchetype`/`factElement` are both empty (an ordinary
// entry) or both set (a knowledge beat). When set, the entry is REFUSED unless
// the archetype exists in `bestiary`, the element appears in `spell_catalog`,
// and the claim is materially true against `resistance`. A catalog entry may
// not promise a falsehood about the rules — the same discipline as the
// narrator's canon-verbatim gate, applied to foreshadowing.
//
// Event-free — a latent entry has not happened. Returns the minted id.
int64_t writeCatalogEntry(Db& db, const std::string& kind,
                          const std::string& handle, const std::string& name,
                          const std::string& blurb, const std::string& motive,
                          int64_t tier,
                          const std::string& factArchetype = "",
                          const std::string& factElement = "");

// Bind one placeholder to a world entity. INSERT-only, so a binding is frozen
// the moment it is made (REQ: StoryVerse placeholder semantics). Event-free —
// recorded by the paired 'materialized' event.
void bindCatalogPlaceholder(Db& db, int64_t catalog, const std::string& name,
                            int64_t entity);

// THE L0 -> L2 transition, and the sole writer of the 'materialized' verb.
// Latches catalog.entity (WHERE entity IS NULL, so it fires at most once) AND
// appends the event, both in the caller's tick transaction — one call, one
// fact. Returns false if the entry was already materialized.
bool materializeCatalogEntry(Db& db, int64_t catalog, int64_t entity,
                             int64_t actor);

// Latch `seeded` (WHERE seeded = 0): this entry has now been hinted in prose.
// Event-free bookkeeping.
void markCatalogSeeded(Db& db, int64_t catalog);

// Upsert meta.bard_journal — the bard's PRIVATE working memory, read by nothing
// else, so free rewrite is safe.
void writeBardJournal(Db& db, const std::string& text);

// Upsert meta.bard_focus — the SHORT public line the architect reads via
// buildArchitectContext (design 4). Free rewrite like the journal, but truncated
// to the cap: this string is paid for on every room generation, and an
// unbounded one would quietly become the largest term in that context.
void writeBardFocus(Db& db, const std::string& text);
```

`writeCatalogEntry` is the one place the codebase's usual "every mutation gets an event row" convention is deliberately relaxed, and the comment says why: a latent catalog entry has not *happened*. It becomes an event when it materializes. `dropGrimoire` and `placeEnemy` already set this precedent — both mint entities and emit no event of their own.

## Decision 5 — Materialization and the wake trigger are one fact

The brainstorm's decision 14 claims that "an irreversible event *is* a materialization." That has to be true in the code, not just in the prose, or there are two bookkeeping systems that will drift.

`materializeCatalogEntry` writes the latch **and** appends the event in one helper, exactly as `writeGeneratedRoom` writes the room rows and the `generated` event together. The wake-trigger predicate then reads the event log and nothing else:

```sql
SELECT 1 FROM events
 WHERE turn > ?                       -- since the bard's last wake
   AND verb IN ('generated','defeated','learned','materialized')
 LIMIT 1;
```

One new verb, `materialized` — needed because a catalog entry can materialize into an *existing* room, producing no `generated`. Subject = the world entity, object = the catalog id, detail = the handle.

A room generated *with* a materialized character in it fires both `generated` and `materialized` in the same tick. That is not double-counting: the coalescing rule (decision 11) means one wake either way.

## Decision 6 — The Thornmere motive vocabulary

Apocalypse World's eight scarcities (hunger, thirst, envy, ambition, fear, ignorance, decay, despair) are a wasteland vocabulary and wrong for a school that has "gone soft and scholarly." Authored replacements, drawn from what `seed/setting.txt` actually contains:

| motive | blurb (model-facing) |
|---|---|
| `curiosity` | wants to know something they have not been told |
| `secrecy` | has something to keep hidden, and is arranging for it to stay that way |
| `rivalry` | wants to be first, or to be seen to be first |
| `obligation` | is bound by a duty they did not choose |
| `grief` | is holding on to someone or something already gone |
| `appetite` | wants to take and carry off — the goblins' motive |
| `pride` | would rather be wrong than corrected |
| `homesickness` | does not belong here yet, and feels it |

Eight, closed, engine-enforced by the `motive_catalog` table. `writeCatalogEntry` throws on an unknown motive, so the bard cannot invent a ninth. Values are a starting set to be tuned against real output, not a claim of completeness.

## Decision 7 — Read side: mirror the combat menu exactly

Not built here, but the schema must support it, so the signatures are pinned now:

```cpp
// The eligible catalog choices for `room`, each rendered as its HANDLE + BLURB.
// Read-only. Gates compose, and NONE of them is temporal (decision 16):
//   - not already materialized (entity IS NULL);
//   - tier <= distanceFromSeed(room) — the SAME spatial metric the bestiary
//     menu uses, so story and combat escalate on one dial rather than two.
// Deterministic order by id. Empty is a valid, common answer.
//
// `kind` is 'character' or 'beat'. There is deliberately no 'lock' kind and no
// key-reachability gate: "enemies are locks, spells are keys" is COMBAT's model
// (specs/combat-and-enemies.md) and is not generalized here. Thornmere's
// non-combat affordances — moving staircases, doors with opinions, restless
// books — are navigational, social, and knowledge-shaped, not lock-shaped.
std::vector<std::string> eligibleCatalogHandles(Db& db, int64_t room,
                                                const std::string& kind);

// Resolve a model-selected handle back to a catalog id, ONLY if it is currently
// eligible — the engine re-checks the menu authoritatively, so a hallucinated
// or stale selection resolves to 0 and places nothing.
int64_t catalogForHandle(Db& db, int64_t room, const std::string& handle);
```

The storylet literature's sharpest warning applies here and should be repeated in the eventual spec: **eligibility must come from general predicates, never per-entry unlock flags.** Short's "time cave" failure is a catalog with a bespoke condition per row — hand-authored branching wearing a catalog costume. The three gates above are general by construction; a `catalog.unlock_condition TEXT` column would not be, which is why there isn't one.

## Schema impact

Three new tables and one new `meta` row. `meta.bard_journal` is free (a row, not a shape); the tables are not.

**`SCHEMA_VERSION` 5 → 6.** Migration story is unchanged and already stated in `world.cpp`: delete the world file and let it recreate from seed. Content is disposable at this stage.

`motive_catalog` is seeded in `base.sql` alongside `spell_catalog` and `bestiary`, since it is engine-owned constants rather than runtime data.

## What this design does not settle

- **What a non-combat puzzle actually is in Thornmere.** Open, and deliberately not answered here. An earlier draft asserted a lock/key model generalized from combat and called it blocking; both were wrong — nothing is blocked, because the catalog ships with `character` and `beat` only. When a puzzle `kind` is added it should follow from the setting's own affordances rather than from combat's schema.
- **The `sketch` lane / L1.** Deferred with the rest of LOD. The catalog goes L0 → L2 in one step until it lands.
- **Overture and wake call sites.** `openWorld` must report whether it just created the world so `main.cpp` can run the overture exactly once; the wake hook mirrors `architectQueuePregen`'s post-commit placement. Both are mechanical and belong in the plan, not here.
- **Retcon, still.** This design makes rewriting *impossible* rather than *discouraged*, which is stronger than the brainstorm's proposal. It does not stop the bard appending a new entry that contradicts an old one — only makes the contradiction visible in the table. Detection is a later concern; suppression is not attempted.

## Decision summary

1. **Two writable lanes, not three.** `meta.setting` is human-authored and immutable; the bard never writes it. The append-path idea is dropped as unenforceable and unmotivated.
2. **Two freeform `meta` rows, split by readership** (amended by design 4) — `meta.bard_journal` private, `meta.bard_focus` short and read by the architect. Zero DDL for both.
3. **`catalog` + `catalog_binding` + `motive_catalog`** — a `bestiary` for story: engine-minted ids, model-facing handles and blurbs, ids never on the wire, and a separate `name` for parser vocabulary.
4. **Append-only enforced by absent code paths**, with `entity` and `seeded` as SQL-guarded one-way latches. Corrections append; nothing rewrites.
5. **`materializeCatalogEntry` writes the latch and the `materialized` event together**, so the L0→L2 transition and the wake trigger are one fact.
6. **Eight closed Thornmere motives** in an engine-owned constants table.
7. **Knowledge beats carry `fact_archetype` / `fact_element`, validated at admission against `resistance`.** The bard can make a combat weakness learnable through fiction instead of only through taking damage — and cannot assert a weakness that is not mechanically true. Combat stays the only puzzle system; the catalog only makes it teachable.
8. **LOD by row presence, no level column**, honoring story-seed Decision 5; L1 deferred.
9. **`SCHEMA_VERSION` 5 → 6.**
