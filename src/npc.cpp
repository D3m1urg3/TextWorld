// See npc.hpp for the contract, and for the one property that governs this
// whole file: a conversation cannot write to the world.
#include "npc.hpp"

#include <stdexcept>
#include <string>

#include "aihttp.hpp"     // modelForRole + the shared production transport
#include "combat.hpp"     // hostileInRoom — the refusal this shares with resolveGo
#include "json.hpp"
#include "log.hpp"
#include "mutations.hpp"  // appendEvent + the npc store's readers and writers

namespace {

using nlohmann::json;

// Room the actor currently stands in. This unit has its OWN copy deliberately:
// the same lookup is independently reimplemented in seven translation units
// already (band, prose, combat, nlresolve, systems, loop, render) under the
// house "reimplement, don't reach across TUs" rule. This is the eighth, not an
// exception to it.
int64_t roomOf(Db& db, int64_t actor) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, actor);
    if (!s.step()) {
        throw std::runtime_error("resolveSay: entity " + std::to_string(actor) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// The in-world parser noun of an entity, or "" if it has no name row. In-world
// prose, not a machine token: the resolver's scope payload already carries these
// same nouns, and REQ-NPCTALK-24 names ids, tiers, flags, and internal tags —
// never the words the player themself reads on screen.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

// The entity's canon prose — the same text `examine` prints, verbatim.
std::string descriptionOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT prose FROM description WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

// The world's setting document, or "" if the world was built without one. The
// architect reads the same row for the same reason.
std::string settingText(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'setting'");
    if (!s.step()) return "";
    return s.colText(0);
}

}  // namespace

// --- the engine-authored lines (REQ-NPCTALK-3, -4, -31) ---------------------
// Constants, not literals at the append site, so a test can assert the EXACT
// string and so a failure can never surface as a fabricated line of dialogue.

const char* const kNoOneToTalkTo = "There is no one here to talk to.";
const char* const kNoTalkingInCombat = "There is no time for talk in a fight.";
const char* const kNoReply = "You get no reply.";

int64_t characterInRoom(Db& db, int64_t room) {
    // kind IN ('character','major') — a beat is an object and is examinable,
    // never talkable (REQ-NPCTALK-2). At most one can exist by rules already
    // set (REQ-NPCTALK-1); the ORDER BY makes the read deterministic anyway,
    // so a world that somehow held two would behave the same way twice rather
    // than picking differently on each turn.
    Stmt s = db.prepare(
        "SELECT c.entity FROM catalog c "
        "JOIN location l ON l.entity = c.entity "
        "WHERE l.container = ? AND c.entity IS NOT NULL "
        "  AND c.kind IN ('character','major') "
        "ORDER BY c.entity LIMIT 1");
    s.bind(1, room);
    if (!s.step()) return 0;
    return s.colInt(0);
}

// --- the prompt (REQ-NPCTALK-20..-24) ---------------------------------------

// Stable constant, versioned by git. These rules are NOT in the character's
// profile file (REQ-NPCTALK-23), because a rule a player will actively attack
// cannot live in a file an author can edit or forget. They are defence in
// DEPTH: the real defence is that this unit has no code path to a component
// write, so persuasion has nothing to attack. Reword with care — the suite
// spot-checks one distinctive phrase from each of the three prohibitions.
const char* const kSpeakRulesPrompt =
    R"(You are playing one character in a text adventure, speaking to the player in that character's own voice. You write only what this character says aloud. You never narrate, never describe actions or the room, never speak as the game, and never address the player as a player.

Answer the line the player just said to you, in character, briefly - a few sentences at most, the length a person actually answers in. Stay in the voice the character document below gives you, and stay consistent with what you remember of this conversation.

Rules, absolute:
- Never explain a mechanic. Not a weakness, not damage, not a cooldown, not a resistance, not how a spell or a creature works in the rules. A character knows what a person in this world would know, and the rules are not that. If the player asks how to beat something, answer as a person would - with rumour, opinion, fear, or refusal - never with a number or a rule.
- Never volunteer background unprompted. Say what answers the line in front of you. Do not deliver history, biography, or exposition the player did not ask for, however much of it the character document gives you.
- Never name a place, a person, or an object that has not already been established. Speak only of what the conversation, the character document, and the setting have already put in the world. Invent no proper nouns.
- You cannot act. You cannot open, unlock, give, hand over, take, move, or change anything, and no answer you write makes any of those things happen. If the player asks for something like that, answer as a person would - agree, refuse, stall, or lie - but nothing in the world changes either way.
- Never speak for the player, and never invent what they said or did.)";

