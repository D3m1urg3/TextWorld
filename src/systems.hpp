// Action resolution: turns a parsed Action into world mutations + events.
#pragma once

#include <cstdint>

#include "action.hpp"
#include "db.hpp"

// Resolve one Action for `player`. Runs inside the caller's ambient tick
// transaction, AFTER the caller has already incremented meta.turn — every
// resolved action ticks the world, success or refusal.
//
// All reads are free-form SELECTs; ALL writes go through the mutations
// helpers (appendEvent/moveEntity). Event verbs are the fixed six:
// moved, took, dropped, looked, waited, failed.
//
// Verb::Quit never reaches resolve (the game loop handles it before opening
// the tick transaction); resolve throws std::logic_error if it does.
void resolve(Db& db, const Action& action, int64_t player);
