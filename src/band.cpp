// The status band (see band.hpp for the contract). READ-ONLY BY CONTRACT —
// only SELECT statements appear here. No INSERT, UPDATE, or DELETE may ever be
// added (REQ-UI-2, design §6), and the suite scans this file for them.
#include "band.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "architect.hpp"  // architectEnabled() — the latent-exit DISPLAY gate
#include "combat.hpp"     // hostileInRoom() — the shared "in combat" predicate

namespace {

// --- role → color (plan Decision 2) -----------------------------------------
//
// Ten roles, ten DISTINCT treatments — no color does two jobs (REQ-UI-24), and
// every colored fact is also carried by its text, so nothing is lost when color
// is suppressed. Basic 16 only (REQ-UI-19). The suite asserts these codes are
// pairwise distinct, because a mapping that is merely APPLIED but not distinct
// would pass every other check while violating "one role per color".
constexpr Color kExitColor = Color::Cyan;              // 36 — navigation
constexpr Color kObjectColor = Color::Green;           // 32 — takeable
constexpr Color kHostileColor = Color::Red;            // 31 — danger
constexpr Color kTelegraphColor = Color::BrightRed;    // 1;91 — loudest (bold)
constexpr Color kLowHpColor = Color::Yellow;           // 33 — warning
constexpr Color kSpellReadyColor = Color::BrightBlue;  // 94 — available action
constexpr Color kSpellCoolColor = Color::BrightBlack;  // 90 — recedes
constexpr Color kStatusColor = Color::Magenta;         // 35 — active magic
constexpr Color kResistColor = Color::BrightMagenta;   // 95 — knowledge of state
// Background only, and only for the player's health bar when health is fine —
// the one place the band needs a colour meaning "this much is left" rather than
// a colour meaning a role. See the note at its use site in playerRow.
constexpr Color kHealthyColor = Color::Green;          // 42 as a BACKGROUND
// The room-name header is BOLD with no color, so it survives NO_COLOR
// (REQ-UI-23); it is the band's anchor.

// Player HP is "low" at current * 3 <= max — integer arithmetic, no floats,
// matching the engine's no-float discipline. This threshold is invented by the
// implementation; nothing in the spec or the engine defines one, so its
// boundary is tested explicitly rather than assumed.
bool hpIsLow(int64_t current, int64_t max) { return current * 3 <= max; }

// --- read-only lookups ------------------------------------------------------

int64_t playerEntity(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("band: world has no player entity");
    return s.colInt(0);
}

int64_t roomOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) {
        throw std::runtime_error("band: entity " + std::to_string(entity) +
                                 " has no location row");
    }
    return s.colInt(0);
}

// REQ-UI-9: a room with no name row yields "", and the header renders its rule
// without a title rather than failing.
std::string nameOf(Db& db, int64_t entity) {
    Stmt s = db.prepare("SELECT value FROM name WHERE entity = ?");
    s.bind(1, entity);
    if (!s.step()) return "";
    return s.colText(0);
}

int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) return 0;
    return s.colInt(0);
}

bool rowExists(Db& db, const char* sql, int64_t entity) {
    Stmt s = db.prepare(sql);
    s.bind(1, entity);
    return s.step();
}

// --- layout helpers ---------------------------------------------------------

std::string rtrim(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(0, end);
}

// Apply one role's treatment to one word. Bold and color compose into a single
// SGR sequence; either may be suppressed independently (REQ-UI-21/-23).
std::string styleWord(std::string_view text, Color color, bool bold,
                      bool background, TermStyle style) {
    // REQ-POLISH-11: a health bar's text is spaces, which carry no foreground —
    // it is the BACKGROUND that draws the bar. Still applied at emission, so
    // the arithmetic above measures the spaces and never an escape byte.
    if (background) return bgColorize(text, color, style);
    return bold ? boldColor(text, color, style) : colorize(text, color, style);
}

// One whitespace-free word carrying its role's styling and the separator that
// follows it. Flattening spans to words is what lets a row wrap at ANY
// whitespace — inside a two-word monster name as readily as between fields.
//
// The separator is split into GLUE and GAP, and the distinction is load-bearing
// for REQ-UI-33. Glue is the separator's VISIBLE part (the comma of ", "); it
// belongs to the word before it and is emitted even when that word ends a line,
// so it must be counted in the line's width. Gap is the whitespace, which
// disappears at a line break and must NOT be counted. Folding the two together
// and trimming at emission is what silently produces a line one column too wide
// — the comma survives the trim while the arithmetic assumed it had gone.
struct Token {
    std::string word;
    Color color;
    bool bold;
    std::string glue;  // visible, stays with `word`
    std::string gap;   // whitespace, dropped at a line break
    bool background = false;
};

