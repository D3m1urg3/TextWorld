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

// --- The story arc's advance rule (specs/story-arc-store.md) -----------------

// Does the condition (`kind`, `arg`) hold right now (REQ-ARC-STORE-12)? A PURE
// READ: no writes, no mutation, one query per kind. The four kinds and their
// semantics are REQ-ARC-STORE-5:
//   enemies_defeated (int)   — count of 'defeated' events  >= arg
//   rooms_built      (int)   — count of 'generated' events >= arg
//   spell_learned    (spell) — the player has a known_spells row for arg
//   reached_depth    (int)   — distanceFromSeed of the player's room >= arg
//
// An unrecognized `kind` THROWS std::runtime_error (REQ-ARC-STORE-13), the way
// moveEntity throws when an entity has no location row. It cannot occur through
// a sanctioned path — writeStoryStep refuses it at admission, and a world file
// from an earlier vocabulary is refused at open — so reaching it is an engine
// bug, and a silent `false` would hide one.
//
// NO condition reads meta.turn or any wall clock (REQ-ARC-STORE-14). That is
// what keeps the story spatial rather than a timer, and it is checked
// mechanically by the suite against this function's source text.
bool stepConditionMet(Db& db, const std::string& kind, const std::string& arg,
                      int64_t player);

// The story arc's advance rule, called ONCE PER TURN from the tick's
// transaction (REQ-ARC-STORE-15, -15a). It reads the LOWEST unreached step,
// evaluates that step's condition, and latches it through advanceStoryStep when
// the condition holds.
//
// `ORDER BY n LIMIT 1` is the requirement, not an optimization. Steps are an
// ordered list, not a set of independent triggers, so step 3 cannot fire before
// step 2 even when its condition holds (REQ-ARC-STORE-16) — and AT MOST ONE
// condition is evaluated per turn (REQ-ARC-STORE-16a), because reached_depth
// runs a breadth-first search and scanning the unreached steps would put that
// search on every turn of a growing world for a result REQ-ARC-STORE-16 would
// then discard. THIS FUNCTION MUST NOT LOOP.
//
// At most one step advances per turn (REQ-ARC-STORE-17): when a step advances
// and the next one's condition is already true, the next advance waits for a
// later turn.
//
// The "no unreached step" branch covers two distinct states with the same
// behavior — every step reached (REQ-ARC-STORE-19) and an EMPTY table
// (REQ-ARC-STORE-19a, the state a failed overture leaves behind). Neither
// writes anything and neither throws.
//
// It does read the lowest unreached step while advanceStoryStep re-derives
// MIN(n) in its own WHERE. That is not a duplicated read to collapse:
// REQ-ARC-STORE-11a is about advanceStoryStep being self-contained, so the row
// it latches and the row it describes are the same row whatever the caller
// thought.
void evaluateStoryAdvance(Db& db, int64_t actor);
