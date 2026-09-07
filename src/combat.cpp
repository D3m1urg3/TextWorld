#include "combat.hpp"

#include <cctype>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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

// Container of an arbitrary entity, or nullopt if it has no location row.
std::optional<int64_t> containerOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return std::nullopt;
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

// --- eligible-menu gating (REQ-COMBAT-32, -33, -34) — read-only, deterministic --

// The player entity (singleton by convention).
int64_t playerEntity(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("combat: world has no player entity");
    return s.colInt(0);
}

// Count of enemies the ARCHITECT has ever placed (REQ-COMBAT-33 ledger). A meta
// row incremented only by architect placement (Step 22), NOT by the seed's
// hand-placed enemy — absent row means zero (no architect spawn yet → bootstrap).
int64_t architectSpawnCount(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'architect_spawn_count'");
    if (!s.step()) return 0;
    return s.colInt(0);
}

// A barriered archetype (defense lock, REQ-COMBAT-17): its bestiary barrier flag.
bool barrierArchetype(Db& db, const std::string& archetype) {
    Stmt s = db.prepare("SELECT barrier FROM bestiary WHERE archetype = ?");
    s.bind(1, archetype);
    if (!s.step()) return false;
    return s.colInt(0) != 0;
}

// The element(s) an archetype is WEAK to (resistance ratio > 1, i.e. num > den) —
// the keys of an element lock. A resistance (num < den) is not a weakness and
// imposes no key.
std::vector<std::string> weaknessElements(Db& db, const std::string& archetype) {
    std::vector<std::string> elems;
    Stmt s = db.prepare(
        "SELECT element FROM resistance "
        "WHERE archetype = ? AND multiplier_num > multiplier_den");
    s.bind(1, archetype);
    while (s.step()) elems.push_back(s.colText(0));
    return elems;
}

// Basic-attack-soluble (REQ-COMBAT-18 floor, gating sense): no hard lock a basic
// attack cannot answer — neither a barrier (negates basic damage entirely) nor an
// element weakness (which the gating treats as requiring its element key). Pure
// telegraph/multiplicity archetypes are basic-soluble.
bool basicSoluble(Db& db, const std::string& archetype) {
    return !barrierArchetype(db, archetype) &&
           weaknessElements(db, archetype).empty();
}

// The tier of the spell this archetype drops (drop_table → spell_catalog), or 0
// if it drops nothing — the ordinal the bootstrap rule keys on (REQ-COMBAT-33).
int64_t dropTier(Db& db, const std::string& archetype) {
    Stmt s = db.prepare(
        "SELECT sc.tier FROM drop_table d "
        "JOIN spell_catalog sc ON sc.spell = d.spell WHERE d.archetype = ?");
    s.bind(1, archetype);
    if (!s.step()) return 0;
    return s.colInt(0);
}

// Whether `player` knows a spell whose catalog `effect` matches (e.g. 'dispel').
bool knowsSpellWithEffect(Db& db, int64_t player, const char* effect) {
    Stmt s = db.prepare(
        "SELECT 1 FROM known_spells ks JOIN spell_catalog sc ON sc.spell = ks.spell "
        "WHERE ks.entity = ? AND sc.effect = ? LIMIT 1");
    s.bind(1, player);
    s.bind(2, std::string(effect));
    return s.step();
}

// Whether `player` knows any spell of catalog `element` (the element lock key).
bool knowsSpellOfElement(Db& db, int64_t player, const std::string& element) {
    Stmt s = db.prepare(
        "SELECT 1 FROM known_spells ks JOIN spell_catalog sc ON sc.spell = ks.spell "
        "WHERE ks.entity = ? AND sc.element = ? LIMIT 1");
    s.bind(1, player);
    s.bind(2, element);
    return s.step();
}

// Whether `player` knows EVERY key `archetype`'s lock requires (REQ-COMBAT-32): a
// dispel for a barrier, and a spell of each weakness element. A basic-soluble
// archetype requires none, so this is trivially true for it.
bool knowsAllRequiredKeys(Db& db, int64_t player, const std::string& archetype) {
    if (barrierArchetype(db, archetype) &&
        !knowsSpellWithEffect(db, player, "dispel")) {
        return false;
    }
    for (const std::string& elem : weaknessElements(db, archetype)) {
        if (!knowsSpellOfElement(db, player, elem)) return false;
    }
    return true;
}

