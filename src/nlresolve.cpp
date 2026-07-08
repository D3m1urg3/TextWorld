// buildResolveContext(): input line + world scope → the LLM user message.
// READ-ONLY BY CONTRACT — SELECTs plus network egress only; writes nothing
// (see nlresolve.hpp).
//
// The lookups below deliberately REIMPLEMENT prose.cpp's (those live in an
// anonymous namespace there); the small duplication is the accepted cost of
// leaving prose.cpp untouched, and mirrors prose's own "reimplement, don't
// reach across TUs" stance.
#include "nlresolve.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "json.hpp"
#include "lookup.hpp"

namespace {

using nlohmann::json;

// --- read-only lookups ------------------------------------------------------

// The player entity, resolved fresh (no actor parameter — the builder owns
// this, mirroring loop.cpp's playerId and prose's actor fallback).
int64_t playerEntity(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("buildResolveContext: world has no player entity");
    return s.colInt(0);
}

// Room the actor currently stands in.
int64_t roomOf(Db& db, int64_t actor) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, actor);
    if (!s.step()) {
        throw std::runtime_error("buildResolveContext: entity " +
                                 std::to_string(actor) + " has no location row");
    }
    return s.colInt(0);
}

// Parser handle of an entity, or empty if it has no name row.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

// Exit direction words for a room, alphabetical (same shape prose uses).
std::vector<std::string> exitsOf(Db& db, int64_t room) {
    std::vector<std::string> dirs;
    Stmt s = db.prepare(
        "SELECT direction FROM exits WHERE room = ? ORDER BY direction");
    s.bind(1, room);
    while (s.step()) dirs.push_back(s.colText(0));
    return dirs;
}

// Names of the portables whose container is `holder`, in entity order
// (character-identical to prose.cpp's portableNamesIn).
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

// --- validation gate helpers (REQ-RESOLVE-13) -------------------------------

// One diagnostic line per rejected response, naming the first failed clause in
// a..e order. Mirrors prose's failClause; std::nullopt_t converts to the
// optional<Action> the gate returns.
std::nullopt_t failClause(char clause, const char* why) {
    std::fprintf(stderr, "aiResolve: response rejected, clause %c failed: %s\n",
                 clause, why);
    return std::nullopt;
}

// The seven ISA verbs, and nothing else (clause b). nullopt for any other word.
std::optional<Verb> verbFromWord(const std::string& word) {
    if (word == "look") return Verb::Look;
    if (word == "go") return Verb::Go;
    if (word == "take") return Verb::Take;
    if (word == "drop") return Verb::Drop;
    if (word == "inventory") return Verb::Inventory;
    if (word == "wait") return Verb::Wait;
    if (word == "quit") return Verb::Quit;
    return std::nullopt;
}

// --- production HTTP transport (REQ-RESOLVE-10, -11) -------------------------
// Deliberately REIMPLEMENTS prose.cpp's curlTransport (which is anon-namespace-
// private there); only the HttpTransport *type* is shared (micro-decision #2).
// Same URL / headers / 8 s timeout as prose. This function has no direct unit
// test — it is exercised only by the gated live smoke (Step 9).

// libcurl write callback: append the response bytes to a std::string.
size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One POST to the Anthropic Messages API. URL, headers, and timeout are fixed
// properties of THIS transport, never seam parameters. The API key is read from
// ANTHROPIC_API_KEY at call time and goes into the x-api-key header ONLY: never
// into the payload, logs, or fixtures. Any curl failure → transportError; NO
// retries.
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
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
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

}  // namespace

// --- ISA system prompt (REQ-RESOLVE-12) -------------------------------------

// Stable constant, versioned by git — this prompt IS the instruction-set
// contract: it names all seven verbs, describes each non-overlappingly, and
// states every lowering rule. Prompt QUALITY is verified live (Step 9 /
// spec AI-Validation item 3); the unit test here only pins its STRUCTURE by
// substring, so it can never become a live tune-retry loop. Reword with care:
// tests spot-check its phrases.
const char* const kResolveSystemPrompt =
    R"(You translate a player's raw input line for a text adventure into exactly one action from a fixed instruction set, by calling the emit_action tool. You never write prose, answer questions, or speak to the player - your only output is a tool call, or none.

Each user message is a JSON object of scope facts: "input" (the raw line to translate), "room" (the name of the room the player stands in), "exits" (the direction words leading out of it), "items" (the noun words of items visible in the room), and "inventory" (the noun words of items the player carries).

