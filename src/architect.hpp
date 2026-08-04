// AI architect: the world-generator. When the player walks an UNMAPPED exit,
// this unit proposes one new room (name + description prose) coherent with the
// setting and the room being left, via Claude tool-use, and the engine writes
// it to canon. The frontier analog of the prose renderer / resolver.
//
// READ-WRITE BY CONTRACT, with the write CONFINED. This is the engine's first
// read-write AI unit, so the discipline is explicit: this translation unit
// performs ONLY SELECTs and network egress — NO INSERT, UPDATE, or DELETE may
// ever appear in architect.cpp (grep -En "INSERT|UPDATE|DELETE" src/architect.cpp
// must be empty, REQ-ARCH-6). The sole persistence path is the sanctioned
// mutation helper writeGeneratedRoom() in mutations.cpp: the model PROPOSES a
// room, the engine DISPOSES — it mints the id, owns the exits and reciprocity,
// and writes canon. Entity ids are never sent to or read from the model.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bard.hpp"  // CatalogChoice — the story menu the caller hoists and passes in
#include "db.hpp"
#include "prose.hpp"  // HttpResponse / HttpTransport seam + aiNarrationEnabled(), reused as-is

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

// The model's proposal for one room: just flavor, no structure. Name and
// description prose are AI-driven; ids, exits, and reciprocity are the engine's
// (REQ-ARCH-6). The proposal carries NO id — the mint is mechanical.
struct RoomProposal {
    std::string name;
    std::string description;
    // The cleaned set of invertible onward directions the model declared, minus
    // the entry-return direction (REQ-EXITS-7). Empty = a dead end.
    std::vector<std::string> exits;
    // The enemy BLURB the model optionally selected (REQ-COMBAT-31): the raw,
    // trimmed string from the `enemy` field, or "" for none. Extracted LENIENTLY
    // at the gate; the engine re-checks it against the eligible menu and maps it
    // to an archetype before placing (a hallucinated blurb places nothing).
    std::string enemyBlurb;
    // The story entry the model optionally selected (REQ-BARD-ARCH-5). An empty
    // handle is "no story" — the same ""-means-none convention as enemyBlurb
    // above, so every existing brace-initialized RoomProposal is unaffected.
    StoryProposal story;
};

// The architect's system prompt (REQ-ARCH-7c) — git-versioned, like the
// narrator / resolver prompts. It instructs the model to generate exactly one
// room reachable by travelling <direction> from the origin, coherent with the
// setting and consistent with the origin room, emitted via the create_room tool
// as a name + a standalone description + a declared set of onward exits
// (REQ-EXITS-6). It REQUIRES the model to declare the directions that lead
// onward and describe them in the prose, EXCLUDE the entry-return direction (the
// engine adds it), and narrate no arrival or any id. Exposed so its structure is
// spot-checkable by substring;
// prompt QUALITY is a live concern (Step 10). Reword with care.
extern const char* const kArchitectPrompt;

// Pure function of (db, room, direction, menu): SELECTs only, no network, no
// globals. Builds the architect context sent as the LLM user message. Carries
// the REQ-ARCH-7a set — the setting text (meta.setting), the origin room's
// name, the origin room's canon description, and the direction of travel — plus
// TWO story fields the bard supplies (REQ-BARD-ARCH-1):
//   * `focus`         — meta.bard_focus, the bard's SHORT public line. The one
//                       line the architect reads from the bard; the private
//                       journal is never sent.
//   * `story_options` — one object per entry of `menu`, each {handle, blurb,
//                       motive}. `motive` is the motive BLURB, never the key.
//
// NO IDS, still and always (REQ-ARCH-6, REQ-BARD-ARCH-17): no entity id, no
// catalog id, no `tier`, no `seeded` — CatalogChoice carries none of them, so
// the guarantee is structural rather than filtered here.
//
// BOTH story keys are OMITTED ENTIRELY when empty, never present-and-empty
// (REQ-BARD-ARCH-4). With no focus and an empty menu the payload is byte-for-byte
// the four-field object it was before the bard existed, which is what makes the
// non-regression claim (REQ-BARD-ARCH-15) free rather than argued. An empty
// meta.setting likewise just yields a thinner (but well-formed) payload.
//
// THE MENU IS SUPPLIED BY THE CALLER, not read here, and that is deliberate:
// architectQueuePregen calls this once PER LATENT DIRECTION while the menu is
// per-room, so reading it inside would repeat a BFS and a neighborhood read for
// every direction. The caller reads eligibleCatalogForNewRoom ONCE and passes
// the result to both this and buildArchitectRequestBody, exactly as it already
// hoists eligibleEnemyBlurbs — which also makes the context blurbs and the tool
// schema's enum agree BY CONSTRUCTION rather than by review.
//
// COST, honestly: O(eligible catalog) in the menu, and the caller's
// eligibleCatalogForNewRoom runs a distanceFromSeed BFS that is O(world). The
// constant-cost-in-world-size claim this comment used to make is RETIRED
// (REQ-BARD-ARCH-3) — it stopped being true the moment the menu became part of
// the context, and the phrase is gone from this file so a grep can prove it.
//
// A note for later, not work: `setting` + the story menu is a large, stable,
// recurring prefix across every generation in a session — the shape prompt
// caching exists for. Worth measuring cache_read_input_tokens once this has run
// against real play (design Decision 4).
std::string buildArchitectContext(Db& db, int64_t room,
                                  const std::string& direction,
                                  const std::vector<CatalogChoice>& menu = {});

