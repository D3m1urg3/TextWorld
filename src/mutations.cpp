#include "mutations.hpp"

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "architect.hpp"  // RoomProposal (full definition) + inverseDirection

namespace {

// Current turn number, read inside the caller's ambient transaction.
int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) throw std::runtime_error("meta.turn row missing");
    return s.colInt(0);
}

}  // namespace

void appendEvent(Db& db, int64_t actor, const char* verb, int64_t subj,
                 int64_t obj, const char* detail) {
    const int64_t turn = currentTurn(db);
    Stmt ins = db.prepare(
        "INSERT INTO events(turn, actor, verb, subject, object, detail) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    ins.bind(1, turn);
    ins.bind(2, actor);
    ins.bind(3, std::string(verb));
    ins.bind(4, subj);
    ins.bind(5, obj);
    if (detail) ins.bind(6, std::string(detail));  // unbound param = SQL NULL
    ins.step();
}

void moveEntity(Db& db, int64_t what, int64_t toContainer, int64_t actor,
                const char* verb) {
    Stmt upd = db.prepare("UPDATE location SET container = ? WHERE entity = ?");
    upd.bind(1, toContainer);
    upd.bind(2, what);
    upd.step();
    if (db.changes() == 0) {
        // No location row for `what`: refusing here (before appendEvent) keeps
        // the event log authoritative — no event for a mutation that never
        // happened. Engine error; caller rolls back the ambient transaction.
        throw std::runtime_error("moveEntity: entity " + std::to_string(what) +
                                 " has no location row");
    }
    appendEvent(db, actor, verb, what, toContainer, nullptr);
}

void damageEntity(Db& db, int64_t target, int64_t amount, int64_t actor,
                  const char* verb) {
    // Clamp in code, not DDL: current := clamp(current - amount, 0, max). With
    // amount >= 0 the upper clamp is a no-op, but MIN(max, …) keeps the helper
    // correct for any future negative-amount (heal) caller.
    Stmt upd = db.prepare(
        "UPDATE health SET current = MAX(0, MIN(max, current - ?)) "
        "WHERE entity = ?");
    upd.bind(1, amount);
    upd.bind(2, target);
    upd.step();
    if (db.changes() == 0) {
        // No health row for `target`: refuse before appendEvent so the log
        // never records damage that never landed. Engine error; caller rolls
        // back the ambient transaction.
        throw std::runtime_error("damageEntity: entity " +
                                 std::to_string(target) + " has no health row");
    }
    // subject = the damaged entity; object carries the amount dealt (a per-event
    // number, read verb-specifically by render, like moved's destination room).
    appendEvent(db, actor, verb, target, amount, nullptr);
}

namespace {

// A grimoire's flavor (name + description) for a defeated archetype. Fixed,
// deterministic content (REQ-COMBAT-20). The archetype → learned-SPELL mapping
// lands in Step 18 as the grimoire→spell component; this is only the item's
// costume. Unknown archetypes fall back to a plain grimoire.
struct GrimoireFlavor {
    const char* name;
    const char* description;
};
GrimoireFlavor grimoireFlavorFor(const std::string& archetype) {
    if (archetype == "goblin_grunt") {
        return {"fire grimoire",
                "A slim grimoire bound in charred leather, its spine lettered in "
                "embers that never quite go cold. The rune of Fire glows on the "
                "cover."};
    }
    return {"grimoire",
            "A worn grimoire, its pages dense with a spell you have yet to read."};
}

}  // namespace

int64_t dropGrimoire(Db& db, const std::string& archetype, int64_t room) {
    const GrimoireFlavor flavor = grimoireFlavorFor(archetype);

    // Mint one entity (same pattern as writeGeneratedRoom): INSERT DEFAULT then
    // read the rowid.
    db.exec("INSERT INTO entities DEFAULT VALUES");
    int64_t item = 0;
    {
        Stmt s = db.prepare("SELECT last_insert_rowid()");
        if (!s.step()) throw std::runtime_error("dropGrimoire: rowid read failed");
        item = s.colInt(0);
    }
    {
        Stmt s = db.prepare("INSERT INTO portable(entity) VALUES (?)");
        s.bind(1, item);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO name(entity, value) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, std::string(flavor.name));
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO description(entity, prose) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, std::string(flavor.description));
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO location(entity, container) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, room);
        s.step();
    }
    return item;  // no event: the paired 'defeated' event records the drop
}

