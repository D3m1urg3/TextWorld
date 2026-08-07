---
title: "Catalog selection and the model interface"
date: 2026-08-03
status: implemented
tags: [bard, dungeon-master, catalog, eligibility, tool-use, validation-gate, wire-format, storylets, degradation]
modules: [bard, combat, architect, mutations]
related: [.lore/work/design/bard-fact-store.md, .lore/work/brainstorm/dungeon-master.md, .lore/work/research/drama-manager-prior-art.md, .lore/work/specs/combat-and-enemies.md, .lore/work/specs/ai-prose-renderer.md]
---

# Catalog selection and the model interface

## Scope

Design 2 of 4 for the bard. Covers **everything about what the model sees and what the engine accepts back**: the eligibility gates that build the menu, the wire format, the tool schemas for both call shapes, the validation gate, and the failure posture.

Depends on [bard-fact-store.md](bard-fact-store.md) for the schema. Does not cover *when* the bard runs (design 3) or how its output reaches room generation (design 4).

## Decision 1 — Eligibility composes the same way combat's does

`eligibleArchetypes` is the model to copy, not merely to imitate: three independent gates that compose, a deterministic order, and an empty result treated as a normal answer rather than an error.

```cpp
// The eligible catalog choices for `room`, each rendered as HANDLE + BLURB.
// Read-only, deterministic, O(catalog size). Empty is valid and common.
std::vector<CatalogChoice> eligibleCatalog(Db& db, int64_t room,
                                           const std::string& kind);
```

[Design 4](bard-architect-integration.md) adds the prospective-room companion `eligibleCatalogForNewRoom(db, originRoom)` — distance is the origin's plus one — mirroring how `eligibleEnemyBlurbs` relates to `eligibleArchetypes`.

Gates, all of which must hold:

1. **Not already materialized** — `entity IS NULL`. A catalog entry is cast at most once, the L0→L2 latch from design 1.
2. **Spatially reachable** — `tier <= distanceFromSeed(room)`. The same BFS metric `eligibleArchetypes` uses for front intensity, so **story and combat escalate on one dial**. This is decision 16 in mechanical form: the only intensity gate is distance, and there is deliberately no time term.
3. **Kind matches** the caller's request (`character` or `beat`).
4. **Knowledge beats additionally require a live subject.** A beat carrying `fact_archetype` is offered only if that archetype appears in `eligibleArchetypes` for this room or an adjacent one. Foreshadowing a rime-touched weakness where rime-touched can never appear is noise, and worse, it is a promise the world may never get to keep.

Gate 4 is the one genuinely new composition, and it is a *reuse*: it calls the combat menu rather than reimplementing its logic, so the two can never drift.

Ordered by `catalog.id` for determinism and replayability, mirroring the combat menu's ordering rule.

### What "eligible" must never mean

The research's sharpest warning, worth carrying into the spec verbatim: eligibility must come from **general predicates, never per-entry unlock flags**. Emily Short's "time cave" failure is a catalog with a bespoke condition per row — hand-authored branching wearing a catalog costume, and it "eliminates most of the value of using storylets."

All four gates above are general by construction. There is no `catalog.unlock_condition` column, and adding one would be the moment this design stops being a storylet system.

## Decision 2 — The wire carries handles and blurbs, never ids or numbers

Inherited discipline, stated for this unit. `architect.hpp:12`: *"Entity ids are never sent to or read from the model."* `bestiary`'s schema comment: blurb is *"the ONLY model-facing field."*

| Column | On the wire? | Why |
|---|---|---|
| `handle` | **yes** | the selection token; the model returns it verbatim |
| `blurb` | **yes** | the only prose it selects on |
| `motive` | **yes, as its blurb** | `motive_catalog.blurb`, never the key |
| `id`, `entity` | never | engine identity |
| `tier` | never | a placement number; exposing it invites the model to reason about difficulty budgets |
| `fact_archetype`, `fact_element` | never raw | surfaced only as the beat's own prose |
| `seeded` | never | bookkeeping |