// Anthropic Messages API request body for the architect (REQ-ARCH-7b). Pure
// string→string (plus a TEXTWORLD_MODEL env read). A `tools` array carries ONE
// create_room tool whose input schema is an object with REQUIRED `name` and
// `description` strings plus one OPTIONAL `exits` string-array (invertible
// onward directions, REQ-EXITS-5) — no other fields; `tool_choice` REQUIRES the
// tool
// (there is no decline branch — a non-call is a gate failure → wall). Model =
// modelForRole(AiRole::Generate) — claude-opus-4-8 by default (room prose is
// quality work), with TEXTWORLD_MODEL overriding every role; the precedence rule
// lives in aihttp.hpp and is stated nowhere else. max_tokens 1024, system =
// kArchitectPrompt, one user message carrying the context payload. No thinking,
// no stream, no cache keys.
//
// `enemyBlurbs` (REQ-COMBAT-31): when non-empty, the create_room schema gains one
// OPTIONAL `enemy` string constrained to a schema-enforced ENUM of exactly these
// blurbs — the eligible archetype choices the engine computed (REQ-COMBAT-29/-32:
// blurbs only, never ids/numbers/stats). The model may select at most one or omit
// it. Empty (the default) → no `enemy` field at all, so a non-combat world's body
// is byte-identical to before.
//
// `storyHandles` (REQ-BARD-ARCH-5/-6/-7): the same discipline, one brick over.
// When non-empty, the schema gains one OPTIONAL `story` OBJECT whose `handle` is
// a schema-enforced ENUM of exactly these catalog handles and whose
// `description` is the instance prose the model writes for this room; both are
// required WITHIN that object, and `story` itself is NOT in the tool's
// top-level required set, so the model may bring at most one entry or none.
// Handles only — no id, no tier, no seeded flag (REQ-BARD-ARCH-17). Empty (the
// default) → no `story` field at all, so a bard-free world's body is
// byte-identical to before. `enemy` and `story` are independent: a room may
// carry both.
//
// SCOPE OF "byte-identical", stated rather than left to be discovered: the
// CONTEXT and the TOOL SCHEMA are what the two menus gate, and those are
// byte-identical on an empty menu. kArchitectPrompt is one unconditional
// constant, so its enemy and story clauses ride in `system` regardless — the
// same precedent the combat brick set.
std::string buildArchitectRequestBody(
    const std::string& contextPayload,
    const std::vector<std::string>& enemyBlurbs = {},
    const std::vector<std::string>& storyHandles = {});

// DISPLAY / ontology gate for latent exits (REQ-EXITS-4, micro-decision #2).
// Whether a latent (NULL-dest) exit is an attemptable direction at all — an
// ontological fact about the map — is gated HERE, deliberately named apart from
// aiNarrationEnabled() (prose.hpp), the prose/cosmetic switch. Both are backed
// by the same env today (this returns aiNarrationEnabled()), but keeping the two
// predicates distinct lets a cost/outage toggle for prose NOT silently re-shape
// the map, and lets the concerns diverge later. render.cpp gates the Exits line
// on this; resolveGo gates generation on aiNarrationEnabled().
bool architectEnabled();

