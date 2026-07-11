#include "combat.hpp"

#include <cctype>
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

// True if `entity` currently carries an active (remaining > 0) status of `kind`.
bool hasStatus(Db& db, int64_t entity, const char* kind) {
    Stmt s = db.prepare(
        "SELECT 1 FROM status_effects WHERE entity = ? AND kind = ? "
        "AND remaining > 0");
    s.bind(1, entity);
    s.bind(2, std::string(kind));
    return s.step();
}

// True while `enemy` is incapacitated by crowd control (stun or slow) — its
// turn action is suppressed for the duration (REQ-COMBAT-19).
bool enemyIncapacitated(Db& db, int64_t enemy) {
    return hasStatus(db, enemy, "stun") || hasStatus(db, enemy, "slow");
}

// Applied elemental damage = base × resistance(archetype, element) as an integer
// ratio (REQ-COMBAT-16). No resistance row → neutral (1/1). Element "" (a basic
// attack) never reaches here — its caller skips the lookup entirely, which is
// what keeps the non-zero floor (REQ-COMBAT-18) holding against every archetype.
int64_t resistedDamage(Db& db, const std::string& archetype,
                       const std::string& element, int64_t base) {
    if (element.empty()) return base;
    Stmt s = db.prepare(
        "SELECT multiplier_num, multiplier_den FROM resistance "
        "WHERE archetype = ? AND element = ?");
    s.bind(1, archetype);
    s.bind(2, element);
    if (!s.step()) return base;  // neutral
    const int64_t num = s.colInt(0);
    const int64_t den = s.colInt(1);
    if (den == 0) return base;  // guard against a malformed row
    return base * num / den;    // integer ratio — deterministic
}

// Defeat + drop for a felled hostile: read its room + archetype (still present),
// mint its grimoire into the room, then remove it from play (REQ-COMBAT-20).
// Shared by the two defeat checks in resolveCombat (player-felled and DoT-felled).
void defeatHostile(Db& db, int64_t hostile, int64_t player) {
    const int64_t room = roomOf(db, hostile);
    const std::string archetype = archetypeOf(db, hostile);
    const int64_t grimoire = dropGrimoire(db, archetype, room);
    defeatEnemy(db, hostile, grimoire, player);
}

// Apply one tick of any damage-over-time on `entity` (REQ-COMBAT-19): fixed
// magnitude via damageEntity, dealt each tick BEFORE the status countdown
// removes the effect. Independent of CC — a slowed/stunned enemy still burns.
void applyDot(Db& db, int64_t entity, int64_t actor) {
    Stmt s = db.prepare(
        "SELECT magnitude FROM status_effects "
        "WHERE entity = ? AND kind = 'dot' AND remaining > 0");
    s.bind(1, entity);
    if (s.step()) {
        const int64_t mag = s.colInt(0);
        if (mag > 0) damageEntity(db, entity, mag, actor, "dot");
    }
}

}  // namespace

