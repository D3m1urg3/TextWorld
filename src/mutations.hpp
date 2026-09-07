// World mutation helpers: the ONLY sanctioned write path for systems code.
//
// Convention (the event-log discipline):
//   - Every world mutation is a component write PLUS an `events` row, both
//     performed inside the caller's ambient transaction — "both or neither"
//     is guaranteed by transactionality, not by these helpers.
//   - These helpers never begin/commit/rollback. The caller owns the
//     transaction boundary (typically one transaction per turn).
//   - `appendEvent` alone (no component write) is legal ONLY for the
//     no-write verbs: 'looked', 'waited', 'failed', 'examined', 'said',
//     'spoke'. Speech is a no-write verb because a conversation line is a
//     thing that HAPPENED, with no component to change (REQ-NPCSTORE-2).
//   - ALL other world mutation goes through these helpers; systems code
//     never runs raw SQL writes against component tables or `events`. The
//     'generated' verb is helper-issued too: it is written ONLY by
//     writeGeneratedRoom, alongside that room's component + exit rows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
// object = amount, detail) — the sole write path for combat damage.
//
// CONTRACT CHANGE, and the next person to add a detail needs to know it:
// `events.detail` now carries TWO kinds of value.
//   1. A human-readable, MODEL-FACING fragment — what it has always been. Every
//      such detail is handed to the narrator as a fact (prose.cpp).
//   2. An engine-internal TAG, on the 'burned', 'froze', and 'materialized'
//      verbs only. On 'burned'/'froze' it has the form "<archetype>|<element>"
//      and is what resistance discovery derives from (REQ-UI-46); on
//      'materialized' it is the catalog HANDLE (a machine token like
//      `scorched_lectern`). prose.cpp deliberately WITHHOLDS all three from the
//      narrator — a raw archetype tag or handle in front of the model would
//      otherwise be echoed into prose as a noun (REQ-PROSE-11, REQ-UI-25).
// Adding a fourth tagged verb means updating that shield too.
//
// Throws std::runtime_error (engine error) if `target` has no health row —
// before any event is appended — so the log never records damage that did not
// land. The caller is expected to roll back. Never begins/commits.
void damageEntity(Db& db, int64_t target, int64_t amount, int64_t actor,
                  const char* verb, const char* detail = nullptr);

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

// --- The bard's fact store (specs/bard-fact-store.md) ------------------------

// Mint one catalog entry (the overture's bulk write, and the micro wake's
// append path). INSERT-only: there is deliberately NO helper that edits an
// existing entry's kind/handle/name/blurb/motive/tier — appending a corrected
// entry is the only way to change the bard's mind, so drift stays VISIBLE in
// the table instead of being absorbed into it (REQ-BARD-STORE-17).
//
// Throws std::runtime_error on an invalid argument (REQ-BARD-STORE-9): a `kind`
// other than 'character'/'beat', a `motive` with no motive_catalog row (the menu
// is closed, so the bard cannot invent a ninth), an empty-after-trim `handle`/
// `name`/`blurb`, or a negative `tier`. Every check runs BEFORE any write, so a
// refusal leaves the table untouched. These are engine faults — the caller
// offers only valid values — and the caller rolls back.
//
// THE TRUTH GATE (REQ-BARD-STORE-10). `factArchetype`/`factElement` are both
// empty (an ordinary entry) or both set (a knowledge beat); one alone throws.
// When set, the entry is REFUSED unless the archetype exists in `bestiary`, the
// element appears in `spell_catalog.element`, and a `resistance` row exists for
// the pair. A catalog entry may not promise a falsehood about the rules — the
// same discipline as the narrator's canon-verbatim gate, applied to
// foreshadowing. Trimmed strings are stored; empty fact fields store SQL NULL,
// so "both NULL or both non-NULL" is literally true in the data.
//
// Event-free — a latent entry has not happened (the dropGrimoire/placeEnemy
// precedent). It becomes an event when it materializes. Returns the minted id.
// Never begins/commits.
int64_t writeCatalogEntry(Db& db, const std::string& kind,
                          const std::string& handle, const std::string& name,
                          const std::string& blurb, const std::string& motive,
                          int64_t tier,
                          const std::string& factArchetype = "",
                          const std::string& factElement = "");

