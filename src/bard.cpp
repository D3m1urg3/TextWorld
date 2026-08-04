// The bard's read side, wire format, and validation gates. READ-ONLY here —
// SELECTs only, with every write routed through mutations.cpp's fact-store
// helpers, which contain the SQL; this unit only calls them (REQ-BARD-SEL-21,
// stated in full in bard.hpp). The mechanical guard on that contract is a grep
// over this file for the three write verbs, so they appear nowhere below —
// not even in a comment.
//
// Like architect.cpp and nlresolve.cpp, the one-line lookups below deliberately
// REIMPLEMENT prose.cpp's (which live in an anonymous namespace there); the
// small duplication is the accepted cost of leaving those units untouched.
#include "bard.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "aihttp.hpp"     // modelForRole — the role→model rule lives THERE, not here
#include "combat.hpp"     // eligibleArchetypes / distanceFromSeed — the shared gates
#include "json.hpp"
#include "mutations.hpp"  // the fact-store helpers: the SOLE sanctioned write path

namespace {

using nlohmann::json;

// The setting text loaded at init, or empty if absent.
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

// True iff `s` is empty or all-whitespace — the "non-empty after trim" clause.
bool blankAfterTrim(const std::string& s) {
    for (const unsigned char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
            c != '\v') {
            return false;
        }
    }
    return true;
}

