---
title: "Bard catalog selection: eligibility, wire format, and validation"
date: 2026-08-03
status: draft
tags: [bard, dungeon-master, eligibility, tool-use, validation-gate, wire-format, degradation, determinism]
modules: [bard, combat, mutations]
related: [.lore/work/design/bard-catalog-selection.md, .lore/work/specs/bard-fact-store.md, .lore/work/specs/combat-and-enemies.md]
req-prefix: BARD-SEL
---

# Bard catalog selection: eligibility, wire format, and validation

Everything about **what the model sees and what the engine accepts back**. Depends on [bard-fact-store.md](bard-fact-store.md) for the schema and helpers.

**Still contains no network call.** Context building is SELECT-only, request bodies are pure string→string, and the validation gate is a pure function over a canned `HttpResponse` — the same shape as `buildArchitectContext` / `buildArchitectRequestBody` / `validateRoomProposal`, all of which are tested today without a transport.

Source design: [bard-catalog-selection.md](../design/bard-catalog-selection.md), as amended by [bard-architect-integration.md](../design/bard-architect-integration.md).

## A. Eligibility

**REQ-BARD-SEL-1.** `eligibleCatalog(db, room, kind) -> vector<CatalogChoice>` returns the catalog entries offerable in `room`. `CatalogChoice` carries `handle`, `blurb`, and the motive's blurb — **never** an id, `tier`, `name`, `seeded`, or a raw fact column. Read-only.

**REQ-BARD-SEL-2.** Four gates compose; all must hold for an entry to be offered:

- a. `entity IS NULL` — not already materialized;
- b. `tier <= distanceFromSeed(room)` — the same spatial metric `eligibleArchetypes` uses;
- c. `kind` equals the requested kind;
- d. **knowledge beats only** — an entry with a non-empty `fact_archetype` is offered only if that archetype appears in `eligibleArchetypes` for `room` or for a room one hop away.

**REQ-BARD-SEL-3.** Gate (d) is implemented by *calling* `eligibleArchetypes`, not by reimplementing its logic, so combat and story eligibility cannot drift apart.

**REQ-BARD-SEL-4.** Results are ordered by `catalog.id` ascending. An empty result is a normal, common answer and is never an error.

**REQ-BARD-SEL-5.** `eligibleCatalogForNewRoom(db, originRoom) -> vector<CatalogChoice>` returns the choices for the room about to be created one hop beyond `originRoom`; its front distance is the origin's plus one. This mirrors `eligibleEnemyBlurbs`' relationship to `eligibleArchetypes`.

**REQ-BARD-SEL-6.** `catalogForHandle(db, room, handle) -> int64_t` resolves a model-supplied handle to a catalog id **only if it is currently eligible**, re-checking the live menu. An unknown, stale, already-materialized, or empty handle resolves to 0.

**REQ-BARD-SEL-7.** No eligibility gate reads a per-entry unlock condition, and no such column exists. Eligibility derives only from the general predicates in REQ-BARD-SEL-2.

## B. Wire format

**REQ-BARD-SEL-8.** No entity id, catalog id, `tier` value, `seeded` flag, or raw `fact_archetype`/`fact_element` value appears in any request body sent to the model. The model-facing fields are `handle`, `blurb`, `name` (where an in-world noun is needed), and motive blurbs.

**REQ-BARD-SEL-9.** `buildOvertureContext(db) -> string` is a pure function of the database — SELECTs only, no network, no globals. It carries exactly two things: `meta.setting`, and **the motive vocabulary as key + blurb, read from `motive_catalog`**. Nothing else about world state; at overture time none exists to describe.

The motive blurbs are not optional. The tool schema constrains `motive` to eight bare keys (REQ-BARD-SEL-12), and without their meanings the model is choosing between opaque tokens — it cannot tell `obligation` from `homesickness`. The vocabulary is the one piece of authored content in this feature and it must reach the wire.

