// Micro test harness: CHECK(cond) records failures; main() reports a summary.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>  // architect live smoke's single bounded judge call (gated)

#include "action.hpp"
#include "architect.hpp"
#include "combat.hpp"
#include "db.hpp"
#include "loop.hpp"
#include "mutations.hpp"
#include "nlresolve.hpp"
#include "prose.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "world.hpp"

// vendor/ is a PRIVATE include dir of twcore, so the tests reach the vendored
// nlohmann/json by relative path.
#include "../vendor/json.hpp"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

// Temp database file for one test: removes any leftover from a previous run
// on construction, and cleans up on destruction.
struct TempDbFile {
    std::filesystem::path path;

    explicit TempDbFile(const char* filename)
        : path(std::filesystem::temp_directory_path() / filename) {
        std::filesystem::remove(path);
    }
    ~TempDbFile() { std::filesystem::remove(path); }

    std::string string() const { return path.string(); }
    operator const std::filesystem::path&() const { return path; }
};

static void testDb() {
    const TempDbFile dbPath("textworld_tests.db");

    {
        Db db(dbPath.string());
        db.exec("CREATE TABLE actor (id INTEGER PRIMARY KEY, name TEXT NOT NULL)");

        // Insert rows inside a transaction, binding int + string.
        db.begin();
        {
            Stmt ins = db.prepare("INSERT INTO actor (id, name) VALUES (?, ?)");
            ins.bind(1, static_cast<int64_t>(1));
            ins.bind(2, std::string("hero"));
            CHECK(!ins.step());  // INSERT yields no rows
        }
        {
            Stmt ins = db.prepare("INSERT INTO actor (id, name) VALUES (?, ?)");
            ins.bind(1, static_cast<int64_t>(42));
            ins.bind(2, std::string("goblin"));
            CHECK(!ins.step());
        }
        db.commit();

        // Read back typed values.
        Stmt sel = db.prepare("SELECT id, name FROM actor ORDER BY id");
        CHECK(sel.step());
        CHECK(sel.colInt(0) == 1);
        CHECK(sel.colText(1) == "hero");
        CHECK(sel.step());
        CHECK(sel.colInt(0) == 42);
        CHECK(sel.colText(1) == "goblin");
        CHECK(!sel.step());  // no third row

        // rollback() discards uncommitted work.
        db.begin();
        db.exec("INSERT INTO actor (id, name) VALUES (99, 'ghost')");
        db.rollback();
        Stmt count = db.prepare("SELECT COUNT(*) FROM actor");
        CHECK(count.step());
        CHECK(count.colInt(0) == 2);

        // Bad SQL throws std::runtime_error from exec and prepare.
        bool threw = false;
        try {
            db.exec("THIS IS NOT SQL");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            Stmt bad = db.prepare("SELECT * FROM no_such_table");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
}

// Single-value query helper for the world tests.
static int64_t queryInt(Db& db, const char* sql) {
    Stmt s = db.prepare(sql);
    CHECK(s.step());
    return s.colInt(0);
}

static std::string readFileBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

static void testWorld() {
    const TempDbFile worldPath("textworld_world_tests.db");

    // Tests run from the repo root, so the default seed path resolves.
    const std::string seedPath = "tests/fixture.sql";

    // --- fresh create: schema + seed applied ---
    {
        Db db = openWorld(worldPath.string(), seedPath);

        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == 2);

        CHECK(queryInt(db, "SELECT COUNT(*) FROM portable") >= 2);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM portable p "
                       "JOIN location l ON l.entity = p.entity "
                       "WHERE l.container = 1") >= 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM portable p "
                       "JOIN location l ON l.entity = p.entity "
                       "WHERE l.container = 2") >= 1);

        CHECK(queryInt(db, "SELECT COUNT(*) FROM player") == 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM player p "
                       "JOIN location l ON l.entity = p.entity "
                       "WHERE l.container = 1") == 1);

        // Exits are bidirectional: 1 -north-> 2, 2 -south-> 1.
        CHECK(queryInt(db,
                       "SELECT dest FROM exits WHERE room = 1 AND direction = 'north'") == 2);
        CHECK(queryInt(db,
                       "SELECT dest FROM exits WHERE room = 2 AND direction = 'south'") == 1);

        // Canon prose for both rooms and both items, none for the player.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM description") == 4);

        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 0);
    }

    // --- reopen: recognized as initialized, not re-seeded ---
    {
        Db db = openWorld(worldPath.string(), seedPath);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == 5);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == 2);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") ==
              SCHEMA_VERSION);
    }

    // --- schema_version mismatch: refuse without touching the file ---
    {
        Db db(worldPath.string());
        db.exec("UPDATE meta SET value = 999999 WHERE key = 'schema_version'");
    }
    const std::string bytesBefore = readFileBytes(worldPath);
    CHECK(!bytesBefore.empty());

    bool refused = false;
    try {
        Db db = openWorld(worldPath.string(), seedPath);
    } catch (const SchemaMismatch&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(readFileBytes(worldPath) == bytesBefore);  // byte-identical after refusal
}

// The shipped seed's REQ-PROTO-2 shape, checked structurally — never wording,
// never entity counts beyond what REQ-PROTO-2 mandates (REQ-MAGE-5). The rest
// of the suite runs against tests/fixture.sql; this is the one deterministic
// test that opens the real seed/base.sql, so content swaps stay green here
// without ever touching assertions.
static void testShippedSeedShape() {
    const TempDbFile worldPath("textworld_shipped_seed_tests.db");

    // Shipped seed + default settingPath (seed/setting.txt), from the repo root.
    Db db = openWorld(worldPath.string(), "seed/base.sql");

    // Exactly 2 rooms.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == 2);

    // Exit rows: 2 realized (the cell↔corridor pair) + 2 latent frontier stubs
    // (dest NULL) planted by the seed (REQ-EXITS-9).
    CHECK(queryInt(db, "SELECT COUNT(*) FROM exits") == 4);
    // Exactly 2 latent (dest IS NULL) start exits.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE dest IS NULL") == 2);
    // One bidirectional exit pair whose directions are mutual inverses from
    // the invertible set (REQ-ARCH-8). The join ignores NULL dests, so the
    // realized pair is still exactly 2 — unchanged by the latent frontier.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM exits a "
                   "JOIN exits b ON a.dest = b.room AND b.dest = a.room "
                   "WHERE (a.direction, b.direction) IN "
                   "(('north','south'),('south','north'),"
                   "('east','west'),('west','east'),"
                   "('up','down'),('down','up'),"
                   "('in','out'),('out','in'))") == 2);

    // Player exists, starts in room 1, and has no description row.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM player") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM player p "
                   "JOIN location l ON l.entity = p.entity "
                   "WHERE l.container = 1") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM description d "
                   "JOIN player p ON d.entity = p.entity") == 0);

    // >= 2 portables, at least one located in each of the two rooms.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM portable") >= 2);
    CHECK(queryInt(db,
                   "SELECT COUNT(DISTINCT l.container) FROM portable p "
                   "JOIN location l ON l.entity = p.entity "
                   "JOIN room r ON r.entity = l.container") == 2);

    // Every room and portable has a non-empty name and description.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM room r "
                   "LEFT JOIN name n ON n.entity = r.entity "
                   "LEFT JOIN description d ON d.entity = r.entity "
                   "WHERE n.value IS NULL OR trim(n.value) = '' "
                   "OR d.prose IS NULL OR trim(d.prose) = ''") == 0);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM portable p "
                   "LEFT JOIN name n ON n.entity = p.entity "
                   "LEFT JOIN description d ON d.entity = p.entity "
                   "WHERE n.value IS NULL OR trim(n.value) = '' "
                   "OR d.prose IS NULL OR trim(d.prose) = ''") == 0);

    // meta.turn = 0 and a non-empty meta.setting loaded from the default path.
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 0);
    CHECK(queryInt(db,
                   "SELECT length(value) > 0 FROM meta WHERE key = 'setting'") == 1);

    // Combat foundation (REQ-COMBAT-39): the corridor (room 2) hand-places
    // exactly one hostile, carrying a health row, a non-zero chip, and a
    // name/description. The player carries a health row too (REQ-COMBAT-4).
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM hostile h "
                   "JOIN location l ON l.entity = h.entity "
                   "WHERE l.container = 2") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM hostile h "
                   "JOIN location l ON l.entity = h.entity "
                   "JOIN health hp ON hp.entity = h.entity "
                   "LEFT JOIN name n ON n.entity = h.entity "
                   "LEFT JOIN description d ON d.entity = h.entity "
                   "WHERE l.container = 2 AND h.chip > 0 "
                   "AND hp.current > 0 AND hp.current = hp.max "
                   "AND n.value IS NOT NULL AND trim(n.value) <> '' "
                   "AND d.prose IS NOT NULL AND trim(d.prose) <> ''") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM health hp "
                   "JOIN player p ON p.entity = hp.entity") == 1);
}

// Combat schema shape + seeded spell constants. Grows as later bricks add
// tables (Step 13). Opens the combat fixture so both the DDL shapes AND the
// seeded spell_catalog / known_spells content are asserted. Deterministic.
static void testCombatSchema() {
    const TempDbFile worldPath("textworld_combat_schema_tests.db");

    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // Fresh world opened at the current (bumped) schema version.
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") ==
          SCHEMA_VERSION);

    // health(entity, current, max) exists with exactly those columns.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM sqlite_master "
                   "WHERE type='table' AND name='health'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('health')") == 3);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM pragma_table_info('health') "
                   "WHERE name IN ('entity','current','max')") == 3);

    // hostile(entity, archetype, chip, telegraph_period).
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM sqlite_master "
                   "WHERE type='table' AND name='hostile'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('hostile')") == 4);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM pragma_table_info('hostile') "
                   "WHERE name IN ('entity','archetype','chip','telegraph_period')") == 4);

    // --- Brick 2 tables: existence + exact columns (REQ-COMBAT-13, -19) ---
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('spell_catalog') "
                       "WHERE name IN ('spell','element','cooldown','tier','effect')") == 5);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('spell_catalog')") == 5);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('known_spells') "
                       "WHERE name IN ('entity','spell')") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('cooldowns') "
                       "WHERE name IN ('entity','spell','ready_turn')") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('pending_strike') "
                       "WHERE name IN ('entity','damage','element')") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('status_effects') "
                       "WHERE name IN ('entity','kind','magnitude','remaining')") == 4);

    // --- seeded spell constants + the player's starting known_spells ---
    // The player (entity 3) knows EXACTLY {ward, stun}.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM known_spells WHERE entity = 3") == 2);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM known_spells "
                   "WHERE entity = 3 AND spell IN ('ward','stun')") == 2);
    // spell_catalog holds the two constant rows with their fixed cooldowns/tier.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM spell_catalog "
                   "WHERE spell IN ('ward','stun') AND cooldown > 0 AND tier = 1") == 2);
    // The seed enemy carries a telegraph_period constant.
    CHECK(queryInt(db,
                   "SELECT telegraph_period FROM hostile WHERE entity = 7") > 0);

    // --- Brick 3 tables: elements, defense lock, grimoire bridge ---
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('resistance') "
                       "WHERE name IN ('archetype','element','multiplier_num',"
                       "'multiplier_den')") == 4);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('barrier') "
                       "WHERE name = 'entity'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('grimoire') "
                       "WHERE name IN ('entity','spell')") == 2);

    // Resistance rows are integer ratios (num/den, both non-zero) — no floats.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM resistance") >= 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM resistance "
                   "WHERE multiplier_num > 0 AND multiplier_den > 0") ==
          queryInt(db, "SELECT COUNT(*) FROM resistance"));
    // The rime-touched weakness/resistance ratios are seeded.
    CHECK(queryInt(db,
                   "SELECT multiplier_num FROM resistance "
                   "WHERE archetype = 'rime_touched' AND element = 'fire'") == 2);
}

static void testParser() {
    const TempDbFile worldPath("textworld_parser_tests.db");

    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Unparseable lines → nullopt.
    CHECK(!parse(db, "frobnicate"));       // unknown verb
    CHECK(!parse(db, "go"));               // bare verb (REQ-PROTO-6a)
    CHECK(!parse(db, "take"));             // bare verb (REQ-PROTO-6a)
    CHECK(!parse(db, "take zeppelin"));    // noun not in world

    // take lantern → Take, subject 4.
    {
        auto a = parse(db, "take lantern");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Take);
        CHECK(a->subject == 4);
    }

    // drop key → Drop, subject 5.
    {
        auto a = parse(db, "drop key");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Drop);
        CHECK(a->subject == 5);
    }

    // go north → Go, direction preserved as string.
    {
        auto a = parse(db, "go north");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Go);
        CHECK(a->direction == "north");
    }

    // Input is case-insensitive.
    {
        auto a = parse(db, "TAKE LANTERN");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Take);
        CHECK(a->subject == 4);
    }

    // quit → Quit.
    {
        auto a = parse(db, "quit");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Quit);
    }
}

static void testMutations() {
    const TempDbFile worldPath("textworld_mutations_tests.db");

    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Seed baseline: lantern (4) in stone hall (1), no events yet.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 1);
    const int64_t locRowsBefore = queryInt(db, "SELECT COUNT(*) FROM location");
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 0);

    // --- scenario 1: moveEntity inside a transaction, then commit ---
    db.begin();
    moveEntity(db, /*what=*/4, /*toContainer=*/3, /*actor=*/3, "took");

    // Exactly one location row changed: lantern's container is now 3...
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    // ...all other location rows unchanged (player 3 in room 1, key 5 in room 2),
    // and no rows added or removed.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM location") == locRowsBefore);

    // Exactly one events row added, matching turn/verb/subject/object.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events "
                   "WHERE turn = (SELECT value FROM meta WHERE key = 'turn') "
                   "AND actor = 3 AND verb = 'took' "
                   "AND subject = 4 AND object = 3 AND detail IS NULL") == 1);
    db.commit();

    // Persisted after commit.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 1);

    // --- scenario 2: moveEntity inside a transaction, then rollback ---
    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
    db.begin();
    moveEntity(db, /*what=*/4, /*toContainer=*/2, /*actor=*/3, "dropped");
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore + 1);
    db.rollback();

    // Both tables untouched: row counts and lantern container back to original.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM location") == locRowsBefore);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);

    // --- scenario 3: moveEntity on an entity with no location row throws,
    // and appends no event (engine error; caller rolls back) ---
    db.begin();
    bool threw = false;
    try {
        moveEntity(db, /*what=*/999, /*toContainer=*/1, /*actor=*/3, "took");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    db.rollback();

    // Both tables untouched.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM location") == locRowsBefore);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
}

// One tick, mimicking the future game loop: resolve() assumes the caller
// already incremented meta.turn inside the ambient transaction.
static void tick(Db& db, const Action& a, int64_t player = 3) {
    db.begin();
    db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
    resolve(db, a, player);
    db.commit();
}

static void testSystems() {
    const TempDbFile worldPath("textworld_systems_tests.db");

    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Seed baseline: player 3 in room 1, lantern 4 in room 1, key 5 in room 2.
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 0);

    // --- movement: go north from room 1 → room 2, one 'moved' event ---
    tick(db, Action{Verb::Go, 0, "north"});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE turn = 1 AND actor = 3 "
                   "AND verb = 'moved' AND subject = 3 AND object = 2") == 1);

    // Return to room 1 for the take/drop round-trip.
    tick(db, Action{Verb::Go, 0, "south"});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);

    // --- take/drop round-trip: lantern in room 1 ---
    tick(db, Action{Verb::Take, 4, ""});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'took' "
                   "AND subject = 4 AND object = 3") == 1);

    tick(db, Action{Verb::Drop, 4, ""});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'dropped' "
                   "AND subject = 4 AND object = 1") == 1);

    // --- wall bump: no exit west from room 1 → failed, tick, and NOTHING
    // else changes: exactly one event appended, every entity stays put ---
    {
        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
        const int64_t lanternBefore =
            queryInt(db, "SELECT container FROM location WHERE entity = 4");
        const int64_t keyBefore =
            queryInt(db, "SELECT container FROM location WHERE entity = 5");
        tick(db, Action{Verb::Go, 0, "west"});
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore + 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore + 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == lanternBefore);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == keyBefore);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                       "AND detail = 'You can''t go that way.'") == 1);
    }

    // --- tier boundary (tier b): take key while in room 1, key in room 2.
    // The noun IS recognized, so the action reaches resolve and costs a tick
    // as an in-world refusal. Contrast tier a: an UNKNOWN noun never reaches
    // resolve — parse() returns nullopt (tested in testParser, Phase 4). ---
    {
        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        tick(db, Action{Verb::Take, 5, ""});
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore + 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                       "AND detail = 'You don''t see that here.'") == 1);
    }

    // --- take while already carrying → failed ---
    tick(db, Action{Verb::Take, 4, ""});  // pick the lantern back up
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    tick(db, Action{Verb::Take, 4, ""});  // again, while carried
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                   "AND detail = 'You''re already carrying that.'") == 1);

    // --- drop key not carried → failed, key stays put ---
    tick(db, Action{Verb::Drop, 5, ""});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                   "AND detail = 'You aren''t carrying that.'") == 1);

    // --- look / inventory / wait: event-only verbs ---
    tick(db, Action{Verb::Look, 0, ""});
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'looked' "
                   "AND detail IS NULL") == 1);

    tick(db, Action{Verb::Inventory, 0, ""});
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'looked' "
                   "AND detail = 'inventory'") == 1);

    tick(db, Action{Verb::Wait, 0, ""});
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'waited' "
                   "AND detail IS NULL") == 1);

    // --- cross-room drop: the item lands in the player's CURRENT room at
    // drop time, not the room where it was taken. Lantern was taken in
    // room 1 (still carried here); go north, then drop it in room 2. ---
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    tick(db, Action{Verb::Go, 0, "north"});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
    tick(db, Action{Verb::Drop, 4, ""});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'dropped' "
                   "AND subject = 4 AND object = 2") == 1);

    // Only the fixed six verb strings ever appear in the log.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb NOT IN "
                   "('moved','took','dropped','looked','waited','failed')") == 0);

    // --- Quit never reaches resolve: it throws, and the rollback means
    // no tick is recorded ---
    {
        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
        db.begin();
        db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
        bool threw = false;
        try {
            resolve(db, Action{Verb::Quit, 0, ""}, 3);
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
        db.rollback();
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
    }
}