std::vector<Token> tokenize(const std::vector<BandSpan>& spans) {
    std::vector<Token> tokens;
    for (const BandSpan& span : spans) {
        if (span.text.empty()) continue;
        std::vector<std::string> words;
        std::string cur;
        for (const char c : span.text) {
            if (c == ' ') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) words.push_back(cur);
        // A span whose text is nothing BUT spaces — a health bar (REQ-POLISH-9).
        // Splitting it on its own spaces would erase it entirely, so it becomes
        // ONE atomic token instead. That is also the behaviour a bar wants: a
        // bar broken across two lines is not a bar.
        if (words.empty()) words.push_back(span.text);

        const std::string glue = rtrim(span.pad);
        const std::string gap = span.pad.substr(glue.size());
        for (size_t i = 0; i < words.size(); ++i) {
            const bool last = i + 1 == words.size();
            tokens.push_back({words[i], span.color, span.bold,
                              last ? glue : std::string(),
                              last ? gap : std::string(" "), span.background});
        }
    }
    return tokens;
}

// Greedy wrap of one row's content to `contentWidth` code points, styling each
// word as it is emitted — so the arithmetic below counts PLAIN code points and
// never an escape byte. A single word longer than contentWidth overflows onto
// its own line rather than being split; that is the one case REQ-UI-27's
// rationale explicitly accepts ("only a single word longer than the floor can
// overflow"), and it is why the 20-column floor is safe.
std::vector<std::string> wrapSpans(const std::vector<BandSpan>& spans,
                                   int contentWidth, TermStyle style) {
    const std::vector<Token> tokens = tokenize(spans);
    if (tokens.empty()) return {};  // REQ-UI-8: an empty row renders nothing

    std::vector<std::string> lines;
    std::string cur;
    size_t curLen = 0;
    std::string pendingGap;
    size_t pendingGapLen = 0;
    for (const Token& t : tokens) {
        // What this token costs once committed: its word plus its glue. The gap
        // that precedes it costs nothing if the line breaks here.
        const size_t need = utf8Length(t.word) + utf8Length(t.glue);
        if (!cur.empty() &&
            curLen + pendingGapLen + need > static_cast<size_t>(contentWidth)) {
            lines.push_back(cur);  // never ends in whitespace, so no trim needed
            cur.clear();
            curLen = 0;
        } else if (!cur.empty()) {
            cur += pendingGap;
            curLen += pendingGapLen;
        }
        cur += styleWord(t.word, t.color, t.bold, t.background, style) + t.glue;
        curLen += need;
        pendingGap = t.gap;
        pendingGapLen = utf8Length(t.gap);
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// " Label    " — one space, the label in an 8-wide left-aligned field, one
// space. Every row therefore shares content column 11 (1-based), which is what
// makes the hanging indent uniform (REQ-UI-33).
std::string labelPrefix(const std::string& label) {
    std::string out = " " + label;
    while (utf8Length(out) < static_cast<size_t>(1 + kBandLabelWidth)) out += " ";
    return out + " ";
}

// "-- Stone Hall ------...". All ASCII (REQ-UI-29). When the name leaves no
// room for even one dash, the RULES are dropped — the only width-driven
// degradation permitted (REQ-UI-33b) — and the bare name wraps to the width.
std::string headerLines(std::string_view roomName, int width, TermStyle style) {
    if (roomName.empty()) {
        // REQ-UI-9: a nameless room still gets its rule, just no title.
        return std::string(static_cast<size_t>(width), '-') + "\n";
    }

    const size_t titleLen = utf8Length(roomName);
    const size_t framed = 3 + titleLen + 1;  // "-- " + name + " "
    if (framed + 1 <= static_cast<size_t>(width)) {
        std::string out = "-- " + bolden(roomName, style) + " ";
        out += std::string(static_cast<size_t>(width) - framed, '-');
        return out + "\n";
    }

    // No room for the rule: keep every byte of the name (REQ-UI-33a), wrapped.
    std::string out;
    for (const std::string& line :
         wrapSpans({BandSpan{std::string(roomName), Color::None, true, "", false}},
                   width, style)) {
        out += line + "\n";
    }
    return out;
}

// --- health bars (REQ-POLISH-8..-12) ----------------------------------------

// round(cells * current / max) in integer arithmetic — (2*cells*current + max)
// / (2*max) — with a floor of ONE while current > 0, so a living enemy never
// shows an empty bar, and a ceiling of `cells` so an over-full health row
// (current > max) cannot overrun the bar's width.
int filledCells(int64_t current, int64_t max, int cells) {
    if (current <= 0) return 0;
    if (current >= max) return cells;
    const int64_t n = (2 * cells * current + max) / (2 * max);
    return n < 1 ? 1 : static_cast<int>(n);
}

}  // namespace

std::vector<BandSpan> healthBarSpans(int64_t current, int64_t max, Color fill,
                                     TermStyle style) {
    if (max <= 0) return {};  // no health, no bar — and no division by zero

    if (!style.color) {
        // The degraded form: `[` + eight cells + `]`, ten columns like the
        // coloured one, so REQ-POLISH-10a's budget holds in both modes. Two of
        // the ten columns buy the delimiters REQ-POLISH-12 asks for; the
        // fraction is the same, only its resolution is coarser.
        const int n = filledCells(current, max, kHealthBarCellsAscii);
        std::string bar = "[";
        bar.append(static_cast<size_t>(n), '#');
        bar.append(static_cast<size_t>(kHealthBarCellsAscii - n), '.');
        bar += "]";
        // No spaces in it, so it is one word and cannot be wrapped apart.
        return {BandSpan{bar, Color::None, false, "  ", false}};
    }

    // The coloured form: ten columns of SPACES carrying a background. Filled
    // cells take the row's own colour, so the bar and the number beside it read
    // as one fact; empty cells take BrightBlack, which the band already uses for
    // something out of reach.
    const int n = filledCells(current, max, kHealthBarCellsColor);
    std::vector<BandSpan> spans;
    if (n > 0) {
        spans.push_back({std::string(static_cast<size_t>(n), ' '), fill, false,
                         "", true});
    }
    if (n < kHealthBarCellsColor) {
        spans.push_back({std::string(static_cast<size_t>(kHealthBarCellsColor - n), ' '),
                         kSpellCoolColor, false, "", true});
    }
    if (!spans.empty()) spans.back().pad = "  ";
    return spans;
}

namespace {

// Append a health bar after the "HP: n/m" span that must already be the last
// one in `spans`.
//
// REQ-POLISH-10 buys the bar ten columns plus ONE space of separation, and
// REQ-POLISH-10a caps its whole cost to a row at eleven. The band's usual gap
// between fields is TWO spaces, which would make it twelve — so the preceding
// span's pad is narrowed to one space for exactly this join. The bar's own
// trailing pad stays the band's usual two, so whatever follows it (a
// [WINDING UP], a resistance, a spell) sits where every other field does.
void appendHealthBar(std::vector<BandSpan>& spans, int64_t current, int64_t max,
                     Color fill, TermStyle style) {
    const std::vector<BandSpan> bar = healthBarSpans(current, max, fill, style);
    if (bar.empty()) return;
    if (!spans.empty()) spans.back().pad = " ";
    for (const BandSpan& span : bar) spans.push_back(span);
}

// --- row builders -----------------------------------------------------------

// A comma-separated list row. The last item carries no separator, so the row
// never ends in a dangling comma at any width.
BandRow listRow(const char* label, const std::vector<std::string>& items,
                Color color) {
    BandRow row{label, {}};
    for (size_t i = 0; i < items.size(); ++i) {
        row.spans.push_back(
            {items[i], color, false, i + 1 == items.size() ? "" : ", "});
    }
    return row;
}

// REQ-UI-10: the SAME query shape roomBlock uses (render.cpp:83-88) — same
// ORDER BY, same latent-exit visibility gate — so the band and the room block
// can never disagree, and a latent exit stays textually indistinguishable from
// a realized one.
BandRow exitsRow(Db& db, int64_t room) {
    Stmt s = db.prepare(
        "SELECT direction FROM exits WHERE room = ? "
        "AND (dest IS NOT NULL OR ?) ORDER BY direction");
    s.bind(1, room);
    s.bind(2, architectEnabled() ? 1 : 0);
    std::vector<std::string> dirs;
    while (s.step()) dirs.push_back(s.colText(0));
    return listRow("Exits", dirs, kExitColor);
}

// REQ-UI-11: portableNamesIn's shape (render.cpp:44-54) — entity order, room as
// container.
BandRow objectsRow(Db& db, int64_t room) {
    Stmt s = db.prepare(
        "SELECT n.value FROM portable p "
        "JOIN location l ON l.entity = p.entity "
        "JOIN name n ON n.entity = p.entity "
        "WHERE l.container = ? ORDER BY p.entity");
    s.bind(1, room);
    std::vector<std::string> items;
    while (s.step()) items.push_back(s.colText(0));
    return listRow("Objects", items, kObjectColor);
}

// A hostile's or the player's active states, as KIND plus REMAINING TURNS —
// never magnitude (REQ-UI-35). The player can wait out a slow or burn down a
// DoT; magnitude supports no such decision. Ordered by kind for determinism.
void appendStatusSpans(Db& db, int64_t entity, std::vector<BandSpan>& spans) {
    if (rowExists(db, "SELECT 1 FROM barrier WHERE entity = ?", entity)) {
        // barrier has no duration, so it renders bare.
        spans.push_back({"barrier", kStatusColor, false, "  "});
    }
    Stmt s = db.prepare(
        "SELECT kind, remaining FROM status_effects WHERE entity = ? ORDER BY kind");
    s.bind(1, entity);
    while (s.step()) {
        spans.push_back({s.colText(0) + " " + std::to_string(s.colInt(1)),
                         kStatusColor, false, "  "});
    }
}

// REQ-UI-12/-13: one row per LIVING hostile sharing the room, name plus current
// and maximum HP. The query reuses resolveCombat's swarm shape
// (combat.cpp:525-529) so band order and combat order are the same order by
// construction.
std::vector<BandRow> hostileRows(Db& db, int64_t room, TermStyle style) {
    std::vector<int64_t> bodies;
    {
        Stmt s = db.prepare(
            "SELECT h.entity FROM hostile h "
            "JOIN location l ON l.entity = h.entity "
            "WHERE l.container = ? ORDER BY h.entity");
        s.bind(1, room);
        while (s.step()) bodies.push_back(s.colInt(0));
    }

    std::vector<BandRow> rows;
    for (const int64_t body : bodies) {
        int64_t current = 0;
        int64_t max = 0;
        {
            Stmt s = db.prepare("SELECT current, max FROM health WHERE entity = ?");
            s.bind(1, body);
            if (!s.step()) continue;
            current = s.colInt(0);
            max = s.colInt(1);
        }
        if (current <= 0) continue;  // not a living hostile

        // The label is the fixed word "Enemy" with the NAME in the content, so a
        // long monster name never shifts the shared content column.
        BandRow row{"Enemy", {}};
        row.spans.push_back({nameOf(db, body), kHostileColor, false, "  "});
        row.spans.push_back({"HP: " + std::to_string(current) + "/" +
                                 std::to_string(max),
                             kHostileColor, false, "  "});
        // REQ-POLISH-8: ALONGSIDE the numbers, never instead of them — a bar
        // alone cannot tell 3 HP from 4, and carrying numbers the player can
        // trust over the prose is the band's job. Immediately after HP: n/m,
        // and BEFORE [WINDING UP], so the loudest thing in the row stays the
        // last thing the eye lands on (REQ-UI-34). It takes the row's own
        // colour, so bar and number read as one fact.
        appendHealthBar(row.spans, current, max, kHostileColor, style);

        // REQ-UI-34: a pending strike is the loudest thing in the band. Text
        // alone identifies it, which is what keeps it legible under TERM=dumb
        // where the bold is stripped.
        if (rowExists(db, "SELECT 1 FROM pending_strike WHERE entity = ?", body)) {
            row.spans.push_back({"[WINDING UP]", kTelegraphColor, true, "  "});
        }

        appendStatusSpans(db, body, row.spans);

        // REQ-UI-41/-42: only what this transcript has already taught.
        {
            Stmt s = db.prepare("SELECT archetype FROM hostile WHERE entity = ?");
            s.bind(1, body);
            if (s.step()) {
                for (const std::string& fact : discoveredResistances(db, s.colText(0))) {
                    row.spans.push_back({fact, kResistColor, false, "  "});
                }
            }
        }

        if (!row.spans.empty()) row.spans.back().pad = "";
        rows.push_back(row);
    }
    return rows;
}

// REQ-UI-15/-16/-17: HP unconditionally; spell readiness only in combat.
BandRow playerRow(Db& db, int64_t player, int64_t room, TermStyle style) {
    BandRow row{"You", {}};

    {
        Stmt s = db.prepare("SELECT current, max FROM health WHERE entity = ?");
        s.bind(1, player);
        if (s.step()) {
            const int64_t current = s.colInt(0);
            const int64_t max = s.colInt(1);
            const bool low = hpIsLow(current, max);
            row.spans.push_back(
                {"HP: " + std::to_string(current) + "/" + std::to_string(max),
                 low ? kLowHpColor : Color::None, false, "  "});
            // REQ-POLISH-8, same placement as the hostile rows.
            //
            // The bar's fill is NOT the HP number's own colour, which is
            // Color::None while health is fine (REQ-UI-16). bgColorize with
            // Color::None emits nothing, and a bar with no colour is not a bar —
            // seen directly in a real fight, where a healthy player's bar came
            // out as ten plain spaces. So a healthy bar is green and a low one
            // takes the number's yellow, which is the one case where the two
            // must agree.
            //
            // Green here does not collide with kObjectColor under REQ-UI-24's
            // one-role-one-colour rule: this is a BACKGROUND, a channel nothing
            // else in the band uses, and a solid block of colour is not read as
            // coloured text. That separation is the same one REQ-POLISH-13 makes.
            appendHealthBar(row.spans, current, max,
                            low ? kLowHpColor : kHealthyColor, style);
        }
    }

    // REQ-UI-36: the player's own states, notably a held ward — it decides
    // whether an incoming telegraphed strike lands.
    appendStatusSpans(db, player, row.spans);

    // REQ-UI-18: "in combat" is the engine's OWN predicate, so the band and
    // combat resolution can never disagree about whether a fight is happening.
    if (hostileInRoom(db, room) != 0) {
        // REQ-UI-17, reproducing combatStatusLine's semantics exactly
        // (combat.cpp:495-511): alphabetical by spell, first letter
        // capitalized, "ready" when ready_turn - now <= 0 OR no cooldowns row
        // exists at all, else the integer turns remaining. The absent row is a
        // state meaning ready, not a third thing to display. The suite asserts
        // parity against that helper, which REQ-UI-6a keeps alive as the oracle.
        const int64_t now = currentTurn(db);
        Stmt s = db.prepare(
            "SELECT ks.spell, c.ready_turn, c.ready_turn IS NULL "
            "FROM known_spells ks "
            "LEFT JOIN cooldowns c ON c.entity = ks.entity AND c.spell = ks.spell "
            "WHERE ks.entity = ? ORDER BY ks.spell");
        s.bind(1, player);
        while (s.step()) {
            std::string spell = s.colText(0);
            if (!spell.empty()) {
                spell[0] = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(spell[0])));
            }
            const bool noCooldown = s.colInt(2) != 0;
            const int64_t remaining = s.colInt(1) - now;
            const bool ready = noCooldown || remaining <= 0;
            row.spans.push_back({spell + ": " +
                                     (ready ? "ready" : std::to_string(remaining)),
                                 ready ? kSpellReadyColor : kSpellCoolColor, false,
                                 "  "});
        }
    }

    if (!row.spans.empty()) row.spans.back().pad = "";
    return row;
}

