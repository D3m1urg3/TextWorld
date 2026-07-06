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
//     never runs raw SQL writes against component tables or `events`.
#pragma once

#include <cstdint>

#include "db.hpp"

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
