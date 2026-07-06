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

-- the event log (append-only)
CREATE TABLE events(
  id INTEGER PRIMARY KEY,
  turn INTEGER NOT NULL,
  actor INTEGER,            -- who did it (player entity for now)
  verb TEXT NOT NULL,       -- 'moved','took','dropped','looked','waited','failed'
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

void initialize(Db& db, const std::string& seedPath) {
    const std::string seedSql = readFile(seedPath);
    db.begin();
    try {
        db.exec(SCHEMA_DDL);
        db.exec(seedSql.c_str());
        Stmt meta = db.prepare(
            "INSERT INTO meta(key, value) VALUES ('schema_version', ?), ('turn', 0)");
        meta.bind(1, SCHEMA_VERSION);
        meta.step();
        db.commit();
    } catch (...) {
        db.rollback();
        throw;
    }
}

}  // namespace

Db openWorld(const std::string& path, const std::string& seedPath) {
    Db db(path);

    if (!hasMetaTable(db)) {
        // Absent, zero-byte, or otherwise uninitialized: build the world.
        initialize(db, seedPath);
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
