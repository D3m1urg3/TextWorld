// Action: the parsed intent handed to resolution. This struct is the seam
// for a future Action Resolver agent — it must stay free of parser types.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

enum class Verb { Look, Go, Take, Drop, Inventory, Wait, Quit, Attack };

struct Action {
    Verb verb;
    int64_t subject = 0;   // entity id for Take/Drop; target enemy for Attack
                           // (0 = the hostile in the room); 0 when unused
    std::string direction; // for Go; empty when unused
};

// Parse one input line into an Action, consulting the world db for noun
// recognition. Returns nullopt when the line is unparseable.
std::optional<Action> parse(Db& db, const std::string& line);