// Basic attack (REQ-COMBAT-6, -18): the player's offensive verb drives the seed
// enemy's health down by the fixed floor damage; attacking an empty room is an
// in-world refusal. Driven through the `tick` helper (direct resolve); the enemy
// turn / chip lane arrives in Step 4, so the player takes no damage here.
static void testCombatAttack() {
    const TempDbFile worldPath("textworld_combat_attack_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // Player 3 in the cell (room 1); goblin 7 at 8/8 in the corridor (room 2).
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == 8);

    // --- attack with no hostile present (cell) → failed, nothing changes ---
    tick(db, Action{Verb::Attack, 0, ""});
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == 8);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") == 12);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                   "AND detail = 'There''s nothing here to attack.'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'attacked'") == 0);

    // --- move to the corridor, then attack the goblin → exactly one hit ---
    tick(db, Action{Verb::Go, 0, "north"});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);

    tick(db, Action{Verb::Attack, 0, ""});
    // health.current drops by EXACTLY kBasicAttackDamage; one 'attacked' event
    // carrying subject = enemy, object = the damage dealt.
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") ==
          8 - kBasicAttackDamage);
    CHECK(queryInt(db,
                   ("SELECT COUNT(*) FROM events WHERE verb = 'attacked' "
                    "AND actor = 3 AND subject = 7 AND object = " +
                    std::to_string(kBasicAttackDamage))
                       .c_str()) == 1);
    // No enemy turn yet (Step 4): the player is untouched.
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") == 12);

    // --- a second attack stacks deterministically ---
    tick(db, Action{Verb::Attack, 0, ""});
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") ==
          8 - 2 * kBasicAttackDamage);
}

// The chip clock + enemy-turn tick integrity (REQ-COMBAT-1, -2, -3, -9, -12).
// Driven through the real runTurn (the enemy turn fires only in the loop, not
// the direct-resolve `tick` helper). AI is disabled hermetically, so runTurn
// takes the fixed-verb parser path — deterministic, no network.
static void testCombatChipClock() {
    const TempDbFile worldPath("textworld_combat_chip_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    const int64_t chip = queryInt(db, "SELECT chip FROM hostile WHERE entity = 7");
    CHECK(chip > 0);

    // Tick 1: walk from the cell into the corridor. At tick start the player was
    // in the cell (no hostile), so NO chip lands this tick.
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") == 12);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'chip'") == 0);

    // Tick 2: a non-combat verb (Wait) in the hostile's room STILL costs chip
    // (REQ-COMBAT-3/-8). meta.turn increments exactly once; the enemy idles
    // otherwise (Brick 1), so the only enemy footprint is the chip event.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 2);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") == 12 - chip);
    // The chip event is paired, stamped with THIS turn, actor = enemy, subject =
    // player, object = the chip amount.
    CHECK(queryInt(db,
                   ("SELECT COUNT(*) FROM events WHERE turn = 2 AND verb = 'chip' "
                    "AND actor = 7 AND subject = 3 AND object = " +
                    std::to_string(chip))
                       .c_str()) == 1);
    // Player-then-enemy ordering within the one transaction: the player's
    // 'waited' event precedes the enemy's 'chip' event by id.
    CHECK(queryInt(db,
                   "SELECT (SELECT id FROM events WHERE turn = 2 AND verb = 'waited') "
                   "< (SELECT id FROM events WHERE turn = 2 AND verb = 'chip')") == 1);

    // Tick 3: the enemy telegraphed on tick 2 (period 2), so its strike lands
    // now — attack + strike + chip all resolve in the SAME tick (REQ-COMBAT-12:
    // chip is irreducible and a land tick deals strike + chip). The goblin still
    // loses kBasicAttackDamage from the player's attack.
    const int64_t hpBeforeAttack =
        queryInt(db, "SELECT current FROM health WHERE entity = 3");
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 3);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") ==
          8 - kBasicAttackDamage);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") ==
          hpBeforeAttack - kStrikeDamage - chip);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE turn = 3 AND verb = 'attacked'") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE turn = 3 AND verb = 'chip'") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE turn = 3 AND verb = 'struck'") == 1);
}

// Defeat + grimoire drop (REQ-COMBAT-20, -30). Driving the seed enemy to 0 hp
// removes it from play (its entity id and name survive) and mints a portable
// grimoire into the room.
static void testCombatDefeat() {
    const TempDbFile worldPath("textworld_combat_defeat_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    const int64_t portablesInCorridorBefore =
        queryInt(db,
                 "SELECT COUNT(*) FROM portable p JOIN location l ON l.entity = p.entity "
                 "WHERE l.container = 2");

    // Into the corridor, then attack until the 8-hp goblin falls (2 hits at 4).
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);  // 8 -> 4, alive
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == 4);
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);  // 4 -> 0, defeated

    // Removed from play: hostile/health/location gone; the entity id and its
    // name SURVIVE (defeat is absence of hostile/location, not deletion).
    CHECK(queryInt(db, "SELECT COUNT(*) FROM hostile WHERE entity = 7") == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM health WHERE entity = 7") == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM location WHERE entity = 7") == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM entities WHERE id = 7") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM name WHERE entity = 7") == 1);

    // A portable grimoire dropped into the corridor (room 2).
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM portable p "
                   "JOIN location l ON l.entity = p.entity "
                   "JOIN name n ON n.entity = p.entity "
                   "WHERE l.container = 2 AND n.value LIKE '%grimoire%'") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM portable p "
                   "JOIN location l ON l.entity = p.entity WHERE l.container = 2") ==
          portablesInCorridorBefore + 1);

    // One 'defeated' event, subject = the enemy, object = the dropped grimoire
    // (a portable, so narration can name it in Step 6).
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'defeated' AND subject = 7") == 1);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events e JOIN portable p ON p.entity = e.object "
                   "WHERE e.verb = 'defeated' AND e.subject = 7") == 1);
}

// The "downed, not dead" model (REQ-COMBAT-23, -24, -25). Chip the player to 0
// and they wake in the dormitory cell at full health, having dropped their
// carried items where they fell; the enemy is restored and stays put.
static void testCombatDowned() {
    const TempDbFile worldPath("textworld_combat_downed_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    const int64_t maxHp = queryInt(db, "SELECT max FROM health WHERE entity = 3");
    const int64_t enemyMax = queryInt(db, "SELECT max FROM health WHERE entity = 7");

    // Pick up the wand (in the cell), carry it into the corridor.
    CHECK(runTurn(db, "take wand").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);

    // Wait out the chip clock until downed. The downing tick restores HP and
    // relocates the player, so loop until they are no longer in the corridor.
    for (int i = 0;
         i < 100 &&
         queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2;
         ++i) {
        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    }

    // Downed: relocated to the dormitory cell (room 1) at full health.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") == maxHp);

    // Dropped the wand at the fall room (the corridor, room 2); nothing deleted.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM entities WHERE id = 4") == 1);

    // The enemy is restored to full health and remains in its room.
    CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == enemyMax);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 7") == 2);

    // Exactly one 'downed' event: subject = player, object = the dormitory cell.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'downed' "
                   "AND subject = 3 AND object = 1") == 1);
}

// Substring helper for renderer output checks.
static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Telegraph → strike lane (REQ-COMBAT-9, -10, -12). A telegraph tick writes a
// pending_strike and deals no strike damage; the enemy's next turn lands it
// (strike + chip) and clears the row. Deterministic, driven by telegraph_period.
static void testCombatTelegraph() {
    const TempDbFile worldPath("textworld_combat_telegraph_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    const int64_t chip = queryInt(db, "SELECT chip FROM hostile WHERE entity = 7");

    auto playerHp = [&] {
        return queryInt(db, "SELECT current FROM health WHERE entity = 3");
    };
    auto pendingCount = [&] {
        return queryInt(db, "SELECT COUNT(*) FROM pending_strike WHERE entity = 7");
    };

    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // enter combat

    // Advance (waiting) until the enemy telegraphs. Capture HP just before the
    // telegraph tick so we can prove that tick dealt CHIP ONLY (no strike).
    int64_t hpBeforeTelegraph = 0;
    std::string telegraphText;
    bool telegraphed = false;
    for (int i = 0; i < 6 && !telegraphed; ++i) {
        hpBeforeTelegraph = playerHp();
        telegraphText = runTurn(db, "wait").output;
        if (pendingCount() == 1) telegraphed = true;
    }
    CHECK(telegraphed);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'telegraph'") >= 1);
    // No strike landed on the telegraph tick: HP fell by exactly chip.
    CHECK(playerHp() == hpBeforeTelegraph - chip);
    CHECK(contains(telegraphText, "winds up"));  // render template

    // Next tick, no counter: the strike lands — HP falls by strike + chip, and
    // the pending_strike row is cleared.
    const int64_t hpPreStrike = playerHp();
    const std::string strikeText = runTurn(db, "wait").output;
    CHECK(pendingCount() == 0);
    CHECK(playerHp() == hpPreStrike - kStrikeDamage - chip);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'struck'") >= 1);
    CHECK(contains(strikeText, "lands its blow"));  // render template
}

// Cast availability gate (REQ-COMBAT-7, -13, -14). An unknown or cooling spell is
// declined WITHOUT a tick; a valid cast ticks and sets the cooldown; basic
// attack is never blocked. Deterministic, AI disabled.
static void testCombatCastGate() {
    const TempDbFile worldPath("textworld_combat_castgate_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    auto turn = [&] { return queryInt(db, "SELECT value FROM meta WHERE key = 'turn'"); };
    auto playerHp = [&] {
        return queryInt(db, "SELECT current FROM health WHERE entity = 3");
    };

    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // enter combat

    // --- unknown spell: cataloged but not learned → declined, NO tick ---
    // Simulate an unlearned-but-cataloged spell by forgetting stun.
    db.exec("DELETE FROM known_spells WHERE entity = 3 AND spell = 'stun'");
    {
        const int64_t turnBefore = turn();
        const int64_t hpBefore = playerHp();
        const TurnResult r = runTurn(db, "cast stun");
        CHECK(r.outcome == TurnOutcome::NoTick);
        CHECK(turn() == turnBefore);        // no turn consumed
        CHECK(playerHp() == hpBefore);      // no enemy turn either
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM cooldowns WHERE spell = 'stun'") == 0);
    }

    // --- valid cast: ward is known and ready → ticks, sets the cooldown ---
    const int64_t wardCd =
        queryInt(db, "SELECT cooldown FROM spell_catalog WHERE spell = 'ward'");
    {
        const int64_t turnBefore = turn();
        CHECK(runTurn(db, "cast ward").outcome == TurnOutcome::Ticked);
        const int64_t castTurn = turn();
        CHECK(castTurn == turnBefore + 1);
        // ready_turn == the cast's execution turn + the immutable cooldown.
        CHECK(queryInt(db,
                       "SELECT ready_turn FROM cooldowns WHERE entity = 3 "
                       "AND spell = 'ward'") == castTurn + wardCd);
    }

    // --- recast ward while on cooldown → declined, NO tick ---
    {
        const int64_t turnBefore = turn();
        CHECK(runTurn(db, "cast ward").outcome == TurnOutcome::NoTick);
        CHECK(turn() == turnBefore);
    }

    // --- basic attack is NEVER blocked, even mid-cooldown ---
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);

    // --- after the cooldown elapses, ward is castable again ---
    // Wait until currentTurn reaches ready_turn, then the cast should tick.
    const int64_t readyTurn = queryInt(
        db, "SELECT ready_turn FROM cooldowns WHERE entity = 3 AND spell = 'ward'");
    for (int i = 0; i < 20 && turn() < readyTurn &&
                    queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2;
         ++i) {
        runTurn(db, "wait");
    }
    if (queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2) {
        CHECK(runTurn(db, "cast ward").outcome == TurnOutcome::Ticked);
    }
}

// Engine-appended status line: HP + per-spell cooldown readiness (REQ-COMBAT-15).
// Present in combat, absent outside it, and the SAME shared helper both render
// paths append (byte-identical). Deterministic.
static void testCombatStatusLine() {
    const TempDbFile worldPath("textworld_combat_statusline_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    const int64_t wardCd =
        queryInt(db, "SELECT cooldown FROM spell_catalog WHERE spell = 'ward'");

    // Outside combat: no status line.
    CHECK(!contains(runTurn(db, "look").output, "HP:"));

    // Entering combat: HP plus both known spells shown ready.
    {
        const std::string out = runTurn(db, "go north").output;
        CHECK(contains(out, "HP: 12/12"));
        CHECK(contains(out, "Stun: ready"));
        CHECK(contains(out, "Ward: ready"));
    }

    // Cast ward (cooldown N): the line shows "Ward: N" the moment it is cast.
    {
        const std::string out = runTurn(db, "cast ward").output;
        CHECK(contains(out, "Ward: " + std::to_string(wardCd)));
    }
    // It counts down each tick to "Ward: ready" at exactly tick T+N.
    {
        const std::string out = runTurn(db, "wait").output;
        CHECK(contains(out, "Ward: " + std::to_string(wardCd - 1)));
    }
    {
        // (wardCd == 2, so one more wait reaches ready.)
        const std::string out = runTurn(db, "wait").output;
        CHECK(contains(out, "Ward: ready"));
    }

    // Byte-identity of the append: render()'s output ends with exactly the shared
    // combatStatusLine helper — the same helper the AI path appends, so the line
    // is identical on both paths by construction.
    {
        const int64_t t = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const std::string rendered = render(db, t);
        const std::string sl = combatStatusLine(db, 3);
        CHECK(!sl.empty());
        CHECK(rendered.size() >= sl.size());
        CHECK(rendered.compare(rendered.size() - sl.size(), sl.size(), sl) == 0);
    }
}

// Counter resolution: Ward blocks, Stun interrupts (REQ-COMBAT-11, -19, -8).
// The counter window is the single player action on the tick the strike resolves.
static void testCombatCounter() {
    auto pending = [](Db& db) {
        return queryInt(db, "SELECT COUNT(*) FROM pending_strike WHERE entity = 7");
    };
    auto hp = [](Db& db) {
        return queryInt(db, "SELECT current FROM health WHERE entity = 3");
    };
    auto inCorridor = [](Db& db) {
        return queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2;
    };
    // Advance (waiting) into combat and up to a telegraph, so the NEXT tick is
    // the strike (the counter window). Leaves a pending strike set.
    auto advanceToTelegraph = [&](Db& db) {
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        for (int i = 0; i < 6 && pending(db) == 0; ++i) runTurn(db, "wait");
        CHECK(pending(db) == 1);
    };

    // --- Ward: the strike deals 0 (blocked); only chip lands ---
    {
        const TempDbFile p("textworld_combat_counter_ward.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        const int64_t chip = queryInt(db, "SELECT chip FROM hostile WHERE entity = 7");
        advanceToTelegraph(db);
        const int64_t hpBefore = hp(db);
        const std::string out = runTurn(db, "cast ward").output;
        CHECK(pending(db) == 0);                    // the strike resolved
        CHECK(hp(db) == hpBefore - chip);           // blocked: chip only, no strike
        const int64_t wardTurn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(wardTurn) + " AND verb = 'warded'").c_str()) == 1);
        // Offense XOR defense: no attack-damage event shares this tick (REQ-COMBAT-8).
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(wardTurn) + " AND verb = 'attacked'").c_str()) == 0);
        CHECK(contains(out, "against your ward"));  // render template
    }

    // --- No counter (Attack): the strike lands for its fixed damage ---
    {
        const TempDbFile p("textworld_combat_counter_attack.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        const int64_t chip = queryInt(db, "SELECT chip FROM hostile WHERE entity = 7");
        advanceToTelegraph(db);
        const int64_t hpBefore = hp(db);
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
        CHECK(pending(db) == 0);
        CHECK(hp(db) == hpBefore - kStrikeDamage - chip);  // strike + chip landed
    }

    // --- Stun: cancels the pending strike, suppresses the enemy for the CC
    // duration, then it resumes ---
    {
        const TempDbFile p("textworld_combat_counter_stun.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        advanceToTelegraph(db);
        const std::string out = runTurn(db, "cast stun").output;
        CHECK(pending(db) == 0);                                     // cancelled
        CHECK(queryInt(db, "SELECT COUNT(*) FROM status_effects "
                           "WHERE entity = 7 AND kind = 'stun' AND remaining > 0") == 1);
        CHECK(contains(out, "bind"));                                // render template
        // Still stunned next tick → no new telegraph created.
        if (inCorridor(db)) {
            CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
            CHECK(pending(db) == 0);
        }
        // After the CC lapses the enemy resumes telegraphing within a few ticks.
        bool resumed = false;
        for (int i = 0; i < 6 && !resumed && inCorridor(db); ++i) {
            runTurn(db, "wait");
            if (pending(db) == 1) resumed = true;
        }
        CHECK(resumed);
    }
}

// Combat narration via the permanent template path + the HP status line
// (REQ-COMBAT-37, -15). AI is disabled hermetically, so runTurn renders through
// the templates; the full Brick-1 loop is playable end to end, deterministically.
static void testCombatRender() {
    const TempDbFile worldPath("textworld_combat_render_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // Non-combat tick (in the cell, no hostile): NO status line.
    CHECK(!contains(runTurn(db, "look").output, "HP:"));

    // Walk into the corridor: a hostile now shares the room, so the status line
    // appears (full HP, no chip this tick — the player was in the cell at tick
    // start).
    CHECK(contains(runTurn(db, "go north").output, "HP: 12/12"));

    // Attack tick: the 'attacked' + 'chip' combat lines render, plus the status
    // line reflecting the chip already taken.
    {
        const std::string out = runTurn(db, "attack").output;
        CHECK(contains(out, "You strike the goblin grunt for"));
        CHECK(contains(out, "damage"));
        CHECK(contains(out, "goblin grunt wounds you for"));  // chip line
        CHECK(contains(out, "HP: 11/12"));
    }

    // Killing blow: the 'defeated' line names the fallen foe and its dropped
    // grimoire; combat is over, so NO status line follows.
    {
        const std::string out = runTurn(db, "attack").output;
        CHECK(contains(out, "goblin grunt falls"));
        CHECK(contains(out, "grimoire"));
        CHECK(!contains(out, "HP:"));
    }

    // The downed template (a fresh world; chip the player to 0). The downing
    // tick renders the wake-in-cell line and the dormitory room block.
    {
        const TempDbFile downPath("textworld_combat_render_down_tests.db");
        Db db2 = openWorld(downPath.string(), "tests/combat_fixture.sql");
        CHECK(runTurn(db2, "go north").outcome == TurnOutcome::Ticked);
        std::string downText;
        for (int i = 0;
             i < 100 &&
             queryInt(db2, "SELECT container FROM location WHERE entity = 3") == 2;
             ++i) {
            downText = runTurn(db2, "wait").output;
        }
        CHECK(contains(downText, "wake on the cold floor"));
        CHECK(queryInt(db2, "SELECT container FROM location WHERE entity = 3") == 1);
    }
}

static void testRender() {
    const TempDbFile worldPath("textworld_render_tests.db");

    Db db = openWorld(worldPath.string(), "tests/fixture.sql");
    int64_t turn = 0;

    // --- turn with no events renders as the empty string ---
    CHECK(render(db, 0).empty());
    CHECK(render(db, 999).empty());

    // --- moved: full room block for the destination (garden) ---
    tick(db, Action{Verb::Go, 0, "north"});
    ++turn;
    {
        const std::string out = render(db, turn);
        // Canon prose from the seed's garden description.
        CHECK(contains(out, "An overgrown walled garden"));
        // Exits list mentions the way back south.
        CHECK(contains(out, "south"));
        // The key (portable, in the garden) is visible by name.
        CHECK(contains(out, "key"));
    }

    // --- looked with NULL detail: room block for the actor's current room ---
    tick(db, Action{Verb::Look, 0, ""});
    ++turn;
    {
        const std::string out = render(db, turn);
        CHECK(contains(out, "An overgrown walled garden"));
        CHECK(contains(out, "Exits: south."));
    }

    // Back to the stone hall, where the lantern waits.
    tick(db, Action{Verb::Go, 0, "south"});
    ++turn;
    {
        const std::string out = render(db, turn);
        CHECK(contains(out, "A vaulted hall of grey stone"));
        CHECK(contains(out, "Exits: north."));
        CHECK(contains(out, "lantern"));
    }

    // --- inventory while empty-handed ---
    tick(db, Action{Verb::Inventory, 0, ""});
    ++turn;
    CHECK(contains(render(db, turn), "You are carrying nothing."));

    // --- took template ---
    tick(db, Action{Verb::Take, 4, ""});
    ++turn;
    CHECK(render(db, turn) == "You take the lantern.\n");

    // --- inventory while carrying the lantern ---
    tick(db, Action{Verb::Inventory, 0, ""});
    ++turn;
    {
        const std::string out = render(db, turn);
        CHECK(contains(out, "lantern"));
        CHECK(!contains(out, "carrying nothing"));
    }

    // --- dropped template ---
    tick(db, Action{Verb::Drop, 4, ""});
    ++turn;
    CHECK(render(db, turn) == "You drop the lantern.\n");

    // --- waited template ---
    tick(db, Action{Verb::Wait, 0, ""});
    ++turn;
    CHECK(render(db, turn) == "Time passes.\n");

    // --- failed: the event's detail text verbatim ---
    tick(db, Action{Verb::Go, 0, "west"});
    ++turn;
    CHECK(render(db, turn) == "You can't go that way.\n");

    // --- renderError: tier-a path, not event-sourced ---
    CHECK(renderError("I don't understand that.") == "I don't understand that.\n");

    // --- render purity: a render call performs no writes. All ticks above
    // are committed (default DELETE journal mode, no WAL), so the world file
    // bytes are the full committed state; byte-identical before/after proves
    // render touched nothing. ---
    {
        const std::string bytesBefore = readFileBytes(worldPath);
        CHECK(!bytesBefore.empty());
        for (int64_t t = 0; t <= turn; ++t) (void)render(db, t);
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }
}

static void testLoop() {
    // --- ticked turn: go north → garden prose, meta.turn incremented ---
    {
        const TempDbFile worldPath("textworld_loop_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 0);

        const TurnResult r = runTurn(db, "go north");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(contains(r.output, "An overgrown walled garden"));
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);

        // --- tier a: unknown verb and bare verb → NoTick, turn unchanged ---
        const TurnResult junk = runTurn(db, "frobnicate");
        CHECK(junk.outcome == TurnOutcome::NoTick);
        CHECK(!junk.output.empty());
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);

        const TurnResult bare = runTurn(db, "go");
        CHECK(bare.outcome == TurnOutcome::NoTick);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);

        // --- quit: Quit outcome, no tick ---
        const TurnResult quit = runTurn(db, "quit");
        CHECK(quit.outcome == TurnOutcome::Quit);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 1);  // only the move
    }

    // --- tier boundary through the production path (REQ-PROTO-6): both legs
    // driven through runTurn on a fresh world (player in room 1, key in 2) ---
    {
        const TempDbFile worldPath("textworld_loop_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        // Tier b: recognized noun, wrong room → Ticked, one 'failed' event,
        // key untouched.
        const TurnResult b = runTurn(db, "take key");
        CHECK(b.outcome == TurnOutcome::Ticked);
        CHECK(contains(b.output, "You don't see that here."));
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'failed'") == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);

        // Tier a: unknown noun → NoTick, turn and events unchanged.
        const TurnResult a = runTurn(db, "take zeppelin");
        CHECK(a.outcome == TurnOutcome::NoTick);
        CHECK(!a.output.empty());
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 1);
    }

    // --- scripted sequence on a fresh world: each line Ticked, turn == 6 ---
    {
        const TempDbFile worldPath("textworld_loop_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        const char* script[] = {"look",         "take lantern", "go north",
                                "drop lantern", "inventory",    "wait"};
        for (const char* line : script) {
            const TurnResult r = runTurn(db, line);
            CHECK(r.outcome == TurnOutcome::Ticked);
        }
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 6);
        // Complete transcript: every committed turn appears in events.
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT turn) FROM events") == 6);
        // The lantern ended up dropped in the garden (room 2).
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);
    }

    // --- tier c through the production path: a mid-tick engine error rolls
    // back (turn and events untouched) and runTurn returns EngineError instead
    // of letting the exception — or a throw from rollback itself — escape. ---
    {
        const TempDbFile worldPath("textworld_loop_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("DELETE FROM player");  // playerId() will throw mid-tick
        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

        // Fault on a MUTATING verb: had the tick partially executed, the
        // player would have moved. Rollback must leave every component row
        // exactly as it was.
        const TurnResult r = runTurn(db, "go north");
        CHECK(r.outcome == TurnOutcome::EngineError);
        CHECK(!r.output.empty());
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);

        // The connection is still usable afterward: restore the player tag
        // and a normal turn ticks — proving the error path left no
        // transaction dangling and rollback itself did not throw.
        db.exec("INSERT INTO player (entity) VALUES (3)");
        const TurnResult ok = runTurn(db, "wait");
        CHECK(ok.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore + 1);
    }
}

