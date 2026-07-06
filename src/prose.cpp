// aiRender(): events → LLM prose. READ-ONLY BY CONTRACT — SELECTs plus
// network egress only; writes nothing (see prose.hpp).
//
// The lookups below deliberately REIMPLEMENT render.cpp's (those live in an
// anonymous namespace there); the small duplication is the accepted cost of
// leaving render.cpp untouched. One deliberate difference: no "something"
// placeholder — an unresolvable name is OMITTED from the payload rather than
// replaced with a meaningless noun the model could narrate.
#include "prose.hpp"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <vector>

#include <curl/curl.h>

#include "json.hpp"

namespace {

using nlohmann::json;

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

// --- production HTTP transport (REQ-PROSE-9, REQ-PROSE-10) -----------------

// libcurl write callback: append the response bytes to a std::string.
size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One POST to the Anthropic Messages API. URL, headers, and timeout are
// fixed here — they are properties of THIS transport, never seam parameters.
// The API key is read from ANTHROPIC_API_KEY at call time and goes into the
// x-api-key header ONLY: never into the payload, logs, or fixtures. Any curl
// failure (timeout, connect failure, ...) → transportError; NO retries.
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
            "FROM events WHERE turn = ? ORDER BY id");
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
            if (!detailIsNull) e["detail"] = detail;
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
            "WHERE turn < ? ORDER BY id DESC LIMIT 6");
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

std::optional<std::string> aiRender(Db& db, int64_t turn,
                                    const HttpTransport& transport) {
    // Request-body assembly is a later step; for now the facts payload rides
    // as the body. Exactly ONE transport call — no retry loop, ever.
    const TurnFacts facts = buildFacts(db, turn);
    const HttpResponse resp = transport(facts.payload);
    (void)resp;  // response validation is the next step
    return std::nullopt;
}

std::optional<std::string> aiRender(Db& db, int64_t turn) {
    return aiRender(db, turn, curlTransport);
}