**REQ-BARD-SEL-10.** `buildWakeContext(db) -> string` is a pure function of the database, carrying exactly: `meta.setting`, the motive vocabulary as key + blurb, `meta.bard_journal`, the event rows since `meta.bard_last_wake_turn` rendered as human-readable lines, and the full catalog rendered as handle + blurb + motive blurb + materialized-or-not. No ids anywhere. The vocabulary is carried here for the same reason as REQ-BARD-SEL-9: `append_catalog` requires choosing a motive.

## C. Request bodies and tool schemas

**REQ-BARD-SEL-11.** `buildOvertureRequestBody(contextPayload) -> string` produces an Anthropic Messages API body with one `write_catalog` tool. Its input schema is an object with a required `entries` array and a required `journal` string. Each entry has required `kind`, `handle`, `name`, `blurb`, `motive`, `tier`, and an optional `fact` object of `{archetype, element}`.

**REQ-BARD-SEL-12.** `kind` is a schema-enforced enum of exactly `["character","beat"]`. `motive` is a schema-enforced enum of exactly the eight keys present in `motive_catalog`, read from the database rather than hardcoded in the request builder.

**REQ-BARD-SEL-13.** `buildWakeRequestBody(contextPayload) -> string` produces a body with four tools: `write_focus {text}`, `write_journal {text}`, `append_catalog {entry}`, and `mark_seeded {handle}`.

`entry` is **one element of** REQ-BARD-SEL-11's `entries` array — an object of `kind`, `handle`, `name`, `blurb`, `motive`, `tier`, and optional `fact` — **not** the whole `write_catalog` input object. `append_catalog` carries no `entries` array and no `journal`.

**REQ-BARD-SEL-14.** `tool_choice` is `{"type":"auto"}` on both call shapes. A response containing no tool call is a valid outcome, not a gate failure.

**REQ-BARD-SEL-15.** There is no `place_catalog` tool. The bard cannot express a room, and placement is the architect's (see [bard-architect-integration](bard-architect-integration.md)).

## D. Validation

**REQ-BARD-SEL-16 (pure gate).** `validateOvertureResponse(response) -> optional<OvertureProposal>` and `validateWakeResponse(response) -> optional<WakeProposal>` are pure functions of the `HttpResponse`. They touch no database, **never throw**, and emit one stderr diagnostic naming the first failed clause.

**REQ-BARD-SEL-17.** An entry within a bulk write is **dropped**, with one diagnostic, when: `handle`, `name`, or `blurb` is empty after trim; `kind` or `motive` is outside its vocabulary; `tier` is not a non-negative integer; or `fact` carries one field without the other. **Dropping an entry never rejects the response.**

**REQ-BARD-SEL-18.** An overture response is rejected in full only when the HTTP status is non-200, the body is unparseable, or it contains no `write_catalog` tool call. A rejected overture yields an empty catalog.

**REQ-BARD-SEL-19.** A `fact` that fails the truth gate at admission (REQ-BARD-STORE-10) causes its **entire entry** to be dropped, not merely the fact — a knowledge beat whose knowledge is false has no remaining purpose. Admission drops it and continues with the remaining entries.

*Mechanism amended 2026-08-03* (plan micro-decision 6a, approved). This originally read "enforced by catching the helper's throw at the call site." It is instead enforced by a **read-only pre-flight** at the call site that mirrors every refusal `writeCatalogEntry` makes, so a refused entry never reaches the helper. Admission catches nothing: `db.hpp` raises `std::runtime_error` for a genuine SQLite fault and for a validation refusal alike, so a catch could not tell them apart and would swallow the fault that REQ-BARD-WAKE-7/-23 require to roll the whole write back. The requirement above — and its test 14 — are unchanged.

**REQ-BARD-SEL-20.** A `mark_seeded` naming an unknown handle is ignored with a diagnostic; the rest of the wake still applies.

**REQ-BARD-SEL-24.** `mark_seeded` resolves its handle with `catalogIdForHandle(db, handle) -> int64_t`, a **room-free, gate-free** lookup that matches any catalog row by handle. It must not use `catalogForHandle` (REQ-BARD-SEL-6): a wake is not scoped to a room, so there is no room argument to supply, and the eligibility gates are the wrong question — seeding records that an entry was *hinted*, which can be true of an entry that is not currently offerable anywhere. Unknown handle resolves to 0, handled by REQ-BARD-SEL-20.