// --- facts builder (REQ-PROSE-6, REQ-PROSE-7): pure (db, turn) → TurnFacts,
// payload parseable JSON with exactly the REQ-PROSE-7 keys, name-resolved,
// id-free, with validation anchors. No network anywhere. ---

// Single text-value query helper.
static std::string queryText(Db& db, const char* sql) {
    Stmt s = db.prepare(sql);
    CHECK(s.step());
    return s.colText(0);
}

// Collect every string VALUE in a JSON tree (keys are the fixed payload
// schema; values are what carries world data to the model).
static void collectStrings(const nlohmann::json& j, std::vector<std::string>& out) {
    if (j.is_string()) {
        out.push_back(j.get<std::string>());
    } else if (j.is_object() || j.is_array()) {
        for (const auto& el : j) collectStrings(el, out);
    }
}

// REQ-PROSE-6 hygiene: no entity/row id (no digit ever appears in a payload
// string — turn numbers travel as JSON numbers), no file path, no table name,
// and never the "something" placeholder.
static void checkPayloadHygiene(const std::string& payload) {
    CHECK(!contains(payload, "something"));

    std::vector<std::string> values;
    collectStrings(nlohmann::json::parse(payload), values);
    CHECK(!values.empty());
    for (const std::string& v : values) {
        CHECK(v.find_first_of("0123456789") == std::string::npos);  // no ids
        CHECK(!contains(v, "/"));    // no file paths
        CHECK(!contains(v, ".db"));  // no file paths
        for (const char* table :
             {"entities", "location", "portable", "description", "meta"}) {
            CHECK(!contains(v, table));  // no table names
        }
    }
}

// Exact shape of a recent_events entry: turn (integer) + verb (string), plus
// subject (string) only when present — no other keys. Pins the numeric side
// the string-value sweep can't see: an entity id leaking as a JSON number
// (e.g. "subject": 4) would slip past checkPayloadHygiene.
static void checkRecentEntryShape(const nlohmann::json& e) {
    CHECK(e.is_object());
    CHECK(e.contains("turn"));
    CHECK(e["turn"].is_number_integer());  // the only numeric field
    CHECK(e.contains("verb"));
    CHECK(e["verb"].is_string());
    if (e.contains("subject")) {
        CHECK(e["subject"].is_string());  // name, never a numeric id
        CHECK(e.size() == 3);
    } else {
        CHECK(e.size() == 2);
    }
    CHECK(!e.contains("object"));  // ids never travel, not even as numbers
}

static void testProseFacts() {
    using nlohmann::json;

    const TempDbFile worldPath("textworld_prose_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Canon prose fetched independently, compared verbatim below.
    const std::string hallProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 1");
    const std::string gardenProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 2");

    // --- turn 1: take lantern — plain turn, full payload shape ---
    CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 1);
        const json p = json::parse(f.payload);

        // Top-level keys are EXACTLY the REQ-PROSE-7 set.
        CHECK(p.is_object());
        CHECK(p.size() == 4);
        CHECK(p.contains("events"));
        CHECK(p.contains("room"));
        CHECK(p.contains("inventory"));
        CHECK(p.contains("recent_events"));

        CHECK(p["events"].size() == 1);
        CHECK(p["events"][0]["verb"] == "took");
        CHECK(p["events"][0]["subject"] == "lantern");  // name, not id 4
        CHECK(!p["events"][0].contains("detail"));      // NULL detail omitted
        CHECK(!p["events"][0].contains("object"));      // ids never travel

        // Room slice: name, canon_description, exits, items — nothing else.
        CHECK(p["room"].size() == 4);
        CHECK(p["room"]["name"] == "stone hall");
        CHECK(p["room"]["canon_description"] == hallProse);
        CHECK(p["room"]["exits"] == json::array({"north"}));
        CHECK(p["room"]["items"].empty());  // the lantern is in hand now

        CHECK(p["inventory"] == json::array({"lantern"}));
        CHECK(p["recent_events"].empty());  // nothing precedes turn 1

        // Anchors: plain turn — no canon required, no failures.
        CHECK(!f.canonRequired);
        CHECK(f.failedDetails.empty());
    }

    // --- turn 2: go north — 'moved' turn, room slice is the DESTINATION ---
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 2);
        const json p = json::parse(f.payload);

        CHECK(p["events"].size() == 1);
        CHECK(p["events"][0]["verb"] == "moved");

        CHECK(p["room"]["name"] == "garden");
        CHECK(p["room"]["canon_description"] == gardenProse);  // verbatim
        CHECK(p["room"]["exits"] == json::array({"south"}));
        CHECK(p["room"]["items"] == json::array({"key"}));
        CHECK(p["inventory"] == json::array({"lantern"}));

        CHECK(p["recent_events"].size() == 1);
        CHECK(p["recent_events"][0]["turn"] == 1);
        CHECK(p["recent_events"][0]["verb"] == "took");
        CHECK(p["recent_events"][0]["subject"] == "lantern");

        // Anchors: room-describing turn carries the destination canon.
        CHECK(f.canonRequired);
        CHECK(f.canonText == gardenProse);
        CHECK(f.failedDetails.empty());
    }

    // --- turn 3: look — room-describing; zero-id event omits subject ---
    CHECK(runTurn(db, "look").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 3);
        const json p = json::parse(f.payload);
        CHECK(p["events"].size() == 1);
        CHECK(p["events"][0]["verb"] == "looked");
        CHECK(!p["events"][0].contains("subject"));  // subject=0 → omitted
        CHECK(!p["events"][0].contains("object"));   // object=0 → omitted
        CHECK(!p["events"][0].contains("detail"));   // NULL → omitted
        CHECK(p["room"]["canon_description"] == gardenProse);
        CHECK(f.canonRequired);
        CHECK(f.canonText == gardenProse);
        CHECK(f.failedDetails.empty());
    }

    // --- turn 4: inventory — 'looked' with detail is NOT room-describing ---
    CHECK(runTurn(db, "inventory").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 4);
        const json p = json::parse(f.payload);
        CHECK(p["events"][0]["verb"] == "looked");
        CHECK(p["events"][0]["detail"] == "inventory");
        CHECK(!p["events"][0].contains("subject"));
        CHECK(!f.canonRequired);
        CHECK(f.failedDetails.empty());
    }

    // --- turn 5: wall bump — 'failed' anchor carries the detail verbatim ---
    CHECK(runTurn(db, "go east").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 5);
        const json p = json::parse(f.payload);
        CHECK(p["events"][0]["verb"] == "failed");
        CHECK(p["events"][0]["detail"] == "You can't go that way.");
        CHECK(!p["events"][0].contains("subject"));
        CHECK(!f.canonRequired);
        CHECK(f.failedDetails.size() == 1);
        CHECK(f.failedDetails[0] == "You can't go that way.");
    }

    // --- turns 6-8: waits, so turn 8 has 7 preceding events → cap at 6 ---
    for (int i = 0; i < 3; ++i) {
        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    }
    {
        const TurnFacts f = buildFacts(db, 8);
        const json p = json::parse(f.payload);
        CHECK(p["events"][0]["verb"] == "waited");
        CHECK(p["recent_events"].size() == 6);  // capped
        // Chronological window: the six turns preceding 8 are 2..7.
        CHECK(p["recent_events"].front()["turn"] == 2);
        CHECK(p["recent_events"].back()["turn"] == 7);
        CHECK(p["recent_events"].front()["verb"] == "moved");
    }

    // --- hygiene sweep over every payload built this session, plus exact
    // entry shape for every recent_events row (with- and without-subject
    // entries both occur across turns 1..8) ---
    for (int64_t t = 1; t <= 8; ++t) {
        const TurnFacts f = buildFacts(db, t);
        checkPayloadHygiene(f.payload);
        // Bind the parsed payload to a named json first: iterating
        // json::parse(...)["recent_events"] directly would range-for over a
        // reference into a destroyed temporary (silently zero iterations).
        const json p = json::parse(f.payload);
        for (const auto& e : p["recent_events"]) {
            checkRecentEntryShape(e);
        }
    }

    // --- purity: buildFacts performs no writes. All ticks above are
    // committed, so the world file bytes are the full committed state;
    // byte-identical before/after proves the builder touched nothing. ---
    {
        const std::string bytesBefore = readFileBytes(worldPath);
        CHECK(!bytesBefore.empty());
        for (int64_t t = 0; t <= 8; ++t) (void)buildFacts(db, t);
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }
}

