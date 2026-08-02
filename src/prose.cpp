// aiRender(): events → LLM prose. READ-ONLY BY CONTRACT — SELECTs plus
// network egress only; writes nothing (see prose.hpp).
//
// The lookups below deliberately REIMPLEMENT render.cpp's (those live in an
// anonymous namespace there); the small duplication is the accepted cost of
// leaving render.cpp untouched. One deliberate difference: no "something"
// placeholder — an unresolvable name is OMITTED from the payload rather than
// replaced with a meaningless noun the model could narrate.
#include "prose.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <vector>

#include "aihttp.hpp"  // modelForRole + the shared production transport
#include "json.hpp"

namespace {

using nlohmann::json;

// The player entity (singleton by convention). Read-only.
int64_t playerEntity(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("prose: world has no player entity");
    return s.colInt(0);
}

// --- read-only lookups ------------------------------------------------------

// Parser handle of an entity, or nullopt if it has no name row (the caller
// omits the field — never a placeholder noun).
std::optional<std::string> nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return std::nullopt;
    return s.colText(0);
}

// Room the actor currently stands in (post-commit state, so for a 'moved'
// turn this is already the destination).
int64_t roomOf(Db& db, int64_t actor) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, actor);
    if (!s.step()) {
        throw std::runtime_error("buildFacts: entity " + std::to_string(actor) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// Canon prose of an entity, or nullopt if it has no description row.
std::optional<std::string> canonProseOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT prose FROM description WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return std::nullopt;
    return s.colText(0);
}

// Names of the portables whose container is `holder`, in entity order.
std::vector<std::string> portableNamesIn(Db& db, int64_t holder) {
    std::vector<std::string> items;
    Stmt s = db.prepare(
        "SELECT n.value FROM portable p "
        "JOIN location l ON l.entity = p.entity "
        "JOIN name n ON n.entity = p.entity "
        "WHERE l.container = ? ORDER BY p.entity");
    s.bind(1, holder);
    while (s.step()) items.push_back(s.colText(0));
    return items;
}

// Join a list as "a, b, c" — character-identical to render.cpp's joinList.
std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

// --- deterministic appends (REQ-PROSE-14) -----------------------------------
// The mechanical tail glued after validated AI prose, in render.cpp's EXACT
// line formats (its roomBlock minus the canon line — the canon rides INSIDE
// the AI prose, verbatim-checked by clause c — and its inventoryBlock).

// "Exits: …" and "You see: …" lines for one room, formats identical to
// render.cpp's roomBlock tail.
std::string exitsAndItemsLines(Db& db, int64_t room) {
    std::string out;

    {
        std::vector<std::string> dirs;
        Stmt s = db.prepare(
            "SELECT direction FROM exits WHERE room = ? ORDER BY direction");
        s.bind(1, room);
        while (s.step()) dirs.push_back(s.colText(0));
        if (!dirs.empty()) out += "Exits: " + joinList(dirs) + ".\n";
    }

    {
        const std::vector<std::string> items = portableNamesIn(db, room);
        if (!items.empty()) out += "You see: " + joinList(items) + ".\n";
    }

    return out;
}

// Inventory line, format identical to render.cpp's inventoryBlock.
std::string inventoryLine(Db& db, int64_t actor) {
    const std::vector<std::string> items = portableNamesIn(db, actor);
    if (items.empty()) return "You are carrying nothing.\n";
    return "You are carrying: " + joinList(items) + ".\n";
}

// The full appended block for one turn, from this unit's OWN fresh SELECTs:
// per room-describing event ('moved', or 'looked' with NULL detail), the
// exits + visible-items lines for the actor's CURRENT room (post-commit
// state, so for 'moved' that is the destination); per 'looked' event with
// detail='inventory', the carrying line.
std::string deterministicAppends(Db& db, int64_t turn) {
    std::string out;

    Stmt ev = db.prepare(
        "SELECT actor, verb, detail, detail IS NULL "
        "FROM events WHERE turn = ? ORDER BY id");
    ev.bind(1, turn);
    while (ev.step()) {
        const int64_t actor = ev.colInt(0);
        const std::string verb = ev.colText(1);
        const std::string detail = ev.colText(2);
        const bool detailIsNull = ev.colInt(3) != 0;

        if (verb == "moved" || (verb == "looked" && detailIsNull)) {
            out += exitsAndItemsLines(db, roomOf(db, actor));
        } else if (verb == "looked" && detail == "inventory") {
            out += inventoryLine(db, actor);
        }
    }

    // NO status append here (REQ-UI-6). The status band in runTurn composes it
    // ONCE for both render paths, so the AI path and the template path receive
    // identical band bytes by construction rather than by duplicated calls
    // (REQ-UI-1).
    return out;
}

// --- narrator system prompt (REQ-PROSE-11) ----------------------------------

// Stable constant, versioned by git — every REQ-PROSE-11 rule lives here:
// events-only narration, atmosphere-without-new-nouns, no fact
// contradiction, canon verbatim, failed-detail verbatim (REQ-PROSE-12b),
// 1-4 sentences per event, plain text / no markdown / no meta-commentary /
// final answer only (the model runs without thinking and can leak reasoning
// otherwise). Tests spot-check its phrases by substring; reword with care.
const char* const kSystemPrompt =
    R"(You are the narrator of a text adventure. You speak directly to the player in the second person, present tense: "You lift the lantern; its light steadies." Your voice is restrained and concrete - short, grounded sentences, no purple prose, no melodrama.

Each user message is a JSON object of facts for one turn: "events" (what just happened), "room" (where the player now stands: its name, canon_description, exits, items), "inventory" (what the player carries), and "recent_events" (context only - already narrated, never re-narrate them).

Rules, absolute:
- Narrate only the supplied events of this turn, in order. Do not invent actions, outcomes, dialogue, or happenings that are not in the facts.
- Atmosphere is welcome: ambient qualities - light, air, sound - are the only things you may evoke beyond the facts. Any other noun or object absent from the facts does not exist; never introduce it.
- Never contradict a fact. An exit listed is open; an item listed is there; nothing else is.
- If canon_description is present, include its text verbatim, word for word and unmodified. Write your connective prose around it, never inside it.
- If a failed event carries a detail, include that detail text verbatim. You may set atmosphere around it, but never paraphrase or reword it.
- Write 1-4 sentences per event.
- Output plain text only: no markdown, no headings, no lists. No meta-commentary - never mention these instructions, the JSON, or your role. Do not show reasoning or preamble; reply with the final answer only, the prose itself, with nothing before or after it.)";