// THE L0 -> L2 transition, and the sole writer of the 'materialized' verb
// (REQ-BARD-STORE-12). Latches catalog.entity (WHERE entity IS NULL, so it
// fires at most once) AND appends the event — actor, subject = `entity`,
// object = `catalog`, detail = the entry's handle — both in the caller's
// ambient transaction. One call, one fact: the L0→L2 transition and the bard's
// wake trigger cannot drift apart, the same shape as writeGeneratedRoom writing
// its rows and its 'generated' event together.
//
// The latch is the WHERE clause, never a prior read (REQ-BARD-STORE-13): a
// second call — or a call for a catalog id that does not exist — changes
// nothing, appends nothing, and returns false. Never begins/commits.
bool materializeCatalogEntry(Db& db, int64_t catalog, int64_t entity,
                             int64_t actor);

// Cast one world entity from a latent catalog entry (REQ-BARD-STORE-14), the
// way placeEnemy casts an instance from its bestiary archetype. Mints one
// entity and writes its `name` (from catalog.name — the parser noun, not the
// handle), `description` (from the `description` ARGUMENT, never the blurb: the
// blurb is selection prose the model has already seen), and `location` (=
// `room`) rows, then calls materializeCatalogEntry to latch and log.
//
// Returns the minted entity id, or 0 if the entry was already materialized (or
// does not exist) — in which case NO entity is minted. That is why the latch is
// pre-checked before the mint rather than after. Never begins/commits.
int64_t placeCatalogEntry(Db& db, int64_t catalog, int64_t room,
                          const std::string& description, int64_t actor);

// Latch `seeded` (WHERE seeded = 0): this entry has now been hinted in prose,
// and is therefore costlier to walk back than one the player never heard of
// (REQ-BARD-STORE-15). Set once, never cleared. Event-free bookkeeping; a
// second call is a silent no-op by construction, the learnSpell idempotence
// shape. Never begins/commits.
void markCatalogSeeded(Db& db, int64_t catalog);

// The cap on meta.bard_focus, in CODE POINTS (REQ-BARD-STORE-16a). "Tunable"
// means this one line: nothing reads it from the environment. The cap matters
// because this string is paid for on EVERY room generation — the architect
// reads it via buildArchitectContext — and an unbounded focus would quietly
// become the largest term in that context.
inline constexpr size_t kBardFocusMaxChars = 300;

// Upsert meta.bard_journal — the bard's PRIVATE working memory, read by nothing
// else, so free rewrite is safe and the text is stored verbatim and uncapped
// (REQ-BARD-STORE-16). A second call REPLACES the stored value; it never
// appends. Event-free. Never begins/commits.
void writeBardJournal(Db& db, const std::string& text);

// Upsert meta.bard_focus — the SHORT public line the architect reads. Free
// rewrite like the journal (REQ-BARD-STORE-16), but NORMALIZED first
// (REQ-BARD-STORE-16a): each run of newlines/carriage returns collapses to a
// single space, then the result is truncated to kBardFocusMaxChars code points.
// This is what makes "one short line" enforced rather than merely described — a
// length cap alone would admit a multi-line focus that reads as prose in the
// architect's context. Event-free. Never begins/commits.
void writeBardFocus(Db& db, const std::string& text);

// Upsert meta.bard_last_wake_turn. Stamped when a wake is QUEUED, never when
// it completes (REQ-BARD-WAKE-11), so an in-flight wake cannot re-trigger
// itself: the trigger query reads events strictly after this value, and a wake
// that took several turns to answer therefore re-finds only what arrived
// after it was sent. Free rewrite, like the journal — the stamp is a position,
// not a log. Event-free. Never begins, commits, or rolls back — the caller
// owns the transaction, exactly like every other helper in this file.
//
// REQ-BARD-STORE-18: this file is the ONLY unit that may write this row.
void writeBardWakeTurn(Db& db, int64_t turn);

// --- The story arc store (specs/story-arc-store.md) --------------------------

