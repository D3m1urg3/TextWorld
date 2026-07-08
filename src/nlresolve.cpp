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
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

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

}  // namespace

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