void setPendingStrike(Db& db, int64_t enemy, int64_t damage, const char* element) {
    Stmt s = db.prepare(
        "INSERT INTO pending_strike(entity, damage, element) VALUES (?, ?, ?) "
        "ON CONFLICT(entity) DO UPDATE SET damage = excluded.damage, "
        "element = excluded.element");
    s.bind(1, enemy);
    s.bind(2, damage);
    if (element) s.bind(3, std::string(element));  // unbound param = SQL NULL
    s.step();
    appendEvent(db, enemy, "telegraph", 0, 0, nullptr);
}

void clearPendingStrike(Db& db, int64_t enemy) {
    Stmt s = db.prepare("DELETE FROM pending_strike WHERE entity = ?");
    s.bind(1, enemy);
    s.step();
}

void defeatEnemy(Db& db, int64_t enemy, int64_t droppedItem, int64_t actor) {
    // Remove from play WITHOUT deleting the entity id or its name/description:
    // defeat is the persistent absence of hostile + location (REQ-COMBAT-30).
    // pending_strike is cleared too — a fight leaves no dangling wind-up.
    for (const char* table : {"hostile", "health", "location", "pending_strike"}) {
        Stmt s = db.prepare(
            ("DELETE FROM " + std::string(table) + " WHERE entity = ?").c_str());
        s.bind(1, enemy);
        s.step();
    }
    appendEvent(db, actor, "defeated", enemy, droppedItem, nullptr);
}

void downPlayer(Db& db, int64_t player, int64_t enemy, int64_t safeRoom,
                int64_t actor) {
    const int64_t fallRoom = [&] {
        Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
        s.bind(1, player);
        if (!s.step()) throw std::runtime_error("downPlayer: player has no location");
        return s.colInt(0);
    }();

    // Drop every carried portable at the fall room (REQ-COMBAT-24): knowledge is
    // permanent, possessions are droppable. Collect ids first, then move — never
    // mutate a table mid-iteration over it.
    std::vector<int64_t> carried;
    {
        Stmt s = db.prepare(
            "SELECT p.entity FROM portable p "
            "JOIN location l ON l.entity = p.entity "
            "WHERE l.container = ? ORDER BY p.entity");
        s.bind(1, player);
        while (s.step()) carried.push_back(s.colInt(0));
    }
    for (const int64_t item : carried) {
        moveEntity(db, item, fallRoom, actor, "dropped");
    }

    // Relocate the player to the safe room and restore health (a raw location
    // write recorded by the single 'downed' event below, not a 'moved').
    {
        Stmt s = db.prepare("UPDATE location SET container = ? WHERE entity = ?");
        s.bind(1, safeRoom);
        s.bind(2, player);
        s.step();
    }
    {
        Stmt s = db.prepare("UPDATE health SET current = max WHERE entity = ?");
        s.bind(1, player);
        s.step();
    }
    // The enemy that downed the player is restored to its initial combat state
    // (REQ-COMBAT-25): full health AND any pending wind-up cleared — the fight
    // resets to its opening position.
    {
        Stmt s = db.prepare("UPDATE health SET current = max WHERE entity = ?");
        s.bind(1, enemy);
        s.step();
    }
    clearPendingStrike(db, enemy);
    appendEvent(db, actor, "downed", player, safeRoom, nullptr);
}