// Trim leading/trailing ASCII whitespace. The gates compare trimmed values, so
// a handle the model padded still matches the row writeCatalogEntry stored.
std::string trim(const std::string& s) {
    auto isWs = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v';
    };
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && isWs(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && isWs(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

// One candidate row, read before the gates that need code rather than SQL are
// applied. `tier` and `factArchetype` are engine-facing only — they gate, and
// are then dropped on the way into CatalogChoice (REQ-BARD-SEL-8).
struct CatalogRow {
    std::string handle;
    std::string blurb;
    std::string motiveBlurb;
    std::string factArchetype;
    int64_t tier = 0;
};

// Every UNMATERIALIZED catalog row (gate a), optionally of one `kind` (gate c),
// ordered by catalog.id ascending (REQ-BARD-SEL-4) — an empty `kind` spans
// both, which is what the prospective-room menu wants (plan micro-decision 5).
//
// LEFT JOIN on motive_catalog deliberately: an entry whose motive row is
// somehow absent still offers, with an empty motive blurb, rather than silently
// vanishing from the menu. writeCatalogEntry makes that unreachable today, and
// a disappearance would be the harder failure to diagnose if it ever is not.
std::vector<CatalogRow> catalogRows(Db& db, const std::string& kind) {
    std::vector<CatalogRow> rows;
    const char* sql =
        kind.empty()
            ? "SELECT c.handle, c.blurb, COALESCE(m.blurb, ''), "
              "COALESCE(c.fact_archetype, ''), c.tier "
              "FROM catalog c LEFT JOIN motive_catalog m ON m.motive = c.motive "
              "WHERE c.entity IS NULL ORDER BY c.id"
            : "SELECT c.handle, c.blurb, COALESCE(m.blurb, ''), "
              "COALESCE(c.fact_archetype, ''), c.tier "
              "FROM catalog c LEFT JOIN motive_catalog m ON m.motive = c.motive "
              "WHERE c.entity IS NULL AND c.kind = ? ORDER BY c.id";
    Stmt s = db.prepare(sql);
    if (!kind.empty()) s.bind(1, kind);
    while (s.step()) {
        CatalogRow row;
        row.handle = s.colText(0);
        row.blurb = s.colText(1);
        row.motiveBlurb = s.colText(2);
        row.factArchetype = s.colText(3);
        row.tier = s.colInt(4);
        rows.push_back(std::move(row));
    }
    return rows;
}

// The archetypes whose subjects are LIVE around `room` (gate d): its own
// eligible menu, unioned with the menu of every room a realized exit leads to.
//
// Every neighbor, not "a" neighbor: a room with three exits has three adjacent
// menus, and picking one of them would be arbitrary. eligibleArchetypes is
// CALLED, never reimplemented (REQ-BARD-SEL-3) — its three gates are combat's
// to own, and a second copy here is exactly the drift the requirement forbids.
std::set<std::string> neighborhoodArchetypes(Db& db, int64_t room) {
    std::set<std::string> live;
    for (const std::string& a : eligibleArchetypes(db, room)) live.insert(a);
    std::vector<int64_t> neighbors;
    {
        Stmt s = db.prepare(
            "SELECT dest FROM exits WHERE room = ? AND dest IS NOT NULL");
        s.bind(1, room);
        while (s.step()) neighbors.push_back(s.colInt(0));
    }
    for (const int64_t neighbor : neighbors) {
        for (const std::string& a : eligibleArchetypes(db, neighbor)) {
            live.insert(a);
        }
    }
    return live;
}

// The motive vocabulary as key + blurb, ordered by key: an array of
// {key, blurb} objects. The blurbs are NOT optional (REQ-BARD-SEL-9): the tool
// schema constrains `motive` to eight bare keys, and without their meanings the
// model is choosing between opaque tokens — it cannot tell `obligation` from
// `homesickness`. Both context builders carry this, for the same reason.
json motiveVocabulary(Db& db) {
    json motives = json::array();
    Stmt s = db.prepare(
        "SELECT motive, COALESCE(blurb, '') FROM motive_catalog ORDER BY motive");
    while (s.step()) {
        motives.push_back({{"key", s.colText(0)}, {"blurb", s.colText(1)}});
    }
    return motives;
}

// The schema of ONE catalog entry, shared by write_catalog's `entries` array
// and append_catalog's `entry` object (REQ-BARD-SEL-13). One builder, so the
// two shapes cannot drift — that sharing IS the requirement's enforcement, and
// a test compares the two serialized sub-objects to prove it held.
//
// `kind` and `motive` are schema-enforced ENUMS, the same treatment the
// architect's `enemy` gets: the motive vocabulary arrives as an argument read
// from motive_catalog (REQ-BARD-SEL-12), never hardcoded here, so a ninth
// motive row changes the enum without a code change. Belt and braces — the
// schema stops a motive outside the vocabulary, and writeCatalogEntry throws if
// one arrives anyway.
json catalogEntrySchema(const std::vector<std::string>& motives) {
    json fact;
    fact["type"] = "object";
    fact["properties"] = {
        {"archetype",
         {{"type", "string"},
          {"description",
           "the kind of creature this knowledge is about, exactly as the rules "
           "name it"}}},
        {"element",
         {{"type", "string"},
          {"description", "the element the knowledge concerns"}}}};
    // BOTH fields or neither: a half-written fact is not a weaker fact, it is a
    // malformed one, and the gate drops the entry that carries it.
    fact["required"] = json::array({"archetype", "element"});
    fact["description"] =
        "Optional. Only for an entry whose point is that the player learns "
        "something TRUE about how a creature works. A false one costs the "
        "whole entry.";

    json properties;
    properties["kind"] = {
        {"type", "string"},
        {"enum", json::array({"character", "beat"})},
        {"description",
         "\"character\" for someone who acts, \"beat\" for a happening or a "
         "thing that can be come to know"}};
    properties["handle"] = {
        {"type", "string"},
        {"description",
         "a short lowercase token with underscores, unique across your "
         "entries; never shown to the player"}};
    properties["name"] = {
        {"type", "string"},
        {"description",
         "the in-world noun the player sees and types, like \"cloistered "
         "scribe\""}};
    properties["blurb"] = {
        {"type", "string"},
        {"description",
         "one sentence describing the entry as a reader would meet it - the "
         "only prose anyone sees when choosing whether to use it"}};
    properties["motive"] = {
        {"type", "string"},
        {"enum", motives},
        {"description", "one key from the motive vocabulary you were given"}};
    properties["tier"] = {
        {"type", "integer"},
        {"minimum", 0},
        {"description",
         "how DEEP from the player's starting point this belongs: 0 for the "
         "first room, larger for further in. Distance, not danger."}};
    properties["fact"] = std::move(fact);

    json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    // `fact` is the only optional field — every other one is load-bearing at
    // admission, and an entry missing any of them is dropped at the gate.
    schema["required"] =
        json::array({"kind", "handle", "name", "blurb", "motive", "tier"});
    return schema;
}

// One diagnostic line per rejected RESPONSE, naming the first failed clause.
// Mirrors the architect's and the resolver's failClause; std::nullopt_t
// converts to whichever optional the caller returns.
std::nullopt_t failClause(const char* who, const char* why) {
    std::fprintf(stderr, "%s: rejected: %s\n", who, why);
    return std::nullopt;
}

// A string field, trimmed, or "" when absent or not a string.
std::string stringField(const json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_string()) return "";
    return trim(obj[key].get<std::string>());
}

// Read one proposed entry (REQ-BARD-SEL-17). Returns "" and fills `out` when
// the entry is well formed, else the reason it is DROPPED — the caller keeps
// every sibling either way. This is shape validation only: whether the world
// agrees with a `fact` is admission's question, not the gate's.
std::string readEntry(const json& e, const std::vector<std::string>& motives,
                      CatalogEntryProposal& out) {
    if (!e.is_object()) return "entry is not an object";

    out.kind = stringField(e, "kind");
    if (out.kind != "character" && out.kind != "beat") {
        return "kind is not \"character\" or \"beat\"";
    }
    out.handle = stringField(e, "handle");
    if (out.handle.empty()) return "handle is empty after trim";
    out.name = stringField(e, "name");
    if (out.name.empty()) return "name is empty after trim";
    out.blurb = stringField(e, "blurb");
    if (out.blurb.empty()) return "blurb is empty after trim";

    out.motive = stringField(e, "motive");
    if (out.motive.empty()) return "motive is empty";
    // An empty vocabulary means "unchecked here" — writeCatalogEntry still
    // refuses a motive with no motive_catalog row, so nothing false gets in.
    if (!motives.empty() &&
        std::find(motives.begin(), motives.end(), out.motive) == motives.end()) {
        return "motive is outside the vocabulary";
    }

    if (!e.contains("tier") || !e["tier"].is_number_integer()) {
        return "tier is missing or not an integer";
    }
    out.tier = e["tier"].get<int64_t>();
    if (out.tier < 0) return "tier is negative";

    // `fact` is optional, but both-or-neither when present: a half-written fact
    // is malformed, not merely weaker.
    out.factArchetype.clear();
    out.factElement.clear();
    if (e.contains("fact") && !e["fact"].is_null()) {
        if (!e["fact"].is_object()) return "fact is not an object";
        out.factArchetype = stringField(e["fact"], "archetype");
        out.factElement = stringField(e["fact"], "element");
        if (out.factArchetype.empty() != out.factElement.empty()) {
            return "fact carries one field without the other";
        }
    }
    return "";
}

// Parse a response body down to its content[] array, exception-free. Returns
// nullptr and leaves `why` set when any transport/parse clause fails — the only
// clauses that reject a response in full.
const json* contentArray(const HttpResponse& response, json& parsed,
                         const char** why) {
    if (response.status != 200) {
        *why = "HTTP status is not 200";
        return nullptr;
    }
    // Parsed ONCE, exception-free: these functions must never throw, and the
    // tests call them directly, with no exception handler around them.
    parsed = json::parse(response.body, /*cb=*/nullptr,
                         /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        *why = "body is not a JSON object";
        return nullptr;
    }
    if (!parsed.contains("content") || !parsed["content"].is_array()) {
        *why = "response has no content array";
        return nullptr;
    }
    return &parsed["content"];
}

// True iff `block` is a tool_use block for `name`. The Anthropic shape is
// content[] with {type:"tool_use", name:..., input:{...}}; navigate it, never
// assume a position.
bool isToolUse(const json& block, const char* name) {
    return block.is_object() && block.value("type", "") == "tool_use" &&
           block.value("name", "") == name;
}

// True iff the one-parameter query returns a row. The pre-flight's whole
// vocabulary of world questions.
bool rowExists(Db& db, const char* sql, const std::string& value) {
    Stmt s = db.prepare(sql);
    s.bind(1, value);
    return s.step();
}

bool pairExists(Db& db, const char* sql, const std::string& a,
                const std::string& b) {
    Stmt s = db.prepare(sql);
    s.bind(1, a);
    s.bind(2, b);
    return s.step();
}

// Gates (b), (c), (d) over the already-(a)-filtered rows, shared by the
// existing-room and prospective-room menus — they differ only in which distance
// and which neighborhood they hand in. `neighborhood` is invoked at most ONCE,
// and only if some surviving entry actually carries a fact, so the common
// (factless) catalog pays nothing for gate (d).
template <typename NeighborhoodFn>
std::vector<CatalogChoice> offerable(Db& db, const std::string& kind,
                                     int64_t distance,
                                     NeighborhoodFn neighborhood) {
    std::vector<CatalogChoice> choices;
    std::set<std::string> live;
    bool liveKnown = false;
    for (const CatalogRow& row : catalogRows(db, kind)) {
        if (row.tier > distance) continue;  // gate (b)
        if (!row.factArchetype.empty()) {   // gate (d)
            if (!liveKnown) {
                live = neighborhood();
                liveKnown = true;
            }
            if (live.find(row.factArchetype) == live.end()) continue;
        }
        choices.push_back({row.handle, row.blurb, row.motiveBlurb});
    }
    return choices;
}

// One event as a human-readable line: "turn 12: defeated rime-touched acolyte".
//
// The narrator's tag shield (prose.cpp), applied to a second model-facing
// surface — this is what keeps REQ-BARD-SEL-8's "no ids on the wire" true of
// the event lines. The `subject` is name-resolved and a subject of 0 is omitted
// rather than resolved; the `object` column is dropped ENTIRELY, because it
// carries room and container ids; and `detail` is withheld on 'burned',
// 'froze', and 'materialized', the three verbs whose detail is an
// engine-internal tag (an "<archetype>|<element>" pair or a catalog handle).
// A fourth tagged verb means updating this shield too.
std::string eventLine(Db& db, int64_t turn, const std::string& verb,
                      int64_t subject, const std::string& detail,
                      bool detailIsNull) {
    std::string line = "turn " + std::to_string(turn) + ": " + verb;
    if (subject != 0) {
        const std::string name = nameOf(db, subject);
        if (!name.empty()) line += " " + name;
    }
    const bool engineInternalTag =
        verb == "burned" || verb == "froze" || verb == "materialized";
    if (!detailIsNull && !engineInternalTag && !detail.empty()) {
        line += " (" + detail + ")";
    }
    return line;
}

// The events a wake may see: those since meta.bard_last_wake_turn, capped at
// the most recent kBardWakeEventLimit and rendered OLDEST-FIRST so the
// narrative order survives the cap.
json wakeEventLines(Db& db) {
    int64_t since = 0;
    {
        Stmt s = db.prepare(
            "SELECT value FROM meta WHERE key = 'bard_last_wake_turn'");
        if (s.step()) since = s.colInt(0);
    }
    // Newest-first with a LIMIT selects the RECENT tail; the vector is then
    // reversed, so the model reads the events in the order they happened.
    std::vector<std::string> lines;
    {
        Stmt s = db.prepare(
            "SELECT turn, verb, subject, detail, detail IS NULL FROM events "
            "WHERE turn > ? ORDER BY id DESC LIMIT ?");
        s.bind(1, since);
        s.bind(2, kBardWakeEventLimit);
        while (s.step()) {
            lines.push_back(eventLine(db, s.colInt(0), s.colText(1), s.colInt(2),
                                      s.colText(3), s.colInt(4) != 0));
        }
    }
    json events = json::array();
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) events.push_back(*it);
    return events;
}

