// Combat system: the enemy-turn tick, damage/status math, and (later) the
// architect's eligible menu. This TU does SELECTs + arithmetic only; ALL writes
// route through the mutations.cpp helpers — the sole sanctioned write path,
// mirroring architect.cpp's discipline. The engine owns every combat number;
// no RNG ever enters here. See .lore/work/specs/combat-and-enemies.md.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

// Basic attack: always available, no cooldown, no element, fixed non-zero
// damage to the hostile in the room — the anti-deadlock floor (REQ-COMBAT-6,
// -18). Engine-owned constant; the model never sets it.
inline constexpr int64_t kBasicAttackDamage = 4;

// The seed safe room (dormitory cell). A downed player wakes here at full health
// (REQ-COMBAT-23). Room 1 by seed convention, in both base.sql and the fixtures.
inline constexpr int64_t kDormitoryCell = 1;

// Damage of a telegraphed enemy strike when it lands (REQ-COMBAT-10). A heavy
// blow — much larger than chip — so reading the wind-up and countering matters.
// Engine-owned constant; per-archetype variation arrives with the Brick 4
// bestiary. Written into the pending_strike row at telegraph time.
inline constexpr int64_t kStrikeDamage = 5;

// Resolve a Verb::Attack for `player`: deal kBasicAttackDamage to the living
// hostile sharing the player's room. No hostile present → a 'failed' event and
// no world write. Runs inside the caller's ambient tick transaction; writes
// only through mutations helpers. Defeat handling is the enemy-turn system's
// job (Step 5), not this function's.
void resolveAttack(Db& db, int64_t player);

// The cooldown/known gate for a Cast (REQ-COMBAT-7, -13). Returns nullopt when
// `spell` is castable NOW by `player` — learned and off cooldown — else a
// player-facing denial reason. Read-only. Called PRE-TICK by the loop so a
// declined cast consumes no turn (the engine, not the model, owns applicability).
std::optional<std::string> castDenialReason(Db& db, int64_t player,
                                            const std::string& spell);

// Resolve a valid Verb::Cast (availability already gated by castDenialReason).
// Sets the spell's cooldown to ready_turn = now + catalog cooldown (an immutable
// constant, never reduced — REQ-COMBAT-14) and applies its effect (Brick 2:
// Ward/Stun — Step 10). Runs in the tick transaction; writes via mutations only.
void resolveCast(Db& db, int64_t player, const std::string& spell);

// The living hostile (health.current > 0) sharing `player`'s room right now, or
// 0 if none. Read-only. The loop captures this at TICK START — before the
// player's action resolves — so the enemy that was present still takes its one
// turn even if the player's action moved them out (flee, Step 11); this is
// micro-decision 2.
int64_t tickStartHostile(Db& db, int64_t player);

// The engine-authored combat status line (REQ-COMBAT-15), appended by BOTH the
// template renderer and the AI deterministic-append path — never left to the
// model. Returns "" outside combat (no living hostile shares the player's room),
// else a single trailing-newline line. Brick 1: "HP: current/max"; per-spell
// cooldown readiness joins in Step 12. Read-only.
std::string combatStatusLine(Db& db, int64_t player);

// The enemy-turn system (REQ-COMBAT-1, -2, -9, -12): the first non-player actor.
// Called from runTurn AFTER resolve(), inside the SAME tick transaction, keyed
// on `hostile` = the foe present at tick start. `hostile == 0`, or a hostile the
// player's action just brought to 0 health, means no combat this tick (no-op;
// defeat removal is Step 5). Otherwise the enemy takes its single turn action
// (Brick 1: idle — the telegraph/strike lane is Step 8) PLUS the always-on chip
// lane: fixed per-instance chip damage to the player, the irreducible HP clock.
// Writes only through mutations helpers.
void resolveCombat(Db& db, int64_t player, int64_t hostile);
