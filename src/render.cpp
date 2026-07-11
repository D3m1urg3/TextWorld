// render(): events → text via dumb templates. READ-ONLY BY CONTRACT — this
// translation unit contains only SELECT statements. No INSERT, UPDATE, or
// DELETE may ever appear here (design §6).
#include "render.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "architect.hpp"  // architectEnabled() — the latent-exit DISPLAY gate
#include "combat.hpp"      // combatStatusLine() — the engine-authored HP/cooldown tail

namespace {

// --- read-only lookups ------------------------------------------------------

// Parser handle of an entity, or a shrug if it has no name row.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "something";
    return s.colText(0);
}

// The player entity (singleton by convention). Read-only.
int64_t playerEntity(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("render: world has no player entity");
    return s.colInt(0);
}

// Room the actor currently stands in (for 'looked' with no destination).
int64_t roomOf(Db& db, int64_t actor) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, actor);
    if (!s.step()) {
        throw std::runtime_error("render: entity " + std::to_string(actor) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// Names of the portables whose container is `holder`, in entity order.
std::vector<std::string> portableNamesIn(Db& db, int64_t holder) {
    std::vector<std::string> items;
    Stmt s = db.prepare(
        "SELECT n.value FROM portable p "
        "JOIN location l ON l.entity = p.entity "
        "JOIN name n ON n.entity = p.entity "
        "WHERE l.container = ? ORDER BY p.entity");
    s.bind(1, holder);
    while (s.step()) items.push_back(s.colText(0));
    return items;
}

// Join a list as "a, b, c".
std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

// The full room description block: canon prose, exits, visible portables.
// Every line here is sourced by the 'moved'/'looked' event that asked for it.
std::string roomBlock(Db& db, int64_t room) {
    std::string out;

    {
        Stmt s = db.prepare("SELECT prose FROM description WHERE entity = ?");
        s.bind(1, room);
        if (s.step()) out += s.colText(0) + "\n";
    }

    {
        std::vector<std::string> dirs;
        // Realized exits (dest non-NULL) always list; latent exits (dest NULL)
        // list ONLY when the architect is enabled — walking one would wall
        // otherwise (REQ-EXITS-4). A latent exit renders IDENTICALLY to a
        // realized one: no marker distinguishes them.
        Stmt s = db.prepare(
            "SELECT direction FROM exits WHERE room = ? "
            "AND (dest IS NOT NULL OR ?) ORDER BY direction");
        s.bind(1, room);
        s.bind(2, architectEnabled() ? 1 : 0);
        while (s.step()) dirs.push_back(s.colText(0));
        if (!dirs.empty()) out += "Exits: " + joinList(dirs) + ".\n";
    }

    {
        const std::vector<std::string> items = portableNamesIn(db, room);
        if (!items.empty()) out += "You see: " + joinList(items) + ".\n";
    }

    return out;
}

// Inventory listing: portables whose container is the actor.
std::string inventoryBlock(Db& db, int64_t actor) {
    const std::vector<std::string> items = portableNamesIn(db, actor);
    if (items.empty()) return "You are carrying nothing.\n";
    return "You are carrying: " + joinList(items) + ".\n";
}

}  // namespace

std::string render(Db& db, int64_t turn) {
    std::string out;

    Stmt ev = db.prepare(
        "SELECT actor, verb, subject, object, detail, detail IS NULL "
        "FROM events WHERE turn = ? ORDER BY id");
    ev.bind(1, turn);

    while (ev.step()) {
        const int64_t actor = ev.colInt(0);
        const std::string verb = ev.colText(1);
        const int64_t subject = ev.colInt(2);
        const int64_t object = ev.colInt(3);
        const std::string detail = ev.colText(4);
        const bool detailIsNull = ev.colInt(5) != 0;

        if (verb == "moved") {
            out += roomBlock(db, object);
        } else if (verb == "took") {
            out += "You take the " + nameOf(db, subject) + ".\n";
        } else if (verb == "dropped") {
            out += "You drop the " + nameOf(db, subject) + ".\n";
        } else if (verb == "looked") {
            if (detailIsNull) {
                out += roomBlock(db, roomOf(db, actor));
            } else if (detail == "inventory") {
                out += inventoryBlock(db, actor);
            }
            // Other 'looked' details do not exist yet; render nothing rather
            // than invent output with no template.
        } else if (verb == "waited") {
            out += "Time passes.\n";
        } else if (verb == "failed") {
            out += detail + "\n";
        } else if (verb == "attacked") {
            // Combat (REQ-COMBAT-37): the permanent template fallback. object
            // carries the engine-owned damage number; the model never sets it.
            out += "You strike the " + nameOf(db, subject) + " for " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "chip") {
            // actor is the enemy; subject is the player (whom it wounds).
            out += "The " + nameOf(db, actor) + " wounds you for " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "telegraph") {
            // A one-tick wind-up (actor = enemy): the counter window opens.
            out += "The " + nameOf(db, actor) +
                   " winds up a heavy blow — strike it down or brace!\n";
        } else if (verb == "struck") {
            // A landed telegraphed strike (actor = enemy, object = damage).
            out += "The " + nameOf(db, actor) + " lands its blow, hitting you for " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "cast") {
            // A cast spell (detail = the spell key). Effect-specific lines
            // (warded/stunned) render below when the effect resolves.
            out += "You cast " + detail + ".\n";
        } else if (verb == "warded") {
            // A telegraphed strike blocked by a ward (actor = the thwarted enemy).
            out += "The " + nameOf(db, actor) +
                   "'s strike breaks against your ward.\n";
        } else if (verb == "stunned") {
            // A telegraphed strike interrupted by a stun (subject = the enemy).
            out += "You bind the " + nameOf(db, subject) +
                   ", its strike collapsing mid-swing.\n";
        } else if (verb == "burned") {
            // Fire damage (subject = enemy, object = resistance-adjusted amount).
            out += "Your fire sears the " + nameOf(db, subject) + " for " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "froze") {
            // Frost damage + a slow (subject = enemy, object = adjusted amount).
            out += "Your frost bites the " + nameOf(db, subject) + " for " +
                   std::to_string(object) + " damage, and its movements slow.\n";
        } else if (verb == "dot") {
            // A damage-over-time tick (subject = enemy, object = per-tick amount).
            out += "The " + nameOf(db, subject) + " smoulders, taking " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "blocked") {
            // Damage negated by a defense-lock barrier (subject = the shielded foe).
            out += "Your attack breaks against the " + nameOf(db, subject) +
                   "'s barrier, doing nothing.\n";
        } else if (verb == "dispelled") {
            // A barrier stripped by Dispel (subject = the enemy).
            out += "Your dispel tears away the " + nameOf(db, subject) +
                   "'s barrier.\n";
        } else if (verb == "aoe") {
            // An area-of-effect hit on one body (subject = that body, object = amount).
            out += "The blast tears into the " + nameOf(db, subject) + " for " +
                   std::to_string(object) + " damage.\n";
        } else if (verb == "learned") {
            // A grimoire read for the first time (subject = grimoire, detail = spell).
            out += "You study the " + nameOf(db, subject) +
                   " and learn to cast " + detail + ".\n";
        } else if (verb == "reread") {
            // A grimoire whose spell is already known (a no-op success).
            out += "You study the " + nameOf(db, subject) +
                   ", but you already know " + detail + ".\n";
        } else if (verb == "defeated") {
            // subject = the fallen enemy (name survives), object = its grimoire.
            out += "The " + nameOf(db, subject) + " falls. It drops the " +
                   nameOf(db, object) + ".\n";
        } else if (verb == "downed") {
            // subject = player, object = the dormitory cell they wake in.
            out += "The world tips and goes black. You wake on the cold floor "
                   "of the dormitory cell.\n";
            out += roomBlock(db, object);
        }
        // Unrecognized verbs (e.g. the architect's 'generated', REQ-ARCH-10)
        // render nothing: the template emits output only for the verbs it knows,
        // so a world-gen turn shows as its 'moved' block with no extra line.
    }

    // Engine-authored combat status line (REQ-COMBAT-15): appended whenever a
    // hostile shares the player's room. Self-gating (empty otherwise), so this
    // is unconditional here and byte-identical to the AI path's append.
    out += combatStatusLine(db, playerEntity(db));

    return out;
}

std::string renderError(const std::string& msg) {
    return msg + "\n";
}

std::string renderRoomOf(Db& db, int64_t actor) {
    return roomBlock(db, roomOf(db, actor));
}