// Upsert the three arc rows — meta.arc_premise, meta.arc_goal, meta.arc_ending
// (REQ-ARC-STORE-2, -9). Rows, not a shape: zero DDL, the meta.setting
// precedent. They are three rows rather than one blob so a consumer can be sent
// the premise without the ending leaking into it.
//
// Free rewrite, like writeBardJournal: a second call REPLACES all three values
// and never appends. Upsert rather than UPDATE so a world whose seed never wrote
// the rows still gets them. Event-free — an arc is not something that happened.
// Never begins/commits.
void writeArc(Db& db, const std::string& premise, const std::string& goal,
              const std::string& ending);

// Insert one story step (REQ-ARC-STORE-10), VALIDATED AT ADMISSION. This is
// writeCatalogEntry's motive gate applied to conditions: a step may not promise
// a condition the engine cannot check.
//
// Throws std::runtime_error on any of: `kind` absent from condition_catalog;
// that kind's arg_kind is 'int' and `arg` is not a non-negative decimal integer;
// its arg_kind is 'spell' and `arg` has no spell_catalog row; `prose` empty
// after trimming; `n` already present. EVERY check runs before ANY write, so a
// refusal leaves story_step byte-identical — the writeCatalogEntry discipline.
// All of these are engine faults, so the caller rolls back.
//
// The trimmed prose is stored, and `reached_turn` is left NULL. Event-free — a
// step not yet reached has not happened (the writeCatalogEntry / dropGrimoire
// precedent). Never begins/commits.
void writeStoryStep(Db& db, int64_t n, const std::string& kind,
                    const std::string& arg, const std::string& prose);

// Latch the LOWEST unreached story step and append its event, both in the
// caller's ambient transaction (REQ-ARC-STORE-11). Returns true IFF it latched
// exactly one row. This is the SOLE writer of the 'advanced' verb.
//
// The latch is the WHERE clause, never a prior read — the
// materializeCatalogEntry discipline. When every step is already reached, or
// the table is empty, it matches no row, appends NOTHING, and returns false
// (REQ-ARC-STORE-19, -19a).
//
// The event it appends describes THE ROW IT JUST LATCHED (REQ-ARC-STORE-11a),
// which is why the UPDATE carries a RETURNING clause rather than being followed
// by a second SELECT: a query that CHOOSES the row before the UPDATE is
// forbidden, because two callers in one transaction could then latch and
// describe different steps.
//
// The event (REQ-ARC-STORE-20): actor, verb 'advanced', subject = 0 (there is
// no entity — the zero-id rule of REQ-PROSE-6), object = the step's `n`,
// detail = the step's prose. Unlike 'materialized', that detail is a
// MODEL-FACING fragment, not an engine tag, so it needs no shield in prose.cpp
// — the verb is withheld from the narrator wholesale instead
// (REQ-ARC-STORE-21). Never begins/commits.
bool advanceStoryStep(Db& db, int64_t actor);

// --- The NPC memory store (specs/npc-memory-store.md) ------------------------
//
// Two stores with OPPOSITE rules. A character's profile is written once and
// never edited, so identity cannot drift. Its memory is rewritten freely and
// capped, because memory is a reconstruction and a character misremembering
// costs nothing mechanical.
//
// The three `npc*` reads below are this file's FIRST read helpers. They are
// SELECT-only, deterministic functions of the database (REQ-NPCSTORE-22), and
// they live here rather than in a new translation unit because the spec's
// `modules:` line names mutations/world/bard/main and no `npc` unit. The
// conversation brick may hoist them; nothing depends on their staying put.
// They do not weaken the file's contract: nothing above this line reads, and
// nothing below this line writes.

// The cap on catalog_profile.profile, in CODE POINTS (REQ-NPCSTORE-17).
// Generous for a hand-written character (~700 words is well under) and a hard
// stop on a model-written one that runs away. This string is re-sent IN FULL on
// every conversation call, which is what the ceiling is protecting.
inline constexpr size_t kProfileCap = 4000;

// The cap on npc_memory.summary, in CODE POINTS (REQ-NPCSTORE-17). Short enough
// that it cannot hold a personality essay. A BRAKE, NOT A GUARANTEE
// (REQ-NPCSTORE-18): the rule that matters — the summary carries facts, never
// voice — is a prompt rule and cannot be enforced on free text. The cap stops
// erosion compounding; it does not prevent it. Do not read it as enforcement.
inline constexpr size_t kSummaryCap = 800;

