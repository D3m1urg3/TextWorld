// The bard: the world's live author. It writes the CAST and the BEATS the
// architect later places — the story-side counterpart of the bestiary, minted
// at runtime instead of seeded. This brick is what the model SEES (eligibility
// + context payloads) and what the engine ACCEPTS BACK (request schemas +
// validation gates + admission).
//
// READ-ONLY BY CONTRACT, and with no write counterpart of its own. This
// translation unit performs ONLY SELECTs and (in Brick 3) network egress — NO
// INSERT, UPDATE, or DELETE may ever appear in bard.cpp (grep -En
// "INSERT|UPDATE|DELETE" src/bard.cpp must be empty, REQ-BARD-SEL-21). Every
// write goes through the mutations.cpp helpers from the fact store
// (writeCatalogEntry / writeBardJournal / writeBardFocus / markCatalogSeeded):
// the model PROPOSES entries, the engine DISPOSES.
//
// Catalog ids, entity ids, `tier` values, and the `seeded` flag are NEVER sent
// to or read from the model (REQ-BARD-SEL-8). The model-facing fields are
// `handle`, `blurb`, `name`, and motive blurbs — nothing else.
//
// See .lore/work/specs/bard-catalog-selection.md.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "db.hpp"
#include "prose.hpp"  // HttpResponse — the same transport seam every AI unit uses

// One offerable catalog entry, as the model sees it. Carries NO id, tier, name,
// seeded flag, or raw fact column (REQ-BARD-SEL-1, -8) — the handle is the
// selection token the model returns verbatim, and the blurb is the only prose
// it selects on.
struct CatalogChoice {
    std::string handle;       // the selection token, returned verbatim
    std::string blurb;        // the only prose the model selects on
    std::string motiveBlurb;  // motive_catalog.blurb, never the key
};

// The catalog entries offerable in `room`, each as handle + blurb + motive
// blurb (REQ-BARD-SEL-1). Read-only, deterministic, O(catalog size). Four gates
// compose (REQ-BARD-SEL-2), all of which must hold:
//   a. entity IS NULL — not already materialized;
//   b. tier <= distanceFromSeed(room) — the SAME spatial metric combat's
//      eligibleArchetypes uses, so story and combat escalate together. A room
//      BFS cannot reach offers NOTHING (its distance is the INT64_MAX
//      sentinel), matching combat's empty menu off the map;
//   c. kind equals the requested kind;
//   d. a knowledge beat (non-empty fact_archetype) is offered only where its
//      subject is live — see REQ-BARD-SEL-2d.
// Ordered by catalog.id ascending (REQ-BARD-SEL-4). An EMPTY result is a
// normal, common answer and never an error — the same contract
// eligibleEnemyBlurbs has on a safe-edge room. No per-entry unlock condition is
// read and no such column exists (REQ-BARD-SEL-7).
std::vector<CatalogChoice> eligibleCatalog(Db& db, int64_t room,
                                           const std::string& kind);

// The choices for the room about to be created one hop beyond `originRoom`
// (REQ-BARD-SEL-5): its front distance is the origin's plus one, exactly as
// eligibleEnemyBlurbs relates to eligibleArchetypes. Spans BOTH kinds — a
// character and a beat both materialize as entities, so the architect's menu is
// the union, still ordered by catalog.id. An origin BFS cannot reach offers
// nothing (and the increment cannot overflow). Read-only.
std::vector<CatalogChoice> eligibleCatalogForNewRoom(Db& db, int64_t originRoom);

// LIVE re-check (REQ-BARD-SEL-6): resolve a model-supplied handle to a catalog
// id ONLY if it is eligible in `room` right now. Unknown, stale,
// already-materialized, ineligible, or empty -> 0. Verbatim the
// archetypeForEnemyBlurb contract, and it matters MORE here: a bard proposal
// may have been snapshotted many turns before it commits. Read-only.
int64_t catalogForHandle(Db& db, int64_t room, const std::string& handle);

// ROOM-FREE, GATE-FREE lookup (REQ-BARD-SEL-24), for mark_seeded ONLY. A wake
// is not scoped to a room, so there is no room to supply, and the eligibility
// gates ask the wrong question: seeding records that an entry was HINTED, which
// can be true of an entry that is not offerable anywhere. Unknown handle -> 0.
//
// Do NOT "simplify" this into catalogForHandle, or vice versa — they answer
// different questions on purpose. Read-only.
int64_t catalogIdForHandle(Db& db, const std::string& handle);

