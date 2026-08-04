#include "mutations.hpp"

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "architect.hpp"  // RoomProposal (full definition) + inverseDirection
#include "term.hpp"       // utf8Truncate — the code-point-safe cut for bard_focus

namespace {

// Current turn number, read inside the caller's ambient transaction.
int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) throw std::runtime_error("meta.turn row missing");
    return s.colInt(0);
}

// Mint one fresh entity id: INSERT a bare row and read its rowid. The single id
// source for every runtime-created entity — generated rooms, dropped grimoires,
// spawned enemies — inside the caller's ambient transaction.
int64_t mintEntity(Db& db) {
    db.exec("INSERT INTO entities DEFAULT VALUES");
    Stmt s = db.prepare("SELECT last_insert_rowid()");
    if (!s.step()) throw std::runtime_error("mintEntity: rowid read failed");
    return s.colInt(0);
}

// Strip leading/trailing ASCII whitespace. Local to this file because there is
// no trim helper in the tree and only the catalog's non-empty checks need one:
// "  " must not pass for a handle (REQ-BARD-STORE-9).
std::string trimAscii(const std::string& s) {
    const char* const ws = " \t\n\r\f\v";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

// Collapse every RUN of newlines/carriage returns to a single space
// (REQ-BARD-STORE-16a). Per-character replacement would turn "\r\n" into two
// spaces; a run yields exactly one. Other whitespace is untouched — the
// requirement names line breaks only.
std::string collapseLineBreaks(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool inBreakRun = false;
    for (const char c : s) {
        if (c == '\n' || c == '\r') {
            if (!inBreakRun) out += ' ';
            inBreakRun = true;
        } else {
            out += c;
            inBreakRun = false;
        }
    }
    return out;
}

// Does a row match this one- or two-parameter lookup? The shape every
// writeCatalogEntry admission check shares: prepare, bind, step. Collapsing
// them here means a future gate is one call rather than a fifth copy of the
// same eight lines.
bool rowExists(Db& db, const char* sql, const std::string& a,
               const std::string& b = "") {
    Stmt s = db.prepare(sql);
    s.bind(1, a);
    if (!b.empty()) s.bind(2, b);
    return s.step();
}

// Free rewrite of one `meta` row, the shared body of the two bard lanes. Upsert
// rather than UPDATE so a world whose row is somehow absent still gets one;
// initialize() writes all three at init (REQ-BARD-STORE-6).
void upsertMeta(Db& db, const char* key, const std::string& value) {
    Stmt s = db.prepare(
        "INSERT INTO meta(key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, std::string(key));
    s.bind(2, value);
    s.step();
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

void damageEntity(Db& db, int64_t target, int64_t amount, int64_t actor,
                  const char* verb, const char* detail) {
    // Defense lock (REQ-COMBAT-17): while a barrier shields the target, ALL
    // damage is negated — a 'blocked' event records the deflection and health is
    // untouched, until Dispel strips the barrier. The player never carries a
    // barrier, so this only ever guards enemies.
    {
        Stmt b = db.prepare("SELECT 1 FROM barrier WHERE entity = ?");
        b.bind(1, target);
        if (b.step()) {
            appendEvent(db, actor, "blocked", target, 0, nullptr);
            return;
        }
    }

    // Clamp in code, not DDL: current := clamp(current - amount, 0, max). With
    // amount >= 0 the upper clamp is a no-op, but MIN(max, …) keeps the helper
    // correct for any future negative-amount (heal) caller.
    Stmt upd = db.prepare(
        "UPDATE health SET current = MAX(0, MIN(max, current - ?)) "
        "WHERE entity = ?");
    upd.bind(1, amount);
    upd.bind(2, target);
    upd.step();
    if (db.changes() == 0) {
        // No health row for `target`: refuse before appendEvent so the log
        // never records damage that never landed. Engine error; caller rolls
        // back the ambient transaction.
        throw std::runtime_error("damageEntity: entity " +
                                 std::to_string(target) + " has no health row");
    }
    // subject = the damaged entity; object carries the amount dealt (a per-event
    // number, read verb-specifically by render, like moved's destination room).
    // `detail` is NULL for every caller but the resistance-scaled elemental one
    // (combat.cpp), which tags it "<archetype>|<element>" — see mutations.hpp.
    appendEvent(db, actor, verb, target, amount, detail);
}

namespace {

// A grimoire's flavor (name + description) AND the spell it teaches for a
// defeated archetype — a fixed, deterministic archetype → grimoire → spell
// mapping (REQ-COMBAT-20), never probabilistic. Unknown archetypes fall back to
// a plain grimoire teaching nothing.
struct GrimoireFlavor {
    const char* name;
    const char* description;
    const char* spell;  // "" = teaches no spell
};
GrimoireFlavor grimoireFlavorFor(const std::string& archetype) {
    if (archetype == "goblin_grunt") {
        return {"fire grimoire",
                "A slim grimoire bound in charred leather, its spine lettered in "
                "embers that never quite go cold. The rune of Fire glows on the "
                "cover.",
                "fire"};
    }
    if (archetype == "rime_touched") {
        return {"frost grimoire",
                "A grimoire cold to the touch, its pages furred with rime.", "frost"};
    }
    if (archetype == "ironhide") {
        return {"dispel grimoire",
                "A grimoire clasped in iron, its counter-sigils gleaming.", "dispel"};
    }
    if (archetype == "book_swarm") {
        return {"blast grimoire",
                "A heavy grimoire scorched at the edges, humming with force.",
                "blast"};
    }
    return {"grimoire",
            "A worn grimoire, its pages dense with a spell you have yet to read.",
            ""};
}

}  // namespace

int64_t dropGrimoire(Db& db, const std::string& archetype, int64_t room) {
    const GrimoireFlavor flavor = grimoireFlavorFor(archetype);

    const int64_t item = mintEntity(db);
    {
        Stmt s = db.prepare("INSERT INTO portable(entity) VALUES (?)");
        s.bind(1, item);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO name(entity, value) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, std::string(flavor.name));
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO description(entity, prose) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, std::string(flavor.description));
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO location(entity, container) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, room);
        s.step();
    }
    // The grimoire → spell bridge (REQ-COMBAT-20): a fixed archetype → spell row,
    // read by Read to learn the spell. Only when the archetype teaches one.
    if (flavor.spell[0] != '\0') {
        Stmt s = db.prepare("INSERT INTO grimoire(entity, spell) VALUES (?, ?)");
        s.bind(1, item);
        s.bind(2, std::string(flavor.spell));
        s.step();
    }
    return item;  // no event: the paired 'defeated' event records the drop
}

