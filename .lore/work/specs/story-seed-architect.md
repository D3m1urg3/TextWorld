---
title: Story-seed + architect — requirements
date: 2026-07-09
status: implemented
tags: [ai-integration, story-seed, world-generation, architect, claude-api, tool-use, mutations, fallback, persistence, requirements]
modules: [world-gen, architect, systems, mutations, world, loop]
related: [.lore/work/design/story-seed-architect.md, .lore/work/brainstorm/story-seed-and-lod-world.md, .lore/work/specs/ai-resolver.md, .lore/work/specs/ai-prose-renderer.md, .lore/vision.md]
req-prefix: ARCH
---

# Story-seed + architect — requirements

## Context

Third AI feature (integration point #2, story-first slice). A hand-authored setting seed is loaded into canon at world init; when the player walks an unmapped exit, an **architect** AI unit generates one new room *coherent with the setting and the room being left*, writes it to canon, and the player moves in. Shape settled in [.lore/work/design/story-seed-architect.md]; this spec encodes the **minimal atomic step** as verifiable requirements, mirroring the transport / fallback / gated-test seams of the prose renderer and resolver.

**Controlling frame.** The architect is a generator of *space*, disciplined exactly like the resolver is a generator of *actions*: the model **proposes** a room (name + description); the **engine disposes** — it mints ids, owns exits/reciprocity, enforces invariants, and writes canon. AI never invents structure (ids, exits, direction inverses); it only writes flavor with no mechanical consequence beyond "a room now exists here."

**Boundary declaration (vision principle 1).** Room *name* and *description prose* are AI-driven (flavor). Entity-id assignment, exit creation and reciprocity, direction inversion, the persistence/no-regeneration rule, and the decision to generate-or-wall are deterministic (engine). This is the **first read-write AI unit** — so the write is confined to one sanctioned mutation helper (REQ-ARCH-9); the architect TU itself issues no raw SQL writes.

**Bard = the setting seed (static).** For this step the "bard" is a freeform setting document loaded once at init (REQ-ARCH-1); there is no live/evolving storyteller. Coherence of independently-generated rooms *is* the test of the seed's sufficiency as shared context.

**Latency note (deferred, stated).** Generation is synchronous inside the tick transaction, so crossing an unmapped exit stalls for one round trip. Background prefetch is the deferred answer; naming it, not building it.

## Requirements

### Setting seed

**REQ-ARCH-1** — At world init, `openWorld` reads a freeform setting document and stores its text as a single `meta` row (`key='setting'`). This is a new *row*, not a new schema shape: **zero DDL**, no `SCHEMA_VERSION` bump. The path is a parameter mirroring the existing `seedPath` (`world.hpp:37`) — `settingPath`, default `"seed/setting.txt"` — so tests exercise the present- and absent-file cases deterministically against scratch paths, never by renaming the committed file. If the file is absent or empty, init still succeeds with `meta.setting` empty/absent — no separate failure path; an empty setting simply yields a thinner architect prompt. **A real (modest) `seed/setting.txt` is committed as part of this step**, coherent with the two rooms in `seed/base.sql` (the dormitory cell / corridor), so the live smoke has real shared context; rich narrative authorship beyond that is not required here.

### Enable & seam dispatch

**REQ-ARCH-2** — Architect generation is enabled under the **same switch** as the other AI features: `aiNarrationEnabled()` (`ANTHROPIC_API_KEY` set and non-empty AND `TEXTWORLD_AI` not exactly `0`). No independent toggle.

**REQ-ARCH-3** — `resolveGo` (`systems.cpp`) gains one branch, evaluated **in this order**: (a) if an exit already exists for `(room, direction)`, move (unchanged); else (b) if generation is enabled **and** the direction is invertible (REQ-ARCH-8) **and** `architectGenerate` succeeds, move through the now-existing exit; else (c) the existing wall: `appendEvent(..., "failed", ..., "You can't go that way.")`. Branch (a) precedes any AI call, so a re-crossing of a generated exit never regenerates (persistence falls out of `exitDest`).

### Fallback & atomicity

**REQ-ARCH-4** — `architectGenerate` has two phases with an explicit catch boundary. **Phase 1 (AI-side: context build → request → transport → `validateRoomProposal`)** is wholly inside a try/catch: every failure of it — HTTP error, timeout, malformed response, no tool call, gate failure (REQ-ARCH-9a–c), a throwing transport — is caught and yields `false`, falling through to the wall (REQ-ARCH-3c). No Phase-1 failure crashes, blocks past the timeout, or leaks AI-flavored text into player output (one non-prose stderr diagnostic is permitted). **No database write occurs in Phase 1**, so the wall path is always reached with nothing written but the imminent `failed` event. Phase 2 is the write (REQ-ARCH-5), entered **only** with a validated proposal; `architectGenerate` returns `true` only after Phase 2 fully succeeds.

**REQ-ARCH-5** — **Phase 2 (the write)** runs only on a validated proposal and is *not* swallowed by Phase 1's catch. It is deterministic engine code operating on validated data through the sanctioned mutation helper (REQ-ARCH-9), inside the caller's existing tick transaction: the new room's component rows, both exit rows, and the `generated` event. A write-phase fault (e.g., a genuine DB error) is a tier-c **engine error** — it propagates out of `architectGenerate` and `resolveGo` to `runTurn`'s existing try/catch, which rolls the whole turn back (no orphan entity, dangling exit, or half-room, and no `moved`) — it is **never** downgraded to a wall, because a DB failure is an engine fault, not graceful AI degradation. Atomicity rests entirely on the existing per-turn transaction (REQ-PROTO-5); the architect introduces no new atomicity mechanism, only new writes inside the established boundary.

### Write boundary (the new discipline)

**REQ-ARCH-6** — The architect TU (`src/architect.cpp`) issues **no raw SQL writes**: `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` returns nothing. All persistence goes through a sanctioned mutation helper in `mutations.{hpp,cpp}` (REQ-ARCH-9), preserving the engine's write discipline (every world mutation = component write + event row, via helpers). Entity ids are minted mechanically by the engine and are **never** sent to or received from the model (mirror REQ-PROSE-6 / REQ-RESOLVE-6).

### Context payload

**REQ-ARCH-7a** — A pure function of `(db, room, direction)`, SELECT-only and network-free, builds the architect context sent as the user message, containing **exactly**: the setting text (`meta.setting`); the origin room's name; the origin room's canon description; and the direction of travel. Nothing else — no ids, no neighborhood, no event history. Context size is O(1) in world size (bounded-context law). Unit-testable with no network.

### API request (tool-use / structured output)

**REQ-ARCH-7b** — The architect uses the Anthropic Messages API **tool-use** path. A single tool `create_room` is defined with a schema-enforced object: required `name` (string) and `description` (string). No other fields. (This narrows the design doc's `create_room` sketch, which listed optional `items[]`; generated-room items are deferred — see Out of scope. The design's schema table is superseded by this requirement.) `tool_choice` requires the tool (the architect's job on this path is to produce a room; there is no "decline" branch — a non-call is a gate failure → wall). Request params mirror the resolver: `POST /v1/messages`, `anthropic-version: 2023-06-01`, model `claude-opus-4-8` overridable via `TEXTWORLD_MODEL`, `max_tokens` 1024, no `thinking`, no `stream`, no cache keys. `system` = the architect prompt (REQ-ARCH-7c). One user message carrying the context payload.

**REQ-ARCH-7c** — A git-versioned system-prompt constant instructs the model: generate exactly one room reachable by travelling `<direction>` from the described origin; it must be **coherent with the setting and consistent with** the origin room; emit it via `create_room` as a `name` and a `description`; the description is standalone room prose (as the player will read it on entry); **do not** describe exits, directions, other rooms, or the player's arrival; **do not** invent ids. Prompt *quality* is a live concern (REQ-ARCH-12); structure is pinned mechanically.

### Validation gate (mechanical)

**REQ-ARCH-8** — Direction invertibility is a fixed engine table: `north↔south`, `east↔west`, `up↔down`, `in↔out`. A `Go` direction outside this set is **not** generatable — `resolveGo` takes the wall (REQ-ARCH-3c) without any AI call. The inverse is used to create the reciprocal exit (REQ-ARCH-9).

**REQ-ARCH-9** — `validateRoomProposal(const HttpResponse&)` → `std::optional<RoomProposal{ name, description }>` is a pure function, no DB, that **never throws**, accepting the model output only if all hold; else `nullopt` (→ REQ-ARCH-4 fallback), emitting one stderr diagnostic naming the first failed clause:
  a. HTTP 200 and the body contains **exactly one** `tool_use` block for `create_room` (0 or ≥2 → fail);
  b. `name` is present and non-empty after trim;
  c. `description` is present and non-empty after trim.
The mechanical **write** is a sanctioned mutation helper `writeGeneratedRoom(db, originRoom, direction, proposal, actor)` in `mutations.cpp` that, inside the caller's transaction: mints one entity; writes its `room` tag, `name`, and `description` (canon) rows; writes the exit `(originRoom, direction) → new` and the reciprocal `(new, inverse(direction)) → originRoom`; and appends one `generated` event (`actor` = player, `subject` = new room, `object` = originRoom, `detail` = direction). Note this `subject`/`object` reading differs deliberately from `moveEntity`'s (`subject` = thing moved, `object` = destination): here `subject` is the entity being asserted into existence, `object` its origin of reference. Ids are engine-minted; the proposal carries none.

### Event-log integration

**REQ-ARCH-10** — The new `generated` event verb is **renderer-invisible**: the template renderer already emits nothing for unrecognized verbs (`render.cpp` — update the stale "fixed six" comment); and `buildFacts` in `prose.cpp` **excludes** `generated` from **both** event SELECTs that feed the LLM payload — the **current-turn** events query (`WHERE turn = ?`, ~line 320, populates `payload["events"]`) *and* the **recent-events** query (`WHERE turn < ? … LIMIT`, ~line 381, populates `payload["recent_events"]`) — by adding `AND verb <> 'generated'` to each. The current-turn one is the load-bearing case, since the `generated` and `moved` events share the same turn number and would otherwise leak the birth into narration on the turn it happens. The player-visible output of a generation turn remains the `moved` room block, unchanged.

### Build

**REQ-ARCH-11** — No new build dependencies (reuse vendored libcurl + nlohmann/json). A new TU `src/architect.{cpp,hpp}` is added to the `twcore` sources in `CMakeLists.txt`. It reuses the `HttpResponse` / `HttpTransport` seam from `prose.hpp` and the `aiNarrationEnabled()` switch; it may carry its own small `curlTransport` (same URL / headers as prose/resolver), only the transport *type* being shared. **The HTTP call has a total timeout of 8 seconds and performs no retries** — the same value the prose renderer already uses for its own `max_tokens: 1024` narration (`prose.cpp`), so a 1024-token generation fits the established bound. `architectGenerate` has a test-visible transport-injected overload; the production form binds libcurl and is called from `resolveGo`. The transport is invoked at most once per generation; timeout counts as a Phase-1 failure (REQ-ARCH-4).

### Testing

**REQ-ARCH-12** — Unit tests (no network, default `tests` target) carry the weight, using a `cannedCreateRoom(name, description)` fixture helper (the architect analog of `cannedToolUse`) built from the documented tool-use shape, never a live probe. They cover: the context builder fields (exactly the REQ-ARCH-7a set, no ids); the request body (`create_room` tool schema, model default + `TEXTWORLD_MODEL` override, `max_tokens` 1024, exact top-level key set so no `thinking`/`stream`/cache key slips in); the gate clauses a–c plus 0-block and ≥2-block; **creation** (a canned proposal through `architectGenerate` with a fake transport creates the room with the canned name/description, both reciprocal exits, and the `generated` event); **persistence/no-regeneration** (after generation, `exitDest(origin, direction)` resolves — so a re-crossing takes REQ-ARCH-3a before any AI call); **atomic fallback** (Phase-1 failure — transport error / invalid proposal → `false`, and because no write was attempted, no orphan entity/exit/description row exists); **ids-not-from-model** (a proposal carrying an id-looking field changes nothing about the minted id); **direction invertibility** (a non-invertible direction walls without a transport call); **generated-event invisibility** (a `generated` event produces no template output, and is absent from **both** the `events` and `recent_events` keys of the `buildFacts` payload). Phase-2 write-fault atomicity (REQ-ARCH-5) is *not* separately unit-tested — it rests on the existing per-turn transaction/rollback (REQ-PROTO-5), already covered engine infrastructure; the architect adds no new atomicity mechanism to test.

**REQ-ARCH-13** — A gated live end-to-end smoke exists, run only under `TEXTWORLD_AI_LIVE_TEST=1` (skipped by default, excluded from any CI), asserting **mechanical invariants only**: generating a room from the seeded world yields a non-empty name and description, two reciprocal exits, and a valid move — never model-specific wording. Coherence is checked by a **single bounded** judge call: feed the setting text plus the descriptions of the generated rooms to one LLM call asking "coherent with the setting and each other? yes/no + one line"; observe the answer. **No tuning loop** — one call, human reads the result ([[verification-must-be-bounded]]).

## Out of scope (explicit deferrals)

- **Generated-room items.** `create_room` emits name + description only; portable items in generated rooms are deferred (keeps the write helper and gate minimal).
- **Live / evolving bard.** Setting is static; no fact-evolution, no beat proposal. Deferred until a consumer exists (#4).
- **LOD sketch lane, prefetch, background generation.** Single L2-realized room, synchronous. The `sketch` table and level gradient are deferred (design Decision 5).
- **World-size cap / latent eviction.** `meta.world_cap` not read; no eviction. Deferred.
- **Dedup / stub-identity / loop-closure.** The world is a tree grown at unmapped exits; no two nodes denote the same place, so dedup is unneeded here. Deferred to the cap feature.
- **Reference-leak onward exits.** A generated room advertises only the way back; onward directions are discovered by trying them (→ more generation). No latent L0 stubs.
- **Feeding events / story-so-far to the architect.** Context is setting + origin room + direction only; story-enriched realization is deferred.
- **Non-invertible directions.** No mechanical reciprocal → no generation (wall). Diagonal/compound directions deferred.

## AI Validation

How the AI verifies completion, behaviorally. Each item names the requirements it verifies.

1. **Build check (REQ-ARCH-11):** `cmake -B build && cmake --build build` succeeds on macOS, no new packages; `src/architect.{cpp,hpp}` present, reusing `prose.hpp`'s transport seam and `aiNarrationEnabled()`.
2. **Write-boundary grep (REQ-ARCH-6):** `grep -En "INSERT|UPDATE|DELETE" src/architect.cpp` returns nothing; the room write lives in a `mutations.cpp` helper.
3. **Setting load (REQ-ARCH-1):** create a fresh world with a `settingPath` pointing at a scratch file; confirm `meta.setting` holds the file text and `SCHEMA_VERSION` is unchanged. Create another with `settingPath` at a non-existent scratch path; init still succeeds with `meta.setting` empty. Confirm a real `seed/setting.txt` is committed and coherent with `seed/base.sql`.
4. **Prompt inspection (REQ-ARCH-7c):** read the architect system-prompt constant; confirm it asks for one room coherent with setting + origin, name+description via `create_room`, and forbids exits/ids/arrival narration.
5. **Disabled-mode run (REQ-ARCH-2, -3):** with `ANTHROPIC_API_KEY` unset, walking an unmapped exit yields today's "You can't go that way." wall — byte-identical to the pre-feature build; existing tests unchanged.
6. **Unit suite (REQ-ARCH-4, -5, -7a, -7b, -8, -9, -10, -12):** `./build/tests` passes, including the REQ-ARCH-12 cases (creation, reciprocity, persistence/no-regen, Phase-1 atomic fallback with no orphan, ids-not-from-model, direction invertibility, request body key set, gate clauses, and generated-event invisibility asserted against **both** `events` and `recent_events` payload keys).
7. **Failure behavior (REQ-ARCH-4):** fake transport timeout / malformed / throwing → `architectGenerate` returns `false`, the turn walls within the timeout bound, no crash, nothing AI-flavored leaks, no partial room persists.
8. **Live smoke + bounded coherence (REQ-ARCH-13 — manual, optional):** with a real key and `TEXTWORLD_AI_LIVE_TEST=1`, generate a small chain of rooms; assert the mechanical invariants; run the single coherence-judge call and read its yes/no.