// Pure function of the database (REQ-BARD-SEL-9): SELECTs only, no network, no
// globals. The overture's user message, carrying EXACTLY two things —
// meta.setting, and the motive vocabulary as key + blurb from motive_catalog,
// ordered by key. Nothing else about world state; at overture time none exists
// to describe. An empty setting still yields a well-formed object.
//
// The motive BLURBS are not optional. The tool schema constrains `motive` to
// eight bare keys, so without their meanings the model is selecting between
// opaque tokens.
std::string buildOvertureContext(Db& db);

// The most recent N events a wake may see. meta.bard_last_wake_turn advances
// when a wake is QUEUED (REQ-BARD-WAKE-11, Brick 3), so a session in which
// every wake fails would otherwise grow this payload without bound. The catalog
// is NOT capped — it is authored once and small, and the spec is explicit that
// the whole table is carried. "Tunable" means this one line.
inline constexpr int64_t kBardWakeEventLimit = 120;

// Pure function of the database (REQ-BARD-SEL-10): the wake's user message,
// carrying exactly five things — meta.setting, the motive vocabulary as key +
// blurb, meta.bard_journal, the events since meta.bard_last_wake_turn as
// human-readable lines, and the FULL catalog as handle + blurb + motive blurb +
// materialized-or-not.
//
// The whole table, not the eligible menu: mark_seeded and the journal reason
// about entries that are not currently offerable anywhere. No ids, no tier, no
// seeded flag (REQ-BARD-SEL-8) — and the event lines pass through the
// narrator's tag shield, so no room id or engine-internal tag rides in on a
// `detail`. Events are capped at kBardWakeEventLimit, most recent first by
// selection but rendered oldest-first.
std::string buildWakeContext(Db& db);

// The bard's two system prompts (REQ-BARD-SEL-22) — git-versioned string
// constants, exposed here so their structure is spot-checkable by substring,
// following kArchitectPrompt and kResolveSystemPrompt.
//
// The overture prompt instructs the model to author the cast and the beats for
// a world that does not exist yet, via one write_catalog call: what each entry
// field means, that `tier` is DEPTH from the start rather than danger, and that
// a `fact` which is not true of the rules costs the whole entry.
//
// The wake prompt instructs it to read what has happened since it last looked
// and to use any, all, or NONE of the four wake tools — declining to act is a
// legitimate turn.
//
// Both carry the situations-not-urgency clause (REQ-BARD-SEL-23): no deadlines,
// no countdowns, nothing that makes standing still or talking at length feel
// expensive. Escalation in this game is spatial — distance from the seed is the
// only intensity dial — and a prompt that introduced time pressure would
// undercut the one mechanic the whole eligibility system is built on.
//
// Prompt QUALITY is judged against real output in Brick 3; the tests here pin
// only STRUCTURE, so they can never become a tune-and-retry loop. Reword with
// care.
extern const char* const kBardOverturePrompt;
extern const char* const kBardWakePrompt;

// The motive vocabulary's KEYS, ordered, straight from motive_catalog
// (REQ-BARD-SEL-12). This one-line reader is what makes the request builders'
// `motive` enum data-driven while leaving them pure string→string: the
// vocabulary is a parameter, exactly as the architect passes its `enemy` blurbs
// in rather than reading them inside the builder. A ninth motive row changes
// what this returns, which changes the enum, with no code change anywhere.
std::vector<std::string> motiveKeys(Db& db);

// The overture request body (REQ-BARD-SEL-11): an Anthropic Messages API body
// with ONE write_catalog tool, whose input schema is a required `entries` array
// of catalog entries plus a required `journal` string. Each entry requires
// kind, handle, name, blurb, motive, and tier, with an optional `fact` object
// that requires BOTH of its fields when present.
//
// Pure function of its two arguments — no database, no network, no globals.
// `kind` is a schema-enforced enum of exactly {character, beat}; `motive` is a
// schema-enforced enum of exactly `motives` (pass motiveKeys(db)).
// tool_choice is AUTO (REQ-BARD-SEL-14). There is no room-placement tool, here
// or anywhere (REQ-BARD-SEL-15) — the bard cannot express a room, and placement
// is the architect's. The guard on that is a grep over src/, so the tool's name
// appears in no source file at all.
std::string buildOvertureRequestBody(const std::string& contextPayload,
                                     const std::vector<std::string>& motives);