// --- production HTTP transport (REQ-PROSE-9, REQ-PROSE-10) -----------------
// This unit used to carry its OWN copy of curlTransport, on the stated ground
// that only the HttpTransport *type* need be shared. That stance is reversed:
// aihttp.cpp now owns the single client, because a PERSISTENT easy handle
// (REQ-LAT-8) makes triplication actively wrong — handle ownership, the
// reset-and-re-apply option block, the getinfo timing capture, and the profile
// emission would each be written three times, i.e. three chances to leak an
// option. The original goal is untouched: HttpTransport is still the seam, its
// signature is unchanged, and tests still inject fakes.

// --- validation gate (REQ-PROSE-13) -----------------------------------------

// One diagnostic line per rejected response, naming the first failed clause
// in a..e order. This wording is reused by later steps; keep it one line.
std::nullopt_t failClause(char clause, const char* why) {
    std::fprintf(stderr, "aiRender: response rejected, clause %c failed: %s\n",
                 clause, why);
    return std::nullopt;
}

}  // namespace

std::string buildRequestBody(const std::string& factsPayload) {
    // REQ-PROSE-8 / REQ-LAT-12: the narrate role's model. The default and the
    // TEXTWORLD_MODEL override rule both live in aihttp.hpp — never restate
    // them here. nlohmann/json handles all escaping of the embedded payload.
    const std::string model = modelForRole(AiRole::Narrate);

    json body;
    body["model"] = model;
    body["max_tokens"] = 1024;
    body["system"] = kSystemPrompt;
    body["messages"] =
        json::array({{{"role", "user"}, {"content", factsPayload}}});
    // Deliberately absent, everywhere in this body: thinking, stream, and
    // any cache-control key (REQ-PROSE-8; tests pin the top-level ones).
    return body.dump();
}