// A plain-English gloss for every effect keyword the catalog defines
// (seed/base.sql:115-122). All seven are present deliberately: `stun` is a real,
// currently-learnable spell, and an incomplete table would leave a player who
// knows it staring at a blank effect. An UNRECOGNIZED keyword falls back to the
// raw keyword rather than rendering empty, so a future spell added to the
// catalog degrades instead of vanishing.
std::string effectGloss(const std::string& effect) {
    if (effect == "ward") return "blocks one telegraphed strike";
    if (effect == "stun") return "interrupts a winding-up strike";
    if (effect == "damage") return "elemental damage to the enemy";
    if (effect == "frost") return "elemental damage and a brief slow";
    if (effect == "dot") return "damage each turn for several turns";
    if (effect == "dispel") return "strips an enemy barrier";
    if (effect == "aoe") return "damage and a burn to every enemy present";
    return effect;
}

}  // namespace

std::string layoutBand(std::string_view roomName, const std::vector<BandRow>& rows,
                       int width, TermStyle style) {
    const int w = clampWidth(width);
    const int contentWidth = std::max(1, w - kBandIndent);

    std::string out = headerLines(roomName, w, style);
    for (const BandRow& row : rows) {
        const std::vector<std::string> lines = wrapSpans(row.spans, contentWidth, style);
        for (size_t i = 0; i < lines.size(); ++i) {
            // REQ-UI-33: continuation lines are indented to the row's CONTENT
            // column, not to column 0.
            out += (i == 0 ? labelPrefix(row.label)
                           : std::string(static_cast<size_t>(kBandIndent), ' '));
            out += lines[i] + "\n";
        }
    }
    return out;
}

