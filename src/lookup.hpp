// Shared world-wide noun lookup. Hoisted from parser.cpp so both the
// deterministic parser and the AI resolver's validation gate resolve a noun
// name to an entity id through the SAME first-match rule (REQ-RESOLVE-14).
// Recognition only — world-wide existence, NOT scope applicability; the
// engine's resolution stays the sole authority on whether an action applies.
#pragma once

#include <cstdint>
#include <string>

#include "db.hpp"

// Look up `noun` against ALL rows of the name table (names are stored
// lowercase in the seed; input is already lowercased). Returns the entity id
// of the first match — no ambiguity handling — or 0 if no match anywhere.
inline int64_t lookupNoun(Db& db, const std::string& noun) {
    Stmt s = db.prepare("SELECT entity FROM name WHERE value = ? LIMIT 1");
    s.bind(1, noun);
    if (s.step()) return s.colInt(0);
    return 0;
}