// --- AI resolver: scope-context builder (REQ-RESOLVE-6, -7). Pure function of
// (db, line): SELECTs only, exactly the five REQ-RESOLVE-7 fields, no ids, no
// network. Mirrors testProseFacts's payload-shape + hygiene + purity checks. ---
static void testNlResolveContext() {
    using nlohmann::json;

    const TempDbFile worldPath("textworld_nlresolve_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Fresh seed: player in the stone hall (1), lantern (4) here, key (5) in
    // the garden (2), inventory empty. The raw line travels verbatim.
    {
        const ResolveContext ctx = buildResolveContext(db, "take the lantern");
        const json p = json::parse(ctx.payload);

        // Top-level keys are EXACTLY the REQ-RESOLVE-7 set — five, nothing else.
        CHECK(p.is_object());
        CHECK(p.size() == 5);
        CHECK(p.contains("input"));
        CHECK(p.contains("room"));
        CHECK(p.contains("exits"));
        CHECK(p.contains("items"));
        CHECK(p.contains("inventory"));

        CHECK(p["input"] == "take the lantern");  // raw line, verbatim
        CHECK(p["room"] == "stone hall");
        CHECK(p["exits"] == json::array({"north"}));
        CHECK(p["items"] == json::array({"lantern"}));
        CHECK(p["inventory"].empty());

        // No entity/row id anywhere (REQ-RESOLVE-6): reuse the prose hygiene
        // sweep — no digit in any string value, no table name, no file path.
        checkPayloadHygiene(ctx.payload);
    }

    // The slice tracks the actor: take the lantern, walk north, and the
    // context now shows the garden, its south exit, the visible key, and the
    // lantern moved into inventory.
    CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    {
        const ResolveContext ctx = buildResolveContext(db, "drop lantern");
        const json p = json::parse(ctx.payload);
        CHECK(p.size() == 5);
        CHECK(p["room"] == "garden");
        CHECK(p["exits"] == json::array({"south"}));
        CHECK(p["items"] == json::array({"key"}));
        CHECK(p["inventory"] == json::array({"lantern"}));
        checkPayloadHygiene(ctx.payload);
    }

    // Purity: the builder performs no writes. All ticks above are committed,
    // so the world file bytes are the full committed state; byte-identical
    // before/after proves the builder touched nothing (mirrors testProseFacts).
    {
        const std::string bytesBefore = readFileBytes(worldPath);
        CHECK(!bytesBefore.empty());
        (void)buildResolveContext(db, "look");
        (void)buildResolveContext(db, "go south");
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }
}

// --- ISA system prompt (REQ-RESOLVE-12): the prompt IS the instruction-set
// spec. Mirrors testProseRequestBody's prompt substring sweep — STRUCTURE
// only (names the seven verbs, states every lowering rule). Prompt QUALITY is
// the live smoke's job (Step 9), never asserted here. ---
static void testNlResolvePrompt() {
    const std::string sys = kResolveSystemPrompt;
    CHECK(!sys.empty());

    // All nine ISA verbs are named (attack + cast added in the combat brick).
    for (const char* verb :
         {"look", "go", "take", "drop", "inventory", "wait", "quit", "attack",
          "cast"}) {
        CHECK(sys.find(verb) != std::string::npos);
    }

    // The tool is named; output is a tool call, not prose.
    CHECK(sys.find("emit_action") != std::string::npos);

    // One action only; multi-intent → no call.
    CHECK(sys.find("exactly one action") != std::string::npos);
    CHECK(sys.find("Never emit more than one action") != std::string::npos);

    // Subject must be a supplied in-scope noun, copied verbatim; no new nouns.
    CHECK(sys.find("copied verbatim") != std::string::npos);
    CHECK(sys.find("Introduce no noun") != std::string::npos);

    // direction for go is a movement/compass word.
    CHECK(sys.find("compass word") != std::string::npos);

    // Unknown / non-single-action → no tool call.
    CHECK(sys.find("make no tool call") != std::string::npos);

    // No pronoun / anaphora resolution (v2).
    CHECK(sys.find("pronoun") != std::string::npos);

    // Recognition, not applicability — the tier-b boundary.
    CHECK(sys.find("recognition only") != std::string::npos);
}

// --- transport seam (REQ-PROSE-9, REQ-PROSE-10, REQ-PROSE-15): aiRender's
// HTTP transport is injected; every transport in this suite is a fake lambda
// returning a canned HttpResponse, so the default test run makes NO network
// access — the libcurl production transport is never invoked here. ---
static void testProseTransport() {
    const TempDbFile worldPath("textworld_transport_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // One committed turn to render.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);

    // --- transportError fake → nullopt, no crash, exactly ONE call ---
    // The counter proves no retry loop hides behind a transport failure.
    {
        int calls = 0;
        const HttpTransport failing = [&calls](const std::string& body) {
            ++calls;
            CHECK(!body.empty());  // the transport is handed a request body
            HttpResponse r;
            r.transportError = true;  // timeout / connect failure / curl error
            return r;
        };
        CHECK(!aiRender(db, 1, failing).has_value());
        CHECK(calls == 1);
    }

    // --- HTTP error status (non-2xx) → nullopt, still exactly one call ---
    {
        int calls = 0;
        const HttpTransport overloaded = [&calls](const std::string&) {
            ++calls;
            HttpResponse r;
            r.status = 529;
            r.body = "{\"type\":\"error\"}";
            return r;
        };
        CHECK(!aiRender(db, 1, overloaded).has_value());
        CHECK(calls == 1);
    }

    // --- 200 with a body but NO stop_reason → rejected by the validation
    // gate (clause a) → nullopt, and still exactly one call ---
    {
        int calls = 0;
        const HttpTransport ok = [&calls](const std::string&) {
            ++calls;
            HttpResponse r;
            r.status = 200;
            r.body = "{\"content\":[{\"type\":\"text\",\"text\":\"Prose.\"}]}";
            return r;
        };
        CHECK(!aiRender(db, 1, ok).has_value());
        CHECK(calls == 1);
    }

    // --- a transport that THROWS → aiRender's try/catch absorbs it
    // (REQ-PROSE-3: no AI-path failure may crash a turn) → nullopt ---
    {
        const HttpTransport throwing = [](const std::string&) -> HttpResponse {
            throw std::runtime_error("socket exploded");
        };
        CHECK(!aiRender(db, 1, throwing).has_value());
    }
}

// Saves ANY env var on construction, restores it on destruction — unsetenv
// if it was unset. Env-var discipline: every test that touches
// ANTHROPIC_API_KEY / TEXTWORLD_AI holds one of these for the duration.
struct ScopedEnvVar {
    const char* name;
    bool hadPrior;
    std::string priorValue;

    explicit ScopedEnvVar(const char* n) : name(n) {
        const char* prior = std::getenv(name);
        hadPrior = prior != nullptr;
        if (hadPrior) priorValue = prior;
    }
    ~ScopedEnvVar() {
        if (hadPrior) {
            setenv(name, priorValue.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
};

// REQ-EXITS-4: the Exits line lists realized exits always, latent exits only
// when the architect is enabled, and a latent exit renders IDENTICALLY to a
// realized one (no marker). Build a room (stone hall, room 1) with one realized
// exit (north→garden, from the fixture) and one latent exit (up, dest NULL),
// then render it with the architect enabled vs disabled. Defined here, after
// ScopedEnvVar, because it toggles ANTHROPIC_API_KEY under a guard.
static void testExitDisplayInvariant() {
    const TempDbFile worldPath("textworld_exitdisplay_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Plant a latent exit on room 1 (the player's room): up, dest NULL.
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");

    // This test owns the AI env vars for its duration (the suite runs hermetic
    // with both unset); the guards restore whatever was there.
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");

    // --- architect ENABLED: both the realized and the latent exit list ---
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    {
        const std::string out = renderRoomOf(db, 3);
        // Both directions present, ORDER BY direction → north, up.
        CHECK(contains(out, "Exits: north, up."));
        // The latent 'up' carries NO marker distinguishing it from the realized
        // 'north' — both are bare direction words joined identically. The exact
        // line above plus the absence of any decoration on 'up' proves it.
        CHECK(!contains(out, "up*"));
        CHECK(!contains(out, "up?"));
        CHECK(!contains(out, "(up"));
    }

    // --- architect DISABLED (key unset): only the realized exit lists ---
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("TEXTWORLD_AI");
    {
        const std::string out = renderRoomOf(db, 3);
        CHECK(contains(out, "Exits: north."));
        // The latent 'up' is hidden entirely (room-1 prose contains no "up").
        CHECK(!contains(out, "up"));
    }
}

// Saves TEXTWORLD_MODEL on construction, restores it on destruction —
// unsetenv if it was unset. Env-var discipline: the suite must pass (and
// leave the environment untouched) regardless of the developer's shell env.
struct ScopedModelEnv {
    bool hadPrior;
    std::string priorValue;

    ScopedModelEnv() {
        const char* prior = std::getenv("TEXTWORLD_MODEL");
        hadPrior = prior != nullptr;
        if (hadPrior) priorValue = prior;
    }
    ~ScopedModelEnv() {
        if (hadPrior) {
            setenv("TEXTWORLD_MODEL", priorValue.c_str(), 1);
        } else {
            unsetenv("TEXTWORLD_MODEL");
        }
    }
};

// --- request body (REQ-PROSE-8) + system prompt content (REQ-PROSE-11):
// buildRequestBody is a pure string→string function (plus the TEXTWORLD_MODEL
// env read), so it is asserted by parsing its output back as JSON. ---
static void testProseRequestBody() {
    const ScopedModelEnv guard;

    // Payload with characters that must survive JSON re-embedding verbatim.
    const std::string payload =
        "{\"events\":[{\"verb\":\"failed\",\"detail\":\"You can't go that "
        "way.\"}]}";

    // --- defaults: model, max_tokens, message shape, forbidden keys ---
    unsetenv("TEXTWORLD_MODEL");
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
        CHECK(j["max_tokens"] == 1024);

        // REQ-PROSE-8 forbidden keys: no thinking, no streaming (and no
        // stray top-level keys at all beyond the four we build).
        CHECK(!j.contains("thinking"));
        CHECK(!j.contains("stream"));
        CHECK(j.size() == 4);  // model, max_tokens, system, messages

        // Exactly one user message carrying the payload verbatim.
        CHECK(j["messages"].is_array());
        CHECK(j["messages"].size() == 1);
        CHECK(j["messages"][0]["role"] == "user");
        CHECK(j["messages"][0]["content"] == payload);

        // --- system prompt: non-empty, contains each REQ-PROSE-11 rule ---
        CHECK(j["system"].is_string());
        const std::string sys = j["system"].get<std::string>();
        CHECK(!sys.empty());
        // Style anchor: second-person, present-tense narrator.
        CHECK(sys.find("second person") != std::string::npos);
        CHECK(sys.find("present tense") != std::string::npos);
        // Narrate only the supplied events.
        CHECK(sys.find("only the supplied events") != std::string::npos);
        // Atmosphere permitted, but no noun absent from the facts.
        CHECK(sys.find("noun") != std::string::npos);
        CHECK(sys.find("absent from the facts") != std::string::npos);
        // Never contradict a fact.
        CHECK(sys.find("Never contradict a fact") != std::string::npos);
        // Canon description verbatim; failed detail verbatim, no paraphrase.
        CHECK(sys.find("canon_description") != std::string::npos);
        CHECK(sys.find("verbatim") != std::string::npos);
        CHECK(sys.find("paraphrase") != std::string::npos);
        // Sentence budget.
        CHECK(sys.find("1-4 sentences") != std::string::npos);
        // Plain text, no markdown, no meta-commentary, final answer only.
        CHECK(sys.find("plain text") != std::string::npos);
        CHECK(sys.find("no markdown") != std::string::npos);
        CHECK(sys.find("meta-commentary") != std::string::npos);
        CHECK(sys.find("final answer") != std::string::npos);
    }

    // --- TEXTWORLD_MODEL set and non-empty → override honored ---
    setenv("TEXTWORLD_MODEL", "claude-test-model", 1);
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildRequestBody(payload));
        CHECK(j["model"] == "claude-test-model");
        CHECK(j["max_tokens"] == 1024);  // override touches ONLY the model
    }

    // --- TEXTWORLD_MODEL set but empty → default, not "" ---
    setenv("TEXTWORLD_MODEL", "", 1);
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
    }
    // guard's destructor restores the caller's TEXTWORLD_MODEL here.
}

// --- resolver request body + emit_action tool schema (REQ-RESOLVE-8, -9).
// Pure string→string (plus env read); parsed back as JSON. Mirrors
// testProseRequestBody, including its exact-top-level-key-set stray-key guard. ---
static void testNlResolveRequestBody() {
    using nlohmann::json;
    const ScopedModelEnv guard;

    // Context payload with characters that must survive JSON re-embedding.
    const std::string payload =
        "{\"input\":\"take the lantern\",\"room\":\"stone hall\","
        "\"exits\":[\"north\"],\"items\":[\"lantern\"],\"inventory\":[]}";

    // --- defaults: model, max_tokens, tool schema, forbidden/stray keys ---
    unsetenv("TEXTWORLD_MODEL");
    {
        const json j = json::parse(buildResolveRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
        CHECK(j["max_tokens"] == 512);

        // REQ-RESOLVE-9 forbidden keys + exact top-level set (no thinking,
        // stream, or cache-control key can slip in) — the resolver analog of
        // testProseRequestBody's j.size()==4 stray-key guard.
        CHECK(!j.contains("thinking"));
        CHECK(!j.contains("stream"));
        CHECK(j.size() == 6);  // model, max_tokens, system, messages, tools, tool_choice

        // system prompt is the ISA prompt; one user message carries the payload.
        CHECK(j["system"] == std::string(kResolveSystemPrompt));
        CHECK(j["messages"].is_array());
        CHECK(j["messages"].size() == 1);
        CHECK(j["messages"][0]["role"] == "user");
        CHECK(j["messages"][0]["content"] == payload);

        // tool_choice: auto (object form).
        CHECK(j["tool_choice"]["type"] == "auto");

        // exactly one emit_action tool.
        CHECK(j["tools"].is_array());
        CHECK(j["tools"].size() == 1);
        const json& tool = j["tools"][0];
        CHECK(tool["name"] == "emit_action");

        // input schema: object; verb enum is exactly the nine ISA verbs
        // (attack + cast added in the combat brick, REQ-COMBAT-38).
        const json& schema = tool["input_schema"];
        CHECK(schema["type"] == "object");
        const json& verb = schema["properties"]["verb"];
        CHECK(verb["type"] == "string");
        CHECK(verb["enum"] ==
              json::array({"look", "go", "take", "drop", "inventory", "wait",
                           "quit", "attack", "cast"}));

        // subject and direction present; verb is the ONLY required field.
        CHECK(schema["properties"].contains("subject"));
        CHECK(schema["properties"].contains("direction"));
        CHECK(schema["required"] == json::array({"verb"}));
    }

    // --- TEXTWORLD_MODEL set and non-empty → override honored, only the model ---
    setenv("TEXTWORLD_MODEL", "claude-test-model", 1);
    {
        const json j = json::parse(buildResolveRequestBody(payload));
        CHECK(j["model"] == "claude-test-model");
        CHECK(j["max_tokens"] == 512);
    }

    // --- TEXTWORLD_MODEL set but empty → default, not "" ---
    setenv("TEXTWORLD_MODEL", "", 1);
    {
        const json j = json::parse(buildResolveRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
    }
    // guard's destructor restores the caller's TEXTWORLD_MODEL here.
}

// Canned 200 tool_use response in the documented Anthropic shape (Step-5
// fixture): stop_reason "tool_use" + one emit_action tool_use block. subject /
// direction are included only when non-empty (bare verbs carry neither) — the
// resolver analog of cannedResponse, built from documented structure, never a
// network probe.
static HttpResponse cannedToolUse(const std::string& verb,
                                  const std::string& subject = "",
                                  const std::string& direction = "") {
    nlohmann::json input;
    input["verb"] = verb;
    if (!subject.empty()) input["subject"] = subject;
    if (!direction.empty()) input["direction"] = direction;

    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = "toolu_test";
    block["name"] = "emit_action";
    block["input"] = std::move(input);

    nlohmann::json j;
    j["stop_reason"] = "tool_use";
    j["content"] = nlohmann::json::array({std::move(block)});

    HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

// Canned 200 response with NO emit_action tool call — the model declining
// (unknown / multi-intent). Zero emit_action blocks drive the gate's clean
// no-action path (nullopt, no diagnostic).
static HttpResponse cannedNoToolUse() {
    nlohmann::json j;
    j["stop_reason"] = "end_turn";
    j["content"] = nlohmann::json::array(
        {{{"type", "text"}, {"text", "I'm not sure what you mean."}}});
    HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

// --- validation & mapping gate (REQ-RESOLVE-13 a-e): pure function of
// (response, db). Fixtures from cannedToolUse cover each clause plus the
// no-tool-call / two-tool-call / out-of-set / unknown-subject paths. Uses the
// seeded db for the clause-c lookup (lantern=4, key=5). No network; the gate is
// called directly here (outside any try/catch), so a throw would crash — it
// must never throw. ---
static void testNlResolveGate() {
    const TempDbFile worldPath("textworld_nlgate_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Clause e: argument-free verbs map with subject 0, direction empty.
    {
        auto a = validateAndLower(cannedToolUse("look"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Look);
        CHECK(a->subject == 0);
        CHECK(a->direction.empty());
    }
    CHECK(validateAndLower(cannedToolUse("inventory"), db)->verb == Verb::Inventory);
    CHECK(validateAndLower(cannedToolUse("wait"), db)->verb == Verb::Wait);
    CHECK(validateAndLower(cannedToolUse("quit"), db)->verb == Verb::Quit);
    // A stray argument on an argument-free verb is ignored (clause e).
    {
        auto a = validateAndLower(cannedToolUse("look", "lantern", "north"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Look);
        CHECK(a->subject == 0);
        CHECK(a->direction.empty());
    }

    // Clause c: take/drop resolve the subject to a non-zero id, assigned
    // mechanically by lookupNoun.
    {
        auto a = validateAndLower(cannedToolUse("take", "lantern"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Take);
        CHECK(a->subject == 4);
    }
    {
        auto a = validateAndLower(cannedToolUse("drop", "key"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Drop);
        CHECK(a->subject == 5);
    }
    // Clause c failures: unknown noun, and missing subject → nullopt.
    CHECK(!validateAndLower(cannedToolUse("take", "zeppelin"), db));
    CHECK(!validateAndLower(cannedToolUse("take"), db));

    // Clause d: go carries the direction verbatim; missing/empty → nullopt.
    {
        auto a = validateAndLower(cannedToolUse("go", "", "north"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Go);
        CHECK(a->direction == "north");
        CHECK(a->subject == 0);
    }
    CHECK(!validateAndLower(cannedToolUse("go"), db));

    // Clause b: out-of-set verb → nullopt.
    CHECK(!validateAndLower(cannedToolUse("frobnicate", "lantern"), db));

    // Clause a: no tool call (0 emit_action blocks) → nullopt, cleanly.
    CHECK(!validateAndLower(cannedNoToolUse(), db));

    // Clause a: two emit_action blocks (multi-intent leak) → nullopt.
    {
        nlohmann::json blk;
        blk["type"] = "tool_use";
        blk["id"] = "toolu_a";
        blk["name"] = "emit_action";
        blk["input"] = {{"verb", "look"}};
        nlohmann::json j;
        j["stop_reason"] = "tool_use";
        j["content"] = nlohmann::json::array({blk, blk});
        HttpResponse r;
        r.status = 200;
        r.body = j.dump();
        CHECK(!validateAndLower(r, db));
    }

    // Clause a HTTP half: non-200 and malformed body → nullopt.
    {
        HttpResponse r;
        r.status = 500;
        r.body = cannedToolUse("take", "lantern").body;  // valid body, bad status
        CHECK(!validateAndLower(r, db));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "}{ not json";
        CHECK(!validateAndLower(r, db));
    }
    {
        HttpResponse r;  // transport error: status 0
        r.transportError = true;
        CHECK(!validateAndLower(r, db));
    }
}

// --- aiResolve orchestration (REQ-RESOLVE-1, -3, -5, -10): context -> body ->
// ONE transport call -> gate, whole body in try/catch. Every transport here is
// a fake lambda, so the default test run makes NO network access. Mirrors
// testProseTransport's fake-transport discipline. ---
static void testNlResolveAiResolve() {
    const TempDbFile worldPath("textworld_nlairesolve_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Canned emit_action -> correct Action; transport invoked EXACTLY once.
    {
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedToolUse("take", "lantern");
        };
        auto a = aiResolve(db, "grab the lantern", fake);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Take);
        CHECK(a->subject == 4);
        CHECK(calls == 1);  // no retries (REQ-RESOLVE-10)
    }

    // Transport error -> nullopt.
    {
        HttpTransport fake = [](const std::string&) {
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        CHECK(!aiResolve(db, "take lantern", fake));
    }

    // Malformed body -> nullopt.
    {
        HttpTransport fake = [](const std::string&) {
            HttpResponse r;
            r.status = 200;
            r.body = "}{ not json";
            return r;
        };
        CHECK(!aiResolve(db, "take lantern", fake));
    }

    // THROWING transport -> caught, nullopt, no crash (REQ-RESOLVE-3).
    {
        HttpTransport fake = [](const std::string&) -> HttpResponse {
            throw std::runtime_error("socket exploded");
        };
        CHECK(!aiResolve(db, "take lantern", fake));
    }

    // No-tool-call -> nullopt cleanly (the fallback path).
    {
        HttpTransport fake = [](const std::string&) { return cannedNoToolUse(); };
        CHECK(!aiResolve(db, "smell the flowers", fake));
    }

    // DB byte-identity across aiResolve (REQ-RESOLVE-5): resolver path is
    // read-only, so the world file bytes are unchanged before/after.
    {
        const std::string bytesBefore = readFileBytes(worldPath);
        CHECK(!bytesBefore.empty());
        HttpTransport takeFake = [](const std::string&) {
            return cannedToolUse("take", "lantern");
        };
        HttpTransport goFake = [](const std::string&) {
            return cannedToolUse("go", "", "north");
        };
        (void)aiResolve(db, "take lantern", takeFake);
        (void)aiResolve(db, "go north", goFake);
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }
}

// --- dispatch fallback chain (REQ-RESOLVE-1, -4, -15): resolveOrParse runs
// aiResolve, then the permanent parser fallback, deterministically with a fake
// no-tool-call transport. Proves the full aiResolve -> parse -> nullopt chain
// without a live call. ---
static void testNlResolveDispatch() {
    const TempDbFile worldPath("textworld_nldispatch_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Fake transport that always makes no tool call: aiResolve declines,
    // so resolveOrParse must fall through to the parser.
    HttpTransport declines = [](const std::string&) { return cannedNoToolUse(); };

    // Resolver declines "take lantern" -> the PARSER yields Take (subject 4).
    {
        auto a = resolveOrParse(db, "take lantern", declines);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Take);
        CHECK(a->subject == 4);
    }

    // Both decline "smell the flowers" -> nullopt (the value that drives
    // renderError in runTurn).
    CHECK(!resolveOrParse(db, "smell the flowers", declines));

    // When the resolver DOES resolve, its Action wins and the parser is not
    // needed — "head north" is not a fixed-verb phrase, so only the resolver
    // could produce this Go.
    {
        HttpTransport resolves = [](const std::string&) {
            return cannedToolUse("go", "", "north");
        };
        auto a = resolveOrParse(db, "head north", resolves);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Go);
        CHECK(a->direction == "north");
    }
}

// --- tier-b passthrough (REQ-PROTO-6b via the boundary declaration): the
// resolver's clause-c recognition must never pre-empt the engine's
// applicability authority. Drives aiResolve + resolve directly (not runTurn). ---
static void testNlResolveTierBPassthrough() {
    const TempDbFile worldPath("textworld_nltierb_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // key (5) exists world-wide but sits in the garden (2), NOT the player's
    // room (stone hall, 1). Clause c is recognition only, so aiResolve returns a
    // valid Take with the id assigned mechanically — applicability is the
    // engine's job, not the gate's. One transport call.
    int calls = 0;
    HttpTransport fake = [&](const std::string&) {
        ++calls;
        return cannedToolUse("take", "key");
    };
    auto action = aiResolve(db, "take the key", fake);
    CHECK(action.has_value());
    CHECK(action->verb == Verb::Take);
    CHECK(action->subject == 5);
    CHECK(calls == 1);

    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
    const int64_t turnBefore =
        queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");

    // Resolve it in a tick: the world ticks and the engine emits a 'failed'
    // event — it neither retries nor re-calls the resolver.
    tick(db, *action);

    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") ==
          turnBefore + 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore + 1);
    CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") ==
          "failed");
    CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
          "You don't see that here.");
    CHECK(calls == 1);  // resolve() never calls the resolver

    // The key did not move — still in the garden.
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);
}

// Canned 200 response in the Anthropic Messages shape: stop_reason plus one
// text content block. Tests below perturb single fields off this baseline.
static HttpResponse cannedResponse(const std::string& text,
                                   const std::string& stopReason = "end_turn") {
    nlohmann::json j;
    j["stop_reason"] = stopReason;
    j["content"] =
        nlohmann::json::array({{{"type", "text"}, {"text", text}}});
    HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

// --- validation gate (REQ-PROSE-13 a-e): pure function of (response,
// anchors); hand-built TurnFacts anchors, no Db, no network. Each clause has
// a dedicated failing response; failures return nullopt, never throw. ---
static void testProseValidation() {
    // Plain anchors: no canon required, no failed events.
    TurnFacts plain;

    // Room-describing anchors: canon must appear verbatim.
    TurnFacts canonFacts;
    canonFacts.canonRequired = true;
    canonFacts.canonText = "An overgrown walled garden, hemmed in by ivy.";

    // Failed-event anchors: every detail must appear verbatim.
    TurnFacts failedFacts;
    failedFacts.failedDetails = {"You can't go that way."};

    // --- fully valid response → exactly the prose text ---
    {
        const auto out =
            validateAiResponse(cannedResponse("The hall is quiet."), plain);
        CHECK(out.has_value());
        CHECK(*out == "The hall is quiet.");
    }

    // --- clause a: non-200 status (e.g. 529 overloaded) ---
    {
        HttpResponse r = cannedResponse("The hall is quiet.");
        r.status = 529;
        CHECK(!validateAiResponse(r, plain).has_value());
    }

    // --- clause a: 200 but stop_reason "max_tokens" (truncated output) ---
    CHECK(!validateAiResponse(
               cannedResponse("The hall is quiet.", "max_tokens"), plain)
               .has_value());

    // --- clause b: malformed JSON body ---
    {
        HttpResponse r;
        r.status = 200;
        r.body = "{\"stop_reason\": \"end_turn\", \"content\": [";
        CHECK(!validateAiResponse(r, plain).has_value());
    }

    // --- clause b: empty text block ---
    CHECK(!validateAiResponse(cannedResponse(""), plain).has_value());

    // --- clause b: missing text block (empty content array) ---
    {
        HttpResponse r;
        r.status = 200;
        r.body = "{\"stop_reason\": \"end_turn\", \"content\": []}";
        CHECK(!validateAiResponse(r, plain).has_value());
    }

    // --- clause b: FIRST block is not a text block ---
    {
        HttpResponse r;
        r.status = 200;
        r.body =
            "{\"stop_reason\": \"end_turn\", \"content\": "
            "[{\"type\": \"tool_use\"}, "
            "{\"type\": \"text\", \"text\": \"Prose.\"}]}";
        CHECK(!validateAiResponse(r, plain).has_value());
    }

    // --- never-throw on type-confused JSON: status-200 bodies whose fields
    // hold the wrong TYPE (or whose root is not an object) return nullopt
    // without aborting — pins the type guards against a future refactor
    // reintroducing a throwing json access ---
    {
        HttpResponse r;
        r.status = 200;

        r.body = "{\"stop_reason\":5,\"content\":42}";  // numeric stop_reason
        CHECK(!validateAiResponse(r, plain).has_value());

        r.body = "{\"stop_reason\":\"end_turn\",\"content\":{}}";  // non-array content
        CHECK(!validateAiResponse(r, plain).has_value());

        // numeric text field
        r.body = "{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":123}]}";
        CHECK(!validateAiResponse(r, plain).has_value());

        r.body = "[1,2,3]";  // bare JSON array
        CHECK(!validateAiResponse(r, plain).has_value());

        r.body = "\"end_turn\"";  // bare JSON string
        CHECK(!validateAiResponse(r, plain).has_value());

        r.body = "null";  // bare JSON null
        CHECK(!validateAiResponse(r, plain).has_value());
    }

    // --- clause c: canon required but absent from the text ---
    CHECK(!validateAiResponse(cannedResponse("You wander into a garden."),
                              canonFacts)
               .has_value());

    // --- clause c: canon paraphrased, not verbatim (one changed word) ---
    CHECK(!validateAiResponse(
               cannedResponse(
                   "An overgrown walled garden, surrounded by ivy."),
               canonFacts)
               .has_value());

    // --- clause c: canon verbatim inside surrounding prose → passes ---
    {
        const auto out = validateAiResponse(
            cannedResponse("You step out. " + canonFacts.canonText +
                           " Birds scatter."),
            canonFacts);
        CHECK(out.has_value());
    }

    // --- clause d: failed detail missing ---
    CHECK(!validateAiResponse(cannedResponse("The wall stops you cold."),
                              failedFacts)
               .has_value());

    // --- clause d: failed detail verbatim → passes ---
    CHECK(validateAiResponse(
              cannedResponse("You can't go that way. The wall is solid."),
              failedFacts)
              .has_value());

    // --- clause e boundary: 1200 chars passes, 1201 fails ---
    CHECK(validateAiResponse(cannedResponse(std::string(1200, 'x')), plain)
              .has_value());
    CHECK(!validateAiResponse(cannedResponse(std::string(1201, 'x')), plain)
               .has_value());

    // --- transport error (empty body) → nullopt without throwing ---
    {
        HttpResponse r;
        r.transportError = true;
        CHECK(!validateAiResponse(r, plain).has_value());
    }
}

// --- enable switch (REQ-PROSE-2): aiNarrationEnabled() truth table. Pure
// env reads — no Db, no network. Both vars are guarded and restored. ---
static void testProseNarrationEnabled() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");

    // No key → off, regardless of TEXTWORLD_AI.
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("TEXTWORLD_AI");
    CHECK(!aiNarrationEnabled());
    setenv("TEXTWORLD_AI", "1", 1);
    CHECK(!aiNarrationEnabled());

    // Empty key counts as no key.
    setenv("ANTHROPIC_API_KEY", "", 1);
    unsetenv("TEXTWORLD_AI");
    CHECK(!aiNarrationEnabled());

    // Key present: on, unless TEXTWORLD_AI is EXACTLY "0".
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    CHECK(aiNarrationEnabled());
    setenv("TEXTWORLD_AI", "1", 1);
    CHECK(aiNarrationEnabled());
    setenv("TEXTWORLD_AI", "0", 1);
    CHECK(!aiNarrationEnabled());  // the kill switch
    setenv("TEXTWORLD_AI", "00", 1);
    CHECK(aiNarrationEnabled());  // not exactly "0"
    setenv("TEXTWORLD_AI", "off", 1);
    CHECK(aiNarrationEnabled());  // not exactly "0"
    setenv("TEXTWORLD_AI", "", 1);
    CHECK(aiNarrationEnabled());  // not exactly "0"
    // guards restore both vars here.
}

// --- aiRender end-to-end (REQ-PROSE-1, REQ-PROSE-5, REQ-PROSE-14) with fake
// transports, plus the runTurn dispatch (template fallback). No network. ---
static void testProseAiRender() {
    const TempDbFile worldPath("textworld_airender_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    const std::string gardenProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 2");

    // A fake transport whose canned text is chosen per test; counts calls.
    int calls = 0;
    std::string cannedText;
    const HttpTransport fake = [&](const std::string& body) {
        ++calls;
        // The transport receives the full REQUEST BODY (not the bare facts
        // payload): a Messages API JSON with model/system/messages.
        const nlohmann::json j = nlohmann::json::parse(body);
        CHECK(j.contains("model"));
        CHECK(j.contains("system"));
        CHECK(j.contains("messages"));
        return cannedResponse(cannedText);
    };

    // --- moved turn: AI prose + the template's OWN Exits/You-see tail ---
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // turn 1
    {
        cannedText = "You step through the archway. " + gardenProse +
                     " Cool air settles around you.";
        calls = 0;
        const auto out = aiRender(db, 1, fake);
        CHECK(calls == 1);
        CHECK(out.has_value());

        // The appended tail must be CHARACTER-IDENTICAL to the tail of the
        // template render for the same turn (everything from "Exits: " on).
        const std::string tmpl = render(db, 1);
        const size_t tailPos = tmpl.find("Exits: ");
        CHECK(tailPos != std::string::npos);
        const std::string tail = tmpl.substr(tailPos);
        CHECK(contains(tail, "Exits: south.\n"));
        CHECK(contains(tail, "You see: key.\n"));
        CHECK(*out == cannedText + "\n" + tail);
    }

    // --- inventory turn, empty-handed: the exact carrying line appended ---
    CHECK(runTurn(db, "inventory").outcome == TurnOutcome::Ticked);  // turn 2
    {
        cannedText = "You pat your pockets.";
        const auto out = aiRender(db, 2, fake);
        CHECK(out.has_value());
        CHECK(render(db, 2) == "You are carrying nothing.\n");
        CHECK(*out == cannedText + "\n" + render(db, 2));
    }

    // --- inventory turn, carrying the key: non-empty carrying line ---
    CHECK(runTurn(db, "take key").outcome == TurnOutcome::Ticked);   // turn 3
    CHECK(runTurn(db, "inventory").outcome == TurnOutcome::Ticked);  // turn 4
    {
        cannedText = "The key's weight is reassuring.";
        const auto out = aiRender(db, 4, fake);
        CHECK(out.has_value());
        CHECK(render(db, 4) == "You are carrying: key.\n");
        CHECK(*out == cannedText + "\n" + render(db, 4));
    }

    // --- refusal fake: canon missing on a room-describing turn → clause c
    // rejects → nullopt → the dispatch's fallback is exactly the template ---
    {
        cannedText = "You wander into some garden or other.";  // paraphrase
        const auto out = aiRender(db, 1, fake);
        CHECK(!out.has_value());  // clause c rejects → aiRender returns nullopt
        // End-to-end fallback byte-identity (dispatch shows the pure template
        // render on this nullopt) is covered by the AI-off runTurn test below.
    }

    // --- purity (REQ-PROSE-5): aiRender with a fake transport leaves the
    // world file BYTE-IDENTICAL (all turns above are committed) ---
    {
        const std::string bytesBefore = readFileBytes(worldPath);
        CHECK(!bytesBefore.empty());
        cannedText = "You step through the archway. " + gardenProse + " Quiet.";
        (void)aiRender(db, 1, fake);
        cannedText = "You pat your pockets.";
        (void)aiRender(db, 2, fake);
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }

    // --- timeout path (REQ-PROSE-3, -9; AI Validation item 7): a
    // transportError fake (an 8 s timeout / connect failure / curl error)
    // yields nullopt without crashing and with no retry, and the dispatch's
    // fallback shows the PURE template render — no AI-flavored content. This
    // pins fallback output == template output BYTE-FOR-BYTE on a transport
    // failure; testProseTransport already covers nullopt/one-call, this adds
    // the missing "fallback == pure template" assertion. Turn 1 is a moved
    // (room-describing) turn, so its template render carries the Exits tail. ---
    {
        int timeoutCalls = 0;
        const HttpTransport timingOut = [&timeoutCalls](const std::string&) {
            ++timeoutCalls;
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        const auto out = aiRender(db, 1, timingOut);
        CHECK(!out.has_value());   // clean nullopt, no crash
        CHECK(timeoutCalls == 1);  // no retry hides behind the timeout
        // End-to-end fallback byte-identity (dispatch shows the pure template
        // render, nothing AI-flavored) is covered by the AI-off runTurn test
        // below, which pins runTurn output == render() with no key — runTurn
        // hardwires curlTransport, so that is the honest dispatch coverage.
    }

    // --- dispatch, AI off (no key): runTurn output BYTE-IDENTICAL to the
    // template render — no transport exists to be touched ---
    {
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("TEXTWORLD_AI");

        const TurnResult r = runTurn(db, "look");  // turn 5
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 5);
        CHECK(r.output == render(db, 5));

        // Kill switch through the production path: key present but
        // TEXTWORLD_AI=0 → enabled() is false BEFORE any transport, so this
        // never reaches the network either.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        setenv("TEXTWORLD_AI", "0", 1);
        const TurnResult w = runTurn(db, "wait");  // turn 6
        CHECK(w.outcome == TurnOutcome::Ticked);
        CHECK(w.output == "Time passes.\n");
        CHECK(w.output == render(db, 6));
        // guards restore both vars here.
    }
}

// --- live end-to-end smoke (REQ-PROSE-17): the ONLY test that touches the
// network, and ONLY when TEXTWORLD_AI_LIVE_TEST=1. It returns immediately
// otherwise, so the default suite is a no-op here and (if CI is ever added)
// the API is never hit there.
//
// ENV-UNSET ORDERING: main() runs a suite-wide hermetic unset of
// ANTHROPIC_API_KEY (so every runTurn-based test stays offline). That unset
// would clobber the real key this test needs. The clean fix: main() invokes
// this function FIRST — before it constructs the ScopedEnvVar guards and
// unsets the vars — while the developer's real environment is still intact.
// This test reads TEXTWORLD_AI_LIVE_TEST and ANTHROPIC_API_KEY straight from
// that live env and mutates no env var itself.
//
// It drives one real turn through the PRODUCTION transport (the 2-arg
// aiRender, which binds the libcurl transport) against the seeded world and
// asserts ONLY mechanical invariants — output non-empty, the Exits line
// present — never anything about the prose content.
static void testProseLiveSmoke() {
    const char* live = std::getenv("TEXTWORLD_AI_LIVE_TEST");
    if (live == nullptr || std::string(live) != "1") {
        return;  // default run: no-op, no network access.
    }

    // Live run requested. Reaching the API needs a real key; without one
    // there is nothing to exercise, so skip LOUDLY (a run-setup gap, not a
    // code defect) rather than fail the suite.
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::fprintf(stderr,
                     "LIVE SMOKE SKIPPED: TEXTWORLD_AI_LIVE_TEST=1 but "
                     "ANTHROPIC_API_KEY is unset/empty.\n");
        return;
    }

    std::fprintf(stderr,
                 "LIVE SMOKE: driving one real turn through the Anthropic "
                 "API (this makes a network call)...\n");

    const TempDbFile worldPath("textworld_live_smoke.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // A moved turn (room-describing): canon rides verbatim inside the prose
    // and the deterministic tail carries the Exits line.
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);

    const auto out = aiRender(db, 1);  // 2-arg = production libcurl transport

    // Mechanical invariants only (REQ-PROSE-17): the turn produces non-empty
    // output carrying the deterministic Exits line, whether that came from the
    // AI or the template fallback. Mirror loop.cpp's dispatch rather than
    // asserting out.has_value() — a live response that paraphrases canon or
    // trips any validation clause returns nullopt, which is a correct fallback,
    // not a smoke-test failure. Asserting on validation outcome would make this
    // flaky against a non-deterministic model.
    const std::string shown = out ? *out : render(db, 1);
    CHECK(!shown.empty());                    // output non-empty
    CHECK(shown.find("Exits:") != std::string::npos);  // deterministic tail present
}

// --- live end-to-end resolver smoke (REQ-RESOLVE-16): structured exactly like
// testProseLiveSmoke — the ONLY resolver test that touches the network, and
// ONLY when TEXTWORLD_AI_LIVE_TEST=1. No-op otherwise, so the default suite is
// offline. It reads TEXTWORLD_AI_LIVE_TEST and ANTHROPIC_API_KEY from the live
// env (main() calls it BEFORE the hermetic unset) and mutates no env var.
//
// It drives real phrasings through the PRODUCTION transport (the 2-arg
// aiResolve, libcurl) and asserts MECHANICAL invariants only: if a phrasing
// resolves it must lower to the expected verb/subject; a nullopt is a correct
// clean fallback, never a failure. This is deliberately NOT a prompt-tuning
// loop — never assert on model wording or that a phrasing MUST resolve.
static void testNlResolveLiveSmoke() {
    const char* live = std::getenv("TEXTWORLD_AI_LIVE_TEST");
    if (live == nullptr || std::string(live) != "1") {
        return;  // default run: no-op, no network access.
    }

    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::fprintf(stderr,
                     "RESOLVER LIVE SMOKE SKIPPED: TEXTWORLD_AI_LIVE_TEST=1 but "
                     "ANTHROPIC_API_KEY is unset/empty.\n");
        return;
    }

    std::fprintf(stderr,
                 "RESOLVER LIVE SMOKE: resolving real phrasings through the "
                 "Anthropic API (this makes network calls)...\n");

    const TempDbFile worldPath("textworld_resolve_live_smoke.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // "take" synonyms the fixed-verb parser can't handle: if the model resolves
    // one, it must lower to Take lantern (id 4, mechanically assigned). A
    // nullopt is a correct clean fallback (model non-determinism), not a
    // failure. Production 2-arg transport; never assert on wording.
    for (const char* line : {"pick up the lantern", "grab lantern"}) {
        const std::optional<Action> a = aiResolve(db, line);
        if (a) {
            CHECK(a->verb == Verb::Take);
            CHECK(a->subject == 4);
        }
    }

    // "head north": Go with a NON-EMPTY direction (the gate guarantees non-empty
    // but never a specific word), or clean fallback.
    {
        const std::optional<Action> a = aiResolve(db, "head north");
        if (a) {
            CHECK(a->verb == Verb::Go);
            CHECK(!a->direction.empty());
        }
    }

    // Nonsense line: no single in-set action → no tool call → nullopt → the
    // parser also declines → runTurn's tier-a renderError, no tick. The key is
    // set, so aiNarrationEnabled() is true and runTurn goes through
    // resolveOrParse (the full production dispatch).
    {
        const TurnResult r = runTurn(db, "smell the flowers");
        CHECK(r.outcome == TurnOutcome::NoTick);
        CHECK(!r.output.empty());
    }
}

// libcurl write callback for the architect smoke's judge call.
static size_t liveJudgeAppend(char* ptr, size_t size, size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One bounded coherence-judge call (REQ-ARCH-13): feed the setting + the
// generated descriptions to a single plain (no-tool) Messages call and return
// the model's text, or "" on any failure. Test-local transport — mirrors the
// codebase's per-unit curlTransport stance. ONE call, no loop; a human reads it.
static std::string liveCoherenceJudge(const std::string& setting,
                                      const std::vector<std::string>& descriptions) {
    nlohmann::json body;
    const char* env = std::getenv("TEXTWORLD_MODEL");
    body["model"] = (env != nullptr && env[0] != '\0') ? env : "claude-opus-4-8";
    body["max_tokens"] = 256;
    body["system"] =
        "You judge whether a set of text-adventure room descriptions are "
        "coherent with a given setting and with each other. Reply with 'yes' or "
        "'no' on the first line, then one short line saying why.";
    std::string user = "SETTING:\n" + setting + "\n\nROOMS:\n";
    for (size_t i = 0; i < descriptions.size(); ++i) {
        user += std::to_string(i + 1) + ". " + descriptions[i] + "\n";
    }
    user +=
        "\nAre these rooms coherent with the setting and with each other? "
        "Answer yes/no + one line.";
    body["messages"] =
        nlohmann::json::array({{{"role", "user"}, {"content", user}}});
    const std::string payload = body.dump();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) return "";
    std::string resp;
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, ("x-api-key: " + std::string(key != nullptr ? key : "")).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, liveJudgeAppend);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return "";

    const nlohmann::json j =
        nlohmann::json::parse(resp, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("content") || !j["content"].is_array()) {
        return "";
    }
    for (const auto& block : j["content"]) {
        if (block.is_object() && block.value("type", "") == "text") {
            return block.value("text", "");
        }
    }
    return "";
}

// --- Step 10, REQ-ARCH-13: gated live end-to-end smoke + one bounded coherence
// judge. Structured like testNlResolveLiveSmoke: no-op unless
// TEXTWORLD_AI_LIVE_TEST=1, reads env without mutating it. main() calls it FIRST
// (before the hermetic ANTHROPIC_API_KEY unset), else the key is cleared and the
// smoke silently no-ops. MECHANICAL assertions only — never model wording — plus
// ONE judge call whose yes/no a human reads. NOT a prompt-tuning loop. ---
static void testArchitectLiveSmoke() {
    const char* live = std::getenv("TEXTWORLD_AI_LIVE_TEST");
    if (live == nullptr || std::string(live) != "1") {
        return;  // default run: no-op, no network access.
    }

    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::fprintf(stderr,
                     "ARCHITECT LIVE SMOKE SKIPPED: TEXTWORLD_AI_LIVE_TEST=1 but "
                     "ANTHROPIC_API_KEY is unset/empty.\n");
        return;
    }

    std::fprintf(stderr,
                 "ARCHITECT LIVE SMOKE: generating a room chain through the "
                 "Anthropic API (this makes network calls)...\n");

    // Default settingPath → the committed seed/setting.txt gives real shared
    // context. tick() drives the PRODUCTION transport (real generation + move).
    const TempDbFile worldPath("textworld_arch_live_smoke.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql");
    const std::string setting =
        queryText(db, "SELECT value FROM meta WHERE key = 'setting'");
    CHECK(!setting.empty());

    // A short chain of unmapped, invertible directions. Each new room advertises
    // only the way back, so any non-back direction is unmapped from it.
    std::vector<std::string> descriptions;
    int roomsWithOnward = 0;  // REQ-EXITS-11: rooms declaring >=1 latent onward exit
    for (const char* dir : {"east", "north", "east"}) {
        const int64_t before =
            queryInt(db, "SELECT container FROM location WHERE entity = 3");
        tick(db, Action{Verb::Go, 0, dir});
        const int64_t after =
            queryInt(db, "SELECT container FROM location WHERE entity = 3");

        if (after == before) {
            // A clean fallback (model non-determinism / transient) is not a
            // failure — the chain simply stops (mirrors the resolver smoke).
            std::fprintf(stderr,
                         "  (generation declined for '%s' — clean fallback, "
                         "chain stops)\n", dir);
            break;
        }

        // Mechanical invariants for the generated room — never its wording.
        CHECK(after > 5);  // engine-minted id, distinct from seed ids 1–5
        const std::string name = queryText(
            db, ("SELECT value FROM name WHERE entity = " + std::to_string(after)).c_str());
        const std::string desc = queryText(
            db, ("SELECT prose FROM description WHERE entity = " + std::to_string(after)).c_str());
        CHECK(!name.empty());
        CHECK(!desc.empty());

        // Two reciprocal exits: origin -dir-> new, new -inverse-> origin.
        CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                            std::to_string(before) + " AND direction = '" + dir + "'").c_str()) == after);
        const std::optional<std::string> inv = inverseDirection(dir);
        CHECK(inv.has_value());
        CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                            std::to_string(after) + " AND direction = '" + *inv + "'").c_str()) == before);

        descriptions.push_back(desc);

        // REQ-EXITS-11: the model's co-authored onward exits land as LATENT
        // (dest-NULL) rows on the new room. Immediately after generation — before
        // the next iteration walks on from it — the realized return is the ONLY
        // non-NULL exit row, and any additional rows are latent onward exits.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                            std::to_string(after) + " AND dest IS NOT NULL").c_str()) == 1);
        if (queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                          std::to_string(after) + " AND dest IS NULL").c_str()) > 0) {
            ++roomsWithOnward;
        }
    }

    CHECK(!descriptions.empty());  // at least one room generated

    // REQ-EXITS-11 anti-degeneration: across the generated chain, a NONZERO
    // fraction of rooms declared >=1 surviving onward exit. A world collapsing to
    // a straight corridor of dead ends — every room declaring zero onward exits,
    // e.g. a systematically dropped exit format that per-drop stderr lines would
    // bury — fails here.
    CHECK(roomsWithOnward > 0);

    // REQ-EXITS-11 displayed == walkable (failure-tolerant). On the room the
    // player ended in: the rendered Exits line must equal the row-backed set
    // (architect is enabled here → realized ∪ latent), and every invertible
    // direction NOT on the line must WALL when walked — no row → resolveGo case
    // (c) returns before any architect call, so the player does not move and no
    // exit row is created (no network). Walking a SHOWN direction is already
    // exercised by the chain above (shown latent → generates/moves); a shown
    // direction that walls while KEEPING its row is a conforming transient
    // failure (REQ-EXITS-3), so shown directions are deliberately not walked here.
    auto parseExits = [](const std::string& block) {
        std::vector<std::string> out;
        const size_t p = block.find("Exits: ");
        if (p == std::string::npos) return out;
        size_t e = block.find(".\n", p);
        if (e == std::string::npos) e = block.size();
        const std::string list = block.substr(p + 7, e - (p + 7));
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = list.find(", ", start);
            const std::string tok = (comma == std::string::npos)
                                        ? list.substr(start)
                                        : list.substr(start, comma - start);
            if (!tok.empty()) out.push_back(tok);
            if (comma == std::string::npos) break;
            start = comma + 2;
        }
        return out;
    };

    const int64_t here =
        queryInt(db, "SELECT container FROM location WHERE entity = 3");
    const std::vector<std::string> shown = parseExits(renderRoomOf(db, 3));
    const int64_t exitsBefore = queryInt(db, "SELECT COUNT(*) FROM exits");
    auto isShown = [&](const std::string& d) {
        return std::find(shown.begin(), shown.end(), d) != shown.end();
    };
    for (const char* dir : {"north", "south", "east", "west",
                            "up", "down", "in", "out"}) {
        const bool hasRow =
            queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                          std::to_string(here) + " AND direction = '" + dir + "'")
                             .c_str()) > 0;
        CHECK(isShown(dir) == hasRow);  // architect enabled → shown iff a row exists
        if (!isShown(dir)) {
            // Unshown ⇒ hard wall: walking it must not move the player.
            tick(db, Action{Verb::Go, 0, dir});
            CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == here);
        }
    }
    // ...and no wall walk generated a room or planted a row (no network).
    CHECK(queryInt(db, "SELECT COUNT(*) FROM exits") == exitsBefore);

    // The single bounded coherence judge (REQ-ARCH-13): one call, observed.
    const std::string verdict = liveCoherenceJudge(setting, descriptions);
    std::fprintf(stderr, "ARCHITECT COHERENCE JUDGE (%zu rooms):\n%s\n",
                 descriptions.size(), verdict.c_str());
    CHECK(!verdict.empty());  // structural only — a human reads the yes/no + line
}