// The living hostile sharing `room` (health.current > 0), or 0 if none. Lowest
// entity id when several share a room — deterministic; the swarm case (Brick 3)
// relies on this stable ordering. Public so resolveGo can consult it.
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
    // Off cooldown? (REQ-COMBAT-13). Evaluated against the current meta.turn so
    // this gate agrees exactly with the status line's readiness display (Step
    // 12): a spell shown "ready" after tick t is castable on the next action.
    // Ready iff ready_turn <= currentTurn (cast at T → declined for ticks
    // T+1..T+cooldown, castable again once meta.turn reaches T+cooldown).
    {
        Stmt s = db.prepare(
            "SELECT ready_turn FROM cooldowns WHERE entity = ? AND spell = ?");
        s.bind(1, player);
        s.bind(2, spell);
        if (s.step() && s.colInt(0) > currentTurn(db)) {
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
    std::string effect;
    std::string element;
    {
        Stmt s = db.prepare(
            "SELECT cooldown, effect, element FROM spell_catalog WHERE spell = ?");
        s.bind(1, spell);
        if (s.step()) {
            cd = s.colInt(0);
            effect = s.colText(1);
            element = s.colText(2);  // NULL element → "" (a non-elemental spell)
        }
    }
    setCooldown(db, player, spell, currentTurn(db) + cd);
    appendEvent(db, player, "cast", 0, 0, spell.c_str());

    // Apply the spell's effect. The hostile in the room is the target for the
    // offensive/CC spells; a defensive spell (ward) targets the player.
    if (effect == "ward") {
        // A one-tick block flag: consumed when the pending strike resolves this
        // tick, else it expires in this tick's status countdown (REQ-COMBAT-11).
        applyStatus(db, player, "ward", 0, 1);
        return;
    }

    const int64_t enemy = hostileInRoom(db, roomOf(db, player));
    if (enemy == 0) return;  // nothing to target (a wasted cast); cooldown stands

    if (effect == "stun") {
        // Interrupt: cancel the pending strike and CC the enemy (REQ-COMBAT-19).
        applyStatus(db, enemy, "stun", 0, kStunDuration);
        clearPendingStrike(db, enemy);
        appendEvent(db, player, "stunned", enemy, 0, nullptr);
    } else if (effect == "damage" || effect == "frost") {
        // Elemental damage (REQ-COMBAT-16): base × resistance ratio for the
        // enemy's archetype. Frost also lays a brief slow (a CC), giving DoT/AoE
        // and the multiplicity lock company in Brick 3.
        const int64_t dmg =
            resistedDamage(db, archetypeOf(db, enemy), element, kSpellDamage);
        damageEntity(db, enemy, dmg, player, effect == "frost" ? "froze" : "burned");
        if (effect == "frost") {
            applyStatus(db, enemy, "slow", 0, kSlowDuration);
        }
    } else if (effect == "dot") {
        // Damage-over-time (REQ-COMBAT-19): a fixed magnitude burns each tick for
        // a fixed duration. The 'cast' event records the application; the per-tick
        // 'dot' damage is applied by resolveCombat's DoT lane.
        applyStatus(db, enemy, "dot", kDotDamage, kDotDuration);
    } else if (effect == "dispel") {
        // Defense lock (REQ-COMBAT-17): strip the enemy's barrier so damage can
        // land — the first key of a two-key sequence (Dispel → any damage).
        removeBarrier(db, enemy);
        appendEvent(db, player, "dispelled", enemy, 0, nullptr);
    }
    // AoE (Step 17) arrives later in Brick 3.
}

std::string combatStatusLine(Db& db, int64_t player) {
    // Only during combat: a living hostile shares the player's room. Self-gating
    // so both render paths can append it unconditionally.
    if (hostileInRoom(db, roomOf(db, player)) == 0) return "";

    std::string line;
    {
        Stmt s = db.prepare("SELECT current, max FROM health WHERE entity = ?");
        s.bind(1, player);
        if (!s.step()) return "";
        line = "HP: " + std::to_string(s.colInt(0)) + "/" +
               std::to_string(s.colInt(1));
    }

    // Per-known-spell readiness (REQ-COMBAT-15), computed from cooldowns vs the
    // current tick — engine-authored, never the model's. Alphabetical by spell
    // for a deterministic, replayable line. remaining = ready_turn - now (matches
    // the cast gate: "ready" ⟺ castable next action); no cooldown row ⟹ ready.
    const int64_t now = currentTurn(db);
    Stmt s = db.prepare(
        "SELECT ks.spell, c.ready_turn, c.ready_turn IS NULL "
        "FROM known_spells ks "
        "LEFT JOIN cooldowns c ON c.entity = ks.entity AND c.spell = ks.spell "
        "WHERE ks.entity = ? ORDER BY ks.spell");
    s.bind(1, player);
    while (s.step()) {
        std::string spell = s.colText(0);
        if (!spell.empty()) spell[0] = static_cast<char>(std::toupper(
                                static_cast<unsigned char>(spell[0])));
        const bool noCooldown = s.colInt(2) != 0;
        const int64_t remaining = s.colInt(1) - now;
        const std::string ready =
            (noCooldown || remaining <= 0) ? "ready" : std::to_string(remaining);
        line += " · " + spell + ": " + ready;
    }

    line += "\n";
    return line;
}

void resolveCombat(Db& db, int64_t player, int64_t hostile) {
    if (hostile == 0) return;  // no hostile was present at tick start → no combat

    const std::optional<int64_t> hp = healthOf(db, hostile);
    if (!hp) return;  // already removed (shouldn't happen mid-tick) → no-op

    // The player's action this tick may have felled the tick-start hostile. If
    // so, combat ends now (REQ-COMBAT-3): the enemy takes NO turn and deals no
    // chip — it is defeated, removed, and drops its grimoire (REQ-COMBAT-20).
    if (*hp <= 0) {
        defeatHostile(db, hostile, player);
        return;
    }

    // The enemy's single turn action (REQ-COMBAT-9): the telegraph → strike lane,
    // driven deterministically by telegraph_period — no RNG. SUPPRESSED entirely
    // while the enemy is incapacitated by CC (stun or frost's slow, REQ-COMBAT-19).
    if (!enemyIncapacitated(db, hostile)) {
        int64_t pendingDamage = 0;
        if (pendingStrikeDamage(db, hostile, pendingDamage)) {
            // A wind-up from last turn lands now (REQ-COMBAT-10) — unless the
            // player's counter this tick was a Ward, which negates it and is
            // consumed (REQ-COMBAT-11).
            if (hasStatus(db, player, "ward")) {
                clearStatus(db, player, "ward");
                appendEvent(db, hostile, "warded", player, 0, nullptr);
            } else {
                damageEntity(db, player, pendingDamage, hostile, "struck");
            }
            clearPendingStrike(db, hostile);
        } else {
            const int64_t period = telegraphPeriodOf(db, hostile);
            if (period > 0 && currentTurn(db) % period == 0) {
                // Telegraph: one-tick wind-up, no damage this tick. The strike
                // lands next turn (REQ-COMBAT-10). Goblin swing = no element.
                setPendingStrike(db, hostile, kStrikeDamage, /*element=*/nullptr);
            }
            // else idle this turn.
        }
    }

    // DoT lane (REQ-COMBAT-19): any damage-over-time on the enemy burns this
    // tick, before the countdown removes it. A DoT that fells the enemy defeats
    // it now — not a tick later — so it never lingers at 0 health.
    applyDot(db, hostile, player);
    if (const auto dotHp = healthOf(db, hostile); dotHp && *dotHp <= 0) {
        defeatHostile(db, hostile, player);
        return;
    }

    // Tick down status effects AFTER the enemy turn + DoT, so a Stun/Ward/DoT
    // cast THIS tick still affects this tick before counting down (an unconsumed
    // ward expires; stun/slow/DoT durations decrement).
    tickStatusEffects(db, player);
    tickStatusEffects(db, hostile);

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
