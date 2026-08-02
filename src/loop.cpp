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
#include "profile.hpp"
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

// The room the player stands in (for the tick-start combat key).
int64_t roomOf(Db& db, int64_t player) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, player);
    if (!s.step()) throw std::runtime_error("player has no location row");
    return s.colInt(0);
}

}  // namespace

TurnResult runTurn(Db& db, const std::string& line) {
    // Profiling (REQ-LAT-2), inert unless TEXTWORLD_PROFILE is on: one process-
    // local turn number shared by every record of this turn, then a stage timer
    // per SEMANTIC phase. The stages are scopes, so a phase this turn never
    // reaches simply constructs no timer and is ABSENT from the log rather than
    // reported as zero. `total` wraps the whole function, early returns included.
    profileNextTurn();
    const ScopedStage totalStage("total");

    // Resolution: AI resolver -> parser fallback when narration is enabled
    // (REQ-RESOLVE-1, -2), otherwise the fixed-verb parser directly — a disabled
    // run never constructs a transport. Tier a: neither yields an Action ->
    // renderError, no transaction, no tick, no world write.
    // The `resolve` stage is semantic: it covers BOTH the AI resolver and the
    // fixed-verb parser (REQ-LAT-2/-6), not "was there a network call".
    std::optional<Action> action;
    {
        const ScopedStage resolveStage("resolve");
        action = aiNarrationEnabled() ? resolveOrParse(db, line) : parse(db, line);
    }
    if (!action) {
        return {TurnOutcome::NoTick, renderError("I don't understand that.")};
    }

    // Quit is handled before any transaction opens; it never reaches resolve.
    if (action->verb == Verb::Quit) {
        return {TurnOutcome::Quit, ""};
    }

    // Cast availability gate (REQ-COMBAT-7/-13): an unknown or still-recharging
    // spell is not a valid action — declined WITHOUT a tick (no turn, no enemy
    // turn), mirroring the tier-a no-Action path above. Cooldown gates
    // availability; it never costs the player a turn.
    if (action->verb == Verb::Cast) {
        if (const auto reason = castDenialReason(db, playerId(db), action->spell)) {
            return {TurnOutcome::NoTick, renderError(*reason)};
        }
    }

    // The tick: one transaction, one turn increment, resolve, enemy turn, commit.
    // The `tick` stage spans the whole transaction — including the architect
    // call resolveGo may make inside it, which is why `generate` is emitted as
    // NESTED in tick (profile.hpp) rather than as a sibling stage.
    {
        const ScopedStage tickStage("tick");
        db.begin();
        try {
            const int64_t player = playerId(db);
            // Capture the ROOM the player stands in at TICK START, before the
            // action can move them out (micro-decision 2): every enemy that was
            // present still takes its one turn even as the player flees.
            const int64_t startRoom = roomOf(db, player);
            db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
            resolve(db, *action, player);
            // The enemy-turn system fires after the player's action, in the SAME
            // transaction (REQ-COMBAT-2): the loop, not resolve, owns the tick.
            resolveCombat(db, player, startRoom);
            db.commit();
        } catch (const std::exception& e) {
            // Tier c: engine error. Roll back — turn counter and world state as
            // if the prompt never happened.
            db.rollback();
            return {TurnOutcome::EngineError, renderError(e.what())};
        }
    }

    // Narration dispatch (REQ-PROSE-1, REQ-PROSE-2): AI prose when enabled
    // and delivered; the template renderer is the always-there fallback
    // (REQ-PROSE-3). Tier-a and tier-c paths above never reach this.
    // `narrate` is semantic too: AI prose and the template renderer alike.
    const ScopedStage narrateStage("narrate");
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

// See loop.hpp: exists so main() can name the room the pre-generation scheduler
// should look at (REQ-PREGEN-4) without duplicating this file's two lookups.
int64_t playerRoom(Db& db) { return roomOf(db, playerId(db)); }
