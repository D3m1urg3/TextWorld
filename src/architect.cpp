// buildArchitectContext(): origin room + setting + direction → the LLM user
// message. READ-ONLY here — SELECTs plus network egress only; the ONLY writes
// go through mutations.cpp's writeGeneratedRoom (see architect.hpp).
//
// Like nlresolve.cpp, the lookups below deliberately REIMPLEMENT prose.cpp's
// (which live in an anonymous namespace there); the small duplication is the
// accepted cost of leaving prose.cpp untouched.
#include "architect.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "json.hpp"
#include "mutations.hpp"  // writeGeneratedRoom — the SOLE sanctioned write path

namespace {

using nlohmann::json;

// --- validation gate helpers (REQ-ARCH-9) -----------------------------------

// One diagnostic line per rejected proposal, naming the first failed clause in
// a..c order. Mirrors the resolver's failClause; std::nullopt_t converts to the
// optional<RoomProposal> the gate returns.
std::nullopt_t failClause(char clause, const char* why) {
    std::fprintf(stderr,
                 "validateRoomProposal: rejected, clause %c failed: %s\n", clause,
                 why);
    return std::nullopt;
}

// True iff `s` is empty or all-whitespace (clauses b/c: non-empty after trim).
bool blankAfterTrim(const std::string& s) {
    for (const unsigned char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
            c != '\v') {
            return false;
        }
    }
    return true;
}

// Trim leading/trailing ASCII whitespace and lowercase, so an exit like
// "North" or " up " matches the fixed direction table (REQ-EXITS-7).
std::string normalizeDirection(const std::string& s) {
    auto isWs = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v';
    };
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && isWs(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && isWs(static_cast<unsigned char>(s[end - 1]))) --end;
    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// --- production HTTP transport (REQ-ARCH-11) --------------------------------
// Deliberately REIMPLEMENTS prose/resolver's curlTransport (both anon-namespace-
// private there); only the HttpTransport *type* is shared (micro-decision #3).
// Same URL / headers / 8 s timeout / no retries. No direct unit test — exercised
// only by the gated live smoke (Step 10).

// libcurl write callback: append the response bytes to a std::string.
size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One POST to the Anthropic Messages API. URL, headers, and timeout are fixed
// properties of THIS transport, never seam parameters. The API key is read from
// ANTHROPIC_API_KEY at call time and goes into the x-api-key header ONLY. Any
// curl failure → transportError; NO retries.
HttpResponse curlTransport(const std::string& body) {
    HttpResponse resp;

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        resp.transportError = true;
        return resp;
    }

    const char* key = std::getenv("ANTHROPIC_API_KEY");
    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, ("x-api-key: " + std::string(key != nullptr ? key : "")).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);  // total budget, seconds
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.transportError = true;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

// --- read-only lookups ------------------------------------------------------

// The setting text loaded at init (REQ-ARCH-1), or empty if absent.
std::string settingText(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'setting'");
    if (!s.step()) return "";
    return s.colText(0);
}

// Parser handle of an entity, or empty if it has no name row.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

// Canon prose of a room (the description table), or empty if none.
std::string canonProseOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT prose FROM description WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

}  // namespace

// --- architect system prompt (REQ-ARCH-7c) ----------------------------------

// Stable constant, versioned by git — this prompt IS the generation contract:
// one room, coherent with the setting and origin, emitted via create_room as a
// name + standalone description + declared onward exits, with the return-exit
// exclusion and the arrival/id prohibitions that keep the model to flavor +
// onward directions and the engine to structure. Prompt QUALITY is verified
// live (Step 10 / spec AI-Validation item 4); the unit test here only pins its
// STRUCTURE by substring, so it can never become a live tune-retry loop. Reword
// with care: tests spot-check its phrases.
const char* const kArchitectPrompt =
    R"(You are the architect of a text adventure world. When the player leaves a room in a direction that has no room beyond it yet, you invent exactly one room that lies that way, by calling the create_room tool. You never write anything else - your only output is a single create_room call.

Each user message is a JSON object of context: "setting" (the world's tone, premise, and scale - the shared frame every room must fit), "origin_name" (the name of the room the player is leaving), "origin_description" (the canon prose of that room), and "direction" (the way the player is travelling out of it).

Your task: generate exactly one room that is reachable by travelling "direction" from the origin room. It must be coherent with the setting - matching its tone, scale, and premise - and consistent with the origin room it adjoins, as though the two have always been neighbours. If the setting is empty, invent a plain, quiet room that could plausibly adjoin the origin.

Emit the room with a single create_room call carrying:
- name: a short handle for the room (a few words, like "stone hall" or "chapter house").
- description: standalone room prose, two to four sentences, written as the player reads it on first entry. Describe what is here - the space, its air, its light, what remains in it - including the ways that lead onward.
- exits: an array of the directions that lead onward from this room, each one of "north", "south", "east", "west", "up", "down", "in", "out". Declare the directions that fit this room and setting - how many is your call (a sealed vault may declare none, a crossroads several).

Rules, absolute:
- Call create_room exactly once. Write no prose outside the tool call.
- The description is of THIS room only. Declare in exits the directions that lead onward, and describe those declared exits in the prose; name no opening you did not declare. EXCLUDE the direction back the way the player came - the engine adds that return exit itself. Do NOT narrate the player's arrival, movement, or the act of entering ("you step into...") - describe the standing room, not the journey to it.
- Invent no ids, numbers, or identifiers of any kind. You name, describe, and declare exit directions; the engine assigns everything else.
- Introduce nothing that contradicts the setting or the origin room.)";

