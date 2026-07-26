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

#include "db.hpp"
#include "prose.hpp"  // HttpResponse / HttpTransport seam + aiNarrationEnabled(), reused as-is

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

// Pure function of (db, room, direction): SELECTs only, no network, no globals.
// Builds the architect context sent as the LLM user message. Contains EXACTLY
// the REQ-ARCH-7a set and no ids: the setting text (meta.setting), the origin
// room's name, the origin room's canon description, and the direction of
// travel. Nothing else — no ids, no neighborhood, no history. O(1) in world
// size. An empty meta.setting simply yields a thinner (but well-formed) payload.
std::string buildArchitectContext(Db& db, int64_t room,
                                  const std::string& direction);

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
std::string buildArchitectRequestBody(
    const std::string& contextPayload,
    const std::vector<std::string>& enemyBlurbs = {});

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

// Production overload: binds a local libcurl transport (same URL / headers /
// 8 s total timeout as the renderer/resolver, no retries — micro-decision #3)
// and delegates to the injected form.
bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor);
