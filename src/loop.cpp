// runTurn(): the per-line dispatch. This is the only place a tick transaction
// is opened, meta.turn is incremented, or a commit/rollback decision is made
// (REQ-PROTO-5: one prompt = at most one tick, exactly one turn increment).
#include "loop.hpp"

#include <exception>
#include <optional>
#include <stdexcept>

#include "action.hpp"
#include "combat.hpp"
#include "nlresolve.hpp"
#include "prose.hpp"
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
    // Resolution: AI resolver -> parser fallback when narration is enabled
    // (REQ-RESOLVE-1, -2), otherwise the fixed-verb parser directly — a disabled
    // run never constructs a transport. Tier a: neither yields an Action ->
    // renderError, no transaction, no tick, no world write.
    const std::optional<Action> action =
        aiNarrationEnabled() ? resolveOrParse(db, line) : parse(db, line);
    if (!action) {
        return {TurnOutcome::NoTick, renderError("I don't understand that.")};
    }

    // Quit is handled before any transaction opens; it never reaches resolve.
    if (action->verb == Verb::Quit) {
        return {TurnOutcome::Quit, ""};
    }

    // The tick: one transaction, one turn increment, resolve, enemy turn, commit.
    db.begin();
    try {
        const int64_t player = playerId(db);
        // Capture the hostile present at TICK START, before the player's action
        // can move them out of the room (micro-decision 2): the enemy still
        // takes its one turn as the player flees. 0 when not in combat.
        const int64_t startHostile = tickStartHostile(db, player);
        db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
        resolve(db, *action, player);
        // The enemy-turn system fires after the player's action, in the SAME
        // transaction (REQ-COMBAT-2): the loop, not resolve, owns the tick.
        resolveCombat(db, player, startHostile);
        db.commit();
    } catch (const std::exception& e) {
        // Tier c: engine error. Roll back — turn counter and world state as
        // if the prompt never happened.
        db.rollback();
        return {TurnOutcome::EngineError, renderError(e.what())};
    }

    // Narration dispatch (REQ-PROSE-1, REQ-PROSE-2): AI prose when enabled
    // and delivered; the template renderer is the always-there fallback
    // (REQ-PROSE-3). Tier-a and tier-c paths above never reach this.
    if (aiNarrationEnabled()) {
        if (auto prose = aiRender(db, currentTurn(db))) {
            return {TurnOutcome::Ticked, *prose};
        }
    }
    return {TurnOutcome::Ticked, render(db, currentTurn(db))};
}

std::string renderStartup(Db& db) {
    return renderRoomOf(db, playerId(db));
}
