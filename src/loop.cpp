// runTurn(): the per-line dispatch. This is the only place a tick transaction
// is opened, meta.turn is incremented, or a commit/rollback decision is made
// (REQ-PROTO-5: one prompt = at most one tick, exactly one turn increment).
#include "loop.hpp"

#include <exception>
#include <optional>
#include <stdexcept>

#include "action.hpp"
#include "architect.hpp"  // architectEnabled() — the latent-exit gate the fallback shares
#include "band.hpp"
#include "combat.hpp"
#include "log.hpp"       // the warn entry REQ-POLISH-6 adds to the band-failure path
#include "nlresolve.hpp"
#include "profile.hpp"
#include "prose.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "term.hpp"

namespace {

// The player entity, looked up fresh each turn (no caching machinery).
int64_t playerId(Db& db) {
    Stmt s = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!s.step()) throw std::runtime_error("world has no player entity");
    return s.colInt(0);
}

int64_t currentTurn(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'turn'");
    if (!s.step()) throw std::runtime_error("meta has no 'turn' row");
    return s.colInt(0);
}

// The room the player stands in (for the tick-start combat key).
int64_t roomOf(Db& db, int64_t player) {
    Stmt s = db.prepare("SELECT container FROM location WHERE entity = ?");
    s.bind(1, player);
    if (!s.step()) throw std::runtime_error("player has no location row");
    return s.colInt(0);
}

// The status band for the world as it now stands, or "" if composing it throws
// (REQ-UI-2: read-only, outside the tick transaction).
//
// The degrade is deliberate and is NOT in the spec. REQ-UI-3 demands a band on
// the EngineError path — where the world was just rolled back — and REQ-UI-9
// establishes the degrade-don't-throw posture ("render without a title rather
// than failing"). A band that crashed the turn it was meant to explain would be
// strictly worse than no band, so the failure is swallowed here rather than
// propagated. The suite tests this path directly rather than trusting it.
// REQ-POLISH-6: the last-resort exits line. Since step 5 the band is the ONLY
// route the exits reach the player, so a band failure must not silently take
// them with it. Same query shape as band.cpp's exitsRow and the roomBlock this
// replaced — same ORDER BY, same latent-exit gate (REQ-UI-10) — so the fallback
// says what the band would have said. Plain text at column 0: this path runs
// when the styled composition has already failed.
std::string fallbackExitsLine(Db& db) {
    Stmt player = db.prepare("SELECT entity FROM player LIMIT 1");
    if (!player.step()) return "";
    Stmt loc = db.prepare("SELECT container FROM location WHERE entity = ?");
    loc.bind(1, player.colInt(0));
    if (!loc.step()) return "";

    Stmt s = db.prepare(
        "SELECT direction FROM exits WHERE room = ? "
        "AND (dest IS NOT NULL OR ?) ORDER BY direction");
    s.bind(1, loc.colInt(0));
    s.bind(2, architectEnabled() ? 1 : 0);
    std::string dirs;
    while (s.step()) {
        if (!dirs.empty()) dirs += ", ";
        dirs += s.colText(0);
    }
    if (dirs.empty()) return "";
    return "Exits: " + dirs + ".\n";
}

std::string bandOrEmpty(Db& db, int width) {
    try {
        return composeBand(db, width);
    } catch (const std::exception& e) {
        // Today this logged nothing. REQ-POLISH-6: a band that fell over is now
        // the difference between the player knowing the exits and not, so it is
        // worth a line in the session log.
        logEmitf(LogLevel::Warn, "band", "band composition failed: %s", e.what());
        try {
            return fallbackExitsLine(db);
        } catch (const std::exception&) {
            // REQ-POLISH-6a: the likely case, not the exotic one — a band that
            // threw for want of a player or a location row will usually deny
            // the fallback its room too. Nothing in this spec may turn a
            // display failure into a failed turn, so the turn keeps its
            // narration and the game continues.
            return "";
        }
    }
}

// Dim every non-empty line of `text` independently, preserving the newlines.
// Per line rather than around the whole block, because one SGR pair spanning a
// newline leaves the colour set across the line break on some terminals, and
// because the indent must stay outside the escape bytes.
std::string dimEachLine(const std::string& text, TermStyle style) {
    std::string out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = nl == std::string::npos ? text.size() : nl;
        const std::string line = text.substr(pos, end - pos);
        if (line.empty()) {
            out += line;
        } else {
            // The indent stays plain: style the text, not the leading spaces.
            // indentProse never emits a whitespace-only line (REQ-POLISH-3a),
            // but npos here would throw, so it is handled rather than assumed.
            const size_t firstText = line.find_first_not_of(' ');
            if (firstText == std::string::npos) {
                out += line;
            } else {
                out += line.substr(0, firstText);
                out += colorize(line.substr(firstText), Color::BrightBlack, style);
            }
        }
        if (nl == std::string::npos) break;
        out += "\n";
        pos = nl + 1;
    }
    return out;
}

