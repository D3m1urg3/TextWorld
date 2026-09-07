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
    // Emit `color` as a BACKGROUND rather than a foreground (REQ-POLISH-11).
    // Only the health bar sets this: its text is spaces, which carry no
    // foreground. Styling still happens at EMISSION, so the width arithmetic
    // measures the spaces and never an escape byte.
    bool background = false;
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

// The bar's width, in columns, in BOTH modes (REQ-POLISH-10, REQ-POLISH-10a) —
// ten plus the one separating space is the 11 columns a bar may add to a row.
inline constexpr int kHealthBarWidth = 10;

// Cells inside the bar, per mode. Under colour every one of the ten columns is
// a cell. Without colour two of them are spent on the `[` and `]` REQ-POLISH-12
// asks for, leaving eight — which is what keeps REQ-POLISH-10a's budget intact
// in the degraded mode too. The fill fraction is the same in both; only the
// resolution differs.
inline constexpr int kHealthBarCellsColor = kHealthBarWidth;       // 10
inline constexpr int kHealthBarCellsAscii = kHealthBarWidth - 2;   // 8

// A health bar as spans, ready to drop into a row beside the numbers it
// illustrates (REQ-POLISH-8, -9, -10, -12). PURE — no db, no environment.
// `fill` is the colour of the row it belongs to, so the bar and the number it
// illustrates read as one fact (REQ-UI-24) rather than as two.
//
// Under colour: kHealthBarCellsColor columns of SPACES, never a block
// character — `█` and `░` are East Asian Ambiguous width and would let the
// TERMINAL decide the column count, the defect REQ-UI-29 already rules out for
// `─` and `·`. Filled cells carry `fill` as a background; empty cells carry
// BrightBlack, the colour the band already gives something out of reach.
//
// Without colour: `[` + kHealthBarCellsAscii cells + `]`, `#` filled and `.`
// empty. Same ten columns, so a row's width never moves between modes either.
//
// Filled = round(cells * current / max), with a floor of ONE filled cell while
// current > 0, so a living enemy never shows an empty bar. `max <= 0` yields no
// spans at all, so a row with no health simply has no bar rather than a
// division by zero.
//
// TWO steps of degradation and no third: there is no reverse-video step
// (REQ-POLISH-12).
std::vector<BandSpan> healthBarSpans(int64_t current, int64_t max, Color fill,
                                     TermStyle style);

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
