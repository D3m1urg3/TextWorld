// render(): events → text via dumb templates. READ-ONLY BY CONTRACT — this
// translation unit contains only SELECT statements. No INSERT, UPDATE, or
// DELETE may ever appear here (design §6).
#include "render.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

// --- read-only lookups ------------------------------------------------------

// Parser handle of an entity, or a shrug if it has no name row.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "something";
    return s.colText(0);
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
        Stmt s = db.prepare(
            "SELECT direction FROM exits WHERE room = ? ORDER BY direction");
        s.bind(1, room);
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
        }
        // Unrecognized verbs (e.g. the architect's 'generated', REQ-ARCH-10)
        // render nothing: the template emits output only for the verbs it knows,
        // so a world-gen turn shows as its 'moved' block with no extra line.
    }

    return out;
}

std::string renderError(const std::string& msg) {
    return msg + "\n";
}

std::string renderRoomOf(Db& db, int64_t actor) {
    return roomBlock(db, roomOf(db, actor));
}