// The FULL catalog as the wake sees it: handle + blurb + motive blurb + whether
// it has materialized. Not the eligible menu — mark_seeded and the journal
// reason about entries that are not currently offerable anywhere. No id, no
// tier, no seeded flag, no raw fact column (REQ-BARD-SEL-8, -10).
json wakeCatalog(Db& db) {
    json entries = json::array();
    Stmt s = db.prepare(
        "SELECT c.handle, c.blurb, COALESCE(m.blurb, ''), c.entity IS NOT NULL "
        "FROM catalog c LEFT JOIN motive_catalog m ON m.motive = c.motive "
        "ORDER BY c.id");
    while (s.step()) {
        entries.push_back({{"handle", s.colText(0)},
                           {"blurb", s.colText(1)},
                           {"motive", s.colText(2)},
                           {"materialized", s.colInt(3) != 0}});
    }
    return entries;
}

}  // namespace

std::vector<CatalogChoice> eligibleCatalog(Db& db, int64_t room,
                                           const std::string& kind) {
    // Gate (b) is applied HERE rather than in SQL, because the metric has a
    // sentinel: distanceFromSeed returns INT64_MAX for a room BFS cannot reach,
    // and `tier <= INT64_MAX` would admit EVERY entry at EVERY tier — the story
    // menu at its most permissive exactly where combat's is empty. Off the map
    // offers nothing, so story and combat agree on what "off the map" means.
    const int64_t distance = distanceFromSeed(db, room);
    if (distance == INT64_MAX) return {};
    return offerable(db, kind, distance,
                     [&db, room] { return neighborhoodArchetypes(db, room); });
}

