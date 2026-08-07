// PERMANENT deterministic fallback for input, the input-side analog of
// render.cpp's template renderer (REQ-RESOLVE-4). The AI resolver lowers a raw
// line to an Action when narration is enabled and available; this fixed-verb
// parser is the always-there path when the resolver is disabled, declines, or
// fails. Nothing downstream of Action knows which front end produced it.
//
// Grammar: <verb-word> [argument]. Lowercase everything, split on the first
// whitespace run. Recognition only — validity is resolution's job.
#include "action.hpp"
#include "lookup.hpp"

#include <cctype>
#include <optional>
#include <string>

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::optional<Action> parse(Db& db, const std::string& line) {
    const std::string lowered = trim(toLower(line));
    if (lowered.empty()) return std::nullopt;

    // First whitespace token = verb word; remainder (trimmed) = argument.
    size_t split = 0;
    while (split < lowered.size() && !isSpace(lowered[split])) ++split;
    const std::string verbWord = lowered.substr(0, split);
    const std::string arg = trim(lowered.substr(split));

    // look/inventory/wait/quit take no argument; any trailing argument is
    // simply ignored (simplest choice — "look around" still looks).
    if (verbWord == "look") return Action{Verb::Look};
    if (verbWord == "inventory") return Action{Verb::Inventory};
    if (verbWord == "wait") return Action{Verb::Wait};
    if (verbWord == "quit") return Action{Verb::Quit};
    // Spell inspection: reference information about the rules, resolved by the
    // loop WITHOUT a tick (REQ-UI-39). Like the four above, a trailing argument
    // is ignored.
    if (verbWord == "spells") return Action{Verb::Spells};

    if (verbWord == "go") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        // Any non-empty direction parses; validity is resolution's job.
        Action a{Verb::Go};
        a.direction = arg;
        return a;
    }

    // Basic attack (REQ-COMBAT-6). The target is the hostile in the room —
    // combat is single-enemy per room — so no noun argument is consulted; any
    // trailing word ("attack goblin") is ignored, and resolution finds the foe.
    if (verbWord == "attack" || verbWord == "hit" || verbWord == "kill" ||
        verbWord == "fight") {
        return Action{Verb::Attack};
    }

    if (verbWord == "take" || verbWord == "drop" || verbWord == "read") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        const int64_t entity = lookupNoun(db, arg);
        if (entity == 0) return std::nullopt;  // not a noun anywhere in world
        Verb v = verbWord == "take"   ? Verb::Take
                 : verbWord == "drop" ? Verb::Drop
                                      : Verb::Read;
        Action a{v};
        a.subject = entity;
        return a;
    }

    // Look at one thing (REQ-EXAMINE-3): "examine <noun>", or its shorthand "x".
    // `look` is deliberately NOT extended — it keeps ignoring its argument
    // (REQ-EXAMINE-4), so "look at the candle" still just looks. Natural
    // phrasings that mean examination are the AI resolver's job, never this
    // parser's. Recognition only: whether the noun is in SCOPE is resolution's
    // decision, so no portability or container check happens here.
    if (verbWord == "examine" || verbWord == "x") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        const int64_t entity = lookupNoun(db, arg);
        if (entity == 0) return std::nullopt;  // not a noun anywhere in world
        Action a{Verb::Examine};
        a.subject = entity;
        return a;
    }

    // Cast a spell (REQ-COMBAT-7): "cast <spell>". Recognition only — the word
    // must name a catalogued spell; whether it is learned or off cooldown is the
    // engine's gate, not the parser's.
    if (verbWord == "cast") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        const std::string spell = lookupSpell(db, arg);
        if (spell.empty()) return std::nullopt;  // not a catalogued spell word
        Action a{Verb::Cast};
        a.spell = spell;
        return a;
    }

    // Speech (REQ-NPCTALK-8): the remainder of the line is the spoken text,
    // taken VERBATIM from the raw input rather than from `lowered`, so the
    // player's casing and punctuation survive into the `said` row. toLower is
    // byte-length preserving and trim cuts the same positions on both, so the
    // split offset computed above applies unchanged to trim(line). The target
    // is NOT resolved here (REQ-NPCTALK-9): subject stays 0 and resolution
    // finds the character in the room, the shape `attack` already uses. Nothing
    // about AI availability is consulted (REQ-NPCTALK-10) — the parser is
    // reached either way, and resolution decides what a `say` with no reachable
    // model produces.
    if (verbWord == "say") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        Action a{Verb::Say};
        a.text = trim(trim(line).substr(split));
        return a;
    }

    // Bare spell word ("ward", "fire") → Cast, the natural shorthand.
    if (const std::string spell = lookupSpell(db, verbWord); !spell.empty()) {
        Action a{Verb::Cast};
        a.spell = spell;
        return a;
    }

    return std::nullopt;  // unknown verb
}