// --- persistence after play (REQ-PROTO-12 item 8, REQ-PROTO-10): a played
// world survives a full close/reopen with turn counter, entity positions, and
// the complete event transcript intact — and is still playable afterward. ---
static void testPersistence() {
    const TempDbFile worldPath("textworld_persist_tests.db");

    // Play three turns through the production path, then close (Db destructor
    // at scope exit releases the connection; DELETE journal mode means the
    // file on disk is the full committed state).
    {
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "drop lantern").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 3);
    }

    // Reopen the same file: everything preserved.
    {
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        // Turn counter survived the close.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 3);

        // Positions survived: lantern dropped in the garden (room 2), player
        // still standing there, key untouched in room 2 all along.
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 5") == 2);

        // Full event history intact: one event per turn, and the DISTINCT
        // turns cover exactly 1..3 (REQ-PROTO-8 transcript completeness).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 3);
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT turn) FROM events") == 3);
        CHECK(queryInt(db, "SELECT MIN(turn) FROM events") == 1);
        CHECK(queryInt(db, "SELECT MAX(turn) FROM events") == 3);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE turn = 1 AND verb = 'took'") == 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE turn = 2 AND verb = 'moved'") == 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE turn = 3 AND verb = 'dropped'") == 1);

        // Still playable: one more turn ticks to N+1 and logs turn 4.
        const TurnResult r = runTurn(db, "take lantern");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 4);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT turn) FROM events") == 4);
    }
}

