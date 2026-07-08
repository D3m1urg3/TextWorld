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

// The resolver's system prompt (REQ-RESOLVE-12) — git-versioned, like the
// narrator prompt. It IS the ISA spec: it defines the seven verbs
// non-overlappingly and states every lowering rule (one action, in-scope-noun
// subject, no new nouns, direction for go, no tool call on unknown/multi-
// intent, no pronoun resolution). Exposed so its structure is spot-checkable
// by substring; reword with care.
extern const char* const kResolveSystemPrompt;

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

// Anthropic Messages API request body for the resolver (REQ-RESOLVE-8, -9).
// Mirrors buildRequestBody except: max_tokens 512; a `tools` array carrying one
// `emit_action` tool whose input schema has a schema-enforced `verb` enum of
// exactly the seven ISA verbs, an optional `subject` string, and an optional
// `direction` string; `tool_choice` = {"type":"auto"}. Model from
// TEXTWORLD_MODEL if set and non-empty (else claude-opus-4-8; shared with the
// renderer). system = kResolveSystemPrompt, one user message carrying the
// context payload. No thinking, no stream, no prompt caching keys — ever.
std::string buildResolveRequestBody(const std::string& contextPayload);

// Validation + mapping gate (REQ-RESOLVE-13): pure function of (response, db) —
// SELECTs only (the clause-c noun lookup), no network, NEVER throws. The
// resolver's analog of validateAiResponse. Returns the lowered Action iff ALL
// clauses hold, std::nullopt otherwise:
//   a. status == 200 AND the body has EXACTLY ONE tool_use block for
//      emit_action (0 blocks → clean no-action; >=2 → fail, one opcode/line);
//   b. input.verb is exactly one of the seven ISA verbs;
//   c. take/drop: input.subject present and lookupNoun() resolves it world-wide
//      to a NON-ZERO id (recognition, NOT scope applicability) — the id is
//      assigned mechanically here, never taken from the model;
//   d. go: input.direction present and non-empty;
//   e. look/inventory/wait/quit: no argument is consulted.
// On a genuine 0-block "no tool call" it returns nullopt WITHOUT a diagnostic
// (the model correctly declined; the caller falls back to the parser). On any
// failed clause it emits one stderr diagnostic naming the first failed clause
// in a..e order and returns nullopt. No entity id is ever read FROM the model.
std::optional<Action> validateAndLower(const HttpResponse& response, Db& db);

// Resolve one input line to an Action via Claude tool-use (REQ-RESOLVE-1, -3).
// Returns std::nullopt when resolution is unavailable or declines — the caller
// falls back to the deterministic parser. The two-arg production version binds
// the libcurl transport; the caller checks aiNarrationEnabled() BEFORE calling,
// so a disabled run never constructs a transport.
std::optional<Action> aiResolve(Db& db, const std::string& line);

// Test-visible overload: same contract, but the HTTP transport is injected.
// buildResolveContext -> buildResolveRequestBody -> ONE transport call ->
// validateAndLower. The whole body sits in try/catch: ANY failure (including a
// throwing transport) yields one stderr line and nullopt (REQ-RESOLVE-3). The
// transport is invoked at most once per call — no retries (REQ-RESOLVE-10).
std::optional<Action> aiResolve(Db& db, const std::string& line,
                                const HttpTransport& transport);

// Full input resolution with fallback (REQ-RESOLVE-1, -4, -15): aiResolve, and
// on nullopt the fixed-verb parse() — the permanent deterministic fallback.
// Returns the first that yields an Action, else nullopt (which drives
// renderError in runTurn). Order: aiResolve -> parse -> (caller) renderError.
// The two-arg production version binds the libcurl transport; the injected-
// transport overload makes the whole chain unit-testable with no live call.
std::optional<Action> resolveOrParse(Db& db, const std::string& line);
std::optional<Action> resolveOrParse(Db& db, const std::string& line,
                                     const HttpTransport& transport);