int64_t placeEnemy(Db& db, const std::string& archetype, int64_t room) {
    // Read the frozen catalog row — the mold every instance is cast from
    // (REQ-COMBAT-29). An unknown archetype is an engine fault: the only callers
    // are the seed (fixed names) and the architect (constrained to catalog names).
    std::string name;
    std::string blurb;
    int64_t health = 0;
    int64_t chip = 0;
    int64_t telegraphPeriod = 0;
    int64_t barrier = 0;
    {
        Stmt s = db.prepare(
            "SELECT name, blurb, health, chip, telegraph_period, barrier "
            "FROM bestiary WHERE archetype = ?");
        s.bind(1, archetype);
        if (!s.step()) {
            throw std::runtime_error("placeEnemy: no bestiary row for archetype '" +
                                     archetype + "'");
        }
        name = s.colText(0);
        blurb = s.colText(1);
        health = s.colInt(2);
        chip = s.colInt(3);
        telegraphPeriod = s.colInt(4);
        barrier = s.colInt(5);
    }

    const int64_t enemy = mintEntity(db);

    // Component rows: the stats are COPIED from the catalog, never authored here.
    {
        Stmt s = db.prepare(
            "INSERT INTO hostile(entity, archetype, chip, telegraph_period) "
            "VALUES (?, ?, ?, ?)");
        s.bind(1, enemy);
        s.bind(2, archetype);
        s.bind(3, chip);
        s.bind(4, telegraphPeriod);
        s.step();
    }
    {
        Stmt s = db.prepare(
            "INSERT INTO health(entity, current, max) VALUES (?, ?, ?)");
        s.bind(1, enemy);
        s.bind(2, health);  // spawns at full health
        s.bind(3, health);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO name(entity, value) VALUES (?, ?)");
        s.bind(1, enemy);
        s.bind(2, name);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO description(entity, prose) VALUES (?, ?)");
        s.bind(1, enemy);
        s.bind(2, blurb);  // the archetype blurb is the spawned instance's canon prose
        s.step();
    }
    if (barrier != 0) {
        Stmt s = db.prepare("INSERT INTO barrier(entity) VALUES (?)");
        s.bind(1, enemy);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO location(entity, container) VALUES (?, ?)");
        s.bind(1, enemy);
        s.bind(2, room);
        s.step();
    }
    return enemy;  // no event: seed placement has none; architect placement rides 'generated'
}

