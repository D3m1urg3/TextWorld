// Action: the parsed intent handed to resolution. This struct is the seam
// for a future Action Resolver agent — it must stay free of parser types.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit, Attack, Cast, Read,
                  Spells, Examine, Say };

struct Action {
    Verb verb;
    int64_t subject = 0;   // entity id for Take/Drop/Examine; target enemy for Attack
                           // (0 = the hostile in the room); 0 when unused. For Say
                           // it stays 0: resolution finds the character in the room.
    std::string direction; // for Go; empty when unused
    std::string spell;     // for Cast: the catalogued spell key; empty otherwise
                           // (spells are string-keyed, not entities, so they
                           // ride their own field rather than `subject`)
    // For Say: the player's own words, ENGINE-SET AND NEVER MODEL-SET
    // (REQ-NPCTALK-6). parse() puts the remainder of the line here; aiResolve
    // puts the whole raw line here. No model response is ever read into it, and
    // the emit_action schema has no text property, so a model cannot paraphrase
    // what the player said. Empty for every other verb.
    std::string text;
};

// Parse one input line into an Action, consulting the world db for noun
// recognition. Returns nullopt when the line is unparseable.
std::optional<Action> parse(Db& db, const std::string& line);