std::string composeBand(Db& db, int width, TermStyle style) {
    const int64_t player = playerEntity(db);
    const int64_t room = roomOf(db, player);

    std::vector<BandRow> rows;
    rows.push_back(exitsRow(db, room));
    rows.push_back(objectsRow(db, room));
    for (BandRow& row : hostileRows(db, room, style)) rows.push_back(std::move(row));
    rows.push_back(playerRow(db, player, room, style));

    // REQ-UI-14: the room's NAME, never its description — the band repeats the
    // name and leaves the prose to its existing move/look trigger.
    return layoutBand(nameOf(db, room), rows, width, style);
}

std::string composeBand(Db& db, int width) {
    return composeBand(db, width, currentStyle());
}

std::string renderSpellRules(Db& db, int64_t player, TermStyle style) {
    // REQ-UI-38: known spells only — the catalog is never enumerated wholesale.
    Stmt s = db.prepare(
        "SELECT ks.spell, sc.element, sc.cooldown, sc.effect, sc.element IS NULL "
        "FROM known_spells ks "
        "JOIN spell_catalog sc ON sc.spell = ks.spell "
        "WHERE ks.entity = ? ORDER BY ks.spell");
    s.bind(1, player);

    std::string out;
    while (s.step()) {
        const std::string spell = s.colText(0);
        // A NULL element renders as "none", never blank.
        const std::string element = s.colInt(4) != 0 ? "none" : s.colText(1);
        out += colorize(spell, kSpellReadyColor, style) + " — element: " + element +
               ", cooldown: " + std::to_string(s.colInt(2)) + ", " +
               effectGloss(s.colText(3)) + "\n";
    }
    if (out.empty()) return "You know no spells yet.\n";
    return "Spells you know:\n" + out;
}