void learnSpell(Db& db, int64_t player, const std::string& spell) {
    // Canon, permanent (REQ-COMBAT-21): add to known_spells, idempotent — reading
    // an already-known grimoire is a no-op. Never removed by any mechanic.
    Stmt s = db.prepare(
        "INSERT OR IGNORE INTO known_spells(entity, spell) VALUES (?, ?)");
    s.bind(1, player);
    s.bind(2, spell);
    s.step();
}

void applyStatus(Db& db, int64_t entity, const char* kind, int64_t magnitude,
                 int64_t remaining) {
    Stmt s = db.prepare(
        "INSERT INTO status_effects(entity, kind, magnitude, remaining) "
        "VALUES (?, ?, ?, ?) ON CONFLICT(entity, kind) DO UPDATE SET "
        "magnitude = excluded.magnitude, remaining = excluded.remaining");
    s.bind(1, entity);
    s.bind(2, std::string(kind));
    s.bind(3, magnitude);
    s.bind(4, remaining);
    s.step();
}

void clearStatus(Db& db, int64_t entity, const char* kind) {
    Stmt s = db.prepare(
        "DELETE FROM status_effects WHERE entity = ? AND kind = ?");
    s.bind(1, entity);
    s.bind(2, std::string(kind));
    s.step();
}

void tickStatusEffects(Db& db, int64_t entity) {
    {
        Stmt s = db.prepare(
            "UPDATE status_effects SET remaining = remaining - 1 WHERE entity = ?");
        s.bind(1, entity);
        s.step();
    }
    {
        Stmt s = db.prepare(
            "DELETE FROM status_effects WHERE entity = ? AND remaining <= 0");
        s.bind(1, entity);
        s.step();
    }
}

void removeBarrier(Db& db, int64_t entity) {
    Stmt s = db.prepare("DELETE FROM barrier WHERE entity = ?");
    s.bind(1, entity);
    s.step();
}

void setCooldown(Db& db, int64_t entity, const std::string& spell,
                 int64_t readyTurn) {
    Stmt s = db.prepare(
        "INSERT INTO cooldowns(entity, spell, ready_turn) VALUES (?, ?, ?) "
        "ON CONFLICT(entity, spell) DO UPDATE SET ready_turn = excluded.ready_turn");
    s.bind(1, entity);
    s.bind(2, spell);
    s.bind(3, readyTurn);
    s.step();
}

void setPendingStrike(Db& db, int64_t enemy, int64_t damage, const char* element) {
    Stmt s = db.prepare(
        "INSERT INTO pending_strike(entity, damage, element) VALUES (?, ?, ?) "
        "ON CONFLICT(entity) DO UPDATE SET damage = excluded.damage, "
        "element = excluded.element");
    s.bind(1, enemy);
    s.bind(2, damage);
    if (element) s.bind(3, std::string(element));  // unbound param = SQL NULL
    s.step();
    appendEvent(db, enemy, "telegraph", 0, 0, nullptr);
}

void clearPendingStrike(Db& db, int64_t enemy) {
    Stmt s = db.prepare("DELETE FROM pending_strike WHERE entity = ?");
    s.bind(1, enemy);
    s.step();
}

