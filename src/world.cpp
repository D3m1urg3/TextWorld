#include "world.hpp"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <string>
#include <utility>

#include "log.hpp"  // logToTerminal — the REQ-LOG-2 schema-refusal exemption
#include "mutations.hpp"  // writeCatalogEntry / writeCatalogProfile — the loader
                          // writes majors through the SANCTIONED path, never raw
                          // SQL, which is what keeps REQ-NPCSTORE-37 true of
                          // this file without needing an exception for it

namespace {

// Schema per .lore/work/design/engine-foundation.md §3, verbatim.
const char* const SCHEMA_DDL = R"sql(
CREATE TABLE meta(key TEXT PRIMARY KEY, value);          -- 'schema_version', 'turn'
CREATE TABLE entities(id INTEGER PRIMARY KEY);           -- id mint: INSERT → rowid

-- components
CREATE TABLE name(entity INTEGER PRIMARY KEY, value TEXT);        -- parser handle, unique enough for now
CREATE TABLE room(entity INTEGER PRIMARY KEY);                    -- tag
CREATE TABLE player(entity INTEGER PRIMARY KEY);                  -- tag, singleton by convention
CREATE TABLE portable(entity INTEGER PRIMARY KEY);                -- tag
CREATE TABLE description(entity INTEGER PRIMARY KEY, prose TEXT); -- canon: row exists = never regenerate
CREATE TABLE location(entity INTEGER PRIMARY KEY, container INTEGER);
CREATE TABLE exits(room INTEGER, direction TEXT, dest INTEGER, PRIMARY KEY(room, direction));

-- combat components (see .lore/work/specs/combat-and-enemies.md)
CREATE TABLE health(entity INTEGER PRIMARY KEY, current INTEGER, max INTEGER);   -- current clamped [0,max] in code
CREATE TABLE hostile(entity INTEGER PRIMARY KEY, archetype TEXT, chip INTEGER,   -- archetype tag; per-instance chip constant
                     telegraph_period INTEGER);                                  -- winds up a strike every N combat turns (0 = never)

-- combat: spells, cooldowns, telegraph/strike, status effects (engine-owned constants)
CREATE TABLE spell_catalog(spell TEXT PRIMARY KEY, element TEXT, cooldown INTEGER,
                           tier INTEGER, effect TEXT);      -- the fixed spell constants
CREATE TABLE known_spells(entity INTEGER, spell TEXT, PRIMARY KEY(entity, spell));  -- canon: learned = permanent
CREATE TABLE cooldowns(entity INTEGER, spell TEXT, ready_turn INTEGER,
                       PRIMARY KEY(entity, spell));         -- cast at T → ready_turn = T + cooldown
CREATE TABLE pending_strike(entity INTEGER PRIMARY KEY, damage INTEGER, element TEXT);  -- row exists = strike pending
CREATE TABLE status_effects(entity INTEGER, kind TEXT, magnitude INTEGER,
                            remaining INTEGER, PRIMARY KEY(entity, kind));  -- DoT / CC, ticks down each tick

-- combat: elements, defense lock, grimoire→spell bridge
CREATE TABLE resistance(archetype TEXT, element TEXT, multiplier_num INTEGER,
                        multiplier_den INTEGER, PRIMARY KEY(archetype, element));  -- integer ratio: no floats, no RNG
CREATE TABLE barrier(entity INTEGER PRIMARY KEY);              -- defense-lock state (row exists = warded)
CREATE TABLE grimoire(entity INTEGER PRIMARY KEY, spell TEXT); -- the dropped item → spell it teaches

-- combat: the bestiary catalog — one frozen record per archetype (the mold every
-- instance is cast from; the engine owns every number, the model sees only blurb)
CREATE TABLE bestiary(archetype TEXT PRIMARY KEY, name TEXT, blurb TEXT,          -- name = instance handle; blurb = the ONLY model-facing field
                      health INTEGER, chip INTEGER, telegraph_period INTEGER,     -- engine-owned stat constants
                      tier INTEGER, barrier INTEGER);                             -- tier = front-intensity rank; barrier = 1 → a defense-lock archetype
CREATE TABLE drop_table(archetype TEXT PRIMARY KEY, spell TEXT);                  -- the fixed archetype → grimoire-spell it drops (REQ-COMBAT-20)

-- The story catalog: entries authored by the bard, materialized at most once.
-- Mirrors `bestiary` — a record the world is cast from — except that rows are
-- minted at RUNTIME by the bard rather than seeded, and each is cast ONCE.
-- APPEND-ONLY: no helper updates kind/handle/name/blurb/motive/tier/fact_*;
-- `entity` and `seeded` are one-way latches guarded in SQL (REQ-BARD-STORE-17).
CREATE TABLE catalog(
  id      INTEGER PRIMARY KEY,          -- engine-minted; NEVER on the wire
  kind    TEXT NOT NULL,                -- 'character' | 'beat' | 'major'
                                        -- 'major' is HAND-AUTHORED and loaded at
                                        -- world creation; it is never model-proposed
                                        -- (REQ-NPCSTORE-23, -24)
  handle  TEXT NOT NULL UNIQUE,         -- the model-facing SELECTION token
  name    TEXT NOT NULL,                -- the in-world parser noun; becomes the
                                        -- minted entity's name row
  blurb   TEXT NOT NULL,                -- the ONLY prose the model sees to select
  motive  TEXT NOT NULL,                -- motive_catalog.motive (closed vocab,
                                        -- enforced in writeCatalogEntry)
  tier    INTEGER NOT NULL,             -- placement gate vs distanceFromSeed
  seeded  INTEGER NOT NULL DEFAULT 0,   -- 1 = hinted in prose, not yet materialized
  entity  INTEGER,                      -- NULL = latent; non-NULL = MATERIALIZED
  -- A KNOWLEDGE beat asserts something TRUE about combat. Both NULL on every
  -- other entry; both non-NULL together, never one. Validated at admission
  -- against bestiary/spell_catalog/resistance (REQ-BARD-STORE-10).
  fact_archetype TEXT,
  fact_element   TEXT
);

-- The closed motive vocabulary, authored for Thornmere. Engine-owned constants
-- like spell_catalog: seeded in base.sql, never written at runtime. The model
-- sees `blurb`, never the key.
CREATE TABLE motive_catalog(motive TEXT PRIMARY KEY, blurb TEXT);

-- The authored identity of a character: who they are, how they talk, what they
-- know, what they will not say. WRITE-ONCE — there is deliberately NO helper
-- that edits a profile, so a character's identity cannot drift
-- (REQ-NPCSTORE-11, -12). Asserted against the source text, not trusted.
--
-- Keyed by CATALOG id, not entity, because a hand-authored major character's
-- profile exists from world creation, long before any entity does.
CREATE TABLE catalog_profile(
  catalog INTEGER PRIMARY KEY,        -- catalog.id
  profile TEXT NOT NULL               -- the full character document, model-facing
);

-- What a character remembers, as it remembers it. Freely rewritten and CAPPED
-- (REQ-NPCSTORE-15, -17) — memory is a reconstruction, and a character
-- misremembering costs nothing mechanical. Keyed by ENTITY: memory exists only
-- once the character does. Rows are NOT pre-created at materialisation; the
-- write helper upserts, so there is no row to branch on (REQ-NPCSTORE-9).
CREATE TABLE npc_memory(
  entity       INTEGER PRIMARY KEY,
  summary      TEXT NOT NULL DEFAULT '',
  summary_turn INTEGER NOT NULL DEFAULT 0   -- the turn the summary last covered
);

-- The story arc's ordered list of steps (specs/story-arc-store.md). A step is
-- one entry in a short list of how the threat gets closer: a CONDITION the
-- engine can check, and a line of prose describing the world once that step is
-- reached. The bard authors them; the engine walks them, lowest unreached first.
--
-- condition_kind and condition_arg are two columns rather than one `kind:arg`
-- token because admission validation and evaluation are then both plain SQL.
-- The combined form is the WIRE format the bard writes over, not a storage
-- format.
--
-- `reached_turn` is a one-way latch guarded in SQL, like catalog.entity: set
-- once by advanceStoryStep, never cleared. NULL means not yet reached, and a
-- freshly created world is at step zero (REQ-ARC-STORE-8).
CREATE TABLE story_step(
  n              INTEGER PRIMARY KEY,  -- 1-based; the list is walked in this order
  condition_kind TEXT NOT NULL,        -- condition_catalog.kind (closed vocabulary)
  condition_arg  TEXT NOT NULL,        -- what may go here: see condition_catalog.arg_kind
  prose          TEXT NOT NULL,        -- what the world looks like once reached
  reached_turn   INTEGER               -- NULL = not yet reached; set once, never cleared
);

-- The closed condition vocabulary: engine-owned constants, seeded exactly like
-- motive_catalog and never written at runtime. writeStoryStep throws on a kind
-- absent from this table — a step may not promise a condition the engine cannot
-- check. The model sees `blurb`, never the key.
CREATE TABLE condition_catalog(
  kind     TEXT PRIMARY KEY,
  blurb    TEXT NOT NULL,   -- the model-facing description; the overture reads this
  arg_kind TEXT NOT NULL    -- 'int' | 'spell'
);

-- the event log (append-only)
CREATE TABLE events(
  id INTEGER PRIMARY KEY,
  turn INTEGER NOT NULL,
  actor INTEGER,            -- who did it (player entity for now)
  verb TEXT NOT NULL,       -- 'moved','took','dropped','looked','waited','failed'; combat: 'attacked','chip',…
                            -- world-gen: 'generated'; story: 'materialized' (REQ-BARD-STORE-7);
                            -- speech: 'said','spoke' (REQ-NPCSTORE-1);
                            -- story arc: 'advanced' (REQ-ARC-STORE-20)
  subject INTEGER,          -- primary entity acted on
  object INTEGER,           -- secondary entity (destination room, container…)
  detail TEXT               -- human-readable fragment or NULL
);
)sql";