// The overture prompt (REQ-BARD-SEL-22): git-versioned, like kArchitectPrompt
// and the narrator/resolver prompts. It names the context keys the model is
// reading and the one tool it may call, states what `tier` means (depth, never
// danger), states what a `fact` costs when it is false, and carries the
// situations-not-urgency clause (REQ-BARD-SEL-23). Prompt STRUCTURE is pinned
// by substring in the tests; prompt QUALITY is judged against real output in
// Brick 3. Reword with care.
const char* const kBardOverturePrompt =
    R"(You are the bard of a text adventure world: the author of its cast and its story beats. Before the world is built, you write down who is in it and what is going on in it, by calling the write_catalog tool exactly once. You never write anything else - your only output is that single call.

The user message is a JSON object of context: "setting" (the world's tone, premise, and scale - the shared frame everything you write must fit), and "motives" (the closed vocabulary of motives, each a key and what that key means). You may use no motive outside that list.

Your task: author the people, creatures, and happenings this world needs, as a list of catalog entries, plus a journal note to yourself. Nothing exists yet - no rooms, no map, no history. You are writing the cast list of a play whose stage has not been built, and the engine will place each entry into a room later, when the world has grown far enough to hold it.

Each entry carries:
- kind: "character" for someone who acts, "beat" for a happening, an object with a story on it, or a thing the player can come to know.
- handle: a short lowercase token with underscores, unique across all your entries. It is how the engine refers to this entry. It is never shown to the player.
- name: the in-world noun the player will see and type, like "cloistered scribe" or "spilled ink". Plain words, no title case, no invented proper nouns unless the setting invites them.
- blurb: one sentence describing the entry as a reader would meet it. This is the only prose anyone sees when choosing whether to use this entry, so make it specific and make it selectable.
- motive: one key from the motive vocabulary - what this entry wants, or what the situation is pulling toward.
- tier: how DEEP from the player's starting point this entry belongs - 0 for something they could meet in the first room, larger numbers for things that belong further in. Tier is distance, not danger and not importance. A tier that is too high simply means the entry waits longer to appear.
- fact: optional, and only for an entry whose point is that the player LEARNS something true about how the world's creatures work. If you write one, both of its fields are required, and what it asserts must be true of the rules as the engine has them. A fact that is not true costs you the WHOLE entry - it is dropped, not corrected - so write one only when you are certain, and leave it off otherwise.

Also write a journal: a private note to yourself about what you are setting up and what you are watching for. Nobody else reads it, and you will be shown it again later.

Write situations, not urgency. No deadlines, no countdowns, no ticking threats, nothing that says the player is running out of time or that standing still and talking is expensive. Escalation here is spatial: things get harder the further the player goes from where they started, and never because a clock ran out. A situation that only matters if the player hurries is the one kind of situation you must not write.

Rules, absolute:
- Call write_catalog exactly once. Write no prose outside the tool call.
- Invent no numbers or identifiers of the engine's: you write handles, names, blurbs, motives, and tiers, and the engine owns everything else.
- Every motive must come from the vocabulary you were given.
- Contradict nothing in the setting.
- Write nothing that assumes a particular room, map, or geography - you do not know what the world looks like, and an entry that only makes sense in one place cannot be placed.)";