// --- file-copy portability (REQ-PROTO-12 item 9, REQ-PROTO-11): cp of the
// world file is a complete world. Playing the copy diverges it without
// touching a byte of the original, and both remain independently playable. ---
static void testPortability() {
    namespace fs = std::filesystem;
    const TempDbFile origPath("textworld_port_orig.db");
    const TempDbFile copyPath("textworld_port_copy.db");

    // Play the original: lantern in hand, player in the garden. Close it.
    {
        Db db = openWorld(origPath.string(), "tests/fixture.sql");
        CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 2);
    }

    // Plain file copy, then snapshot the original's bytes.
    fs::copy_file(origPath, copyPath);
    const std::string origBytes = readFileBytes(origPath);
    CHECK(!origBytes.empty());
    CHECK(readFileBytes(copyPath) == origBytes);  // faithful copy

    // Play the COPY down a divergent path: drop the lantern in the garden,
    // walk back south. Close it.
    {
        Db db = openWorld(copyPath.string(), "tests/fixture.sql");
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 2);
        CHECK(runTurn(db, "drop lantern").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "go south").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 4);
    }

    // The original file was not touched by any of that: byte-identical.
    CHECK(readFileBytes(origPath) == origBytes);

    // Reopen the original: pre-copy state, still playable, and its state
    // provably differs from the copy's (divergence).
    {
        Db orig = openWorld(origPath.string(), "tests/fixture.sql");
        CHECK(queryInt(orig, "SELECT value FROM meta WHERE key = 'turn'") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 3") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 4") == 3);

        Db copy = openWorld(copyPath.string(), "tests/fixture.sql");
        CHECK(queryInt(copy, "SELECT value FROM meta WHERE key = 'turn'") == 4);
        CHECK(queryInt(copy, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryInt(copy, "SELECT container FROM location WHERE entity = 4") == 2);

        // Divergence stated explicitly: same lineage, different worlds now.
        CHECK(queryInt(orig, "SELECT value FROM meta WHERE key = 'turn'") !=
              queryInt(copy, "SELECT value FROM meta WHERE key = 'turn'"));

        // Original still plays: one more turn ticks it to 3.
        const TurnResult r = runTurn(orig, "wait");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(orig, "SELECT value FROM meta WHERE key = 'turn'") == 3);
    }
}