void defeatEnemy(Db& db, int64_t enemy, int64_t droppedItem, int64_t actor) {
    // Remove from play WITHOUT deleting the entity id or its name/description:
    // defeat is the persistent absence of hostile + location (REQ-COMBAT-30).
    // pending_strike is cleared too — a fight leaves no dangling wind-up.
    for (const char* table : {"hostile", "health", "location", "pending_strike"}) {
        Stmt s = db.prepare(
            ("DELETE FROM " + std::string(table) + " WHERE entity = ?").c_str());
        s.bind(1, enemy);
        s.step();
    }
    appendEvent(db, actor, "defeated", enemy, droppedItem, nullptr);
}

void downPlayer(Db& db, int64_t player, int64_t enemy, int64_t safeRoom,
                int64_t actor) {
    const int64_t fallRoom = [&] {
        Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
        s.bind(1, player);
        if (!s.step()) throw std::runtime_error("downPlayer: player has no location");
        return s.colInt(0);
    }();

    // Drop every carried portable at the fall room (REQ-COMBAT-24): knowledge is
    // permanent, possessions are droppable. Collect ids first, then move — never
    // mutate a table mid-iteration over it.
    std::vector<int64_t> carried;
    {
        Stmt s = db.prepare(
            "SELECT p.entity FROM portable p "
            "JOIN location l ON l.entity = p.entity "
            "WHERE l.container = ? ORDER BY p.entity");
        s.bind(1, player);
        while (s.step()) carried.push_back(s.colInt(0));
    }
    for (const int64_t item : carried) {
        moveEntity(db, item, fallRoom, actor, "dropped");
    }

    // Relocate the player to the safe room and restore health (a raw location
    // write recorded by the single 'downed' event below, not a 'moved').
    {
        Stmt s = db.prepare("UPDATE location SET container = ? WHERE entity = ?");
        s.bind(1, safeRoom);
        s.bind(2, player);
        s.step();
    }
    {
        Stmt s = db.prepare("UPDATE health SET current = max WHERE entity = ?");
        s.bind(1, player);
        s.step();
    }
    // The enemy that downed the player is restored to its initial combat state
    // (REQ-COMBAT-25): full health AND any pending wind-up cleared — the fight
    // resets to its opening position.
    {
        Stmt s = db.prepare("UPDATE health SET current = max WHERE entity = ?");
        s.bind(1, enemy);
        s.step();
    }
    clearPendingStrike(db, enemy);
    // Being downed clears the player's active status effects (REQ-COMBAT-23) and
    // the enemy's (fight reset) — a clean opening position on return.
    {
        Stmt s = db.prepare(
            "DELETE FROM status_effects WHERE entity IN (?, ?)");
        s.bind(1, player);
        s.bind(2, enemy);
        s.step();
    }
    appendEvent(db, actor, "downed", player, safeRoom, nullptr);
}

