#include "mutations.hpp"

#include <optional>
#include <stdexcept>
#include <string>

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

    // Both reciprocal exits: origin -direction-> new, new -inverse-> origin.
    // Persistence falls out of these rows — once they exist, exitDest finds the
    // room and no regeneration is possible (REQ-ARCH-3a).
    {
        Stmt s = db.prepare(
            "INSERT INTO exits(room, direction, dest) VALUES (?, ?, ?)");
        s.bind(1, originRoom);
        s.bind(2, direction);
        s.bind(3, newRoom);
        s.step();
    }
    {
        Stmt s = db.prepare(
            "INSERT INTO exits(room, direction, dest) VALUES (?, ?, ?)");
        s.bind(1, newRoom);
        s.bind(2, *inverse);
        s.bind(3, originRoom);
        s.step();
    }

    // One 'generated' event: subject = the new room (asserted into existence),
    // object = its origin of reference, detail = the direction travelled. This
    // subject/object reading is deliberately unlike moveEntity's.
    appendEvent(db, actor, "generated", newRoom, originRoom, direction.c_str());

    return newRoom;
}