// ============================================================================
// Architect (story-seed + world-gen) tests. Source: .lore/work/specs/
// story-seed-architect.md (REQ-ARCH-*). Mirror the resolver's fake-transport
// discipline: the default suite makes NO network access; every transport is a
// fake lambda. The one live smoke (testArchitectLiveSmoke) is gated behind
// TEXTWORLD_AI_LIVE_TEST=1 and registered at the TOP of main() with the others.
// ============================================================================

// Writes `contents` to a scratch file for one test, removing it on destruction.
// Used for the setting-load cases (REQ-ARCH-1) so the committed seed/setting.txt
// is never renamed.
struct TempSettingFile {
    std::filesystem::path path;

    TempSettingFile(const char* filename, const std::string& contents)
        : path(std::filesystem::temp_directory_path() / filename) {
        std::filesystem::remove(path);
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }
    ~TempSettingFile() { std::filesystem::remove(path); }

    std::string string() const { return path.string(); }
};

// --- Step 1, REQ-ARCH-1: setting seed load. A settingPath at a real scratch
// file lands its text in meta.setting with SCHEMA_VERSION unchanged; an absent
// scratch path still initializes with meta.setting empty. Zero DDL. ---
static void testArchitectSettingLoad() {
    // --- present setting file: text lands verbatim in meta.setting ---
    {
        const std::string settingText =
            "A quiet cloister of grey stone and green light.";
        const TempSettingFile setting("textworld_setting_present.txt", settingText);
        const TempDbFile worldPath("textworld_setting_present.db");

        Db db = openWorld(worldPath.string(), "tests/fixture.sql", setting.string());

        CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'setting'") ==
              settingText);
        // Zero DDL: SCHEMA_VERSION row is unchanged.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") ==
              SCHEMA_VERSION);
        // The base world is intact — this is a new row, not a new shape.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == 2);
    }

    // --- absent setting file: init still succeeds, meta.setting empty ---
    {
        const TempDbFile worldPath("textworld_setting_absent.db");
        const std::string missing =
            (std::filesystem::temp_directory_path() / "textworld_no_such_setting.txt")
                .string();
        std::filesystem::remove(missing);  // ensure it does not exist

        Db db = openWorld(worldPath.string(), "tests/fixture.sql", missing);

        // Tolerant read: init succeeded, and meta.setting is present-but-empty.
        CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'setting'").empty());
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") ==
              SCHEMA_VERSION);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == 2);
    }

    // --- the committed seed/setting.txt exists and is non-trivial (AI-Val #3) ---
    CHECK(!readFileBytes("seed/setting.txt").empty());
}

// Recursively assert no key or string value anywhere in `j` looks like an id
// (ARCH context/proposal carry NO ids, REQ-ARCH-6). Reuses the ban-set idea
// from checkPayloadHygiene but works structurally on a parsed object.
static void checkNoIdKeys(const nlohmann::json& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string key = it.key();
            CHECK(key != "id" && key != "entity" && key != "room" &&
                  key != "dest" && key != "container");
            checkNoIdKeys(it.value());
        }
    } else if (j.is_array()) {
        for (const auto& e : j) checkNoIdKeys(e);
    }
}

// --- Step 2, REQ-ARCH-7a / REQ-ARCH-6: the context builder emits EXACTLY the
// four fields (setting, origin name, origin canon description, direction) and
// no ids anywhere. Pure SELECTs; no network. ---
static void testArchitectContext() {
    const std::string settingText =
        "A quiet cloister of grey stone and green light.";
    const TempSettingFile setting("textworld_ctx_setting.txt", settingText);
    const TempDbFile worldPath("textworld_arch_context.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql", setting.string());

    // Origin = the stone hall (room 1); walk an unmapped direction: 'east'.
    const nlohmann::json j =
        nlohmann::json::parse(buildArchitectContext(db, 1, "east"));

    // Exactly the four REQ-ARCH-7a fields — assert the whole key set.
    CHECK(j.is_object());
    CHECK(j.size() == 4);
    CHECK(j.contains("setting"));
    CHECK(j.contains("origin_name"));
    CHECK(j.contains("origin_description"));
    CHECK(j.contains("direction"));

    // Field values come from canon: setting text, the hall's name + canon prose.
    CHECK(j["setting"] == settingText);
    CHECK(j["origin_name"] == "stone hall");
    CHECK(contains(j["origin_description"].get<std::string>(), "vaulted hall"));
    CHECK(j["direction"] == "east");

    // No ids anywhere (REQ-ARCH-6).
    checkNoIdKeys(j);

    // --- empty meta.setting → thinner but well-formed payload ---
    {
        const TempDbFile w2("textworld_arch_context_empty.db");
        const std::string missing =
            (std::filesystem::temp_directory_path() / "textworld_ctx_no_setting.txt")
                .string();
        std::filesystem::remove(missing);
        Db db2 = openWorld(w2.string(), "tests/fixture.sql", missing);

        const nlohmann::json j2 =
            nlohmann::json::parse(buildArchitectContext(db2, 1, "up"));
        CHECK(j2.size() == 4);
        CHECK(j2["setting"] == "");
        CHECK(j2["origin_name"] == "stone hall");
        CHECK(j2["direction"] == "up");
    }
}

// --- Step 4, REQ-ARCH-7b: the request body carries the create_room tool with
// required name+description and no other input fields; tool_choice REQUIRES the
// tool; max_tokens 1024; model default + TEXTWORLD_MODEL override; and the EXACT
// top-level key set (no thinking/stream/cache key can slip in). Pure
// string→string plus an env read. ---
static void testArchitectRequestBody() {
    const ScopedModelEnv guard;

    const std::string payload =
        "{\"setting\":\"grey stone\",\"direction\":\"east\"}";

    // --- defaults: model, max_tokens, tool schema, tool_choice, key set ---
    unsetenv("TEXTWORLD_MODEL");
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildArchitectRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
        CHECK(j["max_tokens"] == 1024);

        // Exact top-level set: model, max_tokens, system, messages, tools,
        // tool_choice — and nothing else (no thinking/stream/cache-control).
        CHECK(j.size() == 6);
        CHECK(!j.contains("thinking"));
        CHECK(!j.contains("stream"));

        // system is the architect prompt; one user message carries the payload.
        CHECK(j["system"] == std::string(kArchitectPrompt));
        CHECK(j["messages"].is_array());
        CHECK(j["messages"].size() == 1);
        CHECK(j["messages"][0]["role"] == "user");
        CHECK(j["messages"][0]["content"] == payload);

        // tool_choice REQUIRES the create_room tool (no auto/decline branch).
        CHECK(j["tool_choice"]["type"] == "tool");
        CHECK(j["tool_choice"]["name"] == "create_room");

        // exactly one create_room tool.
        CHECK(j["tools"].is_array());
        CHECK(j["tools"].size() == 1);
        const nlohmann::json& tool = j["tools"][0];
        CHECK(tool["name"] == "create_room");

        // input schema: object; REQUIRED name + description + OPTIONAL exits.
        const nlohmann::json& schema = tool["input_schema"];
        CHECK(schema["type"] == "object");
        CHECK(schema["properties"]["name"]["type"] == "string");
        CHECK(schema["properties"]["description"]["type"] == "string");
        // exits: optional string-array (REQ-EXITS-5).
        CHECK(schema["properties"]["exits"]["type"] == "array");
        CHECK(schema["properties"]["exits"]["items"]["type"] == "string");
        CHECK(schema["properties"].size() == 3);  // name, description, exits
        // exits is NOT required — name + description only.
        CHECK(schema["required"] ==
              nlohmann::json::array({"name", "description"}));
    }

    // --- TEXTWORLD_MODEL set and non-empty → override honored, only the model ---
    setenv("TEXTWORLD_MODEL", "claude-test-model", 1);
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildArchitectRequestBody(payload));
        CHECK(j["model"] == "claude-test-model");
        CHECK(j["max_tokens"] == 1024);
    }

    // --- TEXTWORLD_MODEL set but empty → default, not "" ---
    setenv("TEXTWORLD_MODEL", "", 1);
    {
        const nlohmann::json j =
            nlohmann::json::parse(buildArchitectRequestBody(payload));
        CHECK(j["model"] == "claude-opus-4-8");
    }
}

// Canned 200 tool_use response in the documented Anthropic shape (Step-5
// fixture): stop_reason "tool_use" + one create_room tool_use block carrying
// name + description. The architect analog of cannedToolUse — built from
// documented structure, NEVER a network probe.
static HttpResponse cannedCreateRoom(
    const std::string& name, const std::string& description,
    const std::vector<std::string>& exits = {}) {
    nlohmann::json input;
    input["name"] = name;
    input["description"] = description;
    // Only add the optional exits array when non-empty — the analog of how
    // cannedToolUse carries optional fields (REQ-EXITS-7 fixture).
    if (!exits.empty()) {
        input["exits"] = exits;
    }

    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = "toolu_test";
    block["name"] = "create_room";
    block["input"] = std::move(input);

    nlohmann::json j;
    j["stop_reason"] = "tool_use";
    j["content"] = nlohmann::json::array({std::move(block)});

    HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

// --- Step 5, REQ-ARCH-9a–c: the validation gate. Pure function of the
// HttpResponse; called directly here (outside any try/catch), so a throw would
// crash — it must never throw. No network. ---
static void testArchitectGate() {
    // Happy path: exactly one create_room block with name + description.
    {
        auto p = validateRoomProposal(
            cannedCreateRoom("chapter house", "A low vaulted room of grey stone."));
        CHECK(p.has_value());
        CHECK(p->name == "chapter house");
        CHECK(p->description == "A low vaulted room of grey stone.");
    }

    // Clause b: empty / whitespace-only name → nullopt.
    CHECK(!validateRoomProposal(cannedCreateRoom("", "prose")));
    CHECK(!validateRoomProposal(cannedCreateRoom("   \t\n", "prose")));

    // Clause c: empty / whitespace-only description → nullopt.
    CHECK(!validateRoomProposal(cannedCreateRoom("name", "")));
    CHECK(!validateRoomProposal(cannedCreateRoom("name", "   ")));

    // Clause a HTTP half: non-200 / transport error / malformed body → nullopt.
    {
        HttpResponse r = cannedCreateRoom("name", "prose");
        r.status = 500;  // valid body, bad status
        CHECK(!validateRoomProposal(r));
    }
    {
        HttpResponse r;
        r.transportError = true;  // status 0
        CHECK(!validateRoomProposal(r));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "}{ not json";
        CHECK(!validateRoomProposal(r));
    }

    // Clause a, 0-block: a valid 200 with NO create_room call → nullopt (the
    // tool was required; a non-call is a gate failure → wall).
    {
        nlohmann::json j;
        j["stop_reason"] = "end_turn";
        j["content"] = nlohmann::json::array(
            {{{"type", "text"}, {"text", "I decline."}}});
        HttpResponse r;
        r.status = 200;
        r.body = j.dump();
        CHECK(!validateRoomProposal(r));
    }

    // Clause a, ≥2-block: two create_room blocks → nullopt.
    {
        nlohmann::json blk;
        blk["type"] = "tool_use";
        blk["id"] = "toolu_a";
        blk["name"] = "create_room";
        blk["input"] = {{"name", "a"}, {"description", "b"}};
        nlohmann::json j;
        j["stop_reason"] = "tool_use";
        j["content"] = nlohmann::json::array({blk, blk});
        HttpResponse r;
        r.status = 200;
        r.body = j.dump();
        CHECK(!validateRoomProposal(r));
    }

    // ids-not-from-model (REQ-ARCH-6): a create_room input carrying a spurious
    // id/entity key still validates to a RoomProposal of JUST name+description —
    // the stray key is ignored at the gate and never reaches the two-field
    // struct. The model can put no id on the wire.
    {
        nlohmann::json input;
        input["name"] = "crypt";
        input["description"] = "A cold undercroft.";
        input["id"] = 999;         // spurious
        input["entity"] = 42;      // spurious
        nlohmann::json block;
        block["type"] = "tool_use";
        block["id"] = "toolu_test";
        block["name"] = "create_room";
        block["input"] = std::move(input);
        nlohmann::json j;
        j["stop_reason"] = "tool_use";
        j["content"] = nlohmann::json::array({std::move(block)});
        HttpResponse r;
        r.status = 200;
        r.body = j.dump();

        auto p = validateRoomProposal(r);
        CHECK(p.has_value());
        CHECK(p->name == "crypt");
        CHECK(p->description == "A cold undercroft.");
        CHECK(p->exits.empty());  // no exits array → dead end
    }

    // --- REQ-EXITS-7: exit sanitization is LENIENT (never rejects the room). ---

    // Normalization: "North"/" up " are trimmed+lowercased and KEPT (not
    // spuriously dropped). NOTE: the plan's example says "travelling west", but
    // inverse("west")=="east" would drop the "East" entry as the return
    // direction, contradicting the stated {north,east,up} result. Travelling
    // "in" (inverse "out", absent from the set) preserves the normalization
    // intent — all three survive. See report for this divergence.
    {
        auto p = validateRoomProposal(
            cannedCreateRoom("hall", "prose", {"north", "East", " up "}), "in");
        CHECK(p.has_value());
        CHECK(p->exits == std::vector<std::string>({"north", "east", "up"}));
    }

    // Junk mix: a non-string, a non-invertible direction, a duplicate, and the
    // return direction (inverse(travel)) are EACH dropped — and the room is
    // STILL created. Travelling "north" ⇒ return direction "south".
    {
        nlohmann::json input;
        input["name"] = "hall";
        input["description"] = "prose";
        input["exits"] = nlohmann::json::array(
            {"east", 42, "northeast", "up", "east", "south"});
        nlohmann::json block;
        block["type"] = "tool_use";
        block["id"] = "toolu_test";
        block["name"] = "create_room";
        block["input"] = std::move(input);
        nlohmann::json j;
        j["stop_reason"] = "tool_use";
        j["content"] = nlohmann::json::array({std::move(block)});
        HttpResponse r;
        r.status = 200;
        r.body = j.dump();

        auto p = validateRoomProposal(r, "north");
        CHECK(p.has_value());  // junk exits never fail the room
        // Survivors: east + up. Dropped: 42 (non-string), "northeast"
        // (non-invertible), the second "east" (duplicate), "south" (return).
        CHECK(p->exits == std::vector<std::string>({"east", "up"}));
    }

    // Blank name / description remain FATAL even when exits are present.
    CHECK(!validateRoomProposal(cannedCreateRoom("", "prose", {"north"}), "east"));
    CHECK(!validateRoomProposal(cannedCreateRoom("name", "  ", {"north"}), "east"));
}

// --- Step 6, REQ-ARCH-9 (write) / REQ-ARCH-6: writeGeneratedRoom mints a room
// with engine-chosen id, both reciprocal exits, and one 'generated' event, all
// inside the caller's transaction. Uses an UNMAPPED invertible direction (east
// from the hall — base.sql maps only north/south) so no exit PK collides. ---
static void testWriteGeneratedRoom() {
    const TempDbFile worldPath("textworld_write_gen_room.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    const int64_t originRoom = 1;  // stone hall
    const int64_t player = 3;

    // --- primary path (REQ-EXITS-8): the origin exit pre-exists as a LATENT
    // stub; realizing it is an UPDATE (upsert, not a second row). ---
    // fixture maps 1 -north-> 2 as realized; downgrade it to latent so the
    // realize path exercises the ON CONFLICT UPDATE, not a fresh insert.
    db.exec("UPDATE exits SET dest = NULL WHERE room = 1 AND direction = 'north'");

    const RoomProposal proposal{
        "chapter house", "A low vaulted room of grey stone, its shelves bare.",
        {"east", "down"}};

    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

    db.begin();
    const int64_t newRoom =
        writeGeneratedRoom(db, originRoom, "north", proposal, player);
    db.commit();

    // Engine-minted id, distinct from the seed ids 1–5.
    CHECK(newRoom > 5);

    // Component rows carry the canned values; the room is tagged; NO location row.
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM room WHERE entity = " +
                        std::to_string(newRoom)).c_str()) == 1);
    CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                         std::to_string(newRoom)).c_str()) == "chapter house");
    CHECK(queryText(db, ("SELECT prose FROM description WHERE entity = " +
                         std::to_string(newRoom)).c_str()) ==
          "A low vaulted room of grey stone, its shelves bare.");
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM location WHERE entity = " +
                        std::to_string(newRoom)).c_str()) == 0);

    // Origin north is REALIZED to the new room via upsert — still exactly ONE
    // row (not a second), now with a non-NULL dest.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'north'") == 1);
    CHECK(queryInt(db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'north'") ==
          newRoom);
    // Realized return: new -south-> origin (inverse of north).
    CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                        std::to_string(newRoom) + " AND direction = 'south'").c_str()) ==
          originRoom);

    // The declared onward exits are planted as LATENT stubs (NULL dest).
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                        std::to_string(newRoom) +
                        " AND direction = 'east' AND dest IS NULL").c_str()) == 1);
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                        std::to_string(newRoom) +
                        " AND direction = 'down' AND dest IS NULL").c_str()) == 1);

    // Exactly one new 'generated' event (latent stubs emit none), subject = new
    // room, object = origin, detail = direction. Also proves no stub events.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore + 1);
    CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") ==
          "generated");
    CHECK(queryInt(db, "SELECT subject FROM events ORDER BY id DESC LIMIT 1") ==
          newRoom);
    CHECK(queryInt(db, "SELECT object FROM events ORDER BY id DESC LIMIT 1") ==
          originRoom);
    CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
          "north");

    // --- sub-case (micro-decision #3): an ABSENT origin latent row still
    // realizes (upsert degrades to insert). It logs one stderr diagnostic
    // (a REQ-EXITS-2b precondition violation) but does NOT throw — this is the
    // log-and-proceed contract that keeps testArchitectGenerate green. Room 1
    // has no 'east' row, so this exercises the no-conflict path. ---
    CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east'") == 0);
    const RoomProposal proposal2{"cellar", "A damp brick cellar.", {"up"}};
    db.begin();
    const int64_t newRoom2 =
        writeGeneratedRoom(db, originRoom, "east", proposal2, player);
    db.commit();
    CHECK(newRoom2 > 5 && newRoom2 != newRoom);
    // Origin east realized to the new room (via insert), return west realized.
    CHECK(queryInt(db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'") ==
          newRoom2);
    CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                        std::to_string(newRoom2) + " AND direction = 'west'").c_str()) ==
          originRoom);
}

