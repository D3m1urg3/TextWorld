---
title: "Bard architect integration: materializing story into rooms"
date: 2026-08-03
status: implemented
tags: [bard, architect, materialization, world-gen, nouns-must-exist, catalog, pregen, context]
modules: [architect, bard, mutations, pregen]
related: [.lore/work/design/bard-architect-integration.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/bard-catalog-selection.md, .lore/work/specs/background-room-pregeneration.md, .lore/work/specs/ai-prose-renderer.md]
req-prefix: BARD-ARCH
---

# Bard architect integration: materializing story into rooms

The seam where catalog intent becomes world state. This is the only bard brick that modifies **existing, working code** — `buildArchitectContext`, `buildArchitectRequestBody`, `validateRoomProposal`, and `architectCommitProposal` — so its first obligation is not regressing the architect.

Depends on all three prior specs.

Source design: [bard-architect-integration.md](../design/bard-architect-integration.md).

## A. Context

**REQ-BARD-ARCH-1.** `buildArchitectContext` additionally carries `meta.bard_focus` and the eligible catalog menu for the **prospective** room, rendered as handle + blurb + motive blurb. "One short line" is guaranteed at the write side by REQ-BARD-STORE-16a (newlines collapsed, then truncated), not assumed here.

**REQ-BARD-ARCH-2.** `eligibleCatalogForNewRoom(db, originRoom)` supplies that menu; the prospective room's front distance is the origin's plus one, mirroring `eligibleEnemyBlurbs`.

**REQ-BARD-ARCH-3.** The `buildArchitectContext` doc comment is updated: it is **no longer O(1) in world size**, it is O(eligible catalog). The existing comment claims O(1) and must not be left to erode silently.

**REQ-BARD-ARCH-4.** An empty catalog, an empty menu, and an empty `bard_focus` each produce a well-formed, thinner context — never a malformed one. With all three empty the payload is equivalent to today's.

## B. Tool schema

**REQ-BARD-ARCH-5.** `buildArchitectRequestBody` gains an optional `story` object on the `create_room` tool, with a required `handle` and a required `description`.

**REQ-BARD-ARCH-6.** `handle` is a schema-enforced enum of exactly the eligible handles the engine computed — the same treatment `enemy` already receives. When the menu is empty, the `story` field is **omitted entirely** from the schema, and the request body is byte-identical to one built with no bard at all.

**REQ-BARD-ARCH-7.** At most one `story` entry per generated room. A room may carry both an `enemy` and a `story`; they are independent.

## C. Validation

**REQ-BARD-ARCH-8.** `validateRoomProposal` extracts `story` **leniently**, in the manner of `exits`: a malformed, empty, or absent `story` is dropped with one diagnostic and **never rejects the room**. `name` and `description` remain the only strict clauses.

**REQ-BARD-ARCH-9.** A `story` whose `handle` is empty after trim, or whose `description` is empty after trim, is dropped in full — a story entry without instance prose has nothing to write into the world.

## D. Materialization

**REQ-BARD-ARCH-10.** `architectCommitProposal` places a story entry immediately after enemy placement, in Phase 2, following the same three steps: re-check against the live menu, then place, then record.

**REQ-BARD-ARCH-11.** The handle is resolved by `catalogForHandle` against the **live** menu at commit time. A handle that is unknown, stale, or already materialized resolves to 0 and places nothing; the room is still created normally.

**REQ-BARD-ARCH-12.** Placement calls `placeCatalogEntry(db, catalog, room, proposal.story.description, actor)`. The minted entity's `description` is the **architect's** instance prose; its `name` comes from `catalog.name`. The catalog `blurb` is never written as a description.

**REQ-BARD-ARCH-13.** Story placement runs **outside** the Phase-1 catch, like the rest of Phase 2, so a genuine DB fault propagates to the tick's rollback rather than being downgraded to a silent wall.

**REQ-BARD-ARCH-14.** The pre-generated path and the synchronous path place story through the **same code**. A story entry cannot behave differently depending on whether pregen hit — the property `architectCommitProposal` was extracted to guarantee.

## E. Non-regression

**REQ-BARD-ARCH-15.** With an empty catalog, every existing architect behavior is unchanged: same context shape, same request body, same validation outcomes, same commit path, same events.

**REQ-BARD-ARCH-16.** The architect's read-only contract is preserved. `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` remains empty; story placement goes through the `mutations.cpp` helper.

**REQ-BARD-ARCH-17.** Entity ids are still never sent to or read from the model. The `story` field carries a handle and prose, nothing else.

## AI Validation

The dominant risk is regression in a shipped subsystem, so the suite must prove the architect is unchanged when the bard is absent before it proves anything new works.

**Mechanical checks:**

1. `cmake --build build` clean.
2. **The entire existing architect, pregen, and combat test suites pass unmodified.** Any test that requires editing to accommodate this change is a regression until proven otherwise.
3. `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` is empty — verifies REQ-BARD-ARCH-16.
4. The `buildArchitectContext` comment no longer claims O(1) — substring check, verifies REQ-BARD-ARCH-3.

**Non-regression tests (run first):**

5. With zero catalog rows and empty `bard_focus`, `buildArchitectContext` produces a payload byte-identical to the pre-change implementation for the same world and direction.
6. With an empty menu, `buildArchitectRequestBody` produces a body containing no `story` key at all — assert by substring absence, verifying REQ-BARD-ARCH-6.
7. Canned responses that exercise every existing `validateRoomProposal` path yield identical results with the `story` extraction in place.

**Context and schema tests:**

8. With a seeded catalog, the context contains the expected handles, blurbs, and motive blurbs, and `bard_focus` verbatim.
9. The context contains no `catalog.id`, no entity id, and no `tier` value — assert by searching for their string forms.
10. The `story.handle` enum in the request body lists exactly the eligible handles for the prospective room — not the whole catalog, and not the origin room's menu. Construct a world where the two menus differ so the test can tell them apart.

**Materialization tests:**

11. A canned proposal selecting an eligible handle creates the room **and** an entity whose `name` is `catalog.name`, whose `description` is the canned `story.description` (explicitly assert it is not the blurb), located in the new room; `catalog.entity` is latched; exactly one `materialized` event is appended.
12. A canned proposal whose `story.handle` is not on the menu creates the room normally and places nothing — no entity minted, no event, `catalog.entity` still NULL.
13. A handle materialized between snapshot and commit resolves to 0 at commit; the room is created, nothing is placed, and no second entity is minted.
14. A room carrying both an `enemy` and a `story` places both.
15. A malformed `story` (missing handle, empty description, wrong type) is dropped and the room is still created — verifies the lenient gate, REQ-BARD-ARCH-8.

**Path-equivalence test:**

16. The same canned proposal committed via the **pregen** path and via the **synchronous** path produces identical rows and identical events — verifies REQ-BARD-ARCH-14. Drive both through `pregenInjectReadyForTest` and a direct synchronous generate.

**Nouns-must-exist test:**

17. After a story entry materializes, its `name` resolves as an in-scope noun for the parser in that room — the player can `examine` it. This is the requirement that beats are entities rather than prose, and it is what prevents the invented-affordance failure the prose renderer exists to avoid.

**Out of scope** — placement into already-explored rooms is deferred (the `catalog.pending` shape in the design); nothing here implements it.