// The turn proper: resolution, the tick, and narration. Wrapped by runTurn
// below, which owns wrapping and the status band — so every return path here
// picks both up without this function knowing they exist.
TurnResult runTurnCore(Db& db, const std::string& line) {
    // Resolution: AI resolver -> parser fallback when narration is enabled
    // (REQ-RESOLVE-1, -2), otherwise the fixed-verb parser directly — a disabled
    // run never constructs a transport. Tier a: neither yields an Action ->
    // renderError, no transaction, no tick, no world write.
    // The `resolve` stage is semantic: it covers BOTH the AI resolver and the
    // fixed-verb parser (REQ-LAT-2/-6), not "was there a network call".
    std::optional<Action> action;
    {
        const ScopedStage resolveStage("resolve");
        action = aiNarrationEnabled() ? resolveOrParse(db, line) : parse(db, line);
    }
    if (!action) {
        return {TurnOutcome::NoTick, renderError("I don't understand that."),
                TurnPresentation::Error};
    }

    // Quit is handled before any transaction opens; it never reaches resolve.
    if (action->verb == Verb::Quit) {
        return {TurnOutcome::Quit, ""};
    }

    // Spell inspection (REQ-UI-37/-39): read-only reference information about
    // the RULES, answered without opening a transaction — meta.turn does not
    // move, no events row is written, and resolveCombat never runs, so no
    // hostile takes a turn. runTurn still appends the band, so the player sees
    // the fight state alongside the rules.
    //
    // REQ-UI-39b: this is a BOUNDED exception to the project's rule that all
    // player-visible output renders from events rows. IT MUST NOT GENERALIZE.
    // No other command may produce output outside the events model on its
    // authority; a future addition wanting the same treatment requires its own
    // decision, not an appeal to this one.
    if (action->verb == Verb::Spells) {
        // REQ-POLISH-3b: a reference table, not narration. It reads at column 0
        // with the band rather than in the narration column.
        return {TurnOutcome::NoTick,
                renderSpellRules(db, playerId(db), currentStyle()),
                TurnPresentation::Reference};
    }

    // Cast availability gate (REQ-COMBAT-7/-13): an unknown or still-recharging
    // spell is not a valid action — declined WITHOUT a tick (no turn, no enemy
    // turn), mirroring the tier-a no-Action path above. Cooldown gates
    // availability; it never costs the player a turn.
    if (action->verb == Verb::Cast) {
        if (const auto reason = castDenialReason(db, playerId(db), action->spell)) {
            return {TurnOutcome::NoTick, renderError(*reason), TurnPresentation::Error};
        }
    }

    // The tick: one transaction, one turn increment, resolve, enemy turn, commit.
    // The `tick` stage spans the whole transaction — including the architect
    // call resolveGo may make inside it, which is why `generate` is emitted as
    // NESTED in tick (profile.hpp) rather than as a sibling stage.
    {
        const ScopedStage tickStage("tick");
        db.begin();
        try {
            const int64_t player = playerId(db);
            // Capture the ROOM the player stands in at TICK START, before the
            // action can move them out (micro-decision 2): every enemy that was
            // present still takes its one turn even as the player flees.
            const int64_t startRoom = roomOf(db, player);
            db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
            resolve(db, *action, player);
            // The enemy-turn system fires after the player's action, in the SAME
            // transaction (REQ-COMBAT-2): the loop, not resolve, owns the tick.
            resolveCombat(db, player, startRoom);
            // The story arc's advance rule (REQ-ARC-STORE-15): once per turn,
            // inside the tick's transaction, after systems resolve and before
            // commit — so a step advance and the change that caused it are one
            // atomic fact, per mutations.hpp's "both or neither". It runs only
            // on turns that actually tick: the tier-a renderError path,
            // Verb::Spells and the cast-cooldown denial all return above,
            // before db.begin(), and evaluate nothing.
            evaluateStoryAdvance(db, player);
            db.commit();
        } catch (const std::exception& e) {
            // Tier c: engine error. Roll back — turn counter and world state as
            // if the prompt never happened.
            db.rollback();
            return {TurnOutcome::EngineError, renderError(e.what()),
                    TurnPresentation::Error};
        }
    }

    // Narration dispatch (REQ-PROSE-1, REQ-PROSE-2): AI prose when enabled
    // and delivered; the template renderer is the always-there fallback
    // (REQ-PROSE-3). Tier-a and tier-c paths above never reach this.
    // `narrate` is semantic too: AI prose and the template renderer alike.
    //
    // No AI narration on a talk turn (REQ-NPCTALK-27): the character's reply is
    // the AI output and prints verbatim, so a narrator here would paraphrase
    // the one thing this feature refuses to paraphrase. Refusals take the same
    // branch — a talk turn has exactly one shape, and the template renderer
    // prints the `failed` detail as it does everywhere else. aiRender is the
    // only AiRole::Narrate call site in the binary and it is called exactly
    // once, right here, so gating this one call gates every narrate request
    // there is: that is what makes "no Narrate request on a talk turn"
    // structural rather than something a test has to count.
    const ScopedStage narrateStage("narrate");
    if (action->verb != Verb::Say && aiNarrationEnabled()) {
        if (auto prose = aiRender(db, currentTurn(db))) {
            return {TurnOutcome::Ticked, *prose};
        }
    }
    return {TurnOutcome::Ticked, render(db, currentTurn(db))};
}

}  // namespace

