// DISPOSABLE. This parser is a throwaway test harness for exercising the
// engine through Action. Nothing downstream of Action may know it exists;
// it will be deleted without ceremony when a real front end arrives.
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

    if (verbWord == "go") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        // Any non-empty direction parses; validity is resolution's job.
        Action a{Verb::Go};
        a.direction = arg;
        return a;
    }

    if (verbWord == "take" || verbWord == "drop") {
        if (arg.empty()) return std::nullopt;  // bare verb, REQ-PROTO-6a
        const int64_t entity = lookupNoun(db, arg);
        if (entity == 0) return std::nullopt;  // not a noun anywhere in world
        Action a{verbWord == "take" ? Verb::Take : Verb::Drop};
        a.subject = entity;
        return a;
    }

    return std::nullopt;  // unknown verb
}
