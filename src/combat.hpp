// Combat system: the enemy-turn tick, damage/status math, and (later) the
// architect's eligible menu. This TU does SELECTs + arithmetic only; ALL writes
// route through the mutations.cpp helpers — the sole sanctioned write path,
// mirroring architect.cpp's discipline. The engine owns every combat number;
// no RNG ever enters here. See .lore/work/specs/combat-and-enemies.md.
#pragma once

#include <cstdint>

#include "db.hpp"

// Basic attack: always available, no cooldown, no element, fixed non-zero
// damage to the hostile in the room — the anti-deadlock floor (REQ-COMBAT-6,
// -18). Engine-owned constant; the model never sets it.
inline constexpr int64_t kBasicAttackDamage = 4;

// The seed safe room (dormitory cell). A downed player wakes here at full health
// (REQ-COMBAT-23). Room 1 by seed convention, in both base.sql and the fixtures.
inline constexpr int64_t kDormitoryCell = 1;

// Resolve a Verb::Attack for `player`: deal kBasicAttackDamage to the living
// hostile sharing the player's room. No hostile present → a 'failed' event and
// no world write. Runs inside the caller's ambient tick transaction; writes
// only through mutations helpers. Defeat handling is the enemy-turn system's
// job (Step 5), not this function's.
void resolveAttack(Db& db, int64_t player);

// The living hostile (health.current > 0) sharing `player`'s room right now, or
// 0 if none. Read-only. The loop captures this at TICK START — before the
// player's action resolves — so the enemy that was present still takes its one
// turn even if the player's action moved them out (flee, Step 11); this is
// micro-decision 2.
int64_t tickStartHostile(Db& db, int64_t player);

// The enemy-turn system (REQ-COMBAT-1, -2, -9, -12): the first non-player actor.
// Called from runTurn AFTER resolve(), inside the SAME tick transaction, keyed
// on `hostile` = the foe present at tick start. `hostile == 0`, or a hostile the
// player's action just brought to 0 health, means no combat this tick (no-op;
// defeat removal is Step 5). Otherwise the enemy takes its single turn action
// (Brick 1: idle — the telegraph/strike lane is Step 8) PLUS the always-on chip
// lane: fixed per-instance chip damage to the player, the irreducible HP clock.
// Writes only through mutations helpers.
void resolveCombat(Db& db, int64_t player, int64_t hostile);
