// Terminal services (see term.hpp for the contract). PURE — no db include,
// no game state. The only impurity is confined to currentStyle()/detectWidth(),
// which read the environment and stdout for the pure predicates below them.
#include "term.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

// REQ-UI-20: a null OR EMPTY variable is unset.
bool isSet(const char* v) { return v != nullptr && v[0] != '\0'; }

// "Set and not exactly 0" — the shape both CLICOLOR_FORCE and CLICOLOR use.
bool isSetNonZero(const char* v) { return isSet(v) && std::string(v) != "0"; }

// The SGR parameter for each of the basic 16 (REQ-UI-19). Bright variants are
// the 90-97 range rather than a bold+dim-color composite, so "bright" and
// "bold" stay independently controllable — which REQ-UI-21/-23 require, since
// they suppress the two on different triggers.
const char* sgrParam(Color color) {
    switch (color) {
        case Color::None: return nullptr;
        case Color::Black: return "30";
        case Color::Red: return "31";
        case Color::Green: return "32";
        case Color::Yellow: return "33";
        case Color::Blue: return "34";
        case Color::Magenta: return "35";
        case Color::Cyan: return "36";
        case Color::White: return "37";
        case Color::BrightBlack: return "90";
        case Color::BrightRed: return "91";
        case Color::BrightGreen: return "92";
        case Color::BrightYellow: return "93";
        case Color::BrightBlue: return "94";
        case Color::BrightMagenta: return "95";
        case Color::BrightCyan: return "96";
        case Color::BrightWhite: return "97";
    }
    return nullptr;
}

// Wrap `text` in one SGR sequence carrying `params`, plus the reset. Callers
// have already decided the sequence is wanted; this never inspects a TermStyle.
std::string sgrWrap(std::string_view text, const std::string& params) {
    return "\x1b[" + params + "m" + std::string(text) + "\x1b[0m";
}

// Cached at static-init time (REQ-UI-20's inputs do not change mid-process
// under normal use). Mirrors profile.cpp's g_enabled: no other TU reads this
// during ITS static init, so the initialization order is unobservable.
TermStyle g_style = styleFor(std::getenv("NO_COLOR"),
                             std::getenv("CLICOLOR_FORCE"),
                             std::getenv("CLICOLOR"), std::getenv("TERM"),
                             isatty(STDOUT_FILENO) != 0);

// 0 = unset; see termSetWidthOverride(). Main-thread only, like g_enabled.
int g_widthOverride = 0;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

// Split `text` on '\n', KEEPING empty pieces — that is what makes a blank line
// survive as a paragraph break (REQ-UI-32). A trailing '\n' yields a trailing
// empty piece, so rejoining with '\n' reproduces it.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

// Greedy wrap of ONE source line. Words are whitespace-delimited; a word longer
// than `width` is emitted whole on its own line rather than split (REQ-UI-30).
std::vector<std::string> wrapOneLine(const std::string& line, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t curLen = 0;

    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSpace(line[i])) ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && !isSpace(line[i])) ++i;
        const std::string word = line.substr(start, i - start);
        const size_t wordLen = utf8Length(word);

        if (cur.empty()) {
            cur = word;
            curLen = wordLen;
        } else if (curLen + 1 + wordLen <= static_cast<size_t>(width)) {
            cur += " " + word;
            curLen += 1 + wordLen;
        } else {
            out.push_back(cur);
            cur = word;
            curLen = wordLen;
        }
    }

    // An all-whitespace or empty source line still emits one (empty) line, so
    // the paragraph structure is preserved exactly.
    out.push_back(cur);
    return out;
}

}  // namespace

TermStyle styleFor(const char* noColor, const char* clicolorForce,
                   const char* clicolor, const char* term, bool isTty) {
    // 0. TERM=dumb: the terminal cannot reliably interpret ANY escape sequence,
    // so nothing styled is emitted — the one case that also kills bold
    // (REQ-UI-21). Checked first: it is a statement about terminal capability,
    // which no color PREFERENCE variable can override.
    if (isSet(term) && std::string(term) == "dumb") return {false, false};

    // Attributes are a CAPABILITY question, not a preference one: bold is worth
    // emitting only to something that interprets escape sequences at all. A
    // pipe or a file does not, so a redirected run emits NO escape byte of any
    // kind — which is what makes piped output clean even though NO_COLOR (below)
    // would otherwise leave bold enabled. Forcing overrides the tty test here
    // exactly as it does for color.
    const bool forced = isSetNonZero(clicolorForce);
    const bool attrs = isTty || forced;

    // 1. NO_COLOR governs COLOR ONLY (REQ-UI-23), per the NO_COLOR spec, so the
    // telegraph keeps its bold on a colorless TERMINAL.
    if (isSet(noColor)) return {false, attrs};

    // 2. CLICOLOR_FORCE: color regardless of tty. CLICOLOR_FORCE=0 is not a
    // suppression, only an absence of forcing — it falls through.
    if (forced) return {true, attrs};

    // 3. CLICOLOR=0 suppresses. See term.hpp for why this reading of
    // REQ-UI-20's table is the one that makes its row 3 carry information.
    if (isSet(clicolor) && std::string(clicolor) == "0") return {false, attrs};

    // 4. Default, and also CLICOLOR=1: color iff stdout is a tty.
    return {isTty, attrs};
}

