// NPC conversation: a character in the room, a line you type, and an answer.
//
// THE LOAD-BEARING PROPERTY OF THIS UNIT: a conversation CANNOT WRITE TO THE
// WORLD. The only rows a talk turn produces are two event rows, at most one
// profile, and at most one memory summary. A character cannot open a door, hand
// over an item, or change a number, because no code path exists for it — not
// because a prompt rule asks it not to. Prompt rules here are defence in depth
// on top of that; the structure is the defence. If something in this unit ever
// wants a component write, the want is wrong, not the constraint
// (REQ-NPCTALK-38 is the recorded revisit condition).
//
// WRITES ONLY THROUGH mutations.cpp: this translation unit contains no INSERT,
// UPDATE, or DELETE — the discipline architect.cpp and bard.cpp already live
// under, and the suite greps for it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"
#include "prose.hpp"  // HttpResponse / HttpTransport — the seam, reused as-is

// The character in `room`, or 0 if there is none: an entity whose catalog row
// has kind IN ('character','major') (REQ-NPCTALK-2). Beats are objects — a
// scorched lectern is examinable, not talkable. At most one can exist, by rules
// already set (REQ-NPCTALK-1), so this returns an id rather than a list; the
// day that rule breaks, this signature is what breaks with it.
int64_t characterInRoom(Db& db, int64_t room);

// The three engine-authored lines. Exposed so tests assert the EXACT string,
// and so a failure can never surface as fabricated dialogue (REQ-NPCTALK-31).
extern const char* const kNoOneToTalkTo;
extern const char* const kNoTalkingInCombat;
extern const char* const kNoReply;

// Unsummarised lines at which the engine asks for a memory fold
// (REQ-NPCTALK-19). Under mutations.hpp's kLineCap of 40, so the bounded read
// never truncates in practice.
inline constexpr size_t kFoldThreshold = 20;

// The engine-owned rules every character is bound by (REQ-NPCTALK-22, -23), a
// git-versioned constant prepended to every composed system block. They are NOT
// in the character's profile file, because a rule a player will actively attack
// cannot live in a file an author can edit or forget. Exposed so each of the
// three prohibitions is spot-checkable by substring, the way kArchitectPrompt
// and kResolveSystemPrompt are.
extern const char* const kSpeakRulesPrompt;

// The system block: engine rules, then this character's profile, then the
// setting. All three are STABLE — the block is byte-identical across two
// conversations with the same character whose memory differs (REQ-NPCTALK-21),
// which is the cache prefix and is asserted as a property, not intended as one.
// Composed at runtime rather than being a constant, because the profile is per
// character — the one departure from kArchitectPrompt / kResolveSystemPrompt,
// which is why the engine-owned half stays a constant that is prepended.
std::string buildSpeakSystem(Db& db, int64_t character);

// Which optional fields this call asks for. Computed ONCE per talk turn and
// passed down, so the ask in the prompt and the field the engine reads are the
// same bit: an unrequested field is ignored because nothing looks at it
// (REQ-NPCTALK-18).
struct SpeakAsks {
    bool profile = false;
    bool summary = false;
};

// The two asks for this character, computed ONCE per talk turn from the store.
//
// The profile condition is stated over the ROW, never over `kind`
// (REQ-NPCTALK-18a): npcProfile returns empty both for a minor character on
// first contact and for a major whose profile row is somehow missing, and both
// take the SAME branch rather than one being a special case that has to be
// discovered later.
//
// Exposed rather than inlined into resolveSay so validation items 19 and 19b
// can drive the real condition instead of a value the test computed itself.
SpeakAsks speakAsksFor(Db& db, int64_t character);

// The user message: memory summary, recent lines, the player's line, and the
// engine's asks. VOLATILE, all of it. The asks live here rather than in the
// system block because whether a profile is wanted varies per call for the same
// character, and putting them above would break byte-identity.
std::string buildSpeakUser(Db& db, int64_t character, const std::string& line,
                           SpeakAsks asks);

// Anthropic Messages API request body for a talk turn (REQ-NPCTALK-18).
// model = modelForRole(AiRole::Speak), one user message, one `emit_reply` tool
// carrying all three properties unconditionally with only `reply` required, and
// tool_choice FORCING the call — unlike the resolver's `auto`. A talk turn
// always wants a reply; "no tool call" is a failure here, not a designed path.
// No thinking, no stream, no cache-control key.
std::string buildSpeakRequestBody(const std::string& system,
                                  const std::string& user);

// What one accepted response yielded.
struct SpeechReply {
    std::string reply;    // never empty in a returned value
    std::string profile;  // "" = absent or dropped
    std::string summary;  // "" = absent or dropped
};

// Pure function of the response. SELECTs nothing, never throws — the
// validateAndLower / validateAiResponse shape. Returns nullopt iff the response
// cannot yield a reply; each rejection emits ONE diagnostic naming the clause.
//
// `profile` and `summary` are NEVER clauses (REQ-NPCTALK-32): a missing or
// non-string field is dropped to "" and the reply still lands. A bad part never
// costs the whole turn.
std::optional<SpeechReply> validateSpeech(const HttpResponse& response);

// Resolve a Verb::Say for `player`: find the character in the room, refuse if
// there is none or a hostile is present, otherwise make EXACTLY ONE model call
// and append the exchange. Runs inside the caller's ambient tick transaction;
// writes only through mutations helpers.
//
// No AI failure costs the turn (REQ-NPCTALK-31) — every one of the eight
// enumerated cases yields `said` + the authored no-reply line. A DATABASE fault
// does cost it (REQ-NPCTALK-33): the writes sit OUTSIDE the try block, so a
// fault propagates to runTurn and rolls the tick back rather than degrading to
// a line that reports a world which did not change as one that did.
void resolveSay(Db& db, int64_t player, const std::string& text);

// Test-visible overload: same contract, the transport injected.
void resolveSay(Db& db, int64_t player, const std::string& text,
                const HttpTransport& transport);