// The gated archetype menu given whether the target room is `contested` by the
// front (REQ-COMBAT-32, -33). Shared by the existing-room and prospective-room
// entry points. Empty when not contested. Ordered by archetype (deterministic).
std::vector<std::string> gatedMenu(Db& db, bool contested) {
    std::vector<std::string> menu;
    if (!contested) return menu;
    const int64_t player = playerEntity(db);
    const bool bootstrap = architectSpawnCount(db) == 0;
    Stmt s = db.prepare("SELECT archetype FROM bestiary ORDER BY archetype");
    while (s.step()) {
        const std::string archetype = s.colText(0);
        if (bootstrap) {
            if (basicSoluble(db, archetype) && dropTier(db, archetype) == 1) {
                menu.push_back(archetype);
            }
        } else if (knowsAllRequiredKeys(db, player, archetype)) {
            menu.push_back(archetype);
        }
    }
    return menu;
}

// An archetype's blurb — the ONLY archetype field the model ever sees
// (REQ-COMBAT-29). "" if the archetype has no bestiary row.
std::string blurbOf(Db& db, const std::string& archetype) {
    Stmt s = db.prepare("SELECT blurb FROM bestiary WHERE archetype = ?");
    s.bind(1, archetype);
    if (!s.step()) return "";
    return s.colText(0);
}

}  // namespace

// Whether `player` has `spell` in known_spells — the canon of what they can cast
// (REQ-COMBAT-7) and the "already learned" test for a re-read (REQ-COMBAT-21).
// Public for the same reason distanceFromSeed is: the story arc's
// `spell_learned` condition asks this exact question (REQ-ARC-STORE-5), and a
// second copy of the query in systems.cpp is the drift REQ-BARD-SEL-3 forbids.
// Read-only.
bool knowsSpell(Db& db, int64_t player, const std::string& spell) {
    Stmt s = db.prepare(
        "SELECT 1 FROM known_spells WHERE entity = ? AND spell = ?");
    s.bind(1, player);
    s.bind(2, spell);
    return s.step();
}

// BFS hop-distance from the seed room (kDormitoryCell) to `room` over REALIZED
// exits (dest non-NULL), or a large sentinel if unreachable — the deterministic
// front-intensity metric (REQ-COMBAT-34). Latent (ungenerated) exits are not
// edges: an unrealized frontier does not shorten the front. Public because the
// bard's story eligibility gates on the SAME metric (REQ-BARD-SEL-2b): a second
// BFS in bard.cpp is exactly the drift REQ-BARD-SEL-3 forbids.
int64_t distanceFromSeed(Db& db, int64_t room) {
    if (room == kDormitoryCell) return 0;
    std::unordered_set<int64_t> visited{kDormitoryCell};
    std::queue<int64_t> frontier;
    frontier.push(kDormitoryCell);
    int64_t depth = 0;
    while (!frontier.empty()) {
        ++depth;
        for (size_t level = frontier.size(); level > 0; --level) {
            const int64_t cur = frontier.front();
            frontier.pop();
            Stmt s = db.prepare(
                "SELECT dest FROM exits WHERE room = ? AND dest IS NOT NULL");
            s.bind(1, cur);
            while (s.step()) {
                const int64_t next = s.colInt(0);
                if (visited.insert(next).second) {
                    if (next == room) return depth;
                    frontier.push(next);
                }
            }
        }
    }
    return INT64_MAX;  // unreachable → treat as maximally far (a safe edge)
}

