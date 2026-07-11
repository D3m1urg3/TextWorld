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

// Mint a portable grimoire item into `room`, per a fixed archetype → grimoire
// flavor map (REQ-COMBAT-20; the archetype → SPELL mapping and the grimoire→spell
// component are Step 18 — here only the ITEM appears). Mints one entity and
// writes its portable/name/description/location rows. Emits NO event of its own:
// its appearance is recorded by the paired 'defeated' event that references it
// (mirroring writeGeneratedRoom's mint-under-one-event precedent). Returns the
// minted grimoire's entity id. Never begins/commits.
int64_t dropGrimoire(Db& db, const std::string& archetype, int64_t room);

// Apply (upsert) a status effect on `entity` (REQ-COMBAT-19): a DoT or CC of
// `magnitude`, lasting `remaining` ticks. Event-free bookkeeping — the caller
// emits the player-facing event (the 'cast'/'stunned'/… event). Never begins/commits.
void applyStatus(Db& db, int64_t entity, const char* kind, int64_t magnitude,
                 int64_t remaining);

// Remove one status-effect `kind` from `entity` (delete the row). Event-free.
void clearStatus(Db& db, int64_t entity, const char* kind);

// Tick down every status effect on `entity` by one (decrement remaining; delete
// rows that reach 0). Event-free countdown of CC/ward duration. DoT damage-on-
// tick is applied separately by the combat system. Never begins/commits.
void tickStatusEffects(Db& db, int64_t entity);

// Strip `entity`'s defense-lock barrier (delete the row, if any) so damage can
// land (REQ-COMBAT-17). Event-free — the paired 'dispelled' event records it.
void removeBarrier(Db& db, int64_t entity);

// Set `spell`'s cooldown for `entity` to become ready at tick `readyTurn`
// (REQ-COMBAT-13): upsert the cooldowns row. Event-free bookkeeping — the paired
// 'cast' event records the cast that set it. Never begins/commits.
void setCooldown(Db& db, int64_t entity, const std::string& spell,
                 int64_t readyTurn);

// Record a telegraphed strike on `enemy` (REQ-COMBAT-10): upsert its
// pending_strike row (damage + element, `element` = nullptr for none) and append
// one 'telegraph' event (actor = enemy). The row's existence means a strike is
// pending; it lands on the enemy's next turn unless countered. Never begins/commits.
void setPendingStrike(Db& db, int64_t enemy, int64_t damage, const char* element);

// Clear `enemy`'s pending strike (delete the row). Event-free bookkeeping — the
// visible consequence is recorded by the 'struck' event (when it lands) or the
// 'defeated'/'downed' event (fight-reset). Never begins/commits.
void clearPendingStrike(Db& db, int64_t enemy);

// Remove a defeated enemy from play (REQ-COMBAT-20, -30): delete its hostile,
// health, and location rows — the entity id and its name/description SURVIVE, so
// "defeated" is the persistent absence of hostile/location, not deletion from
// `entities`. Appends one 'defeated' event (actor, subject = enemy, object =
// droppedItem — the grimoire dropGrimoire just minted, so narration can name
// it). Never begins/commits.
void defeatEnemy(Db& db, int64_t enemy, int64_t droppedItem, int64_t actor);

// Teach `player` `spell` by adding it to known_spells (REQ-COMBAT-21): canon and
// permanent, idempotent (already-known is a no-op). Event-free — the paired
// 'learned'/'reread' event records the read. Never begins/commits.
void learnSpell(Db& db, int64_t player, const std::string& spell);

// The "downed, not dead" model (REQ-COMBAT-23, -24, -25), inside the caller's
// transaction. In order: drop every portable the player carries at the fall room
// (each a 'dropped' event); relocate the player to `safeRoom`; restore the
// player's health to max; restore `enemy`'s health to max (the fight resets).
// `known_spells` is never touched (knowledge is permanent). Appends one 'downed'
// event (actor, subject = player, object = safeRoom). Never begins/commits.
void downPlayer(Db& db, int64_t player, int64_t enemy, int64_t safeRoom,
                int64_t actor);

// Cast one enemy INSTANCE from its bestiary archetype (REQ-COMBAT-29, -30), into
// `room`, inside the caller's ambient transaction. Reads the frozen catalog row —
// the engine owns every number, so the instance's stats are byte-copied from the
// mold; the model (via the architect) only ever SELECTS which archetype, never a
// stat. Mints one entity and writes its hostile/health/name rows, a description
// row = the archetype blurb, a barrier row iff the archetype is a defense lock,
// and a location row in `room`. Emits NO event of its own (like dropGrimoire): a
// seed placement has none, and an architect placement is recorded by the room's
// 'generated' event. Throws std::runtime_error if `archetype` has no bestiary row
// (an engine fault — the caller offers only catalog names). Returns the minted
// instance's entity id. Never begins/commits.
int64_t placeEnemy(Db& db, const std::string& archetype, int64_t room);

// Record that the ARCHITECT placed an enemy — increment the bootstrap ledger
// (REQ-COMBAT-33), upserting meta.architect_spawn_count (absent → 1, else +1).
// Called by architectGenerate right after a placeEnemy on the generated room, so
// the "first-ever architect spawn" rule fires exactly once. NOT called by the
// seed's hand-placed enemy (which uses SQL, never this path), so the seed goblin
// never counts against the ledger. Event-free bookkeeping (like meta.turn); the
// enemy's appearance is recorded by the room's 'generated' event. Never begins/commits.
void recordArchitectSpawn(Db& db);

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