The instruction set has exactly seven verbs. Each is distinct; pick the single one the input means:
- look: the player surveys their surroundings. No argument.
- go: the player moves out of the room in a direction. Set "direction" to the movement or compass word (for example north, south, up, in).
- take: the player picks an item up off the floor into hand. Set "subject" to the item's noun word.
- drop: the player sets down an item they carry. Set "subject" to the item's noun word.
- inventory: the player reviews what they are carrying, changing nothing. No argument.
- wait: the player lets a beat of time pass, doing nothing else. No argument.
- quit: the player ends the session and leaves the game. No argument.

Rules, absolute:
- Translate the input to exactly one action and emit it with a single emit_action call. Never emit more than one action; if the line asks for several, make no call.
- A "subject" must be one of the noun words supplied in "items" or "inventory", copied verbatim. A "direction" for go must be a movement or compass word. Introduce no noun that is absent from the scope facts.
- If the input is a question, chatter, an unknown verb, or anything that is not one of these seven single actions, make no tool call at all. When in doubt, make no call.
- Resolve no pronouns or references: "it", "them", "the one on the table" are not supported. The noun word must appear in the input line itself.
- Judge recognition only, never applicability: whether an item is reachable or an exit is open is not your concern. Emit the action the words mean; the engine decides whether it applies.)";

ResolveContext buildResolveContext(Db& db, const std::string& line) {
    const int64_t actor = playerEntity(db);
    const int64_t room = roomOf(db, actor);

    // Exactly the REQ-RESOLVE-7 fields, nothing else. nlohmann/json handles
    // all escaping of the embedded raw line. No ids ever enter the payload
    // (REQ-RESOLVE-6): the room/actor are used only to SELECT names.
    json payload;
    payload["input"] = line;
    payload["room"] = nameOf(db, room);
    payload["exits"] = json(exitsOf(db, room));
    payload["items"] = json(portableNamesIn(db, room));
    payload["inventory"] = json(portableNamesIn(db, actor));

    ResolveContext ctx;
    ctx.payload = payload.dump();
    return ctx;
}

std::string buildResolveRequestBody(const std::string& contextPayload) {
    // Model: TEXTWORLD_MODEL (set AND non-empty) else claude-opus-4-8 — the
    // same rule buildRequestBody uses, so both AI features share one override.
    const char* env = std::getenv("TEXTWORLD_MODEL");
    const std::string model =
        (env != nullptr && env[0] != '\0') ? env : "claude-opus-4-8";

    // The single emit_action tool (REQ-RESOLVE-8): a schema-enforced verb enum
    // of exactly the seven ISA verbs, plus optional subject / direction. Only
    // `verb` is required — bare verbs carry neither argument.
    json emitAction;
    emitAction["name"] = "emit_action";
    emitAction["description"] =
        "Lower the player's input line to exactly one engine action. Call at "
        "most once; make no call when the line maps to no single in-scope "
        "action.";
    json properties;
    properties["verb"] = {
        {"type", "string"},
        {"enum", json::array({"look", "go", "take", "drop", "inventory",
                              "wait", "quit"})},
        {"description", "The single ISA verb the input means."}};
    properties["subject"] = {
        {"type", "string"},
        {"description",
         "For take/drop: the item's noun word, copied verbatim from the "
         "supplied items or inventory."}};
    properties["direction"] = {
        {"type", "string"},
        {"description", "For go: the movement or compass word."}};
    json inputSchema;
    inputSchema["type"] = "object";
    inputSchema["properties"] = std::move(properties);
    inputSchema["required"] = json::array({"verb"});
    emitAction["input_schema"] = std::move(inputSchema);

    json body;
    body["model"] = model;
    body["max_tokens"] = 512;  // one small tool call, per REQ-RESOLVE-9
    body["system"] = kResolveSystemPrompt;
    body["messages"] =
        json::array({{{"role", "user"}, {"content", contextPayload}}});
    body["tools"] = json::array({std::move(emitAction)});
    body["tool_choice"] = {{"type", "auto"}};  // no call on unknown/multi-intent
    // Deliberately absent everywhere in this body: thinking, stream, and any
    // cache-control key (REQ-RESOLVE-9; the test pins the exact top-level set).
    return body.dump();
}

