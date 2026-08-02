#include "systems.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include "architect.hpp"  // architectGenerate + aiNarrationEnabled (via prose.hpp)
#include "combat.hpp"      // resolveAttack (the combat verb resolution)
#include "mutations.hpp"
#include "pregen.hpp"      // the candidate store — the tick's one call into it
#include "profile.hpp"     // PregenRecord — the per-walk outcome

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

// The three exhaustive exit states (REQ-EXITS-1): no row is a Wall, a NULL-dest
// row is Latent (open but ungenerated), a non-NULL-dest row is Realized.
enum class ExitState { Wall, Latent, Realized };

// Three-state read of the exit (room, direction). The null bit is carried in
// the SQL itself so a NULL dest is distinguishable from a missing row without
// touching the frozen db.hpp Stmt API (micro-decision #1). `dest` is set only
// for the Realized case.
ExitState exitState(Db& db, int64_t room, const std::string& direction,
                    int64_t& dest) {
    Stmt s = db.prepare(
        "SELECT dest, dest IS NULL FROM exits WHERE room = ? AND direction = ?");
    s.bind(1, room);
    s.bind(2, direction);
    if (!s.step()) return ExitState::Wall;
    if (s.colInt(1) == 1) return ExitState::Latent;
    dest = s.colInt(0);
    return ExitState::Realized;
}

bool isPortable(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT 1 FROM portable WHERE entity = ?");
    s.bind(1, entity);
    return s.step();
}

// --- per-verb resolution --------------------------------------------------

// The three-case movement table (REQ-EXITS-2), evaluated IN ORDER. `transport`
// is null in production (architectGenerate binds libcurl itself) and non-null
// only under the test-injected resolve overload.
void resolveGo(Db& db, const Action& action, int64_t player,
               const HttpTransport* transport) {
    const int64_t room = roomOf(db, player);

    int64_t dest = 0;
    const ExitState state = exitState(db, room, action.direction, dest);

    // (a) Realized → move. Precedes any AI call, so re-crossing a generated
    // exit never regenerates — persistence falls out of the realized row.
    if (state == ExitState::Realized) {
        moveEntity(db, player, dest, player, "moved");
        return;
    }

    // Flee guard (REQ-COMBAT-26): a realized exit (case a) lets the player flee —
    // the enemy's single parting turn falls out of resolveCombat keying on the
    // tick-start hostile (micro-decision 2), no special code here. But a LATENT
    // exit must NOT generate a room mid-combat: with a hostile present, refuse
    // before any architect call, so the world never grows while an enemy is at
    // the player's back. Room-bound enemies (REQ-COMBAT-27) never follow, so this
    // only blocks fleeing INTO the unknown.
    if (state == ExitState::Latent && hostileInRoom(db, room) != 0) {
        appendEvent(db, player, "failed", 0, 0,
                    "You can't flee into the unknown with an enemy at your back.");
        return;
    }

    // (b) Latent AND generation enabled → realize it, then move through the
    // now-realized exit. Every latent row is invertible by construction
    // (REQ-EXITS-1), so no invertibility guard is needed here. On Phase-1
    // failure the latent row is untouched — the wall below is retryable next
    // turn (REQ-EXITS-3).
    if (state == ExitState::Latent && aiNarrationEnabled()) {
        // Pre-generation, if it has anything for this exit (REQ-PREGEN-14/-15).
        // An empty candidate store makes everything below byte-for-byte what it
        // was: pregenAcquire returns Miss, and the else branch is the original
        // code unchanged. That is what lets every existing test here pass
        // untouched, and it is the regression proof for "a miss is today".
        const PregenResult pre =
            pregenAcquire(room, action.direction, transport);

        bool generated = false;
        if (pre.proposal) {
            // A candidate arrived — by hit, by wait, or by running the queued
            // job here. NO network on this turn's commit path: only Phase 2,
            // and the SAME Phase 2 the synchronous path runs, so the canon is
            // indistinguishable from a room written the old way (REQ-PREGEN-14).
            // The enemy re-check inside it is against the LIVE menu, so a stale
            // blurb spawns nothing and the room is still made (REQ-PREGEN-18).
            architectCommitProposal(db, room, action.direction, *pre.proposal,
                                    player);
            generated = true;
        } else {
            // Miss, or a job that failed: today's synchronous path, verbatim.
            generated =
                transport != nullptr
                    ? architectGenerate(db, room, action.direction, player, *transport)
                    : architectGenerate(db, room, action.direction, player);
        }

        // One outcome record per latent-exit walk (REQ-PREGEN-23), whether or
        // not a room resulted — a `miss` that then failed to generate is still
        // a miss, and a hit rate computed from successes alone would flatter
        // the feature.
        if (profilingEnabled()) {
            PregenRecord record;
            record.turn = profileCurrentTurn();
            record.outcome = pregenOutcomeName(pre.outcome);
            switch (pre.outcome) {
                case PregenOutcome::Hit:
                    // Read ONLY here — profiling on, and only on a hit — so an
                    // ordinary run never pays for the extra row.
                    record.ageTurns = architectWorldTurn(db) - pre.snapshotTurn;
                    break;
                case PregenOutcome::Waited:
                    record.waitMs = pre.waitMs;
                    break;
                case PregenOutcome::RanQueued:
                    record.runMs = pre.runMs;
                    break;
                case PregenOutcome::Miss:
                    break;  // a miss carries no optional key
            }
            profileEmit(record);
        }

        if (generated) {
            int64_t realizedDest = 0;
            exitState(db, room, action.direction, realizedDest);
            moveEntity(db, player, realizedDest, player, "moved");
            return;
        }
    }

    // (c) Otherwise — Wall, or Latent with generation disabled, or a failed
    // realization → the wall, byte-identical text.
    appendEvent(db, player, "failed", 0, 0, "You can't go that way.");
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

// Shared dispatch. `transport` is null in production (Go uses the curl-bound
// architectGenerate) and non-null under the test-injected overload.
void resolveImpl(Db& db, const Action& action, int64_t player,
                 const HttpTransport* transport) {
    switch (action.verb) {
        case Verb::Go:
            resolveGo(db, action, player, transport);
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
        case Verb::Attack:
            resolveAttack(db, player);
            break;
        case Verb::Cast:
            // Availability (known + off cooldown) was gated pre-tick by the loop
            // (REQ-COMBAT-7/-13); reaching here means the cast is valid.
            resolveCast(db, player, action.spell);
            break;
        case Verb::Read:
            resolveRead(db, player, action.subject);
            break;
        case Verb::Quit:
            // Quit is handled by the game loop BEFORE the tick transaction is
            // opened — it must never reach resolve. Throwing (rather than
            // silently ignoring) surfaces the loop bug immediately; the caller
            // rolls back the transaction, so no tick is recorded.
            throw std::logic_error("resolve: Verb::Quit must be handled pre-transaction");
    }
}

}  // namespace

void resolve(Db& db, const Action& action, int64_t player) {
    resolveImpl(db, action, player, /*transport=*/nullptr);
}

void resolve(Db& db, const Action& action, int64_t player,
             const HttpTransport& transport) {
    resolveImpl(db, action, player, &transport);
}