// --- Step 7, REQ-ARCH-4/-5/-6/-11: architectGenerate orchestration with the
// two-phase catch boundary. Every transport is a fake lambda — NO network. A
// canned create_room creates the room end-to-end; every Phase-1 failure returns
// false and, because no write was attempted, leaves NO orphan row. ---
static void testArchitectGenerate() {
    const int64_t player = 3;

    // --- success: canned create_room → true; room + reciprocal exits + event ---
    {
        const TempDbFile worldPath("textworld_arch_gen_ok.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };

        db.begin();
        const bool ok = architectGenerate(db, 1, "east", player, fake);
        db.commit();

        CHECK(ok);
        CHECK(calls == 1);  // one transport call, no retries

        const int64_t newRoom =
            queryInt(db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(newRoom > 5);
        CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                            std::to_string(newRoom) + " AND direction = 'west'").c_str()) == 1);
        CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                             std::to_string(newRoom)).c_str()) == "crypt");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'generated'") == 1);
    }

    // --- Phase-1 atomic fallback: each failure → false, NO orphan written ---
    {
        const TempDbFile worldPath("textworld_arch_gen_fail.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        const int64_t entities0 = queryInt(db, "SELECT COUNT(*) FROM entities");
        const int64_t exits0 = queryInt(db, "SELECT COUNT(*) FROM exits");
        const int64_t desc0 = queryInt(db, "SELECT COUNT(*) FROM description");
        const int64_t events0 = queryInt(db, "SELECT COUNT(*) FROM events");

        // transport error (count the calls: exactly one, no retries).
        int calls = 0;
        HttpTransport err = [&](const std::string&) {
            ++calls;
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        CHECK(!architectGenerate(db, 1, "east", player, err));
        CHECK(calls == 1);

        // malformed body.
        HttpTransport bad = [](const std::string&) {
            HttpResponse r;
            r.status = 200;
            r.body = "}{ not json";
            return r;
        };
        CHECK(!architectGenerate(db, 1, "east", player, bad));

        // throwing transport → caught in Phase 1, no crash.
        HttpTransport thr = [](const std::string&) -> HttpResponse {
            throw std::runtime_error("socket exploded");
        };
        CHECK(!architectGenerate(db, 1, "east", player, thr));

        // valid 200 with no create_room call (gate 0-block).
        HttpTransport none = [](const std::string&) { return cannedNoToolUse(); };
        CHECK(!architectGenerate(db, 1, "east", player, none));

        // No write happened in Phase 1: every table is exactly as it was.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entities0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits") == exits0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM description") == desc0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == events0);
        // Specifically: no 'east' exit was ever created off the hall.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east'") == 0);
    }
}

// Tick helper that injects the architect's transport into resolve (Go world-gen
// seam), mirroring the production tick but with NO network.
static void tickT(Db& db, const Action& a, const HttpTransport& transport,
                  int64_t player = 3) {
    db.begin();
    db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
    resolve(db, a, player, transport);
    db.commit();
}

// --- Step 4, REQ-EXITS-2/-3: the resolveGo three-case movement table, driven
// through the tick with a fake transport. (a) realized → move, no AI call;
// (b) latent + AI-on → generate + realize + move (and persistence: re-crossing
// takes case a with no new call); (c) undeclared direction (no row) → wall,
// zero transport calls; (d) latent + AI-off → wall; (e) failed generation twice
// at the same latent exit → walls both times, latent row still present. Each
// case uses its own fresh world so movement state never couples the cases. ---
static void testResolveGoGenerate() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const int64_t player = 3;

    // (a) Realized exit (fixture maps 1 -north-> 2 realized) → move, and the
    // transport is NEVER invoked (case a precedes any AI call).
    {
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        const TempDbFile worldPath("textworld_resolvego_realized.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Go, 0, "north"}, fake, player);
        CHECK(calls == 0);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
    }

    // (b) Latent exit + AI-on → generate, realize, move; the origin row is now
    // non-NULL; and re-crossing takes case (a) with ZERO further calls.
    {
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        const TempDbFile worldPath("textworld_resolvego_latent.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        // Seed a latent onward exit off the hall (fixture maps no 'east').
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };

        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        CHECK(calls == 1);
        const int64_t newRoom =
            queryInt(db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(newRoom > 5);  // origin row now non-NULL (realized)
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == newRoom);
        CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                            std::to_string(newRoom) + " AND direction = 'west'").c_str()) == 1);

        // persistence / no-regen: back west (case a — realized return), then
        // east again (case a — now realized). The transport is not re-invoked.
        tickT(db, Action{Verb::Go, 0, "west"}, fake, player);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == newRoom);
        CHECK(calls == 1);
    }

    // (c) Undeclared direction (NO row) → hard wall, ZERO transport calls, even
    // with AI enabled ('up' has no row off the hall in the fixture).
    {
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        const TempDbFile worldPath("textworld_resolvego_wall.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Go, 0, "up"}, fake, player);
        CHECK(calls == 0);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") == "failed");
        CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
              "You can't go that way.");
        // No row was created for the undeclared direction.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'up'") == 0);
    }

    // (d) Latent exit + AI-off → wall, transport NEVER invoked; the latent row
    // survives untouched.
    {
        unsetenv("ANTHROPIC_API_KEY");  // aiNarrationEnabled() false
        unsetenv("TEXTWORLD_AI");
        const TempDbFile worldPath("textworld_resolvego_off.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        CHECK(calls == 0);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") == "failed");
        CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
              "You can't go that way.");
        // Latent row still present and still NULL (hidden, not consumed).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east' AND dest IS NULL") == 1);
    }

    // (e) Failed generation (Phase-1 forced to fail) TWICE at the same latent
    // exit → walls both times, and the latent row is STILL present after each
    // attempt (provably retryable, REQ-EXITS-3).
    {
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        const TempDbFile worldPath("textworld_resolvego_retry.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        int calls = 0;
        HttpTransport err = [&](const std::string&) {
            ++calls;
            HttpResponse r;
            r.transportError = true;  // Phase-1 failure, no write
            return r;
        };

        tickT(db, Action{Verb::Go, 0, "east"}, err, player);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") == "failed");
        // Latent row untouched after the first failed attempt.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east' AND dest IS NULL") == 1);

        tickT(db, Action{Verb::Go, 0, "east"}, err, player);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") == "failed");
        // Still present and still NULL after the second — retryable indefinitely.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east' AND dest IS NULL") == 1);
        CHECK(calls == 2);  // one transport call per attempt, no retries
    }
}

// Fleeing + room-bound enemies (REQ-COMBAT-26, -27). A latent (ungenerated)
// exit is refused while a hostile is present — the architect is NEVER called
// mid-combat; a realized exit lets the player flee, the enemy takes its single
// parting turn, and the room-bound enemy stays put, unchanged on return.
static void testCombatFlee() {
    // --- latent flee refused: no generation, no move (AI ON + fake transport) ---
    {
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");  // AI enabled: the guard, not AI-off, must refuse

        const TempDbFile p("textworld_combat_flee_latent.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("cavern", "A dark cavern.");
        };
        // Into the corridor (realized), then attempt to flee via the latent north.
        tickT(db, Action{Verb::Go, 0, "north"}, fake, 3);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
        const int64_t roomsBefore = queryInt(db, "SELECT COUNT(*) FROM room");

        tickT(db, Action{Verb::Go, 0, "north"}, fake, 3);  // latent + hostile present
        CHECK(calls == 0);  // the architect is NEVER called mid-combat
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);  // no move
        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == roomsBefore);             // no new room
        CHECK(queryText(db, "SELECT verb FROM events ORDER BY id DESC LIMIT 1") == "failed");
        CHECK(contains(
            queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1"), "flee"));
    }

    // --- realized flee succeeds: enemy takes one parting turn; room-bound ---
    {
        // AI restored to hermetic-off by the guards above → runTurn uses templates.
        const TempDbFile p("textworld_combat_flee_realized.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // into corridor
        const int64_t fleeTurn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") + 1;

        CHECK(runTurn(db, "go south").outcome == TurnOutcome::Ticked);  // flee via realized
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);  // fled
        // The enemy took exactly one parting turn: a chip event on the flee tick.
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(fleeTurn) + " AND verb = 'chip'").c_str()) == 1);
        // Room-bound (REQ-COMBAT-27): the enemy stays in the corridor, unchanged.
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 7") == 2);
        CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == 8);

        // Return: combat re-engages; the enemy's state is unchanged.
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 2);
        CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 7") == 8);
    }
}

// --- Step 9, REQ-ARCH-10: the 'generated' verb is renderer-invisible. A turn
// carrying a 'generated' event (alongside 'moved') shows as the moved room
// block, and 'generated' is absent from BOTH buildFacts payload keys. ---
static void testGeneratedEventInvisible() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");

    const TempDbFile worldPath("textworld_gen_invisible.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // Generation now fires only on a pre-existing latent row; seed the latent
    // 'east' exit off the hall that this turn will walk (REQ-EXITS-2b).
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

    HttpTransport fake = [](const std::string&) {
        return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
    };

    // Generate a room: this turn carries BOTH a 'generated' and a 'moved' event,
    // sharing the same turn number.
    tickT(db, Action{Verb::Go, 0, "east"}, fake, 3);
    const int64_t genTurn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(genTurn) + " AND verb = 'generated'").c_str()) == 1);
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(genTurn) + " AND verb = 'moved'").c_str()) == 1);

    // current-turn payload key `events`: 'generated' absent, 'moved' present.
    {
        const TurnFacts facts = buildFacts(db, genTurn);
        const nlohmann::json j = nlohmann::json::parse(facts.payload);
        bool sawGenerated = false, sawMoved = false;
        for (const auto& e : j["events"]) {
            if (e.value("verb", "") == "generated") sawGenerated = true;
            if (e.value("verb", "") == "moved") sawMoved = true;
        }
        CHECK(!sawGenerated);
        CHECK(sawMoved);
    }

    // recent-events payload key `recent_events` (turn < ?): 'generated' absent.
    tick(db, Action{Verb::Wait, 0, ""});  // a plain following turn
    {
        const TurnFacts facts = buildFacts(db, genTurn + 1);
        const nlohmann::json j = nlohmann::json::parse(facts.payload);
        bool sawGenerated = false;
        for (const auto& e : j["recent_events"]) {
            if (e.value("verb", "") == "generated") sawGenerated = true;
        }
        CHECK(!sawGenerated);
    }

    // Template renderer: the generation turn renders as the moved room block
    // (the new room), with no output for the 'generated' event.
    {
        const std::string out = render(db, genTurn);
        CHECK(!out.empty());
        CHECK(contains(out, "crypt") || contains(out, "undercroft"));
    }
}

// --- Step 5, REQ-ARCH-8: the direction-invertibility table. Pure, no DB. ---
static void testArchitectInvertible() {
    CHECK(inverseDirection("north") == "south");
    CHECK(inverseDirection("south") == "north");
    CHECK(inverseDirection("east") == "west");
    CHECK(inverseDirection("west") == "east");
    CHECK(inverseDirection("up") == "down");
    CHECK(inverseDirection("down") == "up");
    CHECK(inverseDirection("in") == "out");
    CHECK(inverseDirection("out") == "in");

    // Non-invertible directions → nullopt (not generatable → wall, no AI call).
    CHECK(!inverseDirection("northeast"));
    CHECK(!inverseDirection("widdershins"));
    CHECK(!inverseDirection(""));
}

// --- Step 3, REQ-ARCH-7c: the architect system prompt pins its STRUCTURE by
// substring (quality is a live concern, Step 10). Mirrors the resolver's prompt
// test. ---
static void testArchitectPrompt() {
    const std::string p = kArchitectPrompt;
    CHECK(!p.empty());

    // One room, coherent with the setting.
    CHECK(contains(p, "one room"));
    CHECK(contains(p, "coherent"));
    CHECK(contains(p, "setting"));
    // Emitted via create_room as a name + a description + declared exits.
    CHECK(contains(p, "create_room"));
    CHECK(contains(p, "name"));
    CHECK(contains(p, "description"));
    // REQ-EXITS-6: the prompt now REQUIRES declaring the onward exits, EXCLUDES
    // the entry-return direction, and REQUIRES the prose to describe them.
    CHECK(contains(p, "Declare in exits the directions that lead onward"));
    CHECK(contains(p, "describe those declared exits in the prose"));
    CHECK(contains(p, "EXCLUDE the direction back the way the player came"));
    // It no longer FORBIDS mentioning exits — the old prohibition is gone.
    CHECK(!contains(p, "Do NOT describe exits"));
    // Retained prohibitions: no arrival narration, no ids.
    CHECK(contains(p, "arrival"));
    CHECK(contains(p, "ids"));
}

int main() {
    // Live smoke FIRST, while the developer's real environment is still
    // intact: it needs a real ANTHROPIC_API_KEY, and the hermetic unset below
    // would otherwise clobber it (see testProseLiveSmoke's env-ordering note).
    // No-op unless TEXTWORLD_AI_LIVE_TEST=1, so the default run is unaffected
    // and makes no network access here (REQ-PROSE-17, REQ-RESOLVE-16). Both
    // live smokes run here, before the hermetic unset clobbers the real key.
    testProseLiveSmoke();
    testNlResolveLiveSmoke();
    testArchitectLiveSmoke();  // MUST be here — before the hermetic key unset below

    // Hermetic run: clear both AI env vars for the whole suite (guards
    // restore the developer's values on exit). Otherwise a developer shell
    // with ANTHROPIC_API_KEY set would send every runTurn-based test through
    // the real curl transport — the default test run must make NO network
    // access (REQ-PROSE-15). Tests that need the vars set their own values
    // under their own guards.
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("TEXTWORLD_AI");

    CHECK(1 + 1 == 2);

    testDb();
    testWorld();
    testShippedSeedShape();
    testCombatSchema();
    testParser();
    testMutations();
    testSystems();
    testCombatAttack();
    testCombatChipClock();
    testCombatDefeat();
    testCombatDowned();
    testCombatRender();
    testCombatTelegraph();
    testCombatCastGate();
    testCombatCounter();
    testCombatStatusLine();
    testRender();
    testExitDisplayInvariant();
    testLoop();
    testProseFacts();
    testNlResolveContext();
    testNlResolvePrompt();
    testProseTransport();
    testProseRequestBody();
    testNlResolveRequestBody();
    testNlResolveGate();
    testNlResolveAiResolve();
    testNlResolveDispatch();
    testNlResolveTierBPassthrough();
    testProseValidation();
    testProseNarrationEnabled();
    testProseAiRender();
    testPersistence();
    testPortability();
    testArchitectSettingLoad();
    testArchitectContext();
    testArchitectPrompt();
    testArchitectRequestBody();
    testArchitectGate();
    testArchitectInvertible();
    testWriteGeneratedRoom();
    testArchitectGenerate();
    testResolveGoGenerate();
    testCombatFlee();
    testGeneratedEventInvisible();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
