#include "world.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
  kind    TEXT NOT NULL,                -- 'character' | 'beat'
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

-- the event log (append-only)
CREATE TABLE events(
  id INTEGER PRIMARY KEY,
  turn INTEGER NOT NULL,
  actor INTEGER,            -- who did it (player entity for now)
  verb TEXT NOT NULL,       -- 'moved','took','dropped','looked','waited','failed'; combat: 'attacked','chip',…
                            -- world-gen: 'generated'; story: 'materialized' (REQ-BARD-STORE-7)
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

void initialize(Db& db, const std::string& seedPath,
                const std::string& settingPath) {
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
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

}  // namespace

Db openWorld(const std::string& path, const std::string& seedPath,
             const std::string& settingPath) {
    Db db(path);

    if (!hasMetaTable(db)) {
        // Absent, zero-byte, or otherwise uninitialized: build the world.
        initialize(db, seedPath, settingPath);
        return db;
    }

    const int64_t found = readSchemaVersion(db);
    if (found != SCHEMA_VERSION) {
        std::fprintf(stderr,
                     "world file '%s' has schema_version %lld but this build expects %lld.\n"
                     "No migrations exist yet: delete the world file and let the game "
                     "recreate it from the seed.\n",
                     path.c_str(), static_cast<long long>(found),
                     static_cast<long long>(SCHEMA_VERSION));
        throw SchemaMismatch("schema_version mismatch in " + path);
    }
    return db;
}
