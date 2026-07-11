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

// Archetype tag of a hostile ("" if it has no hostile row).
std::string archetypeOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT archetype FROM hostile WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

// The current (already-incremented) tick number — the global clock the telegraph
// schedule and cooldowns key on.
int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) throw std::runtime_error("combat: meta.turn row missing");
    return s.colInt(0);
}

// Telegraph cadence constant of a hostile (0 = never winds up).
int64_t telegraphPeriodOf(Db& db, int64_t enemy) {
    Stmt s = db.prepare("SELECT telegraph_period FROM hostile WHERE entity = ?");
    s.bind(1, enemy);
    if (!s.step()) return 0;
    return s.colInt(0);
}

// If `enemy` has a pending telegraphed strike, set `damage` and return true.
bool pendingStrikeDamage(Db& db, int64_t enemy, int64_t& damage) {
    Stmt s = db.prepare("SELECT damage FROM pending_strike WHERE entity = ?");
    s.bind(1, enemy);
    if (!s.step()) return false;
    damage = s.colInt(0);
    return true;
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

std::optional<std::string> castDenialReason(Db& db, int64_t player,
                                            const std::string& spell) {
    if (spell.empty()) return std::string("You don't know that spell.");

    // Learned? (REQ-COMBAT-7). known_spells is the canon of what the player can cast.
    {
        Stmt s = db.prepare(
            "SELECT 1 FROM known_spells WHERE entity = ? AND spell = ?");
        s.bind(1, player);
        s.bind(2, spell);
        if (!s.step()) return std::string("You don't know that spell.");
    }
    // Off cooldown? (REQ-COMBAT-13). This runs PRE-TICK, so the cast would
    // execute on the next tick (currentTurn + 1); it is ready iff
    // ready_turn <= currentTurn + 1.
    {
        Stmt s = db.prepare(
            "SELECT ready_turn FROM cooldowns WHERE entity = ? AND spell = ?");
        s.bind(1, player);
        s.bind(2, spell);
        if (s.step() && s.colInt(0) > currentTurn(db) + 1) {
            return std::string("That spell is still recharging.");
        }
    }
    return std::nullopt;  // castable now
}

void resolveCast(Db& db, int64_t player, const std::string& spell) {
    // Availability was gated pre-tick; here the spell is known and ready. Set
    // its cooldown (immutable constant, never reduced — REQ-COMBAT-14). now is
    // the execution turn (meta.turn already incremented): ready_turn = now + cd.
    int64_t cd = 0;
    {
        Stmt s = db.prepare("SELECT cooldown FROM spell_catalog WHERE spell = ?");
        s.bind(1, spell);
        if (s.step()) cd = s.colInt(0);
    }
    setCooldown(db, player, spell, currentTurn(db) + cd);
    appendEvent(db, player, "cast", 0, 0, spell.c_str());
    // The spell's EFFECT (Ward blocks, Stun interrupts, …) is Step 10.
}

std::string combatStatusLine(Db& db, int64_t player) {
    // Only during combat: a living hostile shares the player's room. Self-gating
    // so both render paths can append it unconditionally.
    if (hostileInRoom(db, roomOf(db, player)) == 0) return "";

    Stmt s = db.prepare("SELECT current, max FROM health WHERE entity = ?");
    s.bind(1, player);
    if (!s.step()) return "";
    const int64_t cur = s.colInt(0);
    const int64_t mx = s.colInt(1);
    // Brick 1: HP only. Step 12 prepends per-spell cooldown readiness.
    return "HP: " + std::to_string(cur) + "/" + std::to_string(mx) + "\n";
}

void resolveCombat(Db& db, int64_t player, int64_t hostile) {
    if (hostile == 0) return;  // no hostile was present at tick start → no combat

    const std::optional<int64_t> hp = healthOf(db, hostile);
    if (!hp) return;  // already removed (shouldn't happen mid-tick) → no-op

    // The player's action this tick may have felled the tick-start hostile. If
    // so, combat ends now (REQ-COMBAT-3): the enemy takes NO turn and deals no
    // chip — it is defeated, removed, and drops its grimoire (REQ-COMBAT-20).
    if (*hp <= 0) {
        const int64_t room = roomOf(db, hostile);
        const std::string archetype = archetypeOf(db, hostile);
        const int64_t grimoire = dropGrimoire(db, archetype, room);
        defeatEnemy(db, hostile, grimoire, player);
        return;
    }

    // The enemy's single turn action (REQ-COMBAT-9): the telegraph → strike lane,
    // driven deterministically by telegraph_period — no RNG.
    int64_t pendingDamage = 0;
    if (pendingStrikeDamage(db, hostile, pendingDamage)) {
        // A wind-up from last turn lands now (REQ-COMBAT-10). Counters that
        // block/cancel it are Step 10; here, uncountered, it always lands.
        damageEntity(db, player, pendingDamage, hostile, "struck");
        clearPendingStrike(db, hostile);
    } else {
        const int64_t period = telegraphPeriodOf(db, hostile);
        if (period > 0 && currentTurn(db) % period == 0) {
            // Telegraph: one-tick wind-up, no damage this tick. The strike lands
            // on the enemy's next turn (REQ-COMBAT-10). Goblin swing = no element.
            setPendingStrike(db, hostile, kStrikeDamage, /*element=*/nullptr);
        }
        // else idle this turn.
    }

    // Chip lane (REQ-COMBAT-12): every combat tick, in ADDITION to the turn
    // action, the enemy deals its fixed per-instance chip — the irreducible HP
    // clock. A land tick therefore deals strike + chip.
    const int64_t chip = chipOf(db, hostile);
    if (chip > 0) {
        damageEntity(db, player, chip, hostile, "chip");
    }

    // Did the enemy's turn (a landed strike and/or chip) drop the player to 0?
    // Then the player is downed, not dead (REQ-COMBAT-23): they wake in the
    // dormitory cell at full health, having dropped their carried items where
    // they fell, and the fight resets.
    const std::optional<int64_t> playerHp = healthOf(db, player);
    if (playerHp && *playerHp <= 0) {
        downPlayer(db, player, hostile, kDormitoryCell, player);
    }
}