`tier` deserves the explicit call-out. It is tempting to send it "so the model understands stakes," and that is exactly the mistake `bestiary` already refuses by sending blurbs instead of stats — a model that sees numbers starts optimizing them.

## Decision 3 — Two call shapes, three tools

### The overture: one bulk write

```
tool: write_catalog
  entries: array of {
    kind:   enum["character","beat"]        -- schema-enforced
    handle: string
    blurb:  string
    motive: enum[<the eight motive keys>]   -- schema-enforced
    tier:   integer
    fact:   optional { archetype: string, element: string }
  }
  journal: string                            -- the initial journal
```

`kind` and `motive` are **schema-enforced enums**, exactly as the architect's optional `enemy` field is constrained to the eligible blurb list. The closed vocabulary is then enforced twice — once by the tool schema so the model cannot emit a ninth motive, and once by `writeCatalogEntry` throwing, so a hand-rolled or replayed request cannot either. Belt and braces, matching how enemy selection already works.

`fact` is where the truth gate from design 1 fires. The model may *propose* a weakness; the engine checks it against `resistance` and refuses the row if it is false.

### The micro wake: focus, optionally append, always journal

**Amended 2026-08-03 by [design 4](bard-architect-integration.md).** This section originally carried a `place_catalog { handle }` tool. It has been **removed**: placement requires a room, entity ids never go on the wire, and room names are neither unique nor identifiers — so the bard has no way to express *where*. Placement belongs to the architect, which always has a room in hand (design 4, Finding A). `write_focus` replaces it as the wake's channel to the world.

```
tool: write_focus       { text: string }          -- REQUIRED; the architect reads this
tool: write_journal     { text: string }          -- required; private working memory
tool: append_catalog    { entry: <as above> }     -- optional; the catalog grows
tool: mark_seeded       { handle: string }        -- optional; this entry has been hinted
```

Small tools rather than one large one, so a partial response is still useful: a wake that writes focus but fails to journal has still influenced the world, and one that journals but appends nothing is a legitimate turn — the bard looked and decided to wait.

`tool_choice` is `auto` — unlike the architect, which *requires* its tool because a non-call is a gate failure. The bard declining to act is a normal outcome and must not be treated as an error.

## Decision 4 — Validation is two-phase, pure then live

Mirrors `validateRoomProposal` + `archetypeForEnemyBlurb`, which are already the two halves of this problem in the architect.

**Phase A — pure.** No DB, never throws, callable from tests with a canned `HttpResponse`:

- status 200 and a well-formed body;
- each entry has non-empty `handle` and `blurb` after trim;
- `kind` and `motive` are in the closed vocabularies (the enum is belt; this is braces);
- `tier` is a non-negative integer;
- `fact` is absent, or has both fields non-empty — never one.

**Phase B — live, against the DB.** `catalogForHandle(db, room, handle)` re-checks the selection against the **current** menu and returns 0 on a hallucinated, stale, or already-materialized handle. Verbatim the `archetypeForEnemyBlurb` contract: *"the engine re-checks the menu authoritatively, so a hallucinated, stale, or empty selection resolves to '' and places nothing."*

Phase B matters more here than for enemies, because the bard's proposal may have been snapshotted turns before it commits (design 3's worker). A handle that was eligible at snapshot time may have been materialized since.

### Lenient where a strict gate would be worse

The architect already draws this line: name and description are strict (a missing one rejects the room), but exits are lenient — *"a bad exit NEVER rejects a room"*, each bad entry dropped with one stderr diagnostic.

Applied here, and the asymmetry is deliberate:

