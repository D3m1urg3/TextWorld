// World mutation helpers: the ONLY sanctioned write path for systems code.
//
// Convention (the event-log discipline):
//   - Every world mutation is a component write PLUS an `events` row, both
//     performed inside the caller's ambient transaction — "both or neither"
//     is guaranteed by transactionality, not by these helpers.
//   - These helpers never begin/commit/rollback. The caller owns the
//     transaction boundary (typically one transaction per turn).
//   - `appendEvent` alone (no component write) is legal ONLY for the
//     no-write verbs: 'looked', 'waited', 'failed'.
//   - ALL other world mutation goes through these helpers; systems code
//     never runs raw SQL writes against component tables or `events`. The
//     'generated' verb is helper-issued too: it is written ONLY by
//     writeGeneratedRoom, alongside that room's component + exit rows.
#pragma once

#include <cstdint>
#include <string>

#include "db.hpp"

// A model-proposed room (name + description). Defined in architect.hpp;
// forward-declared here so the mutation helper can name it without pulling the
// architect/prose headers into every includer of mutations.hpp.
struct RoomProposal;

// Append one row to the `events` log, stamped with the CURRENT turn number
// (read from meta.turn inside the caller's ambient transaction).
// `detail` may be nullptr, which stores SQL NULL.
void appendEvent(Db& db, int64_t actor, const char* verb, int64_t subj,
                 int64_t obj, const char* detail);

// Move entity `what` into container `toContainer` (UPDATE location.container)
// AND append the matching event row (subject = what, object = toContainer,
// detail = NULL). One call = component write + event row, inside the
// caller's ambient transaction.
//
// Throws std::runtime_error (engine error) if `what` has no location row —
// before any event is appended — so the log never records a mutation that
// did not happen. The caller is expected to roll back.
void moveEntity(Db& db, int64_t what, int64_t toContainer, int64_t actor,
                const char* verb);

// Apply `amount` of damage to `target`'s health AND append the paired combat
// event, inside the caller's ambient transaction. The health write clamps
// current to [0, max] in code: current := clamp(current - amount, 0, max)
// (REQ-COMBAT-4). The event is (actor = attacker, verb, subject = target,
// object = amount, detail = NULL) — the sole write path for combat damage.
//
// Throws std::runtime_error (engine error) if `target` has no health row —
// before any event is appended — so the log never records damage that did not
// land. The caller is expected to roll back. Never begins/commits.
void damageEntity(Db& db, int64_t target, int64_t amount, int64_t actor,
                  const char* verb);

// The SOLE sanctioned write path for a generated room (REQ-ARCH-9), inside the
// caller's ambient transaction. The model proposes flavor; the engine disposes:
// this helper MINTS one entity (the first runtime entity mint), writes its
// `room` tag, `name` (= proposal.name), and `description` (canon =
// proposal.description) rows — NO location row (rooms have no container) — then
// writes the exit `(originRoom, direction) → new` and the reciprocal
// `(new, inverse(direction)) → originRoom`, and appends one `generated` event
// (actor = player, subject = new room, object = originRoom, detail = direction —
// deliberately unlike moveEntity's subject/object reading). Ids are engine-
// minted; the proposal carries none (REQ-ARCH-6). `direction` must be invertible
// (REQ-ARCH-8) — the caller guarantees it; a non-invertible direction here is an
// engine fault (throws, caller rolls back). Returns the minted room id.
int64_t writeGeneratedRoom(Db& db, int64_t originRoom,
                           const std::string& direction,
                           const RoomProposal& proposal, int64_t actor);