// The wake request body (REQ-BARD-SEL-13): four tools — write_focus {text},
// write_journal {text}, append_catalog {entry}, mark_seeded {handle}. Small
// tools rather than one large one, so a PARTIAL response is still useful.
//
// append_catalog's `entry` is ONE element of the overture's `entries` array —
// the same shape, built by the same file-local builder so the two cannot drift
// — and carries no `entries` array and no `journal`. tool_choice is AUTO, and a
// response calling no tool at all is the bard declining to act, which is a
// success (REQ-BARD-SEL-14). Pure function of its two arguments.
std::string buildWakeRequestBody(const std::string& contextPayload,
                                 const std::vector<std::string>& motives);

// One entry as the MODEL proposed it, before admission (REQ-BARD-SEL-17). A
// proposal, not a row: it carries no id, and nothing here has been checked
// against the world yet beyond the shape checks the gate applies.
struct CatalogEntryProposal {
    std::string kind, handle, name, blurb, motive;
    int64_t tier = 0;
    std::string factArchetype, factElement;  // both empty or both set
};

// The result of one accepted overture response: the bulk cast + the bard's
// private journal (REQ-BARD-SEL-11).
struct OvertureProposal {
    std::vector<CatalogEntryProposal> entries;
    std::string journal;
};

// The result of one accepted wake response (REQ-BARD-SEL-13). Every field is
// optional — a wake that calls no tool at all is a successful, empty wake
// (REQ-BARD-SEL-14). The has* flags exist because "wrote an empty focus" and
// "did not call write_focus" are different facts, and admission treats them
// differently.
struct WakeProposal {
    bool hasFocus = false;
    std::string focus;
    bool hasJournal = false;
    std::string journal;
    std::vector<CatalogEntryProposal> appended;  // append_catalog, 0..n
    std::vector<std::string> seededHandles;      // mark_seeded, 0..n
};

// The overture gate (REQ-BARD-SEL-16): a pure function of the HttpResponse. It
// touches no database, NEVER throws, and emits one stderr diagnostic naming the
// first failed clause. `motives` is the vocabulary to check `motive` against —
// pass motiveKeys(db); an EMPTY vector means "vocabulary unchecked here", and
// the truth of it is then enforced by writeCatalogEntry at admission. The
// vocabulary is passed rather than hardcoded for the same reason the schema's
// enum is (REQ-BARD-SEL-12).
//
// STRICT PER RESPONSE (REQ-BARD-SEL-18) — nullopt, rejecting in full, ONLY
// when: the status is not 200 (a transport error carries status 0, so it fails
// here), the body is unparseable or not an object, there is no `content` array,
// or there is no write_catalog tool_use block. A rejected overture yields an
// empty catalog, which is a supported state everywhere.
//
// LENIENT PER ENTRY (REQ-BARD-SEL-17) — an entry is DROPPED, with one
// diagnostic, and the rest are kept, when: handle/name/blurb is empty after
// trim; kind or motive is outside its vocabulary; tier is not a non-negative
// integer; or `fact` carries one field without the other. Dropping an entry
// NEVER rejects the response: rejecting a whole overture over one malformed
// entry would leave the game with no story at all, strictly worse than a story
// with nine entries instead of ten.
std::optional<OvertureProposal> validateOvertureResponse(
    const HttpResponse& response, const std::vector<std::string>& motives);

// The wake gate (REQ-BARD-SEL-16), same purity contract. Scans content[] for
// ALL FOUR tool names, accumulating: several append_catalog calls in one
// response all land, and so do several mark_seeded calls.
//
// Only the transport/parse clauses reject. A response with ZERO tool calls is a
// SUCCESSFUL, EMPTY wake (REQ-BARD-SEL-14): it returns an empty WakeProposal,
// not nullopt, and emits no diagnostic claiming failure. The bard declining to
// act is normal, and treating it as an error is the single most likely
// misreading of this function.
//
// An append_catalog entry failing the per-entry checks is dropped with a
// diagnostic, like the overture's. A mark_seeded with a blank handle is dropped
// here; an UNKNOWN handle is not detectable without a database and is handled
// at admission (REQ-BARD-SEL-20).
std::optional<WakeProposal> validateWakeResponse(
    const HttpResponse& response, const std::vector<std::string>& motives);