std::string buildSpeakSystem(Db& db, int64_t character) {
    // Every part below is STABLE for this character (REQ-NPCTALK-21): engine
    // rules (global), who this character is (per character), the setting
    // (global). Nothing that varies between two conversations with the same
    // character appears here — that is the cache prefix, and the suite asserts
    // it as a property rather than trusting it as an intention.
    std::string out = kSpeakRulesPrompt;

    // Who the character IS, in the world's own words: the parser noun and the
    // canon prose `examine` prints. Both are already on the player's screen,
    // and both are stable per character.
    //
    // This block is a documented DEPARTURE from REQ-NPCTALK-20's three-row
    // table, taken deliberately: `npcProfile` is empty by definition on a minor
    // character's FIRST contact, which is the one call that writes the profile
    // — and writeCatalogProfile is a one-way latch (REQ-NPCTALK-35). Without an
    // identity here the model would invent a character unrelated to the figure
    // the player is looking at, permanently. Byte-identity and the id shield
    // both survive: a name is not an id, a tier, a flag, or an internal tag.
    if (const std::string name = nameOf(db, character); !name.empty()) {
        out += "\n\nYou are the " + name + ".";
    }
    if (const std::string prose = descriptionOf(db, character); !prose.empty()) {
        out += "\n" + prose;
    }

    // The character document. Empty on first contact, which is normal and
    // common (REQ-NPCSTORE-19) and is exactly the state that makes the engine
    // ask for one.
    if (const std::string profile = npcProfile(db, character); !profile.empty()) {
        out += "\n\nThe character you are playing:\n" + profile;
    }

    if (const std::string setting = settingText(db); !setting.empty()) {
        out += "\n\nThe setting this world shares:\n" + setting;
    }
    return out;
}

SpeakAsks speakAsksFor(Db& db, int64_t character) {
    SpeakAsks asks;
    // Over the ROW, never over kind (REQ-NPCTALK-18a). A major character always
    // has a profile from world creation (REQ-NPCSTORE-33), so in practice this
    // is a minor-character path — but a major that somehow lacks one is handled
    // by this same branch rather than by a special case nobody would find.
    asks.profile = npcProfile(db, character).empty();
    asks.summary = npcLinesSince(db, character).size() >= kFoldThreshold;
    return asks;
}

std::string buildSpeakUser(Db& db, int64_t character, const std::string& line,
                           SpeakAsks asks) {
    // VOLATILE, all of it — memory, the recent lines, the player's line, and
    // the engine's asks. The asks live HERE rather than in the system block
    // because whether a profile is wanted varies per call for the SAME
    // character (REQ-NPCTALK-18), and putting them above would break the
    // byte-identity REQ-NPCTALK-21 requires.
    //
    // THE SHIELD (REQ-NPCTALK-24): only `said`/`spoke` details reach this
    // payload, and those are free text by construction. No entity id, catalog
    // id, tier, `seeded` flag, handle, or archetype|element tag is read
    // anywhere in either builder — it falls out of which columns are selected
    // rather than needing a filter. `character` is used ONLY to SELECT with.
    json payload;

    const NpcMemory memory = npcMemory(db, character);
    if (!memory.summary.empty()) payload["memory"] = memory.summary;

    json recent = json::array();
    for (const SpeechLine& l : npcLinesSince(db, character)) {
        // "player" / "you" rather than ids or names: the model is the
        // character, so its own lines are the ones it said.
        recent.push_back({{"speaker", l.verb == "said" ? "player" : "you"},
                          {"line", l.detail}});
    }
    if (!recent.empty()) payload["recent"] = std::move(recent);

    payload["input"] = line;

    // The asks, as data rather than as prose the model has to parse out. The
    // same two bits the orchestration reads the response fields under, so an
    // unrequested field is ignored because nothing looks at it.
    payload["write_profile"] = asks.profile;
    payload["write_summary"] = asks.summary;

    return payload.dump();
}

