// Turn loop: per-line dispatch shared by the game binary and the tests, so
// the tests exercise the production path (REQ-PROTO-5, REQ-PROTO-6).
#pragma once

#include <string>

#include "db.hpp"

enum class TurnOutcome {
    NoTick,       // tier a: unparseable — no transaction, no tick
    Ticked,       // a tick committed (success or in-world refusal, tier b)
    EngineError,  // tier c: mid-tick exception — rolled back, no tick
    Quit,         // quit verb — handled pre-transaction, no tick
};

struct TurnResult {
    TurnOutcome outcome;
    std::string output;
};

// Dispatch one input line:
//   - unparseable         → {NoTick, error text}; the world file is untouched.
//   - quit                → {Quit, ""}; never opens a transaction.
//   - otherwise           → one tick transaction: increment meta.turn exactly
//                           once, resolve, commit; {Ticked, render of the turn}.
//   - exception mid-tick  → rollback; {EngineError, message}; turn counter and
//                           world state as if the prompt never happened.
TurnResult runTurn(Db& db, const std::string& line);

// Courtesy render for startup: the player's current room, read-only — no
// tick, no transaction, no event row. Lets main() show where you are before
// the first prompt without consuming a turn.
std::string renderStartup(Db& db);
