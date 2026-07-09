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

#include "db.hpp"
#include "prose.hpp"  // HttpResponse / HttpTransport seam + aiNarrationEnabled(), reused as-is

// The model's proposal for one room: just flavor, no structure. Name and
// description prose are AI-driven; ids, exits, and reciprocity are the engine's
// (REQ-ARCH-6). The proposal carries NO id — the mint is mechanical.
struct RoomProposal {
    std::string name;
    std::string description;
};

// The architect's system prompt (REQ-ARCH-7c) — git-versioned, like the
// narrator / resolver prompts. It instructs the model to generate exactly one
// room reachable by travelling <direction> from the origin, coherent with the
// setting and consistent with the origin room, emitted via the create_room tool
// as a name + a standalone description, forbidding exit/direction/arrival
// narration and any id. Exposed so its structure is spot-checkable by substring;
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
// `description` strings and no other fields; `tool_choice` REQUIRES the tool
// (there is no decline branch — a non-call is a gate failure → wall). Model from
// TEXTWORLD_MODEL if set and non-empty (else claude-opus-4-8; shared with the
// renderer/resolver), max_tokens 1024, system = kArchitectPrompt, one user
// message carrying the context payload. No thinking, no stream, no cache keys.
std::string buildArchitectRequestBody(const std::string& contextPayload);

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
// reach the two-field RoomProposal, so the model can put no id on the wire.
std::optional<RoomProposal> validateRoomProposal(const HttpResponse& response);

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
