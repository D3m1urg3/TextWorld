// AI prose renderer: this turn's event rows → LLM-written prose. Design §6.
//
// READ-ONLY BY CONTRACT — this unit performs ONLY SELECTs against the DB;
// no INSERT, UPDATE, or DELETE may ever appear in its translation unit. It
// adds one new effect category — network egress — which the render contract
// didn't anticipate: reads world state, writes nothing, sends facts (not the
// DB) to the LLM. Otherwise it inherits render()'s contract verbatim:
// SELECTs only, and never an output claim without a sourcing event row.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "db.hpp"

// Render every event of `turn` as AI prose. Returns std::nullopt when AI
// rendering is unavailable (caller falls back to the template renderer).
std::optional<std::string> aiRender(Db& db, int64_t turn);
