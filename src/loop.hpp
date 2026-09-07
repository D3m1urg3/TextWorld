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

// How runTurn should PRESENT the text, which the outcome cannot say: `spells`
// and a tier-a refusal are both TurnOutcome::NoTick, yet REQ-POLISH-3b exempts
// the first from the indent and REQ-POLISH-7a requires it on the second. A
// defaulted third member on TurnResult carries the distinction instead.
enum class TurnPresentation {
    Prose,      // the world talking: wrapped to proseWidth, indented
    Reference,  // a reference table (`spells`): full width, column 0, like the band
};

struct TurnResult {
    TurnOutcome outcome;
    std::string output;
    // Defaulted, so aggregate initialisation of the first two members still
    // compiles everywhere it already did.
    TurnPresentation presentation = TurnPresentation::Prose;
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

// The room the player currently occupies. Read-only — no tick, no transaction.
//
// Exists so main() can name the room the pre-generation scheduler should look
// at (REQ-PREGEN-4) without duplicating the two lookups this file already has.
// Deliberately NOT a widened TurnResult: the room is wanted at STARTUP too,
// where no TurnResult exists, and widening the struct would touch every test
// that constructs one.
int64_t playerRoom(Db& db);