TurnResult runTurn(Db& db, const std::string& line) {
    // Profiling (REQ-LAT-2), inert below TEXTWORLD_LOG_LEVEL=debug: one process-
    // local turn number shared by every record of this turn, then a stage timer
    // per SEMANTIC phase. The stages are scopes, so a phase this turn never
    // reaches simply constructs no timer and is ABSENT from the log rather than
    // reported as zero. `total` lives on the OUTER function so wrapping and band
    // composition are inside the measured turn — otherwise `total` would
    // under-report from this release onward. The resolve/tick/narrate stage
    // names and nesting are unchanged.
    profileNextTurn();
    const ScopedStage totalStage("total");

    TurnResult r = runTurnCore(db, line);

    // Quit produces no output, and therefore no band (REQ-UI-3 is scoped to
    // "every turn that produces output").
    if (r.outcome == TurnOutcome::Quit) return r;

    // THE single composition site (REQ-UI-1, -3, -4, -6). Placing it here, after
    // every runTurnCore return path, is what gives those requirements by
    // construction rather than by discipline: no-tick refusals and engine errors
    // get a band for free, and the AI and template paths get identical bytes
    // because there is only one composition. The band goes LAST, below the
    // narration (REQ-UI-4).
    //
    // Width is re-queried per turn (REQ-UI-28), then SPLIT: narration wraps to
    // the capped prose width and is indented (REQ-POLISH-1, -3), while the band
    // keeps the raw `w` (REQ-POLISH-2) — it is a table, not prose, and its rules
    // run the full terminal. A Reference presentation (`spells`) takes the
    // band's side of that split: full width, column 0 (REQ-POLISH-3b).
    const int w = detectWidth();
    if (r.presentation == TurnPresentation::Reference) {
        r.output = wrapProse(r.output, w);
    } else {
        r.output = indentProse(wrapProse(r.output, proseWidth(w)), kProseIndent);
        // REQ-POLISH-7: a refusal is the game speaking, not the world, and it
        // should not read with the same weight as prose. BrightBlack is the
        // colour band.cpp:28 already gives a spell that has receded out of
        // reach, so no new colour enters the vocabulary.
        //
        // Applied HERE, per line, AFTER the width arithmetic — never inside
        // renderError. wrapProse measures with utf8Length, which counts escape
        // bytes as columns, so styling upstream of the wrap would corrupt the
        // layout of any refusal long enough to wrap. It also keeps the escape
        // bytes OUTSIDE the two-space indent, so stripSgr yields the same
        // string styled or not, and render.cpp stays free of TermStyle.
        if (r.presentation == TurnPresentation::Error) {
            r.output = dimEachLine(r.output, currentStyle());
        }
    }
    r.output += bandOrEmpty(db, w);
    return r;
}

std::string renderStartup(Db& db) {
    const int w = detectWidth();
    // REQ-POLISH-16: the courtesy render prints the paragraph ONLY at world
    // creation. It asks whether the events table is empty rather than asking
    // roomSeen, because the starting room is always seen by REQ-POLISH-15 and
    // roomSeen would answer "yes" on the very first launch. An empty events
    // table means world creation — the one launch where the player has never
    // seen the room. Every later launch prints the room name and the band.
    bool seen = true;
    {
        Stmt s = db.prepare("SELECT COUNT(*) FROM events");
        if (s.step()) seen = s.colInt(0) > 0;
    }
    return indentProse(
               wrapProse(renderRoomOf(db, playerId(db), seen), proseWidth(w)),
               kProseIndent) +
           bandOrEmpty(db, w);
}

// See loop.hpp: exists so main() can name the room the pre-generation scheduler
// should look at (REQ-PREGEN-4) without duplicating this file's two lookups.
int64_t playerRoom(Db& db) { return roomOf(db, playerId(db)); }