// The wake prompt (REQ-BARD-SEL-22). Same discipline as the overture's; it
// differs in what it is reading (what has happened since it last looked) and in
// having four small tools rather than one large one — including the right to
// call none of them.
const char* const kBardWakePrompt =
    R"(You are the bard of a text adventure world: the author of its cast and its story beats. The world is running now, and you are being woken to read what has happened since you last looked and to decide what, if anything, it changes.

The user message is a JSON object of context: "setting" (the world's tone, premise, and scale), "motives" (the closed vocabulary of motives, each a key and what it means), "journal" (the private note you left yourself last time), "events" (what has happened since your last waking, oldest first), and "catalog" (everything you have written so far, each with its handle, its blurb, its motive, and whether it has appeared in the world yet).

You have four tools, and you may call any, all, or none of them:
- write_focus: one short line for the room-writer, which it reads while inventing the next room. This is the only thing you write that another author sees, so make it usable - a mood, a thread to pick up, a thing to keep showing. Vague is useless. It replaces whatever focus you set before.
- write_journal: your private note to yourself, replacing the last one. Nobody else reads it.
- append_catalog: add one new entry to the catalog, in the same shape as the ones already there. Use it when what has happened calls for someone or something you had not written yet.
- mark_seeded: name the handle of an entry that has now been hinted at in the prose, so the engine knows it is harder to walk back.

Doing nothing is a legitimate turn. If what has happened does not change your plans, call no tool at all. Do not manufacture a development to justify the waking.

Write situations, not urgency. No deadlines, no countdowns, no ticking threats, nothing that says the player is running out of time or that standing still and talking is expensive. Escalation here is spatial: things get harder the further the player goes from where they started, and never because a clock ran out. A situation that only matters if the player hurries is the one kind of situation you must not write.

Rules, absolute:
- Write no prose outside the tool calls.
- Invent no numbers or identifiers of the engine's, and refer to an entry only by the handle you gave it.
- Every motive must come from the vocabulary you were given.
- An entry you append follows the same rules as the ones you wrote at the start: tier is DEPTH from the player's starting point, not danger, and a fact must be true of the rules or the whole entry is dropped.
- Contradict nothing already in the catalog or the setting. If you have changed your mind about an entry, write the new one and say so in the journal; nothing you have already written can be taken back.)";

std::string buildOvertureContext(Db& db) {
    // EXACTLY two keys and no ids ever (REQ-BARD-SEL-8, -9). Nothing else about
    // world state goes in: at overture time none exists to describe.
    json payload;
    payload["setting"] = settingText(db);
    payload["motives"] = motiveVocabulary(db);
    return payload.dump();
}

std::string buildWakeContext(Db& db) {
    // EXACTLY five keys (REQ-BARD-SEL-10). The motive vocabulary is carried for
    // the same reason the overture carries it: append_catalog requires choosing
    // a motive, and the keys alone are opaque.
    json payload;
    payload["setting"] = settingText(db);
    payload["motives"] = motiveVocabulary(db);
    {
        Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'bard_journal'");
        payload["journal"] = s.step() ? s.colText(0) : "";
    }
    payload["events"] = wakeEventLines(db);
    payload["catalog"] = wakeCatalog(db);
    return payload.dump();
}

std::vector<std::string> motiveKeys(Db& db) {
    std::vector<std::string> keys;
    Stmt s = db.prepare("SELECT motive FROM motive_catalog ORDER BY motive");
    while (s.step()) keys.push_back(s.colText(0));
    return keys;
}

std::string buildOvertureRequestBody(const std::string& contextPayload,
                                     const std::vector<std::string>& motives) {
    // Model: the GENERATE role (story authoring is quality work, so it keeps
    // the expensive default). The default and the TEXTWORLD_MODEL override rule
    // both live in aihttp.hpp — never restate them here.
    json writeCatalog;
    writeCatalog["name"] = "write_catalog";
    writeCatalog["description"] =
        "Write the world's cast and story beats: a list of catalog entries, "
        "plus a private journal note to yourself.";

    json entries;
    entries["type"] = "array";
    entries["items"] = catalogEntrySchema(motives);
    entries["description"] =
        "every entry you are authoring for this world, in any order";

    json properties;
    properties["entries"] = std::move(entries);
    properties["journal"] = {
        {"type", "string"},
        {"description",
         "your private note to yourself about what you are setting up and what "
         "you are watching for; nobody else reads it"}};

    json inputSchema;
    inputSchema["type"] = "object";
    inputSchema["properties"] = std::move(properties);
    inputSchema["required"] = json::array({"entries", "journal"});
    writeCatalog["input_schema"] = std::move(inputSchema);

    json body;
    body["model"] = modelForRole(AiRole::Generate);
    // The one deliberate difference from the architect's 1024: an overture
    // writes a whole cast in a single call, not one room's prose.
    body["max_tokens"] = 4096;
    body["system"] = kBardOverturePrompt;
    body["messages"] =
        json::array({{{"role", "user"}, {"content", contextPayload}}});
    body["tools"] = json::array({std::move(writeCatalog)});
    // AUTO, not required (REQ-BARD-SEL-14): a response with no tool call is a
    // valid outcome here — an empty catalog is a supported state everywhere —
    // unlike the architect, whose room is the whole point of its call.
    body["tool_choice"] = {{"type", "auto"}};
    // Deliberately absent: thinking, stream, and any cache-control key. The
    // test pins the exact top-level set.
    return body.dump();
}