// The eligible menu for the room the architect is ABOUT to create beyond
// `originRoom`. The prospective room's only initial link is back to the origin,
// so its front distance is one hop past the origin's (REQ-COMBAT-34). Used to
// build the architect's enemy enum and to re-check a selection authoritatively,
// and by the bard for the prospective room's story menu (REQ-BARD-SEL-5).
std::vector<std::string> eligibleArchetypesForNewRoom(Db& db, int64_t originRoom) {
    const int64_t originDist = distanceFromSeed(db, originRoom);
    const int64_t newDist =
        (originDist == INT64_MAX) ? INT64_MAX : originDist + 1;
    return gatedMenu(db, newDist <= kFrontRadius);
}

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

void resolveRead(Db& db, int64_t player, int64_t subject) {
    // Is the target a grimoire (does it teach a spell)?
    std::string spell;
    {
        Stmt s = db.prepare("SELECT spell FROM grimoire WHERE entity = ?");
        s.bind(1, subject);
        if (s.step()) spell = s.colText(0);
    }
    if (spell.empty()) {
        appendEvent(db, player, "failed", 0, 0, "There's nothing to read there.");
        return;
    }
    // Reachable: the grimoire is in the player's room or in their inventory.
    const int64_t room = roomOf(db, player);
    const std::optional<int64_t> where = containerOf(db, subject);
    if (!where || (*where != room && *where != player)) {
        appendEvent(db, player, "failed", 0, 0, "You don't see that here.");
        return;
    }
    // Learn it (canon, permanent, idempotent). Distinguish a first learn from a
    // no-op re-read (REQ-COMBAT-21) for narration; both are a success.
    const bool alreadyKnown = knowsSpell(db, player, spell);
    learnSpell(db, player, spell);
    appendEvent(db, player, alreadyKnown ? "reread" : "learned", subject, 0,
                spell.c_str());
}

