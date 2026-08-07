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
#include <functional>
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
    // Canon prose of an entity examined this turn (REQ-EXAMINE-25), verbatim
    // from the description table. EMPTY means clause f DOES NOT APPLY — either
    // no 'examined' event this turn, or the examined entity has no description
    // row (REQ-EXAMINE-25a). It never means "the empty string was not found":
    // requiring the template's fallback line verbatim would pin AI output to
    // template wording, the opposite of what every other verb does.
    std::string examinedText;
};

// Pure function of (db, turn): SELECTs only, no network, no globals. The room
// slice is the ACTOR'S CURRENT room in post-commit state — for 'moved' turns
// that is the destination.
TurnFacts buildFacts(Db& db, int64_t turn);

// Transport seam (REQ-PROSE-9, REQ-PROSE-10): the ONLY thing a transport
// varies is the response. URL, headers (x-api-key, anthropic-version,
// content-type), and timeout are fixed properties of the production
// transport, not parameters — tests substitute fakes that return canned
// HttpResponse values and never touch the network.
struct HttpResponse {
    bool transportError = false;  // timeout, connect failure, curl error
    long status = 0;
    std::string body;
};
using HttpTransport = std::function<HttpResponse(const std::string& body)>;

// Anthropic Messages API request body (REQ-PROSE-8): model =
// modelForRole(AiRole::Narrate) — claude-opus-4-8 by default, with
// TEXTWORLD_MODEL overriding every role; the precedence rule lives in
// aihttp.hpp and is stated nowhere else. max_tokens 1024, the REQ-PROSE-11
// narrator system prompt, and exactly one user message carrying the facts
// payload. No thinking, no stream, no prompt caching keys — ever.
std::string buildRequestBody(const std::string& factsPayload);

// Mechanical validation gate (REQ-PROSE-13): pure function of (response,
// anchors) — no DB, no network, never throws. Returns the extracted prose iff
// ALL clauses hold, std::nullopt otherwise:
//   a. status == 200 AND response JSON stop_reason == "end_turn";
//   b. body parses as JSON with a FIRST content block of type "text" whose
//      text is non-empty;
//   c. if facts.canonRequired: facts.canonText appears as an EXACT substring;
//   d. EVERY facts.failedDetails entry appears as an EXACT substring;
//   e. text length <= 1200 characters;
//   f. if facts.examinedText is NON-EMPTY: it appears as an EXACT substring.
//      An empty examinedText means the clause does not apply (REQ-EXAMINE-25a).
// Consumes ONLY the anchor fields of TurnFacts (payload is ignored). On
// failure it emits one stderr diagnostic line naming the first failed clause
// in a..f order; nothing is ever appended to the returned prose.
std::optional<std::string> validateAiResponse(const HttpResponse& response,
                                              const TurnFacts& facts);

// AI narration switch (REQ-PROSE-2): true iff ANTHROPIC_API_KEY is set and
// non-empty AND TEXTWORLD_AI is unset or anything except exactly "0". The
// caller checks this BEFORE aiRender, so a disabled run never builds a
// request or touches a transport.
bool aiNarrationEnabled();

// Render every event of `turn` as AI prose. Returns std::nullopt when AI
// rendering is unavailable (caller falls back to the template renderer).
std::optional<std::string> aiRender(Db& db, int64_t turn);

// Test-visible overload: same contract, but the HTTP transport is injected.
// The two-arg production version delegates here, binding the libcurl
// transport. The transport is invoked exactly once per call — no retries.
std::optional<std::string> aiRender(Db& db, int64_t turn,
                                    const HttpTransport& transport);
