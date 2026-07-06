#include "systems.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include "mutations.hpp"

namespace {

// --- reads: free-form SELECTs against component tables -------------------

// Room the player currently stands in (location.container of the player).
int64_t roomOf(Db& db, int64_t player) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, player);
    if (!s.step()) {
        throw std::runtime_error("resolve: player " + std::to_string(player) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// Container of an arbitrary entity, or nullopt if it has no location row.
std::optional<int64_t> containerOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return std::nullopt;
    return s.colInt(0);
}

// Destination of the exit (room, direction), or nullopt if no such exit.
std::optional<int64_t> exitDest(Db& db, int64_t room, const std::string& direction) {
    Stmt s = db.prepare("SELECT dest FROM exits WHERE room = ? AND direction = ?");
    s.bind(1, room);
    s.bind(2, direction);
    if (!s.step()) return std::nullopt;
    return s.colInt(0);
}

bool isPortable(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT 1 FROM portable WHERE entity = ?");
    s.bind(1, entity);
    return s.step();
}

// --- per-verb resolution --------------------------------------------------

void resolveGo(Db& db, const Action& action, int64_t player) {
    const int64_t room = roomOf(db, player);
    if (auto dest = exitDest(db, room, action.direction)) {
        moveEntity(db, player, *dest, player, "moved");
    } else {
        appendEvent(db, player, "failed", 0, 0, "You can't go that way.");
    }
}

void resolveTake(Db& db, const Action& action, int64_t player) {
    const int64_t room = roomOf(db, player);
    const std::optional<int64_t> where = containerOf(db, action.subject);
    if (where && *where == player) {
        appendEvent(db, player, "failed", 0, 0, "You're already carrying that.");
    } else if (!isPortable(db, action.subject)) {
        appendEvent(db, player, "failed", 0, 0, "You can't take that.");
    } else if (!where || *where != room) {
        appendEvent(db, player, "failed", 0, 0, "You don't see that here.");
    } else {
        moveEntity(db, action.subject, player, player, "took");
    }
}

void resolveDrop(Db& db, const Action& action, int64_t player) {
    const std::optional<int64_t> where = containerOf(db, action.subject);
    if (!where || *where != player) {
        appendEvent(db, player, "failed", 0, 0, "You aren't carrying that.");
    } else {
        moveEntity(db, action.subject, roomOf(db, player), player, "dropped");
    }
}

}  // namespace

void resolve(Db& db, const Action& action, int64_t player) {
    switch (action.verb) {
        case Verb::Go:
            resolveGo(db, action, player);
            break;
        case Verb::Take:
            resolveTake(db, action, player);
            break;
        case Verb::Drop:
            resolveDrop(db, action, player);
            break;
        case Verb::Look:
            appendEvent(db, player, "looked", 0, 0, nullptr);
            break;
        case Verb::Inventory:
            // Same 'looked' verb; the renderer branches on detail='inventory'.
            // No seventh event-verb string exists.
            appendEvent(db, player, "looked", 0, 0, "inventory");
            break;
        case Verb::Wait:
            appendEvent(db, player, "waited", 0, 0, nullptr);
            break;
        case Verb::Quit:
            // Quit is handled by the game loop BEFORE the tick transaction is
            // opened — it must never reach resolve. Throwing (rather than
            // silently ignoring) surfaces the loop bug immediately; the caller
            // rolls back the transaction, so no tick is recorded.
            throw std::logic_error("resolve: Verb::Quit must be handled pre-transaction");
    }
}