int64_t writeGeneratedRoom(Db& db, int64_t originRoom,
                           const std::string& direction,
                           const RoomProposal& proposal, int64_t actor) {
    // The reciprocal is created mechanically (REQ-ARCH-8/-9): the caller only
    // reaches here for invertible directions, so a missing inverse is an engine
    // fault — throw before writing anything so the caller rolls back cleanly.
    const std::optional<std::string> inverse = inverseDirection(direction);
    if (!inverse) {
        throw std::runtime_error(
            "writeGeneratedRoom: non-invertible direction '" + direction + "'");
    }

    // Mint one entity — the first runtime entity mint (micro-decision #1). No
    // db.hpp change: INSERT DEFAULT VALUES then read last_insert_rowid().
    db.exec("INSERT INTO entities DEFAULT VALUES");
    int64_t newRoom = 0;
    {
        Stmt s = db.prepare("SELECT last_insert_rowid()");
        if (!s.step()) throw std::runtime_error("writeGeneratedRoom: rowid read failed");
        newRoom = s.colInt(0);
    }

    // Component rows: room tag, name, canon description. NO location row —
    // rooms have no container (matching base.sql).
    {
        Stmt s = db.prepare("INSERT INTO room(entity) VALUES (?)");
        s.bind(1, newRoom);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO name(entity, value) VALUES (?, ?)");
        s.bind(1, newRoom);
        s.bind(2, proposal.name);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO description(entity, prose) VALUES (?, ?)");
        s.bind(1, newRoom);
        s.bind(2, proposal.description);
        s.step();
    }

    // Realize the origin exit. In the production path this row pre-exists as a
    // latent (NULL-dest) stub — resolveGo reaches generation only via such a row
    // (REQ-EXITS-2b) — so this is an UPDATE of that row's dest, expressed as an
    // upsert against the (room, direction) PK for defensiveness. An ABSENT row
    // here is a precondition violation, never a normal path: log one stderr
    // diagnostic and proceed (the upsert degrades to a plain insert).
    {
        Stmt chk = db.prepare(
            "SELECT 1 FROM exits WHERE room = ? AND direction = ?");
        chk.bind(1, originRoom);
        chk.bind(2, direction);
        if (!chk.step()) {
            std::fprintf(stderr,
                         "writeGeneratedRoom: latent origin exit absent for "
                         "room %lld direction '%s' (REQ-EXITS-2b precondition "
                         "violation) — realizing via insert\n",
                         static_cast<long long>(originRoom), direction.c_str());
        }
    }
    {
        Stmt s = db.prepare(
            "INSERT INTO exits(room, direction, dest) VALUES (?, ?, ?) "
            "ON CONFLICT(room, direction) DO UPDATE SET dest = excluded.dest");
        s.bind(1, originRoom);
        s.bind(2, direction);
        s.bind(3, newRoom);
        s.step();
    }
    // The realized return exit. A fresh room has no prior rows, so this is a
    // plain insert.
    {
        Stmt s = db.prepare(
            "INSERT INTO exits(room, direction, dest) VALUES (?, ?, ?)");
        s.bind(1, newRoom);
        s.bind(2, *inverse);
        s.bind(3, originRoom);
        s.step();
    }
    // Plant latent stubs for each declared onward exit (NULL dest). The set is
    // already deduped and already excludes the return direction (REQ-EXITS-7),
    // so it cannot collide with the return row above. Latent stubs emit no
    // events.
    for (const std::string& dir : proposal.exits) {
        Stmt s = db.prepare(
            "INSERT INTO exits(room, direction, dest) VALUES (?, ?, NULL)");
        s.bind(1, newRoom);
        s.bind(2, dir);
        s.step();
    }

    // One 'generated' event: subject = the new room (asserted into existence),
    // object = its origin of reference, detail = the direction travelled. This
    // subject/object reading is deliberately unlike moveEntity's.
    appendEvent(db, actor, "generated", newRoom, originRoom, direction.c_str());

    return newRoom;
}
