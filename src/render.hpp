// Renderer: this turn's event rows → text. Design §6.
//
// Contract: render()'s INPUT is the event rows of one turn. It may
// additionally READ world state (canon prose, exits, names, containment)
// to flesh out descriptions, but it performs ONLY SELECTs — never writes —
// and never emits an output line without a sourcing event row. This is
// exactly the contract the future AI prose layer inherits.
#pragma once

#include <cstdint>
#include <string>

#include "db.hpp"

// Render every event of `turn` (in event id order) through dumb templates,
// concatenating the results. Read-only: SELECTs only. A turn with no events
// renders as the empty string.
std::string render(Db& db, int64_t turn);

// Formatting path for tier-a parse failures. NOT event-sourced: no tick
// happened, so this output lives outside the turn contract entirely.
std::string renderError(const std::string& msg);

// The room description block for `actor`'s current room. Read-only, like all
// of render. Exists for the startup courtesy render (show the room before the
// first prompt WITHOUT a tick or a 'looked' event); it reuses the exact block
// the 'moved'/'looked' templates emit.
std::string renderRoomOf(Db& db, int64_t actor);