std::vector<std::string> discoveredResistances(Db& db, const std::string& archetype) {
    // REQ-UI-46: derived from the transcript, with no cache and no shadow table.
    // Deleting a fight's events erases what that fight taught — the falsifiable
    // consequence the requirement names.
    //
    // Deliberately NOT filtered with LIKE ? || '|%': SQLite's LIKE treats '_' as
    // a single-character wildcard, and archetype tags contain underscores
    // ('goblin_grunt'), so that pattern would also match 'goblinXgrunt|fire' and
    // silently credit one archetype with another's discoveries. Splitting on the
    // FIRST '|' in code removes the entire wildcard class of bug, and also keeps
    // an archetype name containing '|' from corrupting the parse.
    std::vector<std::string> elements;
    {
        Stmt s = db.prepare(
            "SELECT DISTINCT detail FROM events "
            "WHERE verb IN ('burned','froze') AND detail IS NOT NULL "
            "ORDER BY detail");
        while (s.step()) {
            const std::string detail = s.colText(0);
            const size_t bar = detail.find('|');
            if (bar == std::string::npos) continue;
            if (detail.substr(0, bar) != archetype) continue;
            const std::string element = detail.substr(bar + 1);
            if (element.empty()) continue;
            // No de-duplication needed: SELECT DISTINCT already made the details
            // unique, and every detail surviving the filter above shares the
            // prefix "<archetype>|", so their element halves are unique too.
            elements.push_back(element);
        }
    }

    std::vector<std::string> facts;
    for (const std::string& element : elements) {
        Stmt s = db.prepare(
            "SELECT multiplier_num, multiplier_den FROM resistance "
            "WHERE archetype = ? AND element = ?");
        s.bind(1, archetype);
        s.bind(2, element);
        if (s.step()) {
            const int64_t num = s.colInt(0);
            const int64_t den = s.colInt(1);
            facts.push_back(element + " x" + std::to_string(num) +
                            (den == 1 ? "" : "/" + std::to_string(den)));
        } else {
            // REQ-UI-42a: a missing resistance row means NEUTRAL, and neutral
            // after testing must not look the same as untested — otherwise the
            // player cannot tell a tested element from an untried one and the
            // discovery mechanic teaches nothing.
            facts.push_back(element + " x1");
        }
    }
    return facts;
}