std::string buildArchitectContext(Db& db, int64_t room,
                                  const std::string& direction) {
    // EXACTLY the REQ-ARCH-7a fields, nothing else. No ids ever enter the
    // payload (REQ-ARCH-6): `room` is used only to SELECT the origin's name and
    // canon description. nlohmann/json handles all escaping. Empty setting or
    // empty origin fields still yield a well-formed object.
    json payload;
    payload["setting"] = settingText(db);
    payload["origin_name"] = nameOf(db, room);
    payload["origin_description"] = canonProseOf(db, room);
    payload["direction"] = direction;
    return payload.dump();
}

std::string buildArchitectRequestBody(const std::string& contextPayload) {
    // Model: TEXTWORLD_MODEL (set AND non-empty) else claude-opus-4-8 — the same
    // rule the renderer/resolver use, so all AI features share one override.
    const char* env = std::getenv("TEXTWORLD_MODEL");
    const std::string model =
        (env != nullptr && env[0] != '\0') ? env : "claude-opus-4-8";

    // The single create_room tool (REQ-ARCH-7b / REQ-EXITS-5): a schema-enforced
    // object with REQUIRED name + description strings and one OPTIONAL exits
    // array (invertible direction names that lead onward, excluding the entry
    // return). The model proposes flavor + onward directions; ids and the return
    // exit are the engine's, never on the wire.
    json createRoom;
    createRoom["name"] = "create_room";
    createRoom["description"] =
        "Create exactly one new room reachable from the origin in the given "
        "direction. Provide its name and its standalone description prose.";
    json properties;
    properties["name"] = {
        {"type", "string"},
        {"description", "A short handle for the room, a few words."}};
    properties["description"] = {
        {"type", "string"},
        {"description",
         "Standalone room prose as the player reads it on entry: the space, "
         "its air and light, what remains in it. Describe the exits you "
         "declare; narrate no arrival."}};
    properties["exits"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description",
         "the invertible directions that lead onward from this room, "
         "excluding the way the player entered"}};
    json inputSchema;
    inputSchema["type"] = "object";
    inputSchema["properties"] = std::move(properties);
    // exits is OPTIONAL — absent/empty is a dead end; only name+description
    // are required (REQ-EXITS-5).
    inputSchema["required"] = json::array({"name", "description"});
    createRoom["input_schema"] = std::move(inputSchema);

    json body;
    body["model"] = model;
    body["max_tokens"] = 1024;  // one room's prose, per REQ-ARCH-7b
    body["system"] = kArchitectPrompt;
    body["messages"] =
        json::array({{{"role", "user"}, {"content", contextPayload}}});
    body["tools"] = json::array({std::move(createRoom)});
    // tool_choice REQUIRES the tool: the architect's job on this path is to
    // produce a room; a non-call is a gate failure → wall (REQ-ARCH-7b).
    body["tool_choice"] = {{"type", "tool"}, {"name", "create_room"}};
    // Deliberately absent everywhere in this body: thinking, stream, and any
    // cache-control key (the test pins the exact top-level set).
    return body.dump();
}

std::optional<std::string> inverseDirection(const std::string& direction) {
    // The fixed engine table (REQ-ARCH-8). Anything outside it is not
    // generatable → nullopt.
    if (direction == "north") return "south";
    if (direction == "south") return "north";
    if (direction == "east") return "west";
    if (direction == "west") return "east";
    if (direction == "up") return "down";
    if (direction == "down") return "up";
    if (direction == "in") return "out";
    if (direction == "out") return "in";
    return std::nullopt;
}

