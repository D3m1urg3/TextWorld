// runTurn(): the per-line dispatch. This is the only place a tick transaction
// is opened, meta.turn is incremented, or a commit/rollback decision is made
// (REQ-PROTO-5: one prompt = at most one tick, exactly one turn increment).
#include "loop.hpp"

#include <exception>
#include <optional>
#include <stdexcept>

#include "action.hpp"
#include "render.hpp"
#include "systems.hpp"

namespace {

// The player entity, looked up fresh each turn (no caching machinery).
int64_t playerId(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("world has no player entity");
    return s.colInt(0);
}

int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) throw std::runtime_error("meta has no 'turn' row");
    return s.colInt(0);
}

}  // namespace

TurnResult runTurn(Db& db, const std::string& line) {
    // Tier a: unparseable. No transaction, no tick, no world write.
    const std::optional<Action> action = parse(db, line);
    if (!action) {
        return {TurnOutcome::NoTick, renderError("I don't understand that.")};
    }

    // Quit is handled before any transaction opens; it never reaches resolve.
    if (action->verb == Verb::Quit) {
        return {TurnOutcome::Quit, ""};
    }

    // The tick: one transaction, one turn increment, resolve, commit.
    db.begin();
    try {
        db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
        resolve(db, *action, playerId(db));
        db.commit();
    } catch (const std::exception& e) {
        // Tier c: engine error. Roll back — turn counter and world state as
        // if the prompt never happened.
        db.rollback();
        return {TurnOutcome::EngineError, renderError(e.what())};
    }

    return {TurnOutcome::Ticked, render(db, currentTurn(db))};
}

std::string renderStartup(Db& db) {
    return renderRoomOf(db, playerId(db));
}