// --- the request body and the emit_reply tool (REQ-NPCTALK-18, -18a, -19) ---

std::string buildSpeakRequestBody(const std::string& system,
                                  const std::string& user) {
    // Named `emit_reply` after emit_action and create_room — the house pattern,
    // rather than the design's "the reply tool" after its required field.
    json emitReply;
    emitReply["name"] = "emit_reply";
    emitReply["description"] =
        "Return this character's spoken answer to the player's line, and "
        "nothing else. Call exactly once.";

    // All three properties, UNCONDITIONALLY — the schema is part of the stable
    // half and never varies (REQ-NPCTALK-21's spirit applied to the tool). What
    // varies is the ask in the user message, and the engine reads a field only
    // when it asked for it. That is what makes "an unrequested field is
    // ignored" (REQ-NPCTALK-18) true by construction rather than by a check.
    json properties;
    properties["reply"] = {
        {"type", "string"},
        {"description",
         "What the character says aloud, in their own voice. Required, always."}};
    properties["profile"] = {
        {"type", "string"},
        {"description",
         "Supply this ONLY when the user message has write_profile true. The "
         "character document for this character, written once and kept "
         "forever: who they are, how they speak, what they want, and what they "
         "will not say. Write it in the third person, from this conversation "
         "and the setting. Omit the field entirely otherwise."}};
    properties["summary"] = {
        {"type", "string"},
        {"description",
         "Supply this ONLY when the user message has write_summary true. A "
         "replacement memory of this conversation so far, folding the previous "
         "memory and the recent lines into one short account of what happened "
         "and what was learned. Facts, never voice. Omit the field entirely "
         "otherwise."}};

    json inputSchema;
    inputSchema["type"] = "object";
    inputSchema["properties"] = std::move(properties);
    // Only `reply` is required. A missing or malformed profile/summary is
    // DROPPED and the reply still lands (REQ-NPCTALK-32) — a bad part never
    // costs the whole turn.
    inputSchema["required"] = json::array({"reply"});
    emitReply["input_schema"] = std::move(inputSchema);

    json body;
    body["model"] = modelForRole(AiRole::Speak);
    // Sized for a reply plus an optional profile and an optional summary — the
    // two long fields are capped downstream at kProfileCap / kSummaryCap, so
    // this only has to be large enough that the model is not cut off mid-field.
    body["max_tokens"] = 1024;
    body["system"] = system;
    body["messages"] = json::array({{{"role", "user"}, {"content", user}}});
    body["tools"] = json::array({std::move(emitReply)});
    // FORCED, unlike the resolver's "auto". A talk turn always wants a reply;
    // "no tool call" is a failure here (REQ-NPCTALK-31 case 6), not a designed
    // no-action path the way it is for resolution.
    body["tool_choice"] = {{"type", "tool"}, {"name", "emit_reply"}};
    // Deliberately absent everywhere in this body: thinking, stream, and any
    // cache-control key — the resolver's posture, and the test pins the exact
    // top-level set.
    return body.dump();
}

// --- the validation gate (REQ-NPCTALK-31, -32) ------------------------------