std::string buildWakeRequestBody(const std::string& contextPayload,
                                 const std::vector<std::string>& motives) {
    // FOUR small tools rather than one large one (REQ-BARD-SEL-13), so a
    // partial response is still useful: a wake that writes a focus but fails to
    // journal has still influenced the world.
    json writeFocus;
    writeFocus["name"] = "write_focus";
    writeFocus["description"] =
        "Set the one short line the room-writer reads while inventing the next "
        "room. Replaces the previous focus.";
    writeFocus["input_schema"] = {
        {"type", "object"},
        {"properties",
         {{"text",
           {{"type", "string"},
            {"description",
             "one short line another author can actually use: a mood, a thread "
             "to pick up, a thing to keep showing"}}}}},
        {"required", json::array({"text"})}};

    json writeJournal;
    writeJournal["name"] = "write_journal";
    writeJournal["description"] =
        "Replace your private journal note. Nobody else reads it.";
    writeJournal["input_schema"] = {
        {"type", "object"},
        {"properties",
         {{"text",
           {{"type", "string"},
            {"description", "your private working note, in full"}}}}},
        {"required", json::array({"text"})}};

    // `entry` is ONE element of write_catalog's `entries` array — the SAME
    // schema builder, so the two cannot drift — and carries no entries array
    // and no journal of its own.
    json appendCatalog;
    appendCatalog["name"] = "append_catalog";
    appendCatalog["description"] =
        "Add one new entry to the catalog, in the same shape as the entries "
        "already there.";
    appendCatalog["input_schema"] = {
        {"type", "object"},
        {"properties", {{"entry", catalogEntrySchema(motives)}}},
        {"required", json::array({"entry"})}};

    json markSeeded;
    markSeeded["name"] = "mark_seeded";
    markSeeded["description"] =
        "Record that an entry has now been hinted at in the prose, and is "
        "therefore costlier to walk back.";
    markSeeded["input_schema"] = {
        {"type", "object"},
        {"properties",
         {{"handle",
           {{"type", "string"},
            {"description", "the handle of the entry that has been hinted"}}}}},
        {"required", json::array({"handle"})}};

    json body;
    body["model"] = modelForRole(AiRole::Generate);
    // Back to the architect's budget: a wake writes a line and a paragraph,
    // not a cast.
    body["max_tokens"] = 1024;
    body["system"] = kBardWakePrompt;
    body["messages"] =
        json::array({{{"role", "user"}, {"content", contextPayload}}});
    body["tools"] = json::array({std::move(writeFocus), std::move(writeJournal),
                                 std::move(appendCatalog),
                                 std::move(markSeeded)});
    // AUTO on both call shapes (REQ-BARD-SEL-14): calling no tool is the bard
    // declining to act, which is normal.
    body["tool_choice"] = {{"type", "auto"}};
    return body.dump();
}

std::optional<OvertureProposal> validateOvertureResponse(
    const HttpResponse& response, const std::vector<std::string>& motives) {
    json parsed;
    const char* why = "";
    const json* content = contentArray(response, parsed, &why);
    if (content == nullptr) return failClause("validateOvertureResponse", why);

    const json* call = nullptr;
    int calls = 0;
    for (const json& block : *content) {
        if (isToolUse(block, "write_catalog")) {
            ++calls;
            if (call == nullptr) call = &block;
        }
    }
    if (calls > 1) {
        // Not a rejection: REQ-BARD-SEL-18 lists the only three clauses that
        // reject in full, and this is not one of them. The first call is the
        // overture; the rest are noted and ignored.
        std::fprintf(stderr,
                     "validateOvertureResponse: %d write_catalog blocks, using "
                     "the first\n",
                     calls);
    }
    if (call == nullptr) {
        // The tool was OPTIONAL, but an overture that calls nothing has
        // authored nothing — there is no partial result to keep, so this is
        // the one non-transport clause that rejects the whole response.
        return failClause("validateOvertureResponse",
                          "no write_catalog tool_use block");
    }
    if (!call->contains("input") || !(*call)["input"].is_object()) {
        return failClause("validateOvertureResponse",
                          "write_catalog block has no input object");
    }
    const json& input = (*call)["input"];

    OvertureProposal proposal;
    if (input.contains("journal") && input["journal"].is_string()) {
        proposal.journal = input["journal"].get<std::string>();
    }
    if (input.contains("entries") && input["entries"].is_array()) {
        for (const json& e : input["entries"]) {
            CatalogEntryProposal entry;
            const std::string refusal = readEntry(e, motives, entry);
            if (!refusal.empty()) {
                // One diagnostic, and the siblings are kept: nine entries beat
                // no story at all.
                std::fprintf(stderr,
                             "validateOvertureResponse: entry dropped: %s\n",
                             refusal.c_str());
                continue;
            }
            proposal.entries.push_back(std::move(entry));
        }
    }
    return proposal;
}

