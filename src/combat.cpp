#include "combat.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include "mutations.hpp"

namespace {

// Room an entity currently stands in (location.container).
int64_t roomOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) {
        throw std::runtime_error("combat: entity " + std::to_string(entity) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// The living hostile sharing `room` (health.current > 0), or 0 if none. Lowest
// entity id when several share a room — deterministic; the swarm case (Brick 3)
// relies on this stable ordering.
int64_t hostileInRoom(Db& db, int64_t room) {
    Stmt s = db.prepare(
        "SELECT h.entity FROM hostile h "
        "JOIN location l ON l.entity = h.entity "
        "JOIN health hp ON hp.entity = h.entity "
        "WHERE l.container = ? AND hp.current > 0 "
        "ORDER BY h.entity LIMIT 1");
    s.bind(1, room);
    if (!s.step()) return 0;
    return s.colInt(0);
}

// health.current of an entity, or nullopt if it has no health row.
std::optional<int64_t> healthOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT current FROM health WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return std::nullopt;
    return s.colInt(0);
}

// Per-instance chip constant of a hostile (0 if it has no hostile row).
int64_t chipOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT chip FROM hostile WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return 0;
    return s.colInt(0);
}

}  // namespace

void resolveAttack(Db& db, int64_t player) {
    const int64_t room = roomOf(db, player);
    const int64_t enemy = hostileInRoom(db, room);
    if (enemy == 0) {
        // No hostile here: an in-world refusal, one 'failed' event, no write.
        appendEvent(db, player, "failed", 0, 0, "There's nothing here to attack.");
        return;
    }
    // Fixed damage, no element (REQ-COMBAT-6/-18). subject reused as the target
    // enemy is not needed here — combat is single-enemy per room; the engine
    // finds it. Defeat is checked by the enemy-turn system (Step 5).
    damageEntity(db, enemy, kBasicAttackDamage, player, "attacked");
}

int64_t tickStartHostile(Db& db, int64_t player) {
    return hostileInRoom(db, roomOf(db, player));
}

void resolveCombat(Db& db, int64_t player, int64_t hostile) {
    if (hostile == 0) return;  // no hostile was present at tick start → no combat

    // Active iff the tick-start hostile is still alive. A player action that
    // brought it to 0 ends combat this tick; the enemy takes no turn and deals
    // no chip (its removal + drop is Step 5).
    const std::optional<int64_t> hp = healthOf(db, hostile);
    if (!hp || *hp <= 0) return;

    // The enemy's single turn action (REQ-COMBAT-9). Brick 1: idle — the
    // telegraph/strike lane is Step 8. The system still fires: it is the first
    // non-player actor, and the chip lane below is its footprint on the tick.

    // Chip lane (REQ-COMBAT-12): every combat tick, in ADDITION to the turn
    // action, the enemy deals its fixed per-instance chip — the irreducible HP
    // clock. Even optimal play costs health.
    const int64_t chip = chipOf(db, hostile);
    if (chip > 0) {
        damageEntity(db, player, chip, hostile, "chip");
    }
}