std::optional<std::string> castDenialReason(Db& db, int64_t player,
                                            const std::string& spell) {
    if (spell.empty()) return std::string("You don't know that spell.");

    // Learned? (REQ-COMBAT-7). known_spells is the canon of what the player can cast.
    if (!knowsSpell(db, player, spell)) {
        return std::string("You don't know that spell.");
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
        const std::string archetype = archetypeOf(db, enemy);
        const int64_t dmg = resistedDamage(db, archetype, element, kSpellDamage);
        // Tag the event with "<archetype>|<element>" (REQ-UI-41, -46). This is
        // the ONLY damage routed through resistedDamage, so it is the only
        // damage that teaches the player anything about resistances — the other
        // five call sites are not resistance-scaled and stay untagged. The tag
        // lives in events.detail rather than a new table, so no DDL and no
        // SCHEMA_VERSION bump (REQ-UI-45), and deleting a fight's events erases
        // what that fight taught (REQ-UI-46).
        const std::string tag = archetype + "|" + element;
        damageEntity(db, enemy, dmg, player,
                     effect == "frost" ? "froze" : "burned", tag.c_str());
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
    } else if (effect == "aoe") {
        // Multiplicity lock (REQ-COMBAT-17): reach EVERY body in the room in one
        // tick — fixed AoE damage plus a lingering DoT on each, the keys that
        // answer a swarm. Deterministic order by id.
        std::vector<int64_t> bodies;
        {
            Stmt s = db.prepare(
                "SELECT h.entity FROM hostile h "
                "JOIN location l ON l.entity = h.entity "
                "WHERE l.container = ? ORDER BY h.entity");
            s.bind(1, roomOf(db, player));
            while (s.step()) bodies.push_back(s.colInt(0));
        }
        for (const int64_t body : bodies) {
            damageEntity(db, body, kAoeDamage, player, "aoe");
            applyStatus(db, body, "dot", kDotDamage, kDotDuration);
        }
    }
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

void resolveCombat(Db& db, int64_t player, int64_t startRoom) {
    if (startRoom == 0) return;

    // Snapshot every hostile in the tick-start room, ordered by id (the swarm
    // case, REQ-COMBAT-17). Includes bodies the player's action just felled —
    // their per-body defeat runs below. Ordering is deterministic (no RNG).
    std::vector<int64_t> bodies;
    {
        Stmt s = db.prepare(
            "SELECT h.entity FROM hostile h "
            "JOIN location l ON l.entity = h.entity "
            "WHERE l.container = ? ORDER BY h.entity");
        s.bind(1, startRoom);
        while (s.step()) bodies.push_back(s.colInt(0));
    }
    if (bodies.empty()) return;  // not in combat this tick

    for (const int64_t body : bodies) {
        const std::optional<int64_t> hp = healthOf(db, body);
        if (!hp) continue;  // already removed

        // Felled by the player's action (attack/AoE) this tick → defeat + drop
        // now, no turn, no chip (REQ-COMBAT-3, -20). Per body, independently.
        if (*hp <= 0) {
            defeatHostile(db, body, player);
            continue;
        }

        // This body's single turn action (REQ-COMBAT-9): the telegraph → strike
        // lane, deterministic by telegraph_period. SUPPRESSED while incapacitated
        // by CC (stun or slow, REQ-COMBAT-19). Swarm bodies (period 0) just idle.
        if (!enemyIncapacitated(db, body)) {
            int64_t pendingDamage = 0;
            if (pendingStrikeDamage(db, body, pendingDamage)) {
                // A wind-up lands now (REQ-COMBAT-10) — unless a Ward negates it
                // and is consumed (REQ-COMBAT-11).
                if (hasStatus(db, player, "ward")) {
                    clearStatus(db, player, "ward");
                    appendEvent(db, body, "warded", player, 0, nullptr);
                } else {
                    damageEntity(db, player, pendingDamage, body, "struck");
                }
                clearPendingStrike(db, body);
            } else {
                const int64_t period = telegraphPeriodOf(db, body);
                if (period > 0 && currentTurn(db) % period == 0) {
                    setPendingStrike(db, body, kStrikeDamage, /*element=*/nullptr);
                }
            }
        }

        // DoT lane (REQ-COMBAT-19): any DoT on this body burns this tick, before
        // the countdown removes it. A DoT kill defeats the body now.
        applyDot(db, body, player);
        if (const auto dotHp = healthOf(db, body); dotHp && *dotHp <= 0) {
            defeatHostile(db, body, player);
            continue;
        }

        // Count down THIS body's status effects (its CC/DoT durations).
        tickStatusEffects(db, body);

        // Chip lane (REQ-COMBAT-12): each body deals its fixed chip in addition
        // to its turn action — the irreducible HP clock.
        const int64_t chip = chipOf(db, body);
        if (chip > 0) damageEntity(db, player, chip, body, "chip");
    }

    // The player's own status effects (a ward) count down once, after all bodies
    // have had their chance to be blocked by it.
    tickStatusEffects(db, player);

    // Downed check once, after every body's turn (REQ-COMBAT-23). The fight
    // resets around the first body (the primary foe of the encounter).
    const std::optional<int64_t> playerHp = healthOf(db, player);
    if (playerHp && *playerHp <= 0) {
        downPlayer(db, player, bodies.front(), kDormitoryCell, player);
    }
}

std::vector<std::string> eligibleArchetypes(Db& db, int64_t room) {
    // The menu for an EXISTING room: contested iff within the front radius of the
    // seed (REQ-COMBAT-34). Front, bootstrap, and gating compose in gatedMenu.
    return gatedMenu(db, distanceFromSeed(db, room) <= kFrontRadius);
}

std::vector<std::string> eligibleEnemyBlurbs(Db& db, int64_t originRoom) {
    std::vector<std::string> blurbs;
    for (const std::string& archetype : eligibleArchetypesForNewRoom(db, originRoom)) {
        blurbs.push_back(blurbOf(db, archetype));
    }
    return blurbs;
}

std::string archetypeForEnemyBlurb(Db& db, int64_t originRoom,
                                   const std::string& blurb) {
    if (blurb.empty()) return "";
    // Re-check eligibility authoritatively (REQ-COMBAT-31): a blurb the model
    // returns is placed ONLY if it is a currently-eligible choice for this room.
    // A hallucinated or stale selection resolves to "" → no spawn.
    for (const std::string& archetype : eligibleArchetypesForNewRoom(db, originRoom)) {
        if (blurbOf(db, archetype) == blurb) return archetype;
    }
    return "";
}