// The fixed engine table of invertible directions (REQ-ARCH-8): north↔south,
// east↔west, up↔down, in↔out. Any other direction → nullopt: it is not
// generatable (no mechanical reciprocal to create), so resolveGo takes the wall
// without an AI call. Pure, no DB, never throws.
std::optional<std::string> inverseDirection(const std::string& direction);

// Validation gate (REQ-ARCH-9a–c): pure function of the HttpResponse, no DB,
// NEVER throws (called outside any try/catch by tests). Returns the proposal iff
// ALL clauses hold, std::nullopt otherwise, emitting one stderr diagnostic
// naming the first failed clause:
//   a. status == 200 AND the body has EXACTLY ONE tool_use block for create_room
//      (0 or ≥2 → fail);
//   b. name present and non-empty after trim;
//   c. description present and non-empty after trim.
// Any stray input field (e.g. a spurious id) is IGNORED — only name+description
// reach the RoomProposal, so the model can put no id on the wire.
//
// The OPTIONAL `exits` array is then sanitized LENIENTLY (REQ-EXITS-7): a bad
// exit NEVER rejects a room. Each entry is dropped, with one stderr diagnostic,
// if it is not a string, is not one of the eight invertible directions (after
// trim+lowercase normalization), duplicates an already-kept entry, or equals the
// return direction inverse(directionOfTravel) the model was told to omit.
// Survivors (≤7) become proposal.exits; absent/empty → empty (dead end). The
// default "" for directionOfTravel maps to no return direction to drop.
//
// The OPTIONAL `story` object is extracted with the SAME leniency
// (REQ-BARD-ARCH-8): a malformed one is dropped, never a rejection. It is
// dropped IN FULL — both fields or neither (REQ-BARD-ARCH-9), because an entry
// without instance prose has nothing to write into the world — when it is not
// an object, or when `handle` or `description` is missing, is not a string, or
// is blank after trim. Absence is silent (the common case); every other drop
// emits one stderr line naming the reason. Survivors are stored trimmed, and no
// id is read from inside the object either (REQ-BARD-ARCH-17).
std::optional<RoomProposal> validateRoomProposal(
    const HttpResponse& response, const std::string& directionOfTravel = "");

// Generate one room beyond the UNMAPPED exit (room, direction) and write it to
// canon, returning true iff a room was created (REQ-ARCH-4/-5). Two phases with
// an EXPLICIT catch boundary:
//   Phase 1 (context → request → ONE transport call → validate) is wholly inside
//     try/catch: ANY failure — HTTP error, timeout, malformed response, no tool
//     call, gate failure, a throwing transport — yields false → the caller
//     walls. NO database write happens in Phase 1.
//   Phase 2 (writeGeneratedRoom) runs ONLY on a validated proposal and is
//     OUTSIDE the catch: a genuine DB fault propagates out to runTurn's tick
//     rollback (REQ-ARCH-5), never downgraded to a wall.
// The caller checks aiNarrationEnabled() AND inverseDirection() BEFORE calling,
// so a disabled or non-invertible turn never constructs a transport. The
// transport is invoked at most once — no retries.
bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor, const HttpTransport& transport);

// Production overload: binds the shared libcurl client from aihttp.hpp for the
// Generate role (same URL / headers / 8 s total timeout as the renderer and
// resolver, which now bind the same client, no retries) and delegates to the
// injected form.
bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor);

// The AI half of Phase 1, after the snapshot: request body → ONE transport call
// → validation gate. Pure of the database by construction — it takes the two
// snapshotted inputs rather than reading them — which is what lets the
// background worker call it without a Db (REQ-PREGEN-7).
//
// Does NOT catch: each caller keeps its own try/catch and its own diagnostic,
// because "the architect walled" and "a background job failed" are different
// events. The transport is invoked at most once; no retries (REQ-PREGEN-9).
//
// The Phase 2 counterpart is architectCommitProposal below. Between them, the
// synchronous path and the pre-generated path run THE SAME code on both halves
// — the request is built and validated identically, with the same
// direction-of-travel passed to the gate, whoever is calling.
//
// `storyHandles` has NO DEFAULT, deliberately, unlike the two builders it
// forwards to. Both call sites are in-tree — architectGenerate and the pregen
// worker — and each must be EXPLICIT about what menu it snapshotted, because a
// silently-defaulted empty one would mean "this path quietly stopped offering
// story" and nothing would fail.
std::optional<RoomProposal> architectProposeRoom(
    const std::string& contextPayload,
    const std::vector<std::string>& enemyBlurbs,
    const std::vector<std::string>& storyHandles, const std::string& direction,
    const HttpTransport& transport);

