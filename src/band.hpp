// The status band: the engine-authored block printed above the prompt every
// turn (REQ-UI-1..-18, -33..-36). READ-ONLY BY CONTRACT — this translation
// unit contains only SELECT statements. No INSERT, UPDATE, or DELETE may ever
// appear here (REQ-UI-2), the same discipline render.cpp lives under, and a
// static source scan in the suite enforces it.
//
// The band NEVER sees narration text. That is structural, not a convention:
// composeBand() takes a Db and a width and nothing else, so no model-supplied
// string can reach a styled span (REQ-UI-25) and no model-supplied quantity can
// reach a number (REQ-UI-40).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "db.hpp"
#include "term.hpp"

// One styled field inside a row's content. Splitting content into spans rather
// than pre-colored strings is what keeps REQ-UI-22/-33 honest: the layout's
// width arithmetic measures `text` (plain code points) and never an escape
// byte, because styling is applied at EMISSION, after the line breaks are
// decided. `pad` is the unstyled separator that follows the field — "  "
// between fields, ", " inside a list — and is trimmed away at a line break.
struct BandSpan {
    std::string text;
    Color color = Color::None;
    bool bold = false;
    std::string pad = "  ";
};

// One labelled row. A row whose spans are all empty renders NOTHING at all,
// which is what makes REQ-UI-8's conditional rows fall out of the layout rather
// than being re-decided per row type.
struct BandRow {
    std::string label;
    std::vector<BandSpan> spans;
};

// Layout constants (plan Decision 3). One leading space, the label left-aligned
// in an 8-wide field, one space — so every row shares content column 11
// (1-based) and the hanging indent of REQ-UI-33 is uniform.
inline constexpr int kBandLabelWidth = 8;
inline constexpr int kBandIndent = 1 + kBandLabelWidth + 1;  // 10 leading columns

// Compose the frame: header rule, labelled rows, continuation lines indented to
// the content column. PURE — no db, no environment. All framing bytes are ASCII
// (REQ-UI-29): no box-drawing and no middle dot, both of which are East Asian
// Ambiguous width, which would make the TERMINAL decide the column count.
//
// Nothing is ever truncated or elided (REQ-UI-33a). The only width-driven
// degradation is dropping the rules (REQ-UI-33b), which happens when the room
// name alone leaves no room for them. Returns text ending in a newline, or ""
// when there is nothing at all to show.
std::string layoutBand(std::string_view roomName, const std::vector<BandRow>& rows,
                       int width, TermStyle style);

// The band for the world's current state, laid out at `width`. Read-only, and
// runs OUTSIDE the tick transaction (REQ-UI-2). Throws if the world has no
// player entity or no location row; the caller degrades rather than failing the
// turn (see loop.cpp).
std::string composeBand(Db& db, int width, TermStyle style);

// composeBand() under the process's cached terminal style.
std::string composeBand(Db& db, int width);

// The player's known spells and what they do — element, cooldown, and effect
// from spell_catalog (REQ-UI-37), for KNOWN spells only (REQ-UI-38); the
// catalog is not a spoiler list. Read-only.
//
// REQ-UI-39b: the no-tick route that reaches this function is a BOUNDED
// exception to the project's rule that all player-visible output renders from
// events rows. IT MUST NOT GENERALIZE. No other command may produce output
// outside the events model on this one's authority; a future addition wanting
// the same treatment needs its own decision, not an appeal to this one.
std::string renderSpellRules(Db& db, int64_t player, TermStyle style);

// Elements this player has already tested against `archetype`, derived from the
// events transcript (REQ-UI-46) — no cache, no shadow table. Each entry is a
// rendered fact ("fire x1/2", "frost x1"), including the discovered ABSENCE of
// a resistance (REQ-UI-42a), which must read differently from never having
// tried. Untested elements yield nothing (REQ-UI-42).
std::vector<std::string> discoveredResistances(Db& db, const std::string& archetype);