TermStyle currentStyle() { return g_style; }

void termRefreshStyle() {
    g_style = styleFor(std::getenv("NO_COLOR"), std::getenv("CLICOLOR_FORCE"),
                       std::getenv("CLICOLOR"), std::getenv("TERM"),
                       isatty(STDOUT_FILENO) != 0);
}

std::string colorize(std::string_view text, Color color, TermStyle style) {
    const char* param = sgrParam(color);
    // REQ-UI-22: suppressed means the plain bytes, not an empty sequence.
    if (!style.color || param == nullptr) return std::string(text);
    return sgrWrap(text, param);
}

std::string bolden(std::string_view text, TermStyle style) {
    // Exactly boldColor with no color: same degradation, one implementation.
    return boldColor(text, Color::None, style);
}

std::string boldColor(std::string_view text, Color color, TermStyle style) {
    const char* param = sgrParam(color);
    if (style.attrs && style.color && param != nullptr) {
        return sgrWrap(text, std::string("1;") + param);
    }
    // Color off but attrs on: bold alone still marks it out (REQ-UI-23).
    if (style.attrs) return sgrWrap(text, "1");
    return std::string(text);
}

std::string stripSgr(std::string_view s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') ++i;
            if (i < s.size()) ++i;  // consume the 'm'
            continue;
        }
        out += s[i];
        ++i;
    }
    return out;
}

int clampWidth(int reported) {
    return reported < kMinWidth ? kMinWidth : reported;
}

int widthFrom(bool ioctlOk, int ioctlCols, const char* columns) {
    if (ioctlOk && ioctlCols > 0) return clampWidth(ioctlCols);

    if (isSet(columns)) {
        // Strict parse: the WHOLE value must be a positive integer. "banana",
        // "80x24", "0" and "-5" all fall through to the default rather than
        // producing a zero or negative width (REQ-UI-27).
        char* end = nullptr;
        const long parsed = std::strtol(columns, &end, 10);
        if (end != nullptr && *end == '\0' && parsed > 0) {
            return clampWidth(static_cast<int>(parsed));
        }
    }

    return kDefaultWidth;
}

int proseWidth(int detected) {
    const int capped = detected < kProseMaxWidth ? detected : kProseMaxWidth;
    const int wrapped = capped - kProseIndent;
    const int floored = kMinWidth - kProseIndent;
    return wrapped < floored ? floored : wrapped;
}

int detectWidth() {
    // The test-suite pin, checked before the chain so a suite run lays out the
    // same under a pipe as under a tty. Never set by the game binary.
    if (g_widthOverride > 0) return clampWidth(g_widthOverride);

    // REQ-UI-26: the STDOUT descriptor, not stdin — output is what is being
    // laid out. Re-queried every call (REQ-UI-28).
    struct winsize ws {};
    const bool ok = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0;
    return widthFrom(ok, static_cast<int>(ws.ws_col), std::getenv("COLUMNS"));
}

void termSetWidthOverride(int width) { g_widthOverride = width; }

size_t utf8Length(std::string_view s) {
    size_t n = 0;
    for (const char c : s) {
        // Continuation bytes are 10xxxxxx; every other byte starts a code point.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::string utf8Truncate(std::string_view s, size_t maxChars) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        // Same rule as utf8Length: a non-continuation byte starts a code point.
        // Cutting at THAT byte offset is what keeps the result valid UTF-8.
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == maxChars) return std::string(s.substr(0, i));
            ++n;
        }
    }
    return std::string(s);  // already short enough
}

std::string wrapProse(const std::string& text, int width) {
    if (width <= 0) return text;

    const std::vector<std::string> sourceLines = splitLines(text);
    std::string out;
    bool first = true;
    for (const std::string& line : sourceLines) {
        for (const std::string& wrapped : wrapOneLine(line, width)) {
            if (!first) out += "\n";
            out += wrapped;
            first = false;
        }
    }
    return out;
}
