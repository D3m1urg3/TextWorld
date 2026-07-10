#include "mutations.hpp"

#include <cstdio>
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
