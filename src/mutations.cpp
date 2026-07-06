#include "mutations.hpp"

#include <stdexcept>
#include <string>

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