// Phase 2 of architectGenerate, verbatim and whole (REQ-PREGEN-14): mint the
// room, realize the origin exit, plant the reciprocal + declared latent stubs
// and the 'generated' event; THEN re-check the proposal's enemy blurb against
// the LIVE eligible menu and, when it still resolves, placeEnemy +
// recordArchitectSpawn; THEN the same for the proposal's story handle
// (REQ-BARD-ARCH-10) — catalogForHandle against the room that now EXISTS, and
// on a hit placeCatalogEntry, which mints the entity, writes catalog.name as
// its parser noun and the ARCHITECT's instance prose as its description, and
// latches the catalog row with a `materialized` event (REQ-BARD-ARCH-12).
// Runs inside the caller's tick transaction. Outside any catch: a genuine DB
// fault propagates to runTurn's rollback (REQ-ARCH-5, REQ-BARD-ARCH-13).
// Returns the minted room id.
//
// THREE HALVES NOW, and the third re-checks LIVE for the same reason the second
// does: an unknown, stale, or already-materialized handle resolves to 0 and
// places nothing, and the room is still created (REQ-BARD-ARCH-11).
//
// Extracted so a PRE-GENERATED candidate and a synchronously generated one
// commit through THE SAME CODE — "indistinguishable canon" is then structural
// rather than asserted. Omitting the enemy half here would silently stop
// spawning on the pregen path (REQ-PREGEN-14, REQ-PREGEN-18); omitting the
// story half would do the same to materialization (REQ-BARD-ARCH-14).
int64_t architectCommitProposal(Db& db, int64_t originRoom,
                                const std::string& direction,
                                const RoomProposal& proposal, int64_t actor);

// THE PRE-GENERATION SCHEDULER. Snapshot and submit one background job per
// latent exit of `room` that has no candidate, no in-flight job, and no attempt
// already made during this occupancy (REQ-PREGEN-4). Depth 1: a candidate's own
// declared exits are never chased (REQ-PREGEN-3). No-op when pre-generation or
// the architect is off.
//
// WHY IT LIVES HERE AND NOT IN pregen.cpp. Every read a job needs happens on
// the MAIN thread at queue time so the job it hands off carries no Db and no
// world pointer (REQ-PREGEN-5) — and pregen.cpp may not contain any SQL at all
// (REQ-PREGEN-7). This file already owns buildArchitectContext, already calls
// eligibleEnemyBlurbs, and is contractually read-only, so the snapshot belongs
// here. It stays READ-ONLY: this function never begins, commits, or writes.
//
// CALL IT AFTER the tick's transaction has committed and after the player's
// text has been flushed, so queuing can never delay the turn they waited on.
//
// "This occupancy" means the room this was last called with: standing still
// never re-queues a slot whose job failed, but leaving and coming back does
// (REQ-PREGEN-10). Idempotent — calling it twice for the same room queues
// nothing the second time, which is what makes calling it after EVERY turn
// cheap and uniform.
void architectQueuePregen(Db& db, int64_t room);

// The world turn (meta.turn) — the basis for a candidate's staleness figure
// (REQ-PREGEN-13). Read-only, one row.
//
// Deliberately meta.turn and NOT profileCurrentTurn(): "how many turns old" is
// a gameplay fact, and the process-local profile counter drifts from it on
// turns that never tick.
//
// Shared rather than reimplemented because BOTH of its callers serve the one
// staleness feature — architectQueuePregen stamps a job with it, and resolveGo
// subtracts that stamp from it to report age_turns. (loop.cpp, combat.cpp and
// mutations.cpp keep their own private copies of this read: those are
// independent units needing an unrelated one-off value, not two halves of one
// computation.)
int64_t architectWorldTurn(Db& db);

// TEST-ONLY. Forgets the current occupancy, so a test can start a scenario
// without inheriting the attempted-direction set a previous one left behind.
// Production code never calls this — in a real session, occupancy changes
// only by the player moving.
void architectResetPregenOccupancyForTest();
