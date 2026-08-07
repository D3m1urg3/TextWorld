// Action: the parsed intent handed to resolution. This struct is the seam
// for a future Action Resolver agent — it must stay free of parser types.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit, Attack, Cast, Read,
                  Spells, Examine };

struct Action {
    Verb verb;
    int64_t subject = 0;   // entity id for Take/Drop/Examine; target enemy for Attack
                           // (0 = the hostile in the room); 0 when unused
    std::string direction; // for Go; empty when unused
    std::string spell;     // for Cast: the catalogued spell key; empty otherwise
                           // (spells are string-keyed, not entities, so they
                           // ride their own field rather than `subject`)
};

// Parse one input line into an Action, consulting the world db for noun
// recognition. Returns nullopt when the line is unparseable.
std::optional<Action> parse(Db& db, const std::string& line);