| Failure | Response |
|---|---|
| One bad entry in an overture's bulk write | **drop that entry**, keep the rest, one diagnostic |
| A `fact` that fails the truth gate | **drop that entry** (not just the fact) — a knowledge beat whose knowledge is false has no remaining purpose |
| The whole overture response is malformed | fall back to an **empty catalog** |
| `mark_seeded` names an unknown handle | ignore that call; the wake is otherwise valid |
| No tool call at all on a micro wake | a normal outcome, not an error |
| The architect's `story.handle` is stale or hallucinated | resolves to 0; the room is created with no story entry (design 4) |

Rejecting an entire overture over one malformed entry would leave the game with no story at all — strictly worse than a story with nine entries instead of ten. The architect makes the same trade for the same reason.

## Decision 5 — Every failure degrades to today's game

The load-bearing property, inherited from every AI unit in this codebase: the prose renderer falls back to templates, the resolver falls back to the fixed parser, the architect falls back to the wall.

| Failure | Result |
|---|---|
| Overture never runs (AI off, no key, network down) | empty catalog; architect sees `meta.setting` exactly as it does today |
| Overture returns nothing valid | empty catalog; identical to above |
| A micro wake fails | catalog unchanged; the next irreversible event triggers another |
| Every micro wake fails, all session | the game is today's game plus an unused overture catalog |

**There is no configuration in which the bard failing makes the game worse than not having a bard.** That is a testable claim, and design 3 owns keeping it true when threads are involved.

The catalog being empty must therefore be a *supported* state everywhere, not a degenerate one — `eligibleCatalog` returning empty is the same normal answer `eligibleEnemyBlurbs` gives on a safe-edge room.

## Decision 6 — The bard's translation unit stays read-only

`bard.cpp` performs only SELECTs and network egress. Every write goes through the design-1 helpers in `mutations.cpp`. The mechanical check joins the two the build already runs:

```
grep -En "INSERT|UPDATE|DELETE" src/bard.cpp    -> empty
```

Same contract as `architect.cpp`, and for the same reason: it makes "the model proposes, the engine disposes" structural rather than remembered.

## Open questions

- **Does the micro wake see the full eligible menu, or a slice?** The menu is O(catalog size) and the catalog is authored once, so it is bounded and probably small enough to send whole. But if an overture writes 40 entries and 30 are eligible, that is a large recurring prefix — and it is also exactly the shape prompt caching now rewards on Opus 4.8 at the 1024-token minimum. Worth measuring before capping.
- **Should `append_catalog` be available on every wake, or only some?** Unrestricted, the bard can grow the catalog indefinitely and dilute the overture's coherence. Restricted, it cannot respond to genuinely novel player behavior. No principled answer yet; the cheap first move is to allow it and count appends per session as a profile field.
- **Where does the journal's size get bounded?** It is rewritten wholesale each wake, so it cannot grow unboundedly by accident — but nothing stops the model writing 4k tokens of it either. A max length belongs in the tool schema, and the number is a guess until there is real output.

## Decision summary

1. **Four composing eligibility gates** — unmaterialized, `tier <= distanceFromSeed`, kind, and (for knowledge beats) a live subject via `eligibleArchetypes`. General predicates only; no per-entry unlock flags, ever.
2. **Handles and blurbs on the wire; never ids, `tier`, or raw fact columns.**
3. **Small tools, not one large one** — bulk `write_catalog` for the overture; `write_focus` / `write_journal` / `append_catalog` / `mark_seeded` for wakes. `kind` and `motive` are schema-enforced enums. `tool_choice: auto` — declining to act is normal. **`place_catalog` was removed by design 4**: the bard cannot name a room, so it cannot place.
4. **Two-phase validation** — a pure gate mirroring `validateRoomProposal`, then a live menu re-check mirroring `archetypeForEnemyBlurb`. The re-check matters most on the architect's `story.handle`, which may have been snapshotted many turns before it commits.
5. **Lenient per-entry, strict per-response** — one bad entry is dropped, never the whole overture; a false `fact` drops its entry entirely.
6. **Every failure path degrades to today's game**, and the empty catalog is a supported state rather than a degenerate one.
7. **`bard.cpp` is read-only by grep-checkable contract.**
