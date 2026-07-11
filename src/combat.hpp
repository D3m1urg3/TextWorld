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

// Resolve a Verb::Attack for `player`: deal kBasicAttackDamage to the living
// hostile sharing the player's room. No hostile present → a 'failed' event and
// no world write. Runs inside the caller's ambient tick transaction; writes
// only through mutations helpers. Defeat handling is the enemy-turn system's
// job (Step 5), not this function's.
void resolveAttack(Db& db, int64_t player);