namespace {

// One diagnostic line per rejected response, naming the first failed clause in
// a..d order. The failClause shape prose.cpp and nlresolve.cpp both use.
std::nullopt_t failSpeechClause(char clause, const char* why) {
    logEmitf(LogLevel::Debug, "npc",
             "validateSpeech: response rejected, clause %c failed: %s", clause,
             why);
    return std::nullopt;
}

// An optional string field, DROPPED to "" when absent or the wrong type
// (REQ-NPCTALK-32). Never a clause: a malformed profile or summary must not
// cost the reply that arrived alongside it.
std::string optionalString(const json& input, const char* key) {
    if (!input.contains(key) || !input[key].is_string()) return "";
    return input[key].get<std::string>();
}

// Is `obj[key]` exactly the string `want`? Written out rather than reached for
// via json::value(key, "") because THAT THROWS: nlohmann raises type_error.302
// when the stored value is a non-string, so a response block carrying
// {"type": 7} would take validateSpeech out through an exception instead of
// through its own gate. This function must never throw (the caller's try/catch
// is for the transport, not for the parser), so the type check is explicit.
bool fieldEquals(const json& obj, const char* key, const char* want) {
    return obj.contains(key) && obj[key].is_string() &&
           obj[key].get<std::string>() == want;
}

}  // namespace

std::optional<SpeechReply> validateSpeech(const HttpResponse& response) {
    // Clause a: HTTP. A transport error carries status 0, so timeouts, connect
    // failures, and curl errors all fail here too — three of REQ-NPCTALK-31's
    // eight cases arriving by different code and leaving by the same door.
    if (response.status != 200) {
        return failSpeechClause('a', "HTTP status is not 200");
    }

    // Parse the body ONCE, exception-free: this function must never throw.
    const json j = json::parse(response.body, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return failSpeechClause('b', "body is not a JSON object");
    }
    if (!j.contains("content") || !j["content"].is_array()) {
        return failSpeechClause('b', "response has no content array");
    }

    // Clause c: exactly one emit_reply tool_use block. ZERO IS A FAILURE HERE,
    // unlike the resolver — a talk turn always wants a reply, and tool_choice
    // forced the call, so no call is a broken response rather than a designed
    // no-action path.
    const json* emit = nullptr;
    int emitCount = 0;
    for (const json& block : j["content"]) {
        if (block.is_object() && fieldEquals(block, "type", "tool_use") &&
            fieldEquals(block, "name", "emit_reply")) {
            ++emitCount;
            emit = &block;
        }
    }
    if (emitCount == 0) {
        return failSpeechClause('c', "no emit_reply tool_use block");
    }
    if (emitCount >= 2) {
        return failSpeechClause('c', "more than one emit_reply tool_use block");
    }
    if (!emit->contains("input") || !(*emit)["input"].is_object()) {
        return failSpeechClause('c', "emit_reply block has no input object");
    }
    const json& input = (*emit)["input"];

    // Clause d: reply present, a string, and non-empty after trim. A reply of
    // whitespace is a reply of nothing, and printing it would show the player a
    // blank line where a person should have answered.
    if (!input.contains("reply") || !input["reply"].is_string()) {
        return failSpeechClause('d', "reply missing or not a string");
    }
    SpeechReply out;
    out.reply = input["reply"].get<std::string>();
    if (out.reply.find_first_not_of(" \t\n\r\f\v") == std::string::npos) {
        return failSpeechClause('d', "reply is empty");
    }

    // Never clauses (REQ-NPCTALK-32).
    out.profile = optionalString(input, "profile");
    out.summary = optionalString(input, "summary");
    return out;
}

namespace {

// The catalog row a materialized entity was cast from, or 0. writeCatalogProfile
// is keyed by CATALOG id, not entity — the profile outlives any one placement.
int64_t catalogOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT id FROM catalog WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return 0;
    return s.colInt(0);
}

