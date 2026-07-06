// AI prose renderer: this turn's event rows → LLM-written prose. Design §6.
//
// READ-ONLY BY CONTRACT — this unit performs ONLY SELECTs against the DB;
// no INSERT, UPDATE, or DELETE may ever appear in its translation unit. It
// adds one new effect category — network egress — which the render contract
// didn't anticipate: reads world state, writes nothing, sends facts (not the
// DB) to the LLM. Otherwise it inherits render()'s contract verbatim:
// SELECTs only, and never an output claim without a sourcing event row.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "db.hpp"

// The facts for one turn, assembled by buildFacts() from fresh SELECTs.
struct TurnFacts {
    // JSON string sent as the LLM user message. Contains exactly the
    // REQ-PROSE-7 keys — events, room, inventory, recent_events — and no
    // entity/row ids anywhere (REQ-PROSE-6).
    std::string payload;

    // Validation anchors for the mechanical pre-display gate (REQ-PROSE-12).
    // canonRequired is true iff the turn contains a room-describing event:
    // verb 'moved', or 'looked' with NULL detail ('looked' with
    // detail='inventory' is NOT room-describing). canonText is the actor's
    // current room's canon prose, verbatim from the description table.
    bool canonRequired = false;
    std::string canonText;
    // Detail texts of this turn's 'failed' events (REQ-PROSE-12b): each must
    // appear verbatim in AI output, checked by the later validation step.
    std::vector<std::string> failedDetails;
};

// Pure function of (db, turn): SELECTs only, no network, no globals. The room
// slice is the ACTOR'S CURRENT room in post-commit state — for 'moved' turns
// that is the destination.
TurnFacts buildFacts(Db& db, int64_t turn);

// Render every event of `turn` as AI prose. Returns std::nullopt when AI
// rendering is unavailable (caller falls back to the template renderer).
std::optional<std::string> aiRender(Db& db, int64_t turn);
