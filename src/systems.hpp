// Action resolution: turns a parsed Action into world mutations + events.
#pragma once

#include <cstdint>

#include "action.hpp"
#include "db.hpp"
#include "prose.hpp"  // HttpTransport seam (threaded to the architect on Go)

// Resolve one Action for `player`. Runs inside the caller's ambient tick
// transaction, AFTER the caller has already incremented meta.turn — every
// resolved action ticks the world, success or refusal.
//
// All reads are free-form SELECTs; ALL writes go through the mutations
// helpers (appendEvent/moveEntity/writeGeneratedRoom). Event verbs: moved,
// took, dropped, looked, waited, failed — plus 'generated' when a Go across an
// UNMAPPED, invertible exit triggers world-gen (REQ-ARCH-3b).
//
// Verb::Quit never reaches resolve (the game loop handles it before opening
// the tick transaction); resolve throws std::logic_error if it does.
void resolve(Db& db, const Action& action, int64_t player);

// Test-visible overload: same contract, but the architect's HTTP transport is
// injected (used only on the Go world-gen branch). The production overload above
// binds libcurl. Lets the resolveGo world-gen seam be driven through the tick
// with a fake transport and NO network.
void resolve(Db& db, const Action& action, int64_t player,
             const HttpTransport& transport);