void resolveSayImpl(Db& db, int64_t player, const std::string& text,
                    const HttpTransport* transport) {
    const int64_t room = roomOf(db, player);
    const int64_t character = characterInRoom(db, room);

    // REQ-NPCTALK-4a: the no-one-here check runs FIRST. An empty room that
    // holds a hostile yields this line, not the combat one — with nobody
    // present, "you can't talk during a fight" would imply there was someone
    // to talk to.
    if (character == 0) {
        appendEvent(db, player, "failed", 0, 0, kNoOneToTalkTo);
        return;
    }
    // REQ-NPCTALK-4: a rule, not a consequence of the chip clock. It keeps a
    // talk turn to exactly one shape — said/spoke only, nothing to narrate, one
    // model call — and it is reachable, because the generator can place an
    // enemy and a character in the same room.
    if (hostileInRoom(db, room) != 0) {
        appendEvent(db, player, "failed", 0, 0, kNoTalkingInCombat);
        return;  // no model call is issued on this path (validation item 2)
    }

    // The two asks, read BEFORE the call — the same two bits the response
    // fields are read under below, so an unrequested field is ignored because
    // nothing looks at it (REQ-NPCTALK-18).
    const SpeakAsks asks = speakAsksFor(db, character);

    std::optional<SpeechReply> got;
    // REQ-NPCTALK-31 case 1, checked first: with AI disabled no request is
    // built and no transport is constructed, exactly as every other AI path in
    // the binary behaves.
    if (aiNarrationEnabled()) {
        try {  // ───────────── AI failure lives INSIDE here ─────────────
            const std::string body = buildSpeakRequestBody(
                buildSpeakSystem(db, character),
                buildSpeakUser(db, character, text, asks));
            // EXACTLY ONE call per talk turn (REQ-NPCTALK-17), no retry, ever —
            // and the narrator is already skipped for this turn by loop.cpp, so
            // dialogue adds no per-turn cost over an ordinary turn.
            const HttpResponse resp = transport != nullptr
                                          ? (*transport)(body)
                                          : makeAnthropicTransport(AiRole::Speak)(body);
            got = validateSpeech(resp);
        } catch (const std::exception& e) {
            // WARN, not ERROR: the turn still ticks, it just ticks without a
            // reply. No response body and no key can reach this message.
            logEmitf(LogLevel::Warn, "npc", "resolveSay: no reply: %s", e.what());
        } catch (...) {
            logEmit(LogLevel::Warn, "npc", "resolveSay: no reply: unknown");
        }
    }

    // ─── and the writes are OUTSIDE it (REQ-NPCTALK-33) ─────────────────────
    // A database fault here propagates to runTurn, which rolls the tick back:
    // no said row, no spoke row, meta.turn unchanged. It must NEVER degrade to
    // the no-reply line, because that would report a world which did not change
    // as one that did. Widening the catch above to cover these calls is the
    // exact downgrade the spec forbids; the suite's fault-injection test is
    // what notices if anyone does it.
    //
    // `said` before `spoke` (REQ-NPCTALK-29) because npcLinesSince returns rows
    // in log order, and a reply preceding its question would read back as one.
    // The player's words come from `text`, which came from the engine — never
    // from `got` (REQ-NPCTALK-6).
    appendEvent(db, player, "said", character, 0, text.c_str());
    if (!got) {
        appendEvent(db, player, "failed", 0, 0, kNoReply);
        return;  // REQ-NPCTALK-34: no profile written, so the next conversation
                 // asks again and a failed first contact costs nothing permanent
    }
    appendEvent(db, character, "spoke", player, 0, got->reply.c_str());
    if (asks.profile && !got->profile.empty()) {
        // A one-way latch (REQ-NPCTALK-35): a second profile in a later
        // response changes nothing, silently, and the engine does not re-check.
        writeCatalogProfile(db, catalogOf(db, character), got->profile);
    }
    if (asks.summary && !got->summary.empty()) {
        writeNpcMemory(db, character, got->summary);  // stamps turn - 1
    }
}

}  // namespace

void resolveSay(Db& db, int64_t player, const std::string& text) {
    resolveSayImpl(db, player, text, /*transport=*/nullptr);
}

void resolveSay(Db& db, int64_t player, const std::string& text,
                const HttpTransport& transport) {
    resolveSayImpl(db, player, text, &transport);
}