bool hasMetaTable(Db& db) {
    Stmt s = db.prepare(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='meta'");
    s.step();
    return s.colInt(0) > 0;
}

int64_t readSchemaVersion(Db& db) {
    Stmt s = db.prepare("SELECT value FROM meta WHERE key='schema_version'");
    if (!s.step()) return -1;  // initialized-looking file with no version row
    return s.colInt(0);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open seed file: " + path +
                                 " (run from the repo root, or pass an explicit seed path)");
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Tolerant reader for the setting document (REQ-ARCH-1): unlike readFile, an
// absent/unreadable file is NOT an error — it yields "" so init still succeeds
// with an empty setting. Deliberately distinct from readFile, which must throw
// for the mandatory seed.
std::string readFileOrEmpty(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Strip leading/trailing ASCII whitespace. A file-local copy: mutations.cpp has
// one in its own anonymous namespace for the same reason, and world.cpp does not
// otherwise depend on that unit's internals.
std::string trimAscii(const std::string& s) {
    const char* const ws = " \t\n\r\f\v";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

// The profile's BLURB is its first non-empty body line (REQ-NPCSTORE-29).
// writeCatalogEntry requires a blurb and a major character is never selected
// from a menu, so no second authoring surface is invented for a field nothing
// reads. "First NON-EMPTY" rather than "first" so a body that opens with a
// blank line still yields a usable one.
std::string firstNonEmptyLine(const std::string& body) {
    size_t pos = 0;
    while (pos <= body.size()) {
        const size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos
                                                ? std::string::npos
                                                : nl - pos);
        const std::string trimmed = trimAscii(line);
        if (!trimmed.empty()) return trimmed;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "";
}

// Write the hand-authored major cast, in the order the caller supplied
// (REQ-NPCSTORE-33). Every failure THROWS with the file's name attached: these
// are hand-authored seed files like base.sql, and a silently dropped major is a
// world missing its most expensive content with nothing to show for it
// (REQ-NPCSTORE-31). The throw reaches initialize()'s catch, which rolls the
// whole transaction back — a half-seeded world is not one of the outcomes.
void writeMajors(Db& db, const std::vector<MajorProfileFile>& files) {
    std::set<std::string> handlesSoFar;
    for (const MajorProfileFile& file : files) {
        const auto fail = [&file](const std::string& why) {
            throw std::runtime_error("major profile '" + file.name + "': " + why);
        };

        MajorProfile parsed;
        const std::string reason = parseMajorProfile(file.text, parsed);
        if (!reason.empty()) fail(reason);

        // Scoped to the FILES, not the catalog (REQ-NPCSTORE-31b): the catalog
        // is empty at this point, so there is nothing else to collide with. A
        // collision with a handle the bard later proposes is the overture's
        // case, handled by catalogEntryRefusal dropping that one entry.
        if (!handlesSoFar.insert(parsed.handle).second) {
            fail("handle '" + parsed.handle +
                 "' duplicates an earlier profile file's handle");
        }

        int64_t id = 0;
        try {
            // The unknown-motive refusal comes free from writeCatalogEntry's
            // existing motive_catalog check; it is caught here only so the
            // message gains the file name the author needs to fix it.
            id = writeCatalogEntry(db, "major", parsed.handle, parsed.name,
                                   firstNonEmptyLine(parsed.profile),
                                   parsed.motive, parsed.tier);
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        // The body is stored BYTE-EXACT (REQ-NPCSTORE-28): nothing the engine
        // owns is injected into it. The engine's rules live in the conversation
        // prompt, not in a file an author can edit or forget.
        writeCatalogProfile(db, id, parsed.profile);
    }
}

void initialize(Db& db, const std::string& seedPath,
                const std::string& settingPath,
                const std::vector<MajorProfileFile>& majors) {
    const std::string seedSql = readFile(seedPath);
    // Read the setting tolerantly BEFORE opening the transaction; an absent
    // file is fine (empty setting), and this keeps any filesystem work out of
    // the write path.
    const std::string setting = readFileOrEmpty(settingPath);
    db.begin();
    try {
        db.exec(SCHEMA_DDL);
        db.exec(seedSql.c_str());
        Stmt meta = db.prepare(
            "INSERT INTO meta(key, value) VALUES ('schema_version', ?), ('turn', 0)");
        meta.bind(1, SCHEMA_VERSION);
        meta.step();
        // meta.setting: a new ROW, not a new shape — zero DDL, no SCHEMA_VERSION
        // bump (REQ-ARCH-1). Written even when empty so the key is present.
        Stmt settingStmt = db.prepare(
            "INSERT INTO meta(key, value) VALUES ('setting', ?)");
        settingStmt.bind(1, setting);
        settingStmt.step();
        // The bard's three meta rows (REQ-BARD-STORE-6): rows, not shapes. Written
        // at init so every helper can UPDATE rather than branch on absence.
        db.exec(
            "INSERT INTO meta(key, value) VALUES "
            "('bard_journal', ''), ('bard_focus', ''), ('bard_last_wake_turn', 0)");
        // The hand-authored cast, LAST and inside this same transaction
        // (REQ-NPCSTORE-33): major rows exist the instant openWorld returns and
        // before any overture call is possible, and a malformed file rolls the
        // whole world back rather than leaving it half-seeded. The empty case is
        // the normal one and is silent — no cast is not a fault
        // (REQ-NPCSTORE-32).
        writeMajors(db, majors);
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

}  // namespace

std::string parseMajorProfile(const std::string& text, MajorProfile& out) {
    // The header ends at the FIRST blank line and never resumes
    // (REQ-NPCSTORE-27). Finding that line first, before parsing anything,
    // is what makes "a `key: value` line in the body is body text" structural
    // rather than a rule the header parser has to remember.
    size_t pos = 0;
    size_t bodyStart = std::string::npos;
    std::vector<std::string> headerLines;
    // `pos < size`, not `<=`: a file's trailing newline must not be read as a
    // phantom empty final line, or a header with no body at all would parse as
    // "header, separator, empty body" instead of failing for what it is.
    while (pos < text.size()) {
        const size_t nl = text.find('\n', pos);
        const bool lastLine = (nl == std::string::npos);
        std::string line = text.substr(pos, lastLine ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        if (trimAscii(line).empty()) {
            // The separator. The body is everything AFTER this line's newline,
            // byte for byte — including a leading blank line of its own.
            bodyStart = lastLine ? text.size() : nl + 1;
            break;
        }
        headerLines.push_back(std::move(line));
        if (lastLine) break;
        pos = nl + 1;
    }
    if (bodyStart == std::string::npos) {
        return "no blank line separating the header from the body";
    }

    bool haveHandle = false, haveName = false, haveMotive = false, haveTier = false;
    MajorProfile parsed;
    for (const std::string& line : headerLines) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return "header line does not parse (no colon): '" + line + "'";
        }
        const std::string key = trimAscii(line.substr(0, colon));
        const std::string value = trimAscii(line.substr(colon + 1));
        // An UNRECOGNISED key is a failure, not a silent skip
        // (REQ-NPCSTORE-31a). The concrete case is an author writing `goal:` —
        // a field this format deliberately does not carry (REQ-NPCSTORE-30) —
        // and getting a world where the line quietly did nothing. A duplicate
        // key is refused for the same reason: the first one would be what
        // quietly did nothing.
        if (value.empty()) return "header key '" + key + "' has an empty value";
        if (key == "handle") {
            if (haveHandle) return "duplicate header key 'handle'";
            parsed.handle = value;
            haveHandle = true;
        } else if (key == "name") {
            if (haveName) return "duplicate header key 'name'";
            parsed.name = value;
            haveName = true;
        } else if (key == "motive") {
            if (haveMotive) return "duplicate header key 'motive'";
            parsed.motive = value;
            haveMotive = true;
        } else if (key == "tier") {
            if (haveTier) return "duplicate header key 'tier'";
            // Whole-string, so `tier: 2 or 3` is refused rather than read as 2.
            size_t consumed = 0;
            try {
                parsed.tier = std::stoll(value, &consumed);
            } catch (const std::exception&) {
                return "header key 'tier' is not an integer: '" + value + "'";
            }
            if (consumed != value.size()) {
                return "header key 'tier' is not an integer: '" + value + "'";
            }
            haveTier = true;
        } else {
            return "unrecognised header key '" + key + "'";
        }
    }
    if (!haveHandle) return "missing required header key 'handle'";
    if (!haveName) return "missing required header key 'name'";
    if (!haveMotive) return "missing required header key 'motive'";
    if (!haveTier) return "missing required header key 'tier'";

    parsed.profile = text.substr(bodyStart);  // verbatim, to the last byte
    out = std::move(parsed);
    return "";
}

OpenedWorld openWorld(const std::string& path, const std::string& seedPath,
                      const std::string& settingPath,
                      const std::vector<MajorProfileFile>& majors) {
    Db db(path);

    if (!hasMetaTable(db)) {
        // Absent, zero-byte, or otherwise uninitialized: build the world.
        initialize(db, seedPath, settingPath, majors);
        return {std::move(db), true};  // created THIS call (REQ-BARD-WAKE-1)
    }

    const int64_t found = readSchemaVersion(db);
    if (found != SCHEMA_VERSION) {
        // One of the two REQ-LOG-2 exemptions: the binary is refusing to
        // start, so there is no game on screen for this to intrude on. One
        // call, both channels — the terminal duplicate (the only sanctioned
        // route to the screen besides game text) and the log, which REQ-LOG-29
        // step 7 guarantees is already open by the time the schema check runs.
        logExempt(LogLevel::Error, "world",
                  "world file '%s' has schema_version %lld but this build "
                  "expects %lld.\nNo migrations exist yet: delete the world "
                  "file and let the game recreate it from the seed.\n",
                  path.c_str(), static_cast<long long>(found),
                  static_cast<long long>(SCHEMA_VERSION));
        throw SchemaMismatch("schema_version mismatch in " + path);
    }
    return {std::move(db), false};  // resumed, not created
}