void recordArchitectSpawn(Db& db) {
    // Upsert the bootstrap ledger (REQ-COMBAT-33): create at 1 on the first
    // architect placement, increment thereafter. A meta bookkeeping row like
    // meta.turn — no event, no component write.
    db.exec(
        "INSERT INTO meta(key, value) VALUES ('architect_spawn_count', 1) "
        "ON CONFLICT(key) DO UPDATE SET value = value + 1");
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

    const int64_t newRoom = mintEntity(db);

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

// --- The bard's fact store (specs/bard-fact-store.md) ------------------------

int64_t writeCatalogEntry(Db& db, const std::string& kind,
                          const std::string& handle, const std::string& name,
                          const std::string& blurb, const std::string& motive,
                          int64_t tier, const std::string& factArchetype,
                          const std::string& factElement) {
    // EVERY check runs before ANY write, so a refusal leaves the catalog
    // byte-identical — the placeEnemy/moveEntity discipline (throw before
    // mutating). All of these are engine faults: the caller offers only valid
    // values, so the caller rolls back the ambient transaction.
    const auto refuse = [](const std::string& why) {
        throw std::runtime_error("writeCatalogEntry: " + why);
    };
    if (kind != "character" && kind != "beat") {
        refuse("kind must be 'character' or 'beat', got '" + kind + "'");
    }
    // Trim first, then test: a whitespace-only handle is empty (REQ-BARD-STORE-9).
    const std::string trimmedHandle = trimAscii(handle);
    const std::string trimmedName = trimAscii(name);
    const std::string trimmedBlurb = trimAscii(blurb);
    if (trimmedHandle.empty()) refuse("handle is empty after trim");
    if (trimmedName.empty()) refuse("name is empty after trim");
    if (trimmedBlurb.empty()) refuse("blurb is empty after trim");
    if (tier < 0) refuse("tier is negative (" + std::to_string(tier) + ")");
    // The motive vocabulary is CLOSED (REQ-BARD-STORE-5): the table is the
    // enforcement, so the bard cannot invent a ninth motive.
    if (!rowExists(db, "SELECT 1 FROM motive_catalog WHERE motive = ?", motive)) {
        refuse("unknown motive '" + motive + "'");
    }

    // THE TRUTH GATE (REQ-BARD-STORE-10). Both fact fields or neither.
    const bool hasArchetype = !factArchetype.empty();
    const bool hasElement = !factElement.empty();
    if (hasArchetype != hasElement) {
        refuse("fact_archetype and fact_element must be both set or both empty "
               "(got archetype '" + factArchetype + "', element '" +
               factElement + "')");
    }
    if (hasArchetype) {
        // a. The archetype must exist. Same refusal as placeEnemy's.
        if (!rowExists(db, "SELECT 1 FROM bestiary WHERE archetype = ?",
                       factArchetype)) {
            refuse("no bestiary row for fact_archetype '" + factArchetype + "'");
        }
        // b. The element must be a live ELEMENT, not merely a spell name. Plain
        // equality suffices: SQL's three-valued logic already excludes the
        // NULL-element rows (ward/stun/dispel/blast), so the admissible
        // vocabulary is exactly {fire, frost} and a beat naming a spell is
        // refused here.
        if (!rowExists(db, "SELECT 1 FROM spell_catalog WHERE element = ?",
                       factElement)) {
            refuse("'" + factElement + "' is not an element in spell_catalog");
        }
        // c. The pair must be materially NON-NEUTRAL. The engine can check the
        // pair but not the blurb's English claim about it, so "the entry may
        // not promise a falsehood" is enforceable only in this form: a missing
        // resistance row means x1, a beat about a neutral matchup teaches the
        // player nothing, and admitting it would let a blurb assert a weakness
        // that does not exist. Refusing it is the honest reading of the rule.
        if (!rowExists(db,
                       "SELECT 1 FROM resistance WHERE archetype = ? AND element = ?",
                       factArchetype, factElement)) {
            refuse("no resistance row for ('" + factArchetype + "', '" +
                   factElement + "') — a neutral matchup teaches nothing");
        }
    }

    Stmt ins = db.prepare(
        "INSERT INTO catalog(kind, handle, name, blurb, motive, tier, "
        "fact_archetype, fact_element) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, kind);
    ins.bind(2, trimmedHandle);
    ins.bind(3, trimmedName);
    ins.bind(4, trimmedBlurb);
    ins.bind(5, motive);
    ins.bind(6, tier);
    if (hasArchetype) {  // unbound params store SQL NULL: both NULL, or neither
        ins.bind(7, factArchetype);
        ins.bind(8, factElement);
    }
    ins.step();
    // No event: a latent entry has not happened. It becomes one when it
    // materializes (dropGrimoire/placeEnemy set the same precedent).
    Stmt id = db.prepare("SELECT last_insert_rowid()");
    if (!id.step()) refuse("rowid read failed");
    return id.colInt(0);
}

bool materializeCatalogEntry(Db& db, int64_t catalog, int64_t entity,
                             int64_t actor) {
    {
        // The one-way latch, guarded in SQL rather than by a prior read
        // (REQ-BARD-STORE-13). A second call — or a call for an id that does
        // not exist — matches no row and falls through to `return false`.
        Stmt upd = db.prepare(
            "UPDATE catalog SET entity = ? WHERE id = ? AND entity IS NULL");
        upd.bind(1, entity);
        upd.bind(2, catalog);
        upd.step();
    }
    if (db.changes() == 0) return false;

    std::string handle;
    {
        Stmt s = db.prepare("SELECT handle FROM catalog WHERE id = ?");
        s.bind(1, catalog);
        if (s.step()) handle = s.colText(0);
    }
    // The latch and the event are ONE fact (design decision 5): the L0→L2
    // transition and the bard's wake trigger read the same write, so they
    // cannot drift. `detail` is the handle — an engine-internal tag, shielded
    // from the narrator in prose.cpp (see the contract note in mutations.hpp).
    appendEvent(db, actor, "materialized", entity, catalog, handle.c_str());
    return true;
}

int64_t placeCatalogEntry(Db& db, int64_t catalog, int64_t room,
                          const std::string& description, int64_t actor) {
    // Read the entry AND pre-check the latch BEFORE minting. The order is
    // load-bearing: REQ-BARD-STORE-14 requires that a second call mint no
    // entity, which a check placed after the mint could not deliver. The
    // authoritative guarantee remains materializeCatalogEntry's WHERE clause
    // below; this pre-check is what keeps `entities` clean.
    std::string name;
    {
        Stmt s = db.prepare(
            "SELECT name FROM catalog WHERE id = ? AND entity IS NULL");
        s.bind(1, catalog);
        if (!s.step()) return 0;  // absent, or already materialized
        name = s.colText(0);
    }

    const int64_t what = mintEntity(db);
    {
        // The parser noun comes from the catalog; the handle never does.
        Stmt s = db.prepare("INSERT INTO name(entity, value) VALUES (?, ?)");
        s.bind(1, what);
        s.bind(2, name);
        s.step();
    }
    {
        // Canon prose is the PASSED description, never the blurb — the blurb is
        // selection prose the model has already seen.
        Stmt s = db.prepare("INSERT INTO description(entity, prose) VALUES (?, ?)");
        s.bind(1, what);
        s.bind(2, description);
        s.step();
    }
    {
        Stmt s = db.prepare("INSERT INTO location(entity, container) VALUES (?, ?)");
        s.bind(1, what);
        s.bind(2, room);
        s.step();
    }
    materializeCatalogEntry(db, catalog, what, actor);  // latches + logs
    return what;
}

void markCatalogSeeded(Db& db, int64_t catalog) {
    // Event-free bookkeeping. The guard makes a second call a no-op BY
    // CONSTRUCTION, not by discipline — the learnSpell idempotence shape.
    Stmt upd = db.prepare(
        "UPDATE catalog SET seeded = 1 WHERE id = ? AND seeded = 0");
    upd.bind(1, catalog);
    upd.step();
}

void writeBardJournal(Db& db, const std::string& text) {
    upsertMeta(db, "bard_journal", text);  // verbatim, uncapped, free rewrite
}

void writeBardFocus(Db& db, const std::string& text) {
    // Normalize, THEN cut (REQ-BARD-STORE-16a): collapsing first means the cap
    // is spent on the line the architect will actually read, and truncating a
    // multi-byte character in half would store invalid UTF-8 into a string that
    // ends up inside a JSON prompt on every room generation.
    upsertMeta(db, "bard_focus",
               utf8Truncate(collapseLineBreaks(text), kBardFocusMaxChars));
}

void writeBardWakeTurn(Db& db, int64_t turn) {
    // Bound as an INTEGER, not through upsertMeta's string path: world.cpp
    // seeds this row as 0 and every reader treats it as a number, so storing
    // "7" here would leave the column's type dependent on who wrote it last.
    // Free rewrite — this is a position marker, not a log (REQ-BARD-WAKE-11).
    Stmt s = db.prepare(
        "INSERT INTO meta(key, value) VALUES ('bard_last_wake_turn', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, turn);
    s.step();
}