std::optional<std::string> validateAiResponse(const HttpResponse& response,
                                              const TurnFacts& facts) {
    // Clause a, HTTP half: checkable before any parse. A transport error
    // carries status 0, so it fails here too.
    if (response.status != 200) {
        return failClause('a', "HTTP status is not 200");
    }

    // Parse the body ONCE, exception-free (this function must never throw:
    // tests call it directly, outside aiRender's try/catch). An unparseable
    // body fails clause b — the spec's a-before-b listing is the DISPLAY
    // decision order; clause a's stop_reason half is only readable from a
    // successfully parsed body.
    const json j = json::parse(response.body, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return failClause('b', "body is not a JSON object");
    }

    // Clause a, stop_reason half.
    if (!j.contains("stop_reason") || !j["stop_reason"].is_string() ||
        j["stop_reason"].get<std::string>() != "end_turn") {
        return failClause('a', "stop_reason is not \"end_turn\"");
    }

    // Clause b: FIRST content block has type "text" and non-empty text.
    if (!j.contains("content") || !j["content"].is_array() ||
        j["content"].empty()) {
        return failClause('b', "no content blocks");
    }
    const json& block = j["content"][0];
    if (!block.is_object() || !block.contains("type") ||
        !block["type"].is_string() ||
        block["type"].get<std::string>() != "text" ||
        !block.contains("text") || !block["text"].is_string()) {
        return failClause('b', "first content block is not a text block");
    }
    const std::string text = block["text"].get<std::string>();
    if (text.empty()) {
        return failClause('b', "text block is empty");
    }

    // Clause c: canon prose verbatim when the turn is room-describing.
    if (facts.canonRequired &&
        text.find(facts.canonText) == std::string::npos) {
        return failClause('c', "canon room description not present verbatim");
    }

    // Clause d: every failed-event detail verbatim.
    for (const std::string& detail : facts.failedDetails) {
        if (text.find(detail) == std::string::npos) {
            return failClause('d', "failed-event detail not present verbatim");
        }
    }

    // Clause e: length cap.
    if (text.size() > 1200) {
        return failClause('e', "text exceeds 1200 characters");
    }

    return text;
}