std::optional<Action> validateAndLower(const HttpResponse& response, Db& db) {
    // Clause a, HTTP half: checkable before any parse. A transport error
    // carries status 0, so it fails here too.
    if (response.status != 200) {
        return failClause('a', "HTTP status is not 200");
    }

    // Parse the body ONCE, exception-free — this function must never throw
    // (aiResolve calls it inside try/catch, but tests call it directly).
    const json j = json::parse(response.body, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return failClause('a', "body is not a JSON object");
    }
    if (!j.contains("content") || !j["content"].is_array()) {
        return failClause('a', "response has no content array");
    }

    // Clause a, block half: count the emit_action tool_use blocks. The Anthropic
    // shape is content[] with objects {type:"tool_use", name:"emit_action",
    // input:{...}} (Step-5 fixture); navigate it, don't assume position.
    const json* emit = nullptr;
    int emitCount = 0;
    for (const json& block : j["content"]) {
        if (block.is_object() && block.value("type", "") == "tool_use" &&
            block.value("name", "") == "emit_action") {
            ++emitCount;
            emit = &block;
        }
    }
    if (emitCount == 0) {
        // The model made no tool call — unknown intent or multi-intent. This is
        // the DESIGNED no-action path (REQ-RESOLVE-3), not a rejection: return
        // nullopt cleanly, no diagnostic. The caller falls back to the parser.
        return std::nullopt;
    }
    if (emitCount >= 2) {
        return failClause('a', "more than one emit_action tool_use block");
    }

    // Exactly one emit_action block. Its input object carries verb / subject /
    // direction.
    if (!emit->contains("input") || !(*emit)["input"].is_object()) {
        return failClause('b', "emit_action block has no input object");
    }
    const json& input = (*emit)["input"];

    // Clause b: verb present, a string, and one of the seven ISA verbs.
    if (!input.contains("verb") || !input["verb"].is_string()) {
        return failClause('b', "verb missing or not a string");
    }
    const std::optional<Verb> verb =
        verbFromWord(input["verb"].get<std::string>());
    if (!verb) {
        return failClause('b', "verb is not one of the seven ISA verbs");
    }

    Action action{*verb};
    switch (*verb) {
        case Verb::Take:
        case Verb::Drop: {
            // Clause c: subject present, and recognized world-wide. The id is
            // assigned MECHANICALLY by lookupNoun, never read from the model.
            // Recognition only — whether the item is in scope is the engine's
            // tier-b job, not this gate's.
            if (!input.contains("subject") || !input["subject"].is_string()) {
                return failClause('c', "take/drop has no subject");
            }
            const int64_t entity =
                lookupNoun(db, input["subject"].get<std::string>());
            if (entity == 0) {
                return failClause('c', "subject is not a noun anywhere in world");
            }
            action.subject = entity;
            break;
        }
        case Verb::Go: {
            // Clause d: direction present and non-empty.
            if (!input.contains("direction") || !input["direction"].is_string() ||
                input["direction"].get<std::string>().empty()) {
                return failClause('d', "go has no direction");
            }
            action.direction = input["direction"].get<std::string>();
            break;
        }
        case Verb::Look:
        case Verb::Inventory:
        case Verb::Wait:
        case Verb::Quit:
            // Clause e: argument-free — any stray subject/direction is ignored.
            break;
    }
    return action;
}

std::optional<Action> aiResolve(Db& db, const std::string& line,
                                const HttpTransport& transport) {
    // REQ-RESOLVE-3: no AI-path failure may derail a turn. The whole pipeline
    // sits inside try/catch; ANY failure yields one stderr diagnostic (never a
    // spoofed Action) and nullopt — the caller falls back to the parser.
    try {
        // Exactly ONE transport call — no retry loop, ever (REQ-RESOLVE-10).
        const ResolveContext ctx = buildResolveContext(db, line);
        const HttpResponse resp = transport(buildResolveRequestBody(ctx.payload));
        // validateAndLower never throws and emits its own clause diagnostic on
        // rejection; a clean no-tool-call returns nullopt silently.
        return validateAndLower(resp, db);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "aiResolve: failed, falling back to parser: %s\n",
                     e.what());
        return std::nullopt;
    } catch (...) {
        std::fprintf(stderr,
                     "aiResolve: failed, falling back to parser: "
                     "unknown exception\n");
        return std::nullopt;
    }
}

std::optional<Action> aiResolve(Db& db, const std::string& line) {
    return aiResolve(db, line, curlTransport);
}