std::optional<WakeProposal> validateWakeResponse(
    const HttpResponse& response, const std::vector<std::string>& motives) {
    json parsed;
    const char* why = "";
    const json* content = contentArray(response, parsed, &why);
    if (content == nullptr) return failClause("validateWakeResponse", why);

    // No tool-call clause here, and deliberately so: a response calling nothing
    // returns an EMPTY proposal, not nullopt, and says nothing to stderr. The
    // bard declining to act is a successful wake (REQ-BARD-SEL-14).
    WakeProposal proposal;
    for (const json& block : *content) {
        if (!block.is_object()) continue;
        const bool hasInput =
            block.contains("input") && block["input"].is_object();
        if (isToolUse(block, "write_focus") && hasInput) {
            proposal.hasFocus = true;
            if (block["input"].contains("text") &&
                block["input"]["text"].is_string()) {
                proposal.focus = block["input"]["text"].get<std::string>();
            }
        } else if (isToolUse(block, "write_journal") && hasInput) {
            proposal.hasJournal = true;
            if (block["input"].contains("text") &&
                block["input"]["text"].is_string()) {
                proposal.journal = block["input"]["text"].get<std::string>();
            }
        } else if (isToolUse(block, "append_catalog") && hasInput) {
            CatalogEntryProposal entry;
            const std::string refusal =
                readEntry(block["input"].value("entry", json::object()), motives,
                          entry);
            if (!refusal.empty()) {
                std::fprintf(stderr,
                             "validateWakeResponse: appended entry dropped: %s\n",
                             refusal.c_str());
            } else {
                proposal.appended.push_back(std::move(entry));
            }
        } else if (isToolUse(block, "mark_seeded") && hasInput) {
            const std::string handle = stringField(block["input"], "handle");
            if (handle.empty()) {
                // A blank handle is detectable here; an UNKNOWN one is not,
                // and is admission's to ignore (REQ-BARD-SEL-20).
                std::fprintf(stderr,
                             "validateWakeResponse: mark_seeded dropped: "
                             "handle is empty after trim\n");
            } else {
                proposal.seededHandles.push_back(handle);
            }
        }
    }
    return proposal;
}

std::string catalogEntryRefusal(Db& db, const CatalogEntryProposal& entry,
                                const std::set<std::string>& admittedThisCall) {
    // The order below mirrors writeCatalogEntry's, so a reader comparing the
    // two can walk them side by side. What matters for the equivalence test is
    // the SET of predicates, not the order.
    if (entry.kind != "character" && entry.kind != "beat") {
        return "kind must be 'character' or 'beat'";
    }
    const std::string handle = trim(entry.handle);
    if (blankAfterTrim(entry.handle)) return "handle is empty after trim";
    if (blankAfterTrim(entry.name)) return "name is empty after trim";
    if (blankAfterTrim(entry.blurb)) return "blurb is empty after trim";
    if (entry.tier < 0) return "tier is negative";
    if (!rowExists(db, "SELECT 1 FROM motive_catalog WHERE motive = ?",
                   entry.motive)) {
        return "unknown motive '" + entry.motive + "'";
    }

    // The truth gate, clause for clause (REQ-BARD-STORE-10).
    const bool hasArchetype = !entry.factArchetype.empty();
    const bool hasElement = !entry.factElement.empty();
    if (hasArchetype != hasElement) {
        return "fact must carry both archetype and element, or neither";
    }
    if (hasArchetype) {
        if (!rowExists(db, "SELECT 1 FROM bestiary WHERE archetype = ?",
                       entry.factArchetype)) {
            return "no bestiary row for fact archetype '" + entry.factArchetype + "'";
        }
        if (!rowExists(db, "SELECT 1 FROM spell_catalog WHERE element = ?",
                       entry.factElement)) {
            return "'" + entry.factElement + "' is not an element";
        }
        if (!pairExists(db,
                        "SELECT 1 FROM resistance WHERE archetype = ? AND element = ?",
                        entry.factArchetype, entry.factElement)) {
            return "no resistance row for ('" + entry.factArchetype + "', '" +
                   entry.factElement + "') — a neutral matchup teaches nothing";
        }
    }

    // Uniqueness, which the helper enforces only through the UNIQUE constraint
    // — and which a BULK overture can plausibly violate against itself, not
    // just against the table.
    if (admittedThisCall.count(handle) != 0) {
        return "handle '" + handle + "' was already admitted in this call";
    }
    if (rowExists(db, "SELECT 1 FROM catalog WHERE handle = ?", handle)) {
        return "handle '" + handle + "' is already in the catalog";
    }
    return "";
}