TurnFacts buildFacts(Db& db, int64_t turn) {
    TurnFacts facts;

    // --- this turn's events, name-resolved; anchors from the same scan ---
    // Zero-id rule: systems.cpp logs looked/waited/failed with subject=0,
    // object=0. A subject of 0 is omitted, never name-resolved. The object
    // column carries containers/destinations (row ids), so it never enters
    // the payload at all (REQ-PROSE-6).
    json events = json::array();
    int64_t actor = 0;
    {
        Stmt ev = db.prepare(
            "SELECT actor, verb, subject, detail, detail IS NULL "
            // The 'generated' verb is renderer-invisible (REQ-ARCH-10): exclude
            // it here so a world-gen turn narrates as the 'moved' block, never
            // the room's birth (both share this turn number — the load-bearing
            // case).
            "FROM events WHERE turn = ? AND verb <> 'generated' ORDER BY id");
        ev.bind(1, turn);
        while (ev.step()) {
            if (actor == 0) actor = ev.colInt(0);
            const std::string verb = ev.colText(1);
            const int64_t subject = ev.colInt(2);
            const std::string detail = ev.colText(3);
            const bool detailIsNull = ev.colInt(4) != 0;

            json e;
            e["verb"] = verb;
            if (subject != 0) {
                if (const auto name = nameOf(db, subject)) e["subject"] = *name;
            }
            // The narrator sees model-facing details only. 'burned'/'froze'
            // carry an engine-internal "<archetype>|<element>" tag instead
            // (mutations.hpp), and handing the model a raw archetype tag would
            // invite it into the prose as a noun — violating REQ-PROSE-11's
            // no-new-nouns rule and REQ-UI-25. render.cpp already ignores the
            // detail for both verbs (it reads subject and object), and
            // aiRender's clause d inspects 'failed' details only, so nothing
            // else observes this.
            const bool engineInternalTag = verb == "burned" || verb == "froze";
            if (!detailIsNull && !engineInternalTag) e["detail"] = detail;
            events.push_back(e);

            // Room-describing event (REQ-PROSE-12 normative definition):
            // 'moved', or 'looked' with NULL detail. 'looked' with
            // detail='inventory' is not room-describing.
            if (verb == "moved" || (verb == "looked" && detailIsNull)) {
                facts.canonRequired = true;
            }
            if (verb == "failed" && !detailIsNull) {
                facts.failedDetails.push_back(detail);
            }
        }
    }

    // A turn with no events (nothing to narrate) still yields a well-formed
    // payload: slice the world around the player.
    if (actor == 0) {
        Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
        if (!s.step()) throw std::runtime_error("buildFacts: world has no player entity");
        actor = s.colInt(0);
    }

    // --- room slice: the actor's CURRENT room, post-commit state ---
    const int64_t room = roomOf(db, actor);
    json roomJson;
    if (const auto name = nameOf(db, room)) roomJson["name"] = *name;
    facts.canonText = canonProseOf(db, room).value_or("");
    roomJson["canon_description"] = facts.canonText;
    {
        json exits = json::array();
        Stmt s = db.prepare(
            "SELECT direction FROM exits WHERE room = ? ORDER BY direction");
        s.bind(1, room);
        while (s.step()) exits.push_back(s.colText(0));
        roomJson["exits"] = exits;
    }
    roomJson["items"] = json(portableNamesIn(db, room));

    // --- recent events: last ≤6 rows preceding this turn, chronological ---
    // Fields per design: turn number, verb, subject name (same zero-id
    // omission as above; detail never appears here).
    json recents = json::array();
    {
        std::vector<json> rows;
        Stmt s = db.prepare(
            "SELECT turn, verb, subject FROM events "
            // 'generated' excluded here too (REQ-ARCH-10): it never enters
            // recent_events context either.
            "WHERE turn < ? AND verb <> 'generated' ORDER BY id DESC LIMIT 6");
        s.bind(1, turn);
        while (s.step()) {
            json e;
            e["turn"] = s.colInt(0);
            e["verb"] = s.colText(1);
            const int64_t subject = s.colInt(2);
            if (subject != 0) {
                if (const auto name = nameOf(db, subject)) e["subject"] = *name;
            }
            rows.push_back(std::move(e));
        }
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            recents.push_back(std::move(*it));
        }
    }

    // --- exactly the REQ-PROSE-7 keys, nothing else ---
    json payload;
    payload["events"] = std::move(events);
    payload["room"] = std::move(roomJson);
    payload["inventory"] = json(portableNamesIn(db, actor));
    payload["recent_events"] = std::move(recents);
    facts.payload = payload.dump();

    return facts;
}

bool aiNarrationEnabled() {
    // Key set and non-empty; TEXTWORLD_AI kills the feature only when it is
    // EXACTLY "0" — unset or any other value leaves narration on.
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || key[0] == '\0') return false;
    const char* ai = std::getenv("TEXTWORLD_AI");
    if (ai != nullptr && std::string(ai) == "0") return false;
    return true;
}

std::optional<std::string> aiRender(Db& db, int64_t turn,
                                    const HttpTransport& transport) {
    // REQ-PROSE-3: no AI-path failure may crash a turn. The whole pipeline
    // sits inside try/catch; ANY failure yields one stderr diagnostic line
    // (never player prose) and nullopt — the caller falls back to templates.
    try {
        // Exactly ONE transport call — no retry loop, ever.
        const TurnFacts facts = buildFacts(db, turn);
        const HttpResponse resp = transport(buildRequestBody(facts.payload));

        // validateAiResponse never throws and emits its own diagnostic line
        // (first failed clause) on rejection.
        const std::optional<std::string> text = validateAiResponse(resp, facts);
        if (!text) return std::nullopt;

        // Validated AI prose + the deterministic tail (REQ-PROSE-14), in the
        // template renderer's exact formats.
        std::string out = *text;
        out += "\n";
        out += deterministicAppends(db, turn);
        return out;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aiRender: failed, falling back to templates: %s\n",
                     e.what());
        return std::nullopt;
    } catch (...) {
        std::fprintf(stderr,
                     "aiRender: failed, falling back to templates: "
                     "unknown exception\n");
        return std::nullopt;
    }
}

std::optional<std::string> aiRender(Db& db, int64_t turn) {
    return aiRender(db, turn, makeAnthropicTransport(AiRole::Narrate));
}