// "" if this entry would be admitted, else the reason it is refused. Mirrors
// every refusal writeCatalogEntry makes — kind in vocabulary, a motive_catalog
// row, handle/name/blurb non-empty after trim, tier >= 0, fact both-or-neither,
// and the three truth-gate clauses (a bestiary row, an element that appears in
// spell_catalog, a resistance row for the pair) — PLUS handle uniqueness,
// against the catalog and against the handles already admitted in this same
// call. SELECTs only.
//
// WHY A PRE-FLIGHT RATHER THAN A CATCH. db.hpp raises std::runtime_error for a
// genuine SQLite fault (a disk error, a lock, a UNIQUE violation) and for a
// validation refusal alike, so `catch (const std::runtime_error&)` could not
// tell "drop this entry" from "the database is broken" — and would swallow the
// second, while REQ-BARD-WAKE-7/-23 require a real fault to roll the whole
// write back. Refusing an entry BEFORE it reaches the helper keeps both true:
// the helper is only ever called with arguments it cannot refuse, so any throw
// that still escapes admission is by construction an engine fault and
// propagates to the caller's transaction. The duplicated predicate is the cost,
// and an equivalence test fences it: this returns non-empty iff a direct
// writeCatalogEntry with the same arguments throws.
std::string catalogEntryRefusal(Db& db, const CatalogEntryProposal& entry,
                                const std::set<std::string>& admittedThisCall);

// Admit an accepted overture (REQ-BARD-SEL-19): every entry the pre-flight
// accepts is written through writeCatalogEntry, and the journal is written
// through writeBardJournal. An entry the pre-flight refuses is dropped with one
// diagnostic and the remaining entries still land — a false `fact` drops the
// ENTIRE entry, not merely the fact, because a knowledge beat whose knowledge
// is false has no remaining purpose. Returns the number of entries admitted.
//
// Neither begins, commits, nor rolls back: Brick 3 owns the transaction around
// this (REQ-BARD-WAKE-7). There is no try/catch here, deliberately — see
// catalogEntryRefusal.
int admitOvertureProposal(Db& db, const OvertureProposal& proposal);

// A DELIBERATE EXCEPTION to the uniform 8 s budget (REQ-BARD-WAKE-5). The
// overture is a bulk generation of the whole catalog at high effort, run ONCE
// per world file, with the player already waiting and told so, with nothing
// else running, and with no fallback that produces a better catalog — the
// alternative to waiting is an empty one. 8 s would cut it off mid-write.
// Do NOT normalize this back to kAiHttpTimeoutSeconds.
inline constexpr long kBardOvertureTimeoutSeconds = 60;

// The one cold call, on the MAIN thread, before any worker exists. Blocking.
// Runs only when the world was created THIS launch (the caller's half of the
// condition, since only main() knows it) and only when the bard and AI are
// enabled — checked here, FIRST THING, so a disabled run constructs no
// transport and makes no call at all, which is a fact a test can assert and a
// guard buried in main() would not be.
//
// NEVER THROWS. Every failure yields an empty catalog and today's game
// (REQ-BARD-WAKE-6), and the whole body is inside the guard rather than just
// the transport call: the context builders and the admission path are ordinary
// db.hpp callers, and db.hpp raises std::runtime_error for any SQLite fault, so
// an unguarded builder would propagate into main()'s catch and end the session
// on world creation — the exact inverse of the degradation claim.
//
// `transport` overrides the production transport; pass nullptr in production,
// exactly as pregenAcquire does.
void bardOverture(Db& db, const HttpTransport* transport);

// The rate ceiling (REQ-BARD-WAKE-9). Engine-owned, NOT model-visible and not
// configurable at runtime by the model — the same standing the combat constants
// have. At a 5-turn floor the bard cannot exceed 0.2 wakes/turn no matter how
// frantically the player generates irreversible events. "Tunable" means this
// one line.
inline constexpr int64_t kBardMinTurnGap = 5;

// THE ONE CALL THE TURN LOOP MAKES, on the main thread, AFTER the tick's
// transaction has committed and AFTER the player's text has been flushed — so
// neither half of it can ever delay the turn the player waited on
// (REQ-BARD-WAKE-8). Commits a ready result FIRST, in its own transaction, then
// evaluates the trigger; committing first is what lets REQ-BARD-WAKE-14's "one
// further evaluation after that wake commits" happen on the same turn rather
// than the next one.
//
// No-op when the bard or AI is off. NEVER THROWS: like bardOverture, the whole
// body is inside the guard, because the trigger query, the snapshot builders,
// and the stamp are all ordinary db.hpp callers and an escape from here would
// kill a turn the player has already been shown.
void bardAfterTurn(Db& db);

// Apply an accepted wake, in order: focus, journal, appended entries (through
// the same pre-flight), then each mark_seeded handle resolved with
// catalogIdForHandle (REQ-BARD-SEL-24 — room-free and gate-free, NOT
// catalogForHandle) and latched with markCatalogSeeded. A handle that resolves
// to 0 is ignored with one diagnostic and the rest of the wake still applies
// (REQ-BARD-SEL-20). Returns the number of writes applied. Never begins,
// commits, or rolls back.
int applyWakeProposal(Db& db, const WakeProposal& proposal);