## E. Unit contract

**REQ-BARD-SEL-21.** The bard's translation unit performs only SELECTs and (in later bricks) network egress. `grep -En "INSERT|UPDATE|DELETE" src/bard.cpp` must be empty. Every write goes through the `mutations.cpp` helpers from [bard-fact-store](bard-fact-store.md).

**REQ-BARD-SEL-22.** The bard's system prompts are git-versioned string constants in the bard unit, exposed in its header so their structure is spot-checkable by substring — following `kArchitectPrompt` and `kResolveSystemPrompt`.

**REQ-BARD-SEL-23.** The bard's prompt instructs it to create **situations, not urgency**: no deadlines, no ticking threats, nothing that makes standing still or talking at length feel expensive. Escalation in this game is spatial, and the prompt must not undercut it.

## AI Validation

Still offline. Every requirement is checkable with a canned `HttpResponse` and a seeded world; nothing needs an API key.

**Mechanical checks:**

1. `cmake --build build` clean; existing suite green.
2. `grep -En "INSERT|UPDATE|DELETE" src/bard.cpp` is empty — verifies REQ-BARD-SEL-21.
3. `grep -n "place_catalog" src/` is empty — verifies REQ-BARD-SEL-15.
4. The overture and wake system prompts each contain a clause forbidding deadlines/urgency — substring check, verifies REQ-BARD-SEL-23.

**Behavioral tests:**

5. **Gate composition.** With a seeded catalog spanning tiers, `eligibleCatalog` at a seed-adjacent room offers only tier-0 entries; the same call deeper offers more. A materialized entry never appears. A `kind` filter excludes the other kind.
6. **Gate (d).** A knowledge beat naming an archetype that `eligibleArchetypes` does not offer nearby is withheld; the same beat becomes offerable once the archetype qualifies. Drive both states from the same world by moving the room argument.
7. **Determinism.** `eligibleCatalog` returns identical order across repeated calls and across process restarts.
8. **Empty is normal.** `eligibleCatalog` on a safe-edge room returns empty without error, and callers treat it as a valid answer.
9. **Live re-check.** `catalogForHandle` returns the id for an eligible handle; returns 0 for an unknown handle, for a handle whose entry was materialized between menu construction and the call, and for `""`.
10. **No ids on the wire.** Build both context payloads and both request bodies against a seeded world, then assert none of the string forms of any `catalog.id`, `entity` id, or `tier` value appears in them. Assert positively that a known handle and blurb *do* appear.
11. **Motive enum is data-driven.** Adding a ninth row to `motive_catalog` in a test world changes the enum in `buildOvertureRequestBody` — verifies REQ-BARD-SEL-12 is read from the DB, not hardcoded.
11a. **Motive blurbs reach the wire.** Both `buildOvertureContext` and `buildWakeContext` contain every seeded motive key **and** its blurb text — verifies REQ-BARD-SEL-9/-10. Without this the model selects between opaque tokens.
11b. **`mark_seeded` resolution is room-free.** `catalogIdForHandle` resolves a handle whose entry is ineligible everywhere (tier far above any reachable room) and one that is already materialized — both non-zero — while `catalogForHandle` returns 0 for the same handles. Verifies REQ-BARD-SEL-24 uses the right lookup.
12. **Lenient per-entry.** A canned overture response with three entries, one malformed, yields two admitted entries and one diagnostic — not a rejection.
13. **Strict per-response.** Canned responses that are non-200, unparseable, or carry no tool call each yield `nullopt` and an empty catalog; the gate never throws on any of them.
14. **False fact drops the entry.** A canned entry asserting a weakness contradicted by `resistance` results in zero rows for that entry, while its sibling entries are admitted.
15. **`tool_choice: auto` honored.** A canned response with no tool call validates as a successful, empty wake — assert no diagnostic is emitted claiming failure.

**Out of scope** — no requirement here builds a transport, spawns a thread, or references `main.cpp`. Those are [bard-overture-and-scheduling](bard-overture-and-scheduling.md).
