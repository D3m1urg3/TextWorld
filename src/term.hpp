// Terminal services: color gating, SGR emission, width detection, and UTF-8
// prose wrapping. PURE BY CONTRACT — this translation unit includes no db
// header, holds no game state, and knows nothing about the world. Everything
// here is either a pure function of its arguments or a thin wrapper reading
// the process environment / stdout.
//
// Each capability is deliberately split in two: a PURE predicate taking its
// inputs explicitly (styleFor, widthFrom, clampWidth) and a thin wrapper that
// supplies them from the environment (currentStyle, detectWidth). The pure
// halves are what the tests drive, so the truth tables are exercised without
// a tty and without mutating the developer's shell.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Whether this run may emit color, and whether it may emit non-color SGR
// attributes (bold). The two are SEPARATE flags because NO_COLOR suppresses
// color only — bold survives (REQ-UI-23) — whereas TERM=dumb suppresses every
// escape sequence including bold (REQ-UI-21).
struct TermStyle {
    bool color = false;
    bool attrs = false;
};

// The basic 16 named ANSI colors and nothing else (REQ-UI-19): no 256-color,
// no truecolor, so every emitted color resolves through the user's terminal
// theme. `None` means "emit no color sequence at all".
enum class Color {
    None,
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
    BrightBlack,
    BrightRed,
    BrightGreen,
    BrightYellow,
    BrightBlue,
    BrightMagenta,
    BrightCyan,
    BrightWhite,
};

// The color-gate precedence (REQ-UI-20/-21), as a pure function of the four
// environment variables and whether stdout is a tty. A null OR EMPTY string is
// treated as unset, per REQ-UI-20's "empty variables treated as unset".
//
// `attrs` is a CAPABILITY test, evaluated independently of the color
// precedence: attrs = TERM != dumb AND (isTty OR CLICOLOR_FORCE). Bold is worth
// emitting only to something that interprets escapes, so a piped or redirected
// run emits no escape byte of ANY kind — the property check 7 asserts against
// the real binary. NO_COLOR keeps bold alive on a TERMINAL (REQ-UI-23); it does
// not resurrect it into a pipe.
//
// `color` is evaluated in this order:
//   0. TERM=dumb            -> false  (and attrs false too — REQ-UI-21)
//   1. NO_COLOR non-empty   -> false  (REQ-UI-23)
//   2. CLICOLOR_FORCE != 0  -> true
//   3. CLICOLOR == 0        -> false
//   4. otherwise            -> isTty
//
// Step 3 is an INTERPRETATION of REQ-UI-20's table, recorded here because the
// table does not state it outright: rows 3 and 4 of that table carry identical
// results, so row 3 ("CLICOLOR set, non-0") only carries information if the
// zero case differs. Treating CLICOLOR=0 as a suppression also matches the
// variable's established meaning everywhere else it is honored.
TermStyle styleFor(const char* noColor, const char* clicolorForce,
                   const char* clicolor, const char* term, bool isTty);

// styleFor() applied to the real environment and isatty(STDOUT_FILENO),
// cached at static-init time the way profilingEnabled() is (profile.cpp:22):
// the getenv/isatty pair happens once per process.
TermStyle currentStyle();

// Re-read the environment into currentStyle()'s cache. Mirrors
// profileRefreshEnabled() (profile.cpp:66) so a test can flip an env var and
// observe the new gate without spawning a process.
void termRefreshStyle();

// Wrap `text` in the SGR sequence for `color`, or return it UNCHANGED when
// color is suppressed. Suppression means no escape bytes at all — never an
// empty sequence and never a bare reset around plain text (REQ-UI-22).
std::string colorize(std::string_view text, Color color, TermStyle style);

// Bold. Survives NO_COLOR (REQ-UI-23); dies under TERM=dumb (REQ-UI-21).
std::string bolden(std::string_view text, TermStyle style);

// Bold AND colored in ONE sequence (e.g. "\x1b[1;91m" for the telegraph).
// Degrades a step at a time: with color off but attrs on, emits bold alone;
// with both off, emits nothing. This is what keeps the telegraph the loudest
// element under color, still distinct under NO_COLOR, and identifiable from
// its text alone under TERM=dumb.
std::string boldColor(std::string_view text, Color color, TermStyle style);

// Strip every SGR sequence from `s`. Used by the tests to compare a colored
// run against a colorless one, and by the width arithmetic's assertions.
std::string stripSgr(std::string_view s);

// The floor from REQ-UI-27. Anything below it — including 0 and negatives —
// is treated as 20. There is no ceiling: a very wide terminal is not a problem.
inline constexpr int kMinWidth = 20;

// The last-resort default from REQ-UI-26 when neither ioctl nor COLUMNS answers.
inline constexpr int kDefaultWidth = 80;

// Clamp a reported width to the floor (REQ-UI-27).
int clampWidth(int reported);

// The width fallback chain (REQ-UI-26/-27) as a pure function. `ioctlOk` is
// true only when the ioctl RETURNED SUCCESS **and** reported a non-zero column
// count — the piped/CI case reports rc=-1, cols=0 and must fall through.
// `columns` is the raw COLUMNS value (null/empty = unset); it is parsed
// STRICTLY, so garbage, zero, and negatives fall through to kDefaultWidth
// rather than yielding a zero width.
int widthFrom(bool ioctlOk, int ioctlCols, const char* columns);

// widthFrom() applied to a fresh ioctl(TIOCGWINSZ) on STDOUT_FILENO and the
// live COLUMNS. Queried at print time, every turn — no SIGWINCH handler, so a
// resize is picked up on the next turn (REQ-UI-28). Always returns >= kMinWidth.
int detectWidth();

// Pin detectWidth() to a fixed value for the whole process; 0 restores the
// normal detection chain. This exists for the TEST SUITE, which otherwise
// measures the developer's real terminal and would assert against a different
// layout under a pipe than under a tty. Same shape and purpose as
// profileSetSink() (profile.hpp). The game binary never calls it, so REQ-UI-26's
// chain is what ships.
void termSetWidthOverride(int width);

// Number of UTF-8 code points in `s` (continuation bytes are not counted).
// This is the unit REQ-UI-31 wraps by: an em-dash is one column, not three.
// No wcwidth: the game ships no CJK content, so full East Asian width
// measurement would be disproportionate.
size_t utf8Length(std::string_view s);

// Wrap prose to `width` code points at whitespace boundaries (REQ-UI-30/-31/-32).
//   - No word is ever split. A single word longer than `width` overflows onto
//     its own over-long line instead.
//   - Existing newlines are preserved verbatim, so a blank line stays a
//     paragraph break and two source lines are never joined.
//   - A trailing newline in the input survives in the output.
std::string wrapProse(const std::string& text, int width);