std::optional<RoomProposal> validateRoomProposal(
    const HttpResponse& response, const std::string& directionOfTravel) {
    // Clause a, HTTP half: a transport error carries status 0, so it fails here.
    if (response.status != 200) {
        return failClause('a', "HTTP status is not 200");
    }

    // Parse the body ONCE, exception-free — this function must never throw
    // (architectGenerate calls it inside try/catch, but tests call it directly).
    const json j = json::parse(response.body, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return failClause('a', "body is not a JSON object");
    }
    if (!j.contains("content") || !j["content"].is_array()) {
        return failClause('a', "response has no content array");
    }

    // Clause a, block half: count create_room tool_use blocks. The Anthropic
    // shape is content[] with {type:"tool_use", name:"create_room", input:{...}};
    // navigate it, don't assume position.
    const json* create = nullptr;
    int createCount = 0;
    for (const json& block : j["content"]) {
        if (block.is_object() && block.value("type", "") == "tool_use" &&
            block.value("name", "") == "create_room") {
            ++createCount;
            create = &block;
        }
    }
    if (createCount == 0) {
        // tool_choice REQUIRED the tool, so a non-call is a gate failure → wall
        // (unlike the resolver, there is no clean decline path here).
        return failClause('a', "no create_room tool_use block");
    }
    if (createCount >= 2) {
        return failClause('a', "more than one create_room tool_use block");
    }

    // Exactly one create_room block. Its input object carries name + description;
    // any other field (a spurious id) is simply never read (REQ-ARCH-6).
    if (!create->contains("input") || !(*create)["input"].is_object()) {
        return failClause('b', "create_room block has no input object");
    }
    const json& input = (*create)["input"];

    // Clause b: name present, a string, non-empty after trim.
    if (!input.contains("name") || !input["name"].is_string()) {
        return failClause('b', "name missing or not a string");
    }
    const std::string name = input["name"].get<std::string>();
    if (blankAfterTrim(name)) {
        return failClause('b', "name is empty after trim");
    }

    // Clause c: description present, a string, non-empty after trim.
    if (!input.contains("description") || !input["description"].is_string()) {
        return failClause('c', "description missing or not a string");
    }
    const std::string description = input["description"].get<std::string>();
    if (blankAfterTrim(description)) {
        return failClause('c', "description is empty after trim");
    }

    // --- exit sanitization (REQ-EXITS-7): LENIENT. A bad exit NEVER rejects the
    // room — from here on we only drop, with one stderr diagnostic per drop, and
    // never fail. Absent/empty exits → empty vector (a dead end).
    RoomProposal proposal{name, description, {}};

    // The return direction the model was told to omit (REQ-EXITS-6): the inverse
    // of the way it travelled. Default "" → inverseDirection("") == nullopt → no
    // return direction to drop (the 11 exit-less call sites keep every entry).
    const std::optional<std::string> returnDir =
        inverseDirection(directionOfTravel);

    if (input.contains("exits") && input["exits"].is_array()) {
        for (const json& entry : input["exits"]) {
            if (!entry.is_string()) {
                std::fprintf(stderr,
                             "validateRoomProposal: dropped exit, not a "
                             "string\n");
                continue;
            }
            const std::string norm =
                normalizeDirection(entry.get<std::string>());
            if (!inverseDirection(norm)) {
                std::fprintf(stderr,
                             "validateRoomProposal: dropped exit '%s', not an "
                             "invertible direction\n",
                             norm.c_str());
                continue;
            }
            if (returnDir && norm == *returnDir) {
                std::fprintf(stderr,
                             "validateRoomProposal: dropped exit '%s', the "
                             "entry-return direction\n",
                             norm.c_str());
                continue;
            }
            bool duplicate = false;
            for (const std::string& kept : proposal.exits) {
                if (kept == norm) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                std::fprintf(stderr,
                             "validateRoomProposal: dropped exit '%s', a "
                             "duplicate\n",
                             norm.c_str());
                continue;
            }
            proposal.exits.push_back(norm);
        }
    }

    return proposal;
}

bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor, const HttpTransport& transport) {
    std::optional<RoomProposal> proposal;
    try {  // ── Phase 1 (AI side): NO database write happens in here ──
        const std::string ctx = buildArchitectContext(db, room, direction);
        const std::string body = buildArchitectRequestBody(ctx);
        const HttpResponse resp = transport(body);  // at most once, no retries
        proposal = validateRoomProposal(resp, direction);  // never throws
    } catch (const std::exception& e) {
        std::fprintf(stderr, "architectGenerate: phase 1 failed: %s\n", e.what());
        return false;  // → wall (REQ-ARCH-3c)
    } catch (...) {  // mirror aiResolve's catch-all
        std::fprintf(stderr, "architectGenerate: phase 1 failed (non-std)\n");
        return false;
    }
    if (!proposal) return false;  // gate failure → wall

    // ── Phase 2 (the write): OUTSIDE the catch. Runs only on a validated
    // proposal; a genuine DB fault propagates out to runTurn's tick rollback
    // (REQ-ARCH-5), never downgraded to a wall.
    writeGeneratedRoom(db, room, direction, *proposal, actor);
    return true;
}

bool architectGenerate(Db& db, int64_t room, const std::string& direction,
                       int64_t actor) {
    return architectGenerate(db, room, direction, actor, curlTransport);
}

bool architectEnabled() {
    // DISPLAY / ontology gate (REQ-EXITS-4). Returns aiNarrationEnabled() today,
    // but is intentionally a SEPARATE predicate from the prose/cosmetic switch:
    // showing a latent exit asserts the direction is traversable, so it must
    // track whether generation can run — not merely whether prose is dressed up.
    return aiNarrationEnabled();
}