int admitOvertureProposal(Db& db, const OvertureProposal& proposal) {
    // No exception handler anywhere in this function, deliberately: a throw here
    // is a genuine engine fault, and it must reach the caller's transaction to
    // be rolled back rather than being downgraded into a dropped entry.
    int admitted = 0;
    std::set<std::string> handles;
    for (const CatalogEntryProposal& entry : proposal.entries) {
        const std::string refusal = catalogEntryRefusal(db, entry, handles);
        if (!refusal.empty()) {
            std::fprintf(stderr,
                         "admitOvertureProposal: entry '%s' dropped: %s\n",
                         entry.handle.c_str(), refusal.c_str());
            continue;
        }
        writeCatalogEntry(db, entry.kind, entry.handle, entry.name, entry.blurb,
                          entry.motive, entry.tier, entry.factArchetype,
                          entry.factElement);
        handles.insert(trim(entry.handle));
        ++admitted;
    }
    // The journal lands whether or not any entry did: a rejected cast still
    // leaves the bard its working memory of what it was trying to do.
    writeBardJournal(db, proposal.journal);
    return admitted;
}

int applyWakeProposal(Db& db, const WakeProposal& proposal) {
    int applied = 0;
    if (proposal.hasFocus) {
        // The FLAG, not the string: an empty focus the bard chose to write is a
        // different fact from a focus it never wrote.
        writeBardFocus(db, proposal.focus);
        ++applied;
    }
    if (proposal.hasJournal) {
        writeBardJournal(db, proposal.journal);
        ++applied;
    }

    std::set<std::string> handles;
    for (const CatalogEntryProposal& entry : proposal.appended) {
        const std::string refusal = catalogEntryRefusal(db, entry, handles);
        if (!refusal.empty()) {
            std::fprintf(stderr,
                         "applyWakeProposal: appended entry '%s' dropped: %s\n",
                         entry.handle.c_str(), refusal.c_str());
            continue;
        }
        writeCatalogEntry(db, entry.kind, entry.handle, entry.name, entry.blurb,
                          entry.motive, entry.tier, entry.factArchetype,
                          entry.factElement);
        handles.insert(trim(entry.handle));
        ++applied;
    }

    for (const std::string& handle : proposal.seededHandles) {
        // Room-free and gate-free (REQ-BARD-SEL-24): seeding records that an
        // entry was HINTED, which can be true of one that is offerable nowhere.
        const int64_t id = catalogIdForHandle(db, handle);
        if (id == 0) {
            std::fprintf(stderr,
                         "applyWakeProposal: mark_seeded ignored: unknown "
                         "handle '%s'\n",
                         handle.c_str());
            continue;
        }
        markCatalogSeeded(db, id);
        ++applied;
    }
    return applied;
}

int64_t catalogIdForHandle(Db& db, const std::string& handle) {
    const std::string key = trim(handle);
    if (key.empty()) return 0;
    Stmt s = db.prepare("SELECT id FROM catalog WHERE handle = ?");
    s.bind(1, key);
    if (!s.step()) return 0;
    return s.colInt(0);
}

int64_t catalogForHandle(Db& db, int64_t room, const std::string& handle) {
    const std::string key = trim(handle);
    if (key.empty()) return 0;
    // Re-run the LIVE menu rather than trusting any snapshot the caller holds
    // (REQ-BARD-SEL-6): a bard proposal may have been built many turns before it
    // commits, and an entry that has since materialized — or a room that is no
    // longer deep enough — must resolve to nothing.
    for (const char* kind : {"character", "beat"}) {
        for (const CatalogChoice& choice : eligibleCatalog(db, room, kind)) {
            if (choice.handle == key) return catalogIdForHandle(db, key);
        }
    }
    return 0;
}

std::vector<CatalogChoice> eligibleCatalogForNewRoom(Db& db, int64_t originRoom) {
    // The prospective room is one hop past the origin, so its front distance is
    // the origin's plus one — combat's own overflow guard, copied, so the
    // increment cannot wrap the sentinel into a very small number.
    const int64_t originDist = distanceFromSeed(db, originRoom);
    if (originDist == INT64_MAX) return {};
    const int64_t newDist = originDist + 1;

    // Gate (d)'s neighborhood for a room that does not exist yet: its own
    // prospective menu, plus the origin's — the origin is its only neighbour by
    // construction (combat says exactly this about the prospective room's one
    // initial link), so no exits read happens on this path.
    return offerable(db, /*kind=*/"", newDist, [&db, originRoom] {
        std::set<std::string> live;
        for (const std::string& a : eligibleArchetypesForNewRoom(db, originRoom)) {
            live.insert(a);
        }
        for (const std::string& a : eligibleArchetypes(db, originRoom)) {
            live.insert(a);
        }
        return live;
    });
}