// The cap on rows returned by one raw-line read (REQ-NPCSTORE-17). The bounded
// read is what stops the recurring conversation prompt growing across a
// session — see the design's decision 4 and the persona-drift finding behind it.
inline constexpr int64_t kLineCap = 40;

// Write a character's authored profile. WRITE-ONCE: guarded in SQL, so a second
// call for the same catalog entry changes nothing and returns false — the
// learnSpell / materializeCatalogEntry idempotence shape. There is deliberately
// NO helper that edits an existing profile: a character's identity cannot drift
// because no code path exists to drift it (REQ-NPCSTORE-12), and the suite
// asserts that against the SOURCE TEXT rather than trusting it
// (REQ-NPCSTORE-36).
//
// A profile for a catalog id that does not exist is refused and returns false
// (REQ-NPCSTORE-14) — the EXISTS clause is in the same statement as the latch,
// so neither guard is a prior read.
//
// Truncated to kProfileCap code points. Event-free — an authored profile has not
// happened; the character's arrival in the world is what produces an event.
// Never begins/commits.
bool writeCatalogProfile(Db& db, int64_t catalog, const std::string& profile);

// Upsert a character's memory summary and stamp summary_turn with the turn
// BEFORE the current one, both inside the caller's ambient transaction. Free
// rewrite: memory is a reconstruction, and a character misremembering costs
// nothing mechanical. No latch, no append semantics — a second call REPLACES
// the summary outright.
//
// The stamp is turn - 1, not the current turn (REQ-NPCTALK-29a), and that is
// load-bearing: a summary is composed by the model from the lines it was
// handed, in the same call that produces this turn's reply, so it can never
// cover this turn's own exchange. Stamping the current turn would hide that
// exchange from every future npcLinesSince read, which filters on
// turn > summary_turn — an off-by-one that silently loses a conversation
// instead of failing. Clamped at 0 so a turn-0 write cannot stamp -1. Still
// read here rather than passed in (REQ-NPCSTORE-8): no caller chooses the stamp.
//
// The row is not pre-created at materialisation (REQ-NPCSTORE-9): this upserts,
// so there is no row to branch on. Truncated to kSummaryCap code points. See
// the design's decision 4: that cap is a brake on personality erosion, not a
// proof against it. Event-free. Never begins/commits.
void writeNpcMemory(Db& db, int64_t entity, const std::string& summary);

// A character's memory summary and the turn it covers to.
struct NpcMemory {
    std::string summary;
    int64_t summaryTurn = 0;
};

// One speech event, as it was appended.
struct SpeechLine {
    std::string verb;    // "said" or "spoke"
    std::string detail;  // the line, byte-exact as appended
};

// The profile the character `entity` is, via its catalog row. Empty if the
// entity is not a catalog character or has no profile yet — which is the
// NORMAL, COMMON state of a minor character before its first conversation
// (REQ-NPCSTORE-19). Never an error, never a throw, never a log line.
std::string npcProfile(Db& db, int64_t entity);

// The character's memory summary and the turn it covers to. An entity with no
// npc_memory row yields {"", 0}, which is not an error (REQ-NPCSTORE-20).
NpcMemory npcMemory(Db& db, int64_t entity);

// The speech events this character took part in since its summary was written,
// OLDEST FIRST, capped at kLineCap (REQ-NPCSTORE-21). When more qualify, the
// MOST RECENT kLineCap are returned — a character forgets the middle of a long
// conversation, never the end of it.
//
// The cap never hands back a reply without its question (REQ-NPCSTORE-21a): a
// leading `spoke` whose paired `said` the cap CUT is dropped, yielding
// kLineCap - 1 rows. A leading `spoke` whose `said` merely predates
// summary_turn is legitimate and is kept — the two cases are told apart by
// probing one row past the cap, not guessed at from the row count. A TRAILING
// `said` with no `spoke` is kept untouched: that is what a failed reply looks
// like, and hiding it would make the character unaware it was spoken to.
//
// `detail` is returned verbatim — no trimming, no normalisation, no shielding.
std::vector<SpeechLine> npcLinesSince(Db& db, int64_t entity);
