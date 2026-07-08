// AI action resolver: one raw input line → the engine's closed Action ISA,
// via Claude tool-use, with the fixed-verb parser as the permanent
// deterministic fallback. The input-side analog of the prose renderer.
//
// READ-ONLY BY CONTRACT — this unit performs ONLY SELECTs against the DB;
// no INSERT, UPDATE, or DELETE may ever appear in its translation unit. Like
// the prose renderer it adds one effect the parser contract didn't have —
// network egress — reading world state, writing nothing, and sending scope
// facts (never the DB, never entity ids) to the LLM. The lowered Action it
// returns carries ids assigned MECHANICALLY here, never from the model.
#pragma once

#include <optional>
#include <string>

#include "action.hpp"  // Action / Verb — the ISA this resolver emits
#include "db.hpp"
#include "prose.hpp"    // HttpResponse / HttpTransport seam, reused as-is

// The scope facts for one input line, assembled by buildResolveContext() from
// fresh SELECTs. `payload` is the JSON string sent as the LLM user message; it
// carries EXACTLY the REQ-RESOLVE-7 fields — input, room, exits, items,
// inventory — and no entity/row ids anywhere (REQ-RESOLVE-6).
struct ResolveContext {
    std::string payload;
};

// Pure function of (db, line): SELECTs only, no network, no globals. Resolves
// the player entity itself (SELECT entity FROM player LIMIT 1) and slices the
// room / visible items / inventory relative to it. The raw line travels
// verbatim; nothing else — no ids, no schema — enters the payload.
ResolveContext buildResolveContext(Db& db, const std::string& line);
