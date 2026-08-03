// Micro test harness: CHECK(cond) records failures; main() reports a summary.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>  // architect live smoke's single bounded judge call (gated)

#include "action.hpp"
#include "aihttp.hpp"
#include "architect.hpp"
#include "band.hpp"
#include "combat.hpp"
#include "db.hpp"
#include "loop.hpp"
#include "mutations.hpp"
#include "nlresolve.hpp"
#include "pregen.hpp"
#include "profile.hpp"
#include "prose.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "term.hpp"
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

    // --- Brick 4 tables: the bestiary catalog + drop map (REQ-COMBAT-28) ---
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('bestiary') "
                       "WHERE name IN ('archetype','name','blurb','health','chip',"
                       "'telegraph_period','tier','barrier')") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('bestiary')") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('drop_table') "
                       "WHERE name IN ('archetype','spell')") == 2);
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

// Elements + resistance multiplier; Fire/Frost; the basic-attack floor
// (REQ-COMBAT-16, -18, -17). Deterministic integer ratios, no RNG.
static void testCombatElements() {
    auto burnedAmt = [](Db& db) {
        return queryInt(db, "SELECT object FROM events WHERE verb = 'burned' "
                            "ORDER BY id DESC LIMIT 1");
    };
    auto frozeAmt = [](Db& db) {
        return queryInt(db, "SELECT object FROM events WHERE verb = 'froze' "
                            "ORDER BY id DESC LIMIT 1");
    };
    // Navigate cell -> corridor -> frost study (room 6, the rime-touched lock).
    auto toStudy = [](Db& db) {
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // corridor
        CHECK(runTurn(db, "go east").outcome == TurnOutcome::Ticked);   // frost study
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 6);
    };

    // --- Fire on the Fire-weak archetype applies the weakness multiplier (2x) ---
    {
        const TempDbFile p("textworld_combat_elem_fire.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'fire')");
        toStudy(db);
        CHECK(runTurn(db, "cast fire").outcome == TurnOutcome::Ticked);
        CHECK(burnedAmt(db) == kSpellDamage * 2);  // rime weak to fire: 2/1
    }

    // --- Frost (wrong element) applies the resist multiplier (1/2) + a slow ---
    {
        const TempDbFile p("textworld_combat_elem_frost.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'frost')");
        toStudy(db);
        CHECK(runTurn(db, "cast frost").outcome == TurnOutcome::Ticked);
        CHECK(frozeAmt(db) == kSpellDamage / 2);  // rime resists frost: 1/2
        // A slow CC was laid on the rime (still active after this tick's countdown).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM status_effects "
                           "WHERE entity = 8 AND kind = 'slow' AND remaining > 0") == 1);
    }

    // --- Basic attack deals >0 to EVERY seeded archetype (the anti-deadlock
    // floor, REQ-COMBAT-18), including the element-lock rime-touched ---
    {
        const TempDbFile p("textworld_combat_elem_floor.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        // Every seeded hostile archetype, none with an active barrier yet:
        // the goblin (corridor, room 2) and the rime-touched (study, room 6).
        for (const auto& [entity, room] :
             std::vector<std::pair<int, int>>{{7, 2}, {8, 6}}) {
            // Reach the enemy's room.
            if (room == 2) {
                CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
            } else {
                CHECK(runTurn(db, "go east").outcome == TurnOutcome::Ticked);
            }
            const std::string hpSql =
                "SELECT current FROM health WHERE entity = " + std::to_string(entity);
            const int64_t before = queryInt(db, hpSql.c_str());
            CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
            const int64_t after = queryInt(db, hpSql.c_str());
            CHECK(after == before - kBasicAttackDamage);
            CHECK(after < before);  // strictly non-zero damage: the floor holds
        }
    }
}

// Grimoire → Read → known_spells learning economy (REQ-COMBAT-20, -21, -22).
// Killing the seed enemy drops a spell grimoire; reading it learns the spell
// permanently (surviving restart); re-reading is a no-op; no growable stat
// exists anywhere. Deterministic.
static void testCombatLearn() {
    const TempDbFile worldPath("textworld_combat_learn_tests.db");

    int64_t grimoireId = 0;
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        // The player does not know fire to begin with.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM known_spells "
                           "WHERE entity = 3 AND spell = 'fire'") == 0);

        // Kill the goblin (corridor): it drops a fire grimoire with a grimoire row.
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);  // 8 -> 4
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);  // 4 -> 0, defeated

        grimoireId = queryInt(db, "SELECT entity FROM grimoire WHERE spell = 'fire'");
        CHECK(grimoireId > 0);
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(grimoireId)).c_str()) == 2);  // in the corridor

        // Read it → learn fire (canon).
        {
            const std::string out = runTurn(db, "read fire grimoire").output;
            CHECK(queryInt(db, "SELECT COUNT(*) FROM known_spells "
                               "WHERE entity = 3 AND spell = 'fire'") == 1);
            CHECK(contains(out, "learn"));  // 'learned' render template
        }
    }  // close the world file

    // Restart: reopen the persisted file. The learned spell survives (canon).
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM known_spells "
                           "WHERE entity = 3 AND spell = 'fire'") == 1);

        // Reading the already-known grimoire again is a no-op success ('reread').
        {
            const std::string out = runTurn(db, "read fire grimoire").output;
            CHECK(queryInt(db, "SELECT COUNT(*) FROM known_spells "
                               "WHERE entity = 3 AND spell = 'fire'") == 1);  // still one
            CHECK(contains(out, "already know"));  // 'reread' render template
        }

        // Anti-goal guard (REQ-COMBAT-22): NO XP/level/growable numeric column
        // exists anywhere in the schema — progression is only known_spells rows.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM sqlite_master m "
                       "JOIN pragma_table_info(m.name) p "
                       "WHERE m.type = 'table' AND lower(p.name) IN "
                       "('xp','level','levels','experience','exp','rank','growth',"
                       "'skillpoints','power')") == 0);
    }
}

// The bestiary catalog is the mold every instance is cast from (REQ-COMBAT-28,
// -29, -30): each seed instance's stats equal its catalog row, placeEnemy casts a
// fresh catalog-equal instance, and both a defeat and a spawn persist as canon
// across a reopen. Deterministic, no network.
static void testBestiaryCatalog() {
    const TempDbFile worldPath("textworld_bestiary_tests.db");

    int64_t spawnedId = 0;
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

        // The catalog is populated: one record per archetype, one drop each, and
        // every drop names a real catalog spell (no dangling key).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM bestiary") == 4);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM drop_table") == 4);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM drop_table d "
                       "LEFT JOIN spell_catalog s ON s.spell = d.spell "
                       "WHERE s.spell IS NULL") == 0);
        // All four archetypes are instantiated in the fixture.
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT archetype) FROM hostile") == 4);

        // REQ-COMBAT-30: EVERY seed instance's stats equal its bestiary row —
        // chip, telegraph_period, health (full), name, and the barrier trait are
        // all copied from the mold, so no instance can drift. Zero mismatches.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM hostile ho "
                       "JOIN bestiary b ON b.archetype = ho.archetype "
                       "JOIN health hp ON hp.entity = ho.entity "
                       "JOIN name n ON n.entity = ho.entity "
                       "WHERE ho.chip <> b.chip "
                       "OR ho.telegraph_period <> b.telegraph_period "
                       "OR hp.max <> b.health OR hp.current <> b.health "
                       "OR n.value <> b.name "
                       "OR (SELECT COUNT(*) FROM barrier ba "
                       "    WHERE ba.entity = ho.entity) <> b.barrier") == 0);

        // Kill the seed goblin (corridor) so defeat-persistence is checkable after
        // a reopen: go north, attack (8 -> 4), attack (4 -> 0, defeated).
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM hostile WHERE entity = 7") == 0);

        // REQ-COMBAT-29/-30: placeEnemy casts a fresh instance whose stats are
        // COPIED from the catalog — the engine owns every number. Cast an ironhide
        // (a barriered archetype) into the cell (room 1). Wrap in a transaction so
        // the helper's ambient-transaction contract holds.
        db.begin();
        spawnedId = placeEnemy(db, "ironhide", 1);
        db.commit();
        CHECK(spawnedId > 0);
        // Its stats equal the ironhide catalog row and it spawned at full health.
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM hostile ho "
                        "JOIN bestiary b ON b.archetype = ho.archetype "
                        "JOIN health hp ON hp.entity = ho.entity "
                        "WHERE ho.entity = " + std::to_string(spawnedId) +
                        " AND ho.archetype = 'ironhide' AND ho.chip = b.chip "
                        "AND ho.telegraph_period = b.telegraph_period "
                        "AND hp.max = b.health AND hp.current = b.health").c_str()) == 1);
        // The barrier trait is catalog-driven: an ironhide spawns warded.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM barrier WHERE entity = " +
                            std::to_string(spawnedId)).c_str()) == 1);
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(spawnedId)).c_str()) == 1);
    }  // close the world file

    // Reopen: a defeat and a spawn both persist as canon (REQ-COMBAT-30).
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        // The defeated goblin stays gone (no hostile/location) but its entity id
        // survives — defeat is persistent absence, not deletion.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM hostile WHERE entity = 7") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM location WHERE entity = 7") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities WHERE id = 7") == 1);
        // The spawned ironhide is still present and canon.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM hostile WHERE entity = " +
                            std::to_string(spawnedId)).c_str()) == 1);
    }
}

// The engine-computed eligible menu (REQ-COMBAT-32, -33, -34): gated by the
// player's known keys, the bootstrap ledger, and front intensity — wholly
// deterministic, no network. A helper: does the menu contain an archetype?
static bool menuHas(const std::vector<std::string>& menu, const char* archetype) {
    return std::find(menu.begin(), menu.end(), std::string(archetype)) != menu.end();
}

static void testCombatGating() {
    const TempDbFile worldPath("textworld_combat_gating_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // The corridor (room 2, distance 1 from the seed) is contested. The player
    // (entity 3) starts knowing {ward, stun}; it lacks fire and dispel.

    // Bootstrap (REQ-COMBAT-33): architect_spawn_count is absent (→ 0), so the
    // menu is the bootstrap menu — basic-soluble archetypes dropping a tier-1
    // spell. Only the goblin qualifies (basic-soluble, drops tier-1 fire): the
    // rime/ironhide aren't basic-soluble, the swarm drops tier-2 blast. The seed's
    // hand-placed goblin does NOT count against the ledger — the menu is non-empty
    // even though a goblin already stands in this world (the ledger ignores it).
    {
        const std::vector<std::string> menu = eligibleArchetypes(db, 2);
        CHECK(menu.size() == 1);
        CHECK(menuHas(menu, "goblin_grunt"));
    }

    // Mark the world as already-bootstrapped (an architect enemy has been placed),
    // so the general gating applies from here.
    db.exec("INSERT INTO meta(key, value) VALUES ('architect_spawn_count', 1)");

    // Gating (REQ-COMBAT-32): lacking fire, the fire-locked rime_touched is NOT
    // offered; lacking dispel, the barriered ironhide is NOT offered. The two
    // keyless archetypes (goblin, swarm) are offered.
    {
        const std::vector<std::string> menu = eligibleArchetypes(db, 2);
        CHECK(!menuHas(menu, "rime_touched"));
        CHECK(!menuHas(menu, "ironhide"));
        CHECK(menuHas(menu, "goblin_grunt"));
        CHECK(menuHas(menu, "book_swarm"));
    }

    // Learn fire → the fire-weak rime_touched becomes eligible; ironhide still
    // gated (no dispel).
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'fire')");
    {
        const std::vector<std::string> menu = eligibleArchetypes(db, 2);
        CHECK(menuHas(menu, "rime_touched"));
        CHECK(!menuHas(menu, "ironhide"));
    }

    // Learn dispel → the barriered ironhide becomes eligible; now all four are.
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'dispel')");
    {
        const std::vector<std::string> menu = eligibleArchetypes(db, 2);
        CHECK(menuHas(menu, "ironhide"));
        CHECK(menu.size() == 4);
    }

    // Front intensity (REQ-COMBAT-34): the outer hall (room 15, distance 3 from
    // the seed — beyond kFrontRadius) is a safe edge; its menu is empty regardless
    // of player keys.
    {
        const std::vector<std::string> menu = eligibleArchetypes(db, 15);
        CHECK(menu.empty());
    }
    // And the seed-adjacent corridor stays contested (non-empty) for contrast.
    CHECK(!eligibleArchetypes(db, 2).empty());
}

// The shipped setting.txt carries the invasion premise as a spreading front
// (REQ-COMBAT-36) while preserving the hushed Thornmere tone — and no numbers
// (the mechanical front is the engine handshake, never prose). Deterministic.
static void testCombatSetting() {
    const TempDbFile worldPath("textworld_combat_setting_tests.db");
    // Shipped world + the committed default settingPath (seed/setting.txt).
    Db db = openWorld(worldPath.string(), "seed/base.sql");

    std::string setting;
    {
        Stmt s = db.prepare("SELECT value FROM meta WHERE key = 'setting'");
        CHECK(s.step());
        setting = s.colText(0);
    }

    // The invasion premise: goblins, a breach, framed as a spreading front with a
    // dangerous core and safe edges (REQ-COMBAT-34/-36).
    CHECK(contains(setting, "goblin"));
    CHECK(contains(setting, "breach"));
    CHECK(contains(setting, "front"));
    CHECK(contains(setting, "contested"));
    CHECK(contains(setting, "edge"));

    // The hushed Thornmere tone is preserved, not replaced.
    CHECK(contains(setting, "Thornmere"));
    CHECK(contains(setting, "hushed"));
    CHECK(contains(setting, "Vigil Lamps"));

    // Tone only, NO numeric stat: the mechanical menu is the engine handshake, so
    // the prose the architect reads carries no digit anywhere.
    CHECK(queryInt(db,
                   "SELECT (value GLOB '*[0-9]*') FROM meta WHERE key = 'setting'") ==
          0);
}

// Run a fixed combat script from a fresh combat-fixture world and return a
// canonical dump of the event stream. Db closes at scope exit → the file on disk
// is the full committed state. Shared by the determinism replay.
static std::string replayCombatDump(const std::filesystem::path& path) {
    Db db = openWorld(path.string(), "tests/combat_fixture.sql");
    const char* script[] = {
        "go north",            // into the corridor with the goblin
        "cast stun",           // CC the goblin (status + cooldown)
        "attack",              // 8 -> 4
        "cast ward",           // a defensive beat (status + cooldown)
        "attack",              // 4 -> 0, defeated → drops a fire grimoire
        "read fire grimoire",  // learn fire (canon)
        "wait",
    };
    for (const char* cmd : script) runTurn(db, cmd);

    std::string dump;
    Stmt s = db.prepare(
        "SELECT turn, actor, verb, subject, object, COALESCE(detail,'') "
        "FROM events ORDER BY id");
    while (s.step()) {
        dump += std::to_string(s.colInt(0)) + "|" + std::to_string(s.colInt(1)) +
                "|" + s.colText(2) + "|" + std::to_string(s.colInt(3)) + "|" +
                std::to_string(s.colInt(4)) + "|" + s.colText(5) + "\n";
    }
    return dump;
}

// Determinism replay (REQ-COMBAT-1, AI-Validation item 1): the SAME scripted
// combat from a fresh seed twice produces byte-identical combat event streams AND
// byte-identical world.db files. The engine owns every number; no RNG anywhere.
static void testCombatDeterminismReplay() {
    const TempDbFile pathA("textworld_replay_a.db");
    const TempDbFile pathB("textworld_replay_b.db");
    const std::string dumpA = replayCombatDump(pathA);
    const std::string dumpB = replayCombatDump(pathB);

    CHECK(!dumpA.empty());
    CHECK(dumpA == dumpB);                                 // event streams identical
    CHECK(readFileBytes(pathA) == readFileBytes(pathB));   // final world.db identical
}

// The final spec sweep (REQ-COMBAT-1, -22, -37): guards that survive the whole
// combat surface — no growable stat at the final schema, no RNG in the source, a
// render template for every combat verb. Deterministic, no network.
static void testCombatFinalSweep() {
    const TempDbFile worldPath("textworld_final_sweep.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // (1) REQ-COMBAT-22 anti-goal at the FINAL schema (after bestiary/drop_table/
    // tier landed): no XP/level/growable numeric column anywhere. tier is a fixed
    // ordinal, not a per-entity growable, and is deliberately NOT in the ban set.
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM sqlite_master m "
                   "JOIN pragma_table_info(m.name) p "
                   "WHERE m.type = 'table' AND lower(p.name) IN "
                   "('xp','level','levels','experience','exp','rank','growth',"
                   "'skillpoints','power')") == 0);

    // (2) REQ-COMBAT-1 no-RNG guard: the combat surface source carries no RNG call
    // (rand/random/seeding) — a durable guard against a future regression.
    for (const char* src : {"src/combat.cpp", "src/combat.hpp", "src/mutations.cpp"}) {
        const std::string code = readFileBytes(src);
        CHECK(!contains(code, "rand("));
        CHECK(!contains(code, "srand"));
        CHECK(!contains(code, "random_device"));
        CHECK(!contains(code, "mt19937"));
    }

    // (3) REQ-COMBAT-37 exhaustive template: EVERY combat event verb the engine
    // can emit has a render() branch, so none falls through to empty output on the
    // AI-disabled path. The list mirrors the verbs emitted in combat.cpp/mutations.cpp.
    const std::string render = readFileBytes("src/render.cpp");
    for (const char* verb : {"attacked", "chip", "struck", "telegraph", "cast",
                             "warded", "stunned", "burned", "froze", "dot", "aoe",
                             "blocked", "dispelled", "learned", "reread",
                             "defeated", "downed"}) {
        CHECK(contains(render, std::string("verb == \"") + verb + "\""));
    }
}

// Multiplicity lock: AoE and DoT reach every body of a swarm; single-target
// basic attack thins them one at a time (REQ-COMBAT-17, -19). Deterministic.
static void testCombatMultiplicity() {
    const TempDbFile worldPath("textworld_combat_multi_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'blast')");

    auto living = [&] {
        return queryInt(db,
                        "SELECT COUNT(*) FROM hostile h JOIN health hp ON hp.entity = h.entity "
                        "JOIN location l ON l.entity = h.entity "
                        "WHERE l.container = 11 AND hp.current > 0");
    };

    // Into the library (off the cell, so the swarm is reached alone).
    CHECK(runTurn(db, "go down").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 11);
    CHECK(living() == 3);

    // AoE reaches EVERY body in one tick — each takes the AoE hit plus its first
    // DoT tick this same turn.
    {
        const std::string out = runTurn(db, "cast blast").output;
        CHECK(contains(out, "blast tears into"));  // render template
        // Each body: 10 - kAoeDamage - kDotDamage (the AoE + one DoT tick).
        for (int64_t body : {12, 13, 14}) {
            CHECK(queryInt(db, ("SELECT current FROM health WHERE entity = " +
                                std::to_string(body)).c_str()) ==
                  10 - kAoeDamage - kDotDamage);
        }
        // One AoE event per body; the DoT reached each distinct body.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'aoe'") == 3);
        CHECK(queryInt(db,
                       "SELECT COUNT(DISTINCT subject) FROM events WHERE verb = 'dot'") == 3);
        CHECK(living() == 3);  // none felled yet
    }

    // Single-target basic attack thins the swarm one body at a time.
    const int64_t before = living();
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
    CHECK(living() == before - 1);
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
    CHECK(living() == before - 2);
}

// Defense lock: barrier negates all damage until Dispel strips it — a two-key
// sequence (REQ-COMBAT-17). The basic-attack floor holds again post-strip
// (REQ-COMBAT-18). Deterministic.
static void testCombatDefenseLock() {
    const TempDbFile worldPath("textworld_combat_defense_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'dispel'), (3, 'fire')");

    auto ironhideHp = [&] {
        return queryInt(db, "SELECT current FROM health WHERE entity = 10");
    };

    // To the armory (cell -> corridor -> armory).
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
    CHECK(runTurn(db, "go up").outcome == TurnOutcome::Ticked);
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 9);
    CHECK(ironhideHp() == 14);

    // Barriered: a basic attack does nothing.
    {
        const std::string out = runTurn(db, "attack").output;
        CHECK(ironhideHp() == 14);  // unchanged
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'blocked'") >= 1);
        CHECK(contains(out, "barrier"));  // render template
    }
    // Elemental damage is blocked too — nothing lands through the barrier.
    CHECK(runTurn(db, "cast fire").outcome == TurnOutcome::Ticked);
    CHECK(ironhideHp() == 14);

    // Dispel strips the barrier (the first key).
    {
        const std::string out = runTurn(db, "cast dispel").output;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM barrier WHERE entity = 10") == 0);
        CHECK(contains(out, "dispel"));  // render template
    }

    // Now a basic attack lands — the floor holds post-strip (REQ-COMBAT-18).
    {
        const int64_t before = ironhideHp();
        CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);
        CHECK(ironhideHp() == before - kBasicAttackDamage);
        CHECK(ironhideHp() < before);
    }
}

// Damage-over-time (REQ-COMBAT-19): a DoT applies its fixed damage for exactly
// its duration, then stops. Stun keeps the player alive so the DoT can be
// observed in isolation. Deterministic.
static void testCombatDoT() {
    const TempDbFile worldPath("textworld_combat_dot_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'ember')");

    auto goblinHp = [&] {
        return queryInt(db, "SELECT current FROM health WHERE entity = 7");
    };
    auto dotEvents = [&] {
        return queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'dot'");
    };

    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // corridor
    CHECK(goblinHp() == 8);

    // Cast ember: the DoT is laid and burns its first tick this turn.
    {
        const std::string out = runTurn(db, "cast ember").output;
        CHECK(goblinHp() == 8 - kDotDamage);
        CHECK(dotEvents() == 1);
        CHECK(contains(out, "smoulders"));  // render template
    }
    // Stun the goblin (keeps the player safe); the DoT still burns its 2nd tick.
    CHECK(runTurn(db, "cast stun").outcome == TurnOutcome::Ticked);
    CHECK(goblinHp() == 8 - 2 * kDotDamage);
    CHECK(dotEvents() == 2);

    // The DoT has now expired (duration 2): no further damage, no further events.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    CHECK(goblinHp() == 8 - 2 * kDotDamage);  // unchanged
    CHECK(dotEvents() == kDotDuration);        // exactly its duration, then stops

    // Every DoT tick dealt exactly the fixed magnitude.
    CHECK(queryInt(db,
                   ("SELECT COUNT(*) FROM events WHERE verb = 'dot' AND object = " +
                    std::to_string(kDotDamage)).c_str()) == kDotDuration);
}

// Engine-appended status line: HP + per-spell cooldown readiness (REQ-COMBAT-15).
// Present in combat, absent outside it, and the SAME shared helper both render
// paths append (byte-identical). Deterministic.
static void testCombatStatusLine() {
    const TempDbFile worldPath("textworld_combat_statusline_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    const int64_t wardCd =
        queryInt(db, "SELECT cooldown FROM spell_catalog WHERE spell = 'ward'");

    // Outside combat: HP is STILL shown. REQ-UI-15 makes the HP row
    // unconditional — the band replaces the old in-combat-only status line, so
    // this assertion is inverted rather than deleted: it still tests something
    // real, namely that HP no longer disappears outside a fight.
    CHECK(contains(runTurn(db, "look").output, "HP:"));

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

    // The byte-identity block that stood here is RETIRED, not rewritten
    // (REQ-UI-7a). It existed to police a duplication — the same status line
    // appended by both render() and the AI path — that the status band
    // eliminates: there is now exactly one composition site, in runTurn, so
    // there are no longer two appends that could disagree. combatStatusLine()
    // itself survives as a helper and as the readiness oracle testBandContent
    // compares against (REQ-UI-6a).
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

    // Non-combat tick (in the cell, no hostile): HP is still shown. Inverted
    // for REQ-UI-15 — the band carries HP unconditionally now.
    CHECK(contains(runTurn(db, "look").output, "HP:"));

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
        // Combat is over, but the band still reports HP (REQ-UI-15). Inverted
        // for the same reason as the two above.
        CHECK(contains(out, "HP:"));
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

    // All ten ISA verbs are named (attack/cast/read added in the combat brick).
    for (const char* verb :
         {"look", "go", "take", "drop", "inventory", "wait", "quit", "attack",
          "cast", "read"}) {
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

// Parse one `twprof` record line into its key=value pairs. The leading
// "twprof" token has no '=' and lands as a valueless key; no value contains a
// space by construction, so whitespace splitting is exact.
static std::map<std::string, std::string> parseProfileRecord(
    const std::string& line) {
    std::map<std::string, std::string> kv;
    std::istringstream in(line);
    std::string tok;
    while (in >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) {
            kv[tok] = "";
        } else {
            kv[tok.substr(0, eq)] = tok.substr(eq + 1);
        }
    }
    return kv;
}

// --- profiling mechanism (REQ-LAT-1, -4, -5) --------------------------------
// ORDERING NOTE: profilingEnabled() caches its getenv (once per process, so the
// turn path pays only a bool read), which is exactly why the test-only
// profileRefreshEnabled() exists — every gate flip below must be followed by
// one, or the cache still holds the value main() installed. This test restores
// BOTH the env var (via the guard) and the cached bool + default sink before
// returning, so no later test emits a profiling line.
static void testProfileRecords() {
    const ScopedEnvVar profGuard("TEXTWORLD_PROFILE");

    // (a) the gate matrix (REQ-LAT-1). "0" counts as off, matching
    // TEXTWORLD_AI's convention — TEXTWORLD_PROFILE=0 must never mean ON.
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
    setenv("TEXTWORLD_PROFILE", "1", 1);
    profileRefreshEnabled();
    CHECK(profilingEnabled());
    setenv("TEXTWORLD_PROFILE", "", 1);
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
    setenv("TEXTWORLD_PROFILE", "0", 1);
    profileRefreshEnabled();
    CHECK(!profilingEnabled());

    // (b) the pure formatters (REQ-LAT-5): one parseable key=value line each.
    {
        const std::string line =
            formatStage(StageRecord{"narrate", 7, 1843.221, nullptr});
        const auto kv = parseProfileRecord(line);
        CHECK(kv.count("twprof") == 1);
        CHECK(kv.at("kind") == "stage");
        CHECK(kv.at("turn") == "7");
        CHECK(kv.at("stage") == "narrate");
        CHECK(kv.at("ms") == "1843.221");
        // A top-level stage carries no nesting key at all.
        CHECK(kv.count("nested_in") == 0);
    }
    {
        // generate is the one nested stage (it runs inside tick).
        const auto kv = parseProfileRecord(
            formatStage(StageRecord{"generate", 7, 12.5, "tick"}));
        CHECK(kv.at("stage") == "generate");
        CHECK(kv.at("nested_in") == "tick");
    }

    CallRecord ok;
    ok.role = "narrate";
    ok.model = "claude-opus-4-8";
    ok.turn = 7;
    ok.status = 200;
    ok.namelookupUs = 12;
    ok.connectUs = 0;
    ok.appconnectUs = 0;
    ok.starttransferUs = 1731004;
    ok.totalUs = 1843102;
    ok.inputTokens = 1420;
    ok.outputTokens = 212;
    ok.tokensKnown = true;
    {
        const auto kv = parseProfileRecord(formatCall(ok));
        CHECK(kv.at("kind") == "call");
        CHECK(kv.at("turn") == "7");
        CHECK(kv.at("role") == "narrate");
        CHECK(kv.at("model") == "claude-opus-4-8");
        CHECK(kv.at("status") == "200");
        CHECK(kv.at("namelookup_us") == "12");
        CHECK(kv.at("connect_us") == "0");
        CHECK(kv.at("appconnect_us") == "0");
        CHECK(kv.at("starttransfer_us") == "1731004");
        CHECK(kv.at("total_us") == "1843102");
        CHECK(kv.at("input_tokens") == "1420");
        CHECK(kv.at("output_tokens") == "212");
        CHECK(kv.count("failed") == 0);
        CHECK(kv.count("tokens") == 0);
    }
    {
        // REQ-LAT-4: a FAILED call notes the failure and emits NO token keys —
        // never a fabricated zero.
        CallRecord bad;
        bad.role = "resolve";
        bad.model = "claude-haiku-4-5";
        bad.turn = 7;
        bad.failed = true;
        const auto kv = parseProfileRecord(formatCall(bad));
        CHECK(kv.at("failed") == "1");
        CHECK(kv.at("status") == "0");
        CHECK(kv.count("input_tokens") == 0);
        CHECK(kv.count("output_tokens") == 0);
        CHECK(kv.count("tokens") == 0);
        // The timing fields still report — a failed call still spent time.
        CHECK(kv.count("total_us") == 1);
    }
    {
        // A 200 whose body carried no readable usage says so explicitly, so an
        // aggregator can tell "not reported" from "not parsed".
        CallRecord unknown = ok;
        unknown.tokensKnown = false;
        const auto kv = parseProfileRecord(formatCall(unknown));
        CHECK(kv.at("tokens") == "unknown");
        CHECK(kv.count("input_tokens") == 0);
        CHECK(kv.count("output_tokens") == 0);
    }

    // (c) the sink, and the gate governing emission (REQ-LAT-1).
    std::vector<std::string> captured;
    profileSetSink(
        [&captured](const std::string& line) { captured.push_back(line); });

    setenv("TEXTWORLD_PROFILE", "0", 1);
    profileRefreshEnabled();
    profileEmit(StageRecord{"resolve", 1, 1.0, nullptr});
    profileEmit(ok);
    { const ScopedStage off("total"); }
    CHECK(captured.empty());  // profiling off => the sink hears nothing

    setenv("TEXTWORLD_PROFILE", "1", 1);
    profileRefreshEnabled();
    profileEmit(StageRecord{"resolve", 1, 1.0, nullptr});
    CHECK(captured.size() == 1);
    profileEmit(ok);
    CHECK(captured.size() == 2);

    // ScopedStage emits ONCE, on scope exit, stamped with the current
    // process-local turn.
    const int64_t turn = profileNextTurn();
    CHECK(profileCurrentTurn() == turn);
    {
        const ScopedStage on("total");
        CHECK(captured.size() == 2);  // nothing yet — the destructor emits
    }
    CHECK(captured.size() == 3);
    {
        const auto kv = parseProfileRecord(captured.back());
        CHECK(kv.at("kind") == "stage");
        CHECK(kv.at("stage") == "total");
        CHECK(kv.at("turn") == std::to_string(turn));
    }

    // Restore the default sink AND the cached gate: the guard only restores the
    // env var, and the cache would otherwise outlive this test.
    profileSetSink({});
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
}

// --- Step 1, REQ-PREGEN-22/-24/-25: the three additions pre-generation needs
// from the profiling mechanism, all provable without a thread of pregen's own:
// the background flag's FORMAT, the dwell record's format and CORRECTNESS, and
// serialized emission under genuine concurrent load. ---
static void testProfileBackgroundAndDwell() {
    const ScopedEnvVar profGuard("TEXTWORLD_PROFILE");

    // (a) REQ-PREGEN-24: `background=1` appears on a background record and the
    // key is ABSENT — not `background=0` — on a foreground one, so every log
    // line written before this feature existed is still byte-identical.
    CallRecord fg;
    fg.role = "generate";
    fg.model = "claude-opus-4-8";
    fg.turn = 7;
    fg.status = 200;
    fg.totalUs = 7300000;
    fg.inputTokens = 1200;
    fg.outputTokens = 340;
    fg.tokensKnown = true;
    {
        const auto kv = parseProfileRecord(formatCall(fg));
        CHECK(kv.count("background") == 0);
    }
    {
        CallRecord bg = fg;
        bg.background = true;
        const std::string line = formatCall(bg);
        const auto kv = parseProfileRecord(line);
        CHECK(kv.at("background") == "1");
        // Exactly one key gained, and the rest of the line is unmoved: the
        // foreground form with " background=1" spliced in after status=.
        const std::string fgLine = formatCall(fg);
        const size_t at = fgLine.find(" namelookup_us=");
        CHECK(at != std::string::npos);
        CHECK(line == fgLine.substr(0, at) + " background=1" + fgLine.substr(at));
    }

    // (b) REQ-PREGEN-25 format: the dwell record's documented shape.
    {
        const auto kv = parseProfileRecord(formatDwell(DwellRecord{7, 1234.567}));
        CHECK(kv.count("twprof") == 1);
        CHECK(kv.at("kind") == "dwell");
        CHECK(kv.at("turn") == "7");
        CHECK(kv.at("ms") == "1234.567");
    }

    std::vector<std::string> captured;
    std::mutex capturedMutex;
    profileSetSink([&](const std::string& line) {
        const std::lock_guard<std::mutex> lock(capturedMutex);
        captured.push_back(line);
    });
    setenv("TEXTWORLD_PROFILE", "1", 1);
    profileRefreshEnabled();

    // (c) REQ-PREGEN-25 CORRECTNESS — the half the live run cannot establish,
    // because a piped script's true dwell is ~0 and an implementation that
    // always emitted zero would pass a presence check. A manufactured delay
    // must show up in the value: a real dependence on the wait, not a constant.
    {
        captured.clear();
        const int64_t turn = profileNextTurn();
        {
            const ScopedDwell dwell;
            CHECK(captured.empty());  // nothing yet — the destructor emits
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        CHECK(captured.size() == 1);
        const auto kv = parseProfileRecord(captured.back());
        CHECK(kv.at("kind") == "dwell");
        CHECK(kv.at("turn") == std::to_string(turn));
        const double ms = std::stod(kv.at("ms"));
        CHECK(ms >= 55.0);   // tracks the delay
        CHECK(ms < 500.0);   // and is not some unrelated large number
    }

    // (d) REQ-PREGEN-22: two threads, 200 records each. Every one of the 400
    // must arrive as a COMPLETE, well-formed line — never interleaved, never
    // truncated, never two records braided into one. The `twprof` marker
    // appearing anywhere past position 0 is exactly what a torn write looks
    // like, so that is what is asserted.
    {
        captured.clear();
        auto emitMany = [](const char* role) {
            for (int i = 0; i < 200; ++i) {
                CallRecord r;
                r.role = role;
                r.model = "claude-opus-4-8";
                r.turn = i;
                r.status = 200;
                r.background = true;
                r.tokensKnown = true;
                r.inputTokens = i;
                r.outputTokens = i;
                profileEmit(r);
            }
        };
        std::thread a([&] { emitMany("generate"); });
        std::thread b([&] { emitMany("narrate"); });
        a.join();
        b.join();

        CHECK(captured.size() == 400);
        bool allWellFormed = true;
        for (const std::string& line : captured) {
            if (line.empty() || line.rfind("twprof ", 0) != 0) {
                allWellFormed = false;
                break;
            }
            if (line.find("twprof", 1) != std::string::npos) {
                allWellFormed = false;  // a second record spliced in
                break;
            }
            const auto kv = parseProfileRecord(line);
            if (kv.count("kind") == 0 || kv.at("kind") != "call" ||
                kv.count("total_us") == 0 || kv.count("output_tokens") == 0) {
                allWellFormed = false;  // truncated before the tail keys
                break;
            }
        }
        CHECK(allWellFormed);
    }

    profileSetSink({});
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
}

// --- Step 3, REQ-PREGEN-8: the worker's own easy handle. What is mechanically
// checkable OFFLINE is handle LIFETIME and thread OWNERSHIP — construct and
// destroy the client on a std::thread, between the suite's aiHttpInit() and its
// shutdown, WITHOUT calling post(). No network, no crash, and the destructor
// runs on the same thread that built it.
//
// The abort callback's RUNTIME behavior is deliberately NOT faked here: it
// cannot be shown without a real in-flight transfer, so it belongs to the live
// run. What IS pinned here is that the option pair exists and that constructing
// a second handle alongside the shared one is safe. ---
static void testAiHttpWorkerClient() {
    std::atomic<bool> abort{false};
    std::atomic<bool> constructed{false};

    std::thread worker([&] {
        const AiHttpWorkerClient client(&abort);
        constructed.store(true);
        // No post() — the offline suite makes no network access.
    });
    worker.join();
    CHECK(constructed.load());

    // A nullptr abort flag is legal too (the no-abort configuration).
    std::thread plain([&] { const AiHttpWorkerClient client(nullptr); });
    plain.join();
    CHECK(true);  // reaching here without a crash IS the assertion
}

// The `stage` values of the captured records, in emission order.
static std::vector<std::string> capturedStages(
    const std::vector<std::string>& captured) {
    std::vector<std::string> stages;
    for (const std::string& line : captured) {
        const auto kv = parseProfileRecord(line);
        if (kv.at("kind") == "stage") stages.push_back(kv.at("stage"));
    }
    return stages;
}

// True iff any captured record is of `kind`.
static bool capturedAnyKind(const std::vector<std::string>& captured,
                            const std::string& kind) {
    for (const std::string& line : captured) {
        if (parseProfileRecord(line).at("kind") == kind) return true;
    }
    return false;
}

// --- turn phase timers (REQ-LAT-2, REQ-LAT-6, REQ-LAT-1) --------------------
// Runs with AI OFF (the suite is hermetic), so resolve and narrate take the
// parser/template paths — which is the point: the stages are SEMANTIC, so they
// are emitted whether or not a network call happened, and no kind=call record
// appears at all (REQ-LAT-6).
static void testProfileTurnStages() {
    const ScopedEnvVar profGuard("TEXTWORLD_PROFILE");
    std::vector<std::string> captured;
    profileSetSink(
        [&captured](const std::string& line) { captured.push_back(line); });

    // (d) identity check (REQ-LAT-1): the same first turn on two identically
    // seeded worlds, profiling off vs on. With profiling OFF the sink — which
    // is installed the whole time — must hear nothing at all.
    std::string offOutput;
    std::string onOutput;
    {
        const TempDbFile worldPath("textworld_profile_off_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        unsetenv("TEXTWORLD_PROFILE");
        profileRefreshEnabled();
        offOutput = runTurn(db, "look").output;
    }
    CHECK(captured.empty());

    {
        const TempDbFile worldPath("textworld_profile_on_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        setenv("TEXTWORLD_PROFILE", "1", 1);
        profileRefreshEnabled();
        onOutput = runTurn(db, "look").output;
    }
    CHECK(!onOutput.empty());
    CHECK(onOutput == offOutput);

    // (a) a normal ticked turn: exactly the four stages, in scope-exit order,
    // and NOTHING else — no curl record, no generate.
    {
        const std::vector<std::string> stages = capturedStages(captured);
        CHECK(stages.size() == 4);
        CHECK(stages == std::vector<std::string>({"resolve", "tick", "narrate",
                                                  "total"}));
        CHECK(!capturedAnyKind(captured, "call"));
        // Every record of one turn shares its process-local turn number.
        const auto first = parseProfileRecord(captured.front());
        for (const std::string& line : captured) {
            CHECK(parseProfileRecord(line).at("turn") == first.at("turn"));
        }
    }

    const TempDbFile worldPath("textworld_profile_stages_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // (b) tier a — an unresolvable line never opens a transaction and never
    // narrates: tick and narrate are ABSENT, not zero-faked (REQ-LAT-2).
    captured.clear();
    {
        const TurnResult r = runTurn(db, "frobnicate");
        CHECK(r.outcome == TurnOutcome::NoTick);
        CHECK(capturedStages(captured) ==
              std::vector<std::string>({"resolve", "total"}));
    }

    // (c) quit — same shape: resolved, then straight out.
    captured.clear();
    {
        const TurnResult r = runTurn(db, "quit");
        CHECK(r.outcome == TurnOutcome::Quit);
        CHECK(capturedStages(captured) ==
              std::vector<std::string>({"resolve", "total"}));
    }

    // Turn numbers advance once per turn, whatever the outcome.
    captured.clear();
    {
        const int64_t before = profileCurrentTurn();
        runTurn(db, "look");
        CHECK(profileCurrentTurn() == before + 1);
    }

    profileSetSink({});
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
}

// --- per-role model (REQ-LAT-12, REQ-LAT-13) --------------------------------
// modelForRole is the ONE place the precedence rule lives, and it has exactly
// two levels. Pure apart from the TEXTWORLD_MODEL read, so it is asserted
// directly under the model guard.
static void testAiRoleModel() {
    const ScopedModelEnv guard;

    // The role names are the strings the profile records carry.
    CHECK(std::string(roleName(AiRole::Resolve)) == "resolve");
    CHECK(std::string(roleName(AiRole::Narrate)) == "narrate");
    CHECK(std::string(roleName(AiRole::Generate)) == "generate");

    // Level 2 — per-role defaults: resolve is the cheap one, prose stays Opus.
    unsetenv("TEXTWORLD_MODEL");
    CHECK(modelForRole(AiRole::Resolve) == "claude-haiku-4-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Generate) == "claude-opus-4-8");

    // Level 1 — the global override wins for EVERY role (REQ-LAT-13), so
    // anyone relying on TEXTWORLD_MODEL today is unaffected by the tiering.
    setenv("TEXTWORLD_MODEL", "claude-sonnet-5", 1);
    CHECK(modelForRole(AiRole::Resolve) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Generate) == "claude-sonnet-5");

    // Set-but-EMPTY is not an override — back to the per-role defaults.
    setenv("TEXTWORLD_MODEL", "", 1);
    CHECK(modelForRole(AiRole::Resolve) == "claude-haiku-4-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Generate) == "claude-opus-4-8");
}

// --- usage / model body parsers (REQ-LAT-4) ---------------------------------
// Both are pure, never throw, and must report "unknown" rather than a
// fabricated zero on anything they cannot read.
static void testAiUsageParse() {
    {
        const AiUsage u = parseUsage(
            R"({"usage":{"input_tokens":1420,"output_tokens":212}})");
        CHECK(u.known);
        CHECK(u.inputTokens == 1420);
        CHECK(u.outputTokens == 212);
    }
    // A realistic body: usage alongside the other response fields.
    {
        const AiUsage u = parseUsage(
            R"({"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}],)"
            R"("usage":{"input_tokens":7,"output_tokens":3}})");
        CHECK(u.known);
        CHECK(u.inputTokens == 7);
    }
    // Everything unreadable reads as unknown — and NEVER as zero counts that a
    // log reader could mistake for a real measurement.
    const char* unreadable[] = {
        "",                                                    // empty
        "not json at all",                                     // garbage
        "[1,2,3]",                                             // not an object
        R"({"content":[]})",                                   // no usage
        R"({"usage":"nope"})",                                 // usage not object
        R"({"usage":{}})",                                     // empty usage
        R"({"usage":{"input_tokens":1420}})",                  // partial
        R"({"usage":{"input_tokens":"1420","output_tokens":212}})",  // string
        R"({"usage":{"input_tokens":1.5,"output_tokens":212}})",     // float
    };
    for (const char* body : unreadable) {
        const AiUsage u = parseUsage(body);
        CHECK(!u.known);
        CHECK(u.inputTokens == 0);
        CHECK(u.outputTokens == 0);
    }

    // modelFromRequestBody reads back what was actually sent.
    CHECK(modelFromRequestBody(R"({"model":"claude-opus-4-8","max_tokens":1024})") ==
          "claude-opus-4-8");
    CHECK(modelFromRequestBody(R"({"model":"claude-haiku-4-5"})") ==
          "claude-haiku-4-5");
    CHECK(modelFromRequestBody("").empty());
    CHECK(modelFromRequestBody("nonsense").empty());
    CHECK(modelFromRequestBody("{}").empty());
    CHECK(modelFromRequestBody(R"({"model":7})").empty());
}

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
        // REQ-LAT-12: the RESOLVE role's own default is the cheap model. The
        // prose and architect body tests below assert Opus and are untouched —
        // that contrast IS the per-role proof.
        CHECK(j["model"] == "claude-haiku-4-5");
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

        // input schema: object; verb enum is exactly the ten ISA verbs
        // (attack/cast/read added in the combat brick, REQ-COMBAT-38).
        const json& schema = tool["input_schema"];
        CHECK(schema["type"] == "object");
        const json& verb = schema["properties"]["verb"];
        CHECK(verb["type"] == "string");
        // ('spells' added by the status band, REQ-UI-37: the model-facing list
        // must carry it too, or inspection would work only via the fixed-verb
        // parser word — a silent half-wiring.)
        CHECK(verb["enum"] ==
              json::array({"look", "go", "take", "drop", "inventory", "wait",
                           "quit", "attack", "cast", "read", "spells"}));

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
        CHECK(j["model"] == "claude-haiku-4-5");
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

    // --- dispatch, AI off (no key): runTurn output is the template render,
    // wrapped, with the status band appended — no transport exists to be
    // touched ---
    //
    // These three assertions used to read `output == render(db, N)`. That
    // identity is what the status band deliberately ends (REQ-UI-1/-3/-4):
    // runTurn now wraps the prose and appends a band, so the equality is
    // restated as the composition rather than dropped. Their point is
    // preserved — the text still derives from render(), not from a model.
    {
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("TEXTWORLD_AI");

        const int width = detectWidth();

        const TurnResult r = runTurn(db, "look");  // turn 5
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 5);
        CHECK(r.output == wrapProse(render(db, 5), width) + composeBand(db, width));

        // Kill switch through the production path: key present but
        // TEXTWORLD_AI=0 → enabled() is false BEFORE any transport, so this
        // never reaches the network either.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        setenv("TEXTWORLD_AI", "0", 1);
        const TurnResult w = runTurn(db, "wait");  // turn 6
        CHECK(w.outcome == TurnOutcome::Ticked);
        CHECK(contains(w.output, "Time passes."));
        CHECK(w.output == wrapProse(render(db, 6), width) + composeBand(db, width));
        // REQ-UI-4: the band is LAST — the narration is above it.
        CHECK(w.output.find("Time passes.") < w.output.find("-- "));
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
//
// This helper deliberately keeps its OWN model read rather than calling
// modelForRole(): the judge is a test-side validator, not one of the game's
// three AI roles, so it has no place in the per-role tiering (REQ-LAT-12).
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

// Combat live smoke (REQ-COMBAT-31, live half of -35/-37/-38): gated behind
// TEXTWORLD_AI_LIVE_TEST=1, never run by default. Mechanical assertions ONLY —
// never model wording, never a prompt-tune loop ([[verification-must-be-bounded]]).
// It drives ONE real generation into a contested room and asserts that IF the
// model placed an enemy, that instance's stats equal the catalog (the model wrote
// no number); a model that places nothing is a clean fallback, not a failure.
static void testCombatLiveSmoke() {
    const char* live = std::getenv("TEXTWORLD_AI_LIVE_TEST");
    if (live == nullptr || std::string(live) != "1") {
        return;  // default run: no-op, no network.
    }
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::fprintf(stderr,
                     "COMBAT LIVE SMOKE SKIPPED: TEXTWORLD_AI_LIVE_TEST=1 but "
                     "ANTHROPIC_API_KEY is unset/empty.\n");
        return;
    }
    std::fprintf(stderr,
                 "COMBAT LIVE SMOKE: generating a contested room through the "
                 "Anthropic API (this makes a network call)...\n");

    const TempDbFile worldPath("textworld_combat_live_smoke.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql");

    // Clear the seed goblin from the corridor so the latent frontier off it is no
    // longer flee-guarded (REQ-COMBAT-26) — these ticks make NO network call.
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);   // cell -> corridor
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);     // 8 -> 4
    CHECK(runTurn(db, "attack").outcome == TurnOutcome::Ticked);     // 4 -> 0, defeated
    CHECK(queryInt(db, "SELECT COUNT(*) FROM hostile WHERE archetype = 'goblin_grunt' "
                       "AND entity = 7") == 0);
    // The seed goblin never counted against the architect ledger (REQ-COMBAT-33).
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = 'architect_spawn_count'") == 0);

    const int64_t before = queryInt(db, "SELECT container FROM location WHERE entity = 3");
    // Walk a latent exit off the now-clear corridor → a room at graph distance 2
    // (contested): the model is offered the bootstrap menu and MAY place an enemy.
    const std::string out = runTurn(db, "go up").output;
    const int64_t after = queryInt(db, "SELECT container FROM location WHERE entity = 3");

    if (after == before) {
        std::fprintf(stderr,
                     "  (generation declined — clean fallback, smoke stops)\n");
        return;
    }

    // A room was generated and entered. If the model placed an enemy in it, the
    // instance's stats MUST equal its catalog row — the model selected a costume,
    // the engine minted every number (REQ-COMBAT-29/-31).
    const int64_t hostiles = queryInt(
        db, ("SELECT COUNT(*) FROM hostile h JOIN location l ON l.entity = h.entity "
             "WHERE l.container = " + std::to_string(after)).c_str());
    if (hostiles == 0) {
        std::fprintf(stderr, "  (model placed no enemy — a clear room, allowed)\n");
    } else {
        std::fprintf(stderr, "  (model placed %lld enemy/enemies — checking catalog "
                             "equality)\n", static_cast<long long>(hostiles));
        // Every placed body equals its bestiary row; zero mismatches.
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM hostile h "
                        "JOIN bestiary b ON b.archetype = h.archetype "
                        "JOIN health hp ON hp.entity = h.entity "
                        "JOIN location l ON l.entity = h.entity "
                        "WHERE l.container = " + std::to_string(after) + " AND ("
                        "h.chip <> b.chip OR h.telegraph_period <> b.telegraph_period "
                        "OR hp.max <> b.health OR hp.current <> b.health)").c_str()) == 0);
        // The bootstrap enemy is basic-soluble and dropped a tier-1 key: the
        // ledger advanced (REQ-COMBAT-33). Mechanical, not wording.
        CHECK(queryInt(db,
                       "SELECT value FROM meta WHERE key = 'architect_spawn_count'") >= 1);
    }
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
    const std::vector<std::string>& exits = {}, const std::string& enemy = "") {
    nlohmann::json input;
    input["name"] = name;
    input["description"] = description;
    // Only add the optional exits array when non-empty — the analog of how
    // cannedToolUse carries optional fields (REQ-EXITS-7 fixture).
    if (!exits.empty()) {
        input["exits"] = exits;
    }
    // Optional enemy selection (REQ-COMBAT-31): a blurb the model "chose".
    if (!enemy.empty()) {
        input["enemy"] = enemy;
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

// --- Step 4, REQ-PREGEN-1/-9/-10/-11/-12/-13/-16/-21: the candidate store as a
// SINGLE-THREADED state machine. No thread exists yet, deliberately: the store
// is the part that can be proven exhaustively without concurrency, so it is
// proven here and the thread is added on top of something already known good.
// Every transport is a fake — no network. ---
static void testPregenStore() {
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");

    // A job as the scheduler will hand one over: no Db, no world pointer, just
    // the snapshot.
    auto makeJob = [](int64_t room, const std::string& dir, int64_t turn) {
        PregenJob job;
        job.room = room;
        job.direction = dir;
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.enemyBlurbs = {};
        job.snapshotTurn = turn;
        return job;
    };

    // (a) REQ-PREGEN-1, the gate: with TEXTWORLD_PREGEN=0 a submit is dropped
    // on the floor and an acquire is a Miss that touches nothing.
    {
        setenv("TEXTWORLD_PREGEN", "0", 1);
        pregenRefreshEnabledForTest();
        CHECK(!pregenEnabled());
        pregenResetForTest();

        pregenSubmit(makeJob(1, "east", 3));
        CHECK(pregenPendingCountForTest() == 0);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("crypt", "A cold undercroft.");
        };
        const PregenResult r = pregenAcquire(1, "east", &fake);
        CHECK(r.outcome == PregenOutcome::Miss);
        CHECK(!r.proposal.has_value());
        CHECK(calls == 0);
    }

    // Everything below runs with the gate ON — unset means on, which is the
    // half of REQ-PREGEN-1 an AI-off script can never exercise.
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    CHECK(pregenEnabled());
    // "0" is the ONLY off value; any other value leaves it on.
    setenv("TEXTWORLD_PREGEN", "1", 1);
    pregenRefreshEnabledForTest();
    CHECK(pregenEnabled());
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();

    // (b) REQ-PREGEN-12, three of the four states (Running needs the worker).
    {
        pregenResetForTest();
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);
        pregenSubmit(makeJob(1, "east", 3));
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);
        CHECK(pregenPendingCountForTest() == 1);

        RoomProposal proposal;
        proposal.name = "crypt";
        proposal.description = "A cold undercroft.";
        pregenInjectReadyForTest(2, "north", proposal, 9);
        CHECK(pregenStateOf(2, "north") == PregenState::Ready);
        // An unrelated key is still Absent — the store is keyed, not global.
        CHECK(pregenStateOf(2, "south") == PregenState::Absent);
    }

    // (c) Hit: a ready candidate is handed over with ZERO transport calls, and
    // carries its snapshot turn for the staleness report (REQ-PREGEN-13).
    {
        pregenResetForTest();
        RoomProposal proposal;
        proposal.name = "crypt";
        proposal.description = "A cold undercroft of grey stone.";
        proposal.exits = {"north"};
        proposal.enemyBlurb = "a hunched goblin";
        pregenInjectReadyForTest(1, "east", proposal, 4);

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("wrong", "should never be built");
        };
        const PregenResult r = pregenAcquire(1, "east", &fake);
        CHECK(r.outcome == PregenOutcome::Hit);
        CHECK(calls == 0);
        CHECK(r.proposal.has_value());
        CHECK(r.proposal->name == "crypt");
        CHECK(r.proposal->description == "A cold undercroft of grey stone.");
        CHECK(r.proposal->exits.size() == 1);
        CHECK(r.proposal->enemyBlurb == "a hunched goblin");
        CHECK(r.snapshotTurn == 4);
        CHECK(r.runMs == 0.0);   // a hit did no work
        CHECK(r.waitMs == 0.0);
        // REQ-PREGEN-11: the candidate is not evicted by being read.
        CHECK(pregenStateOf(1, "east") == PregenState::Ready);
    }

    // (d) RanQueued: a job still in the queue is DEQUEUED and run right here,
    // once, and the queue is left empty (REQ-PREGEN-16 second bullet).
    {
        pregenResetForTest();
        pregenSubmit(makeJob(1, "east", 7));
        CHECK(pregenPendingCountForTest() == 1);

        int calls = 0;
        std::string sentBody;
        HttpTransport fake = [&](const std::string& body) {
            ++calls;
            sentBody = body;
            return cannedCreateRoom("crypt", "A cold undercroft.", {"north"});
        };
        const PregenResult r = pregenAcquire(1, "east", &fake);
        CHECK(r.outcome == PregenOutcome::RanQueued);
        CHECK(calls == 1);  // exactly one call — no duplicate for this key
        CHECK(r.proposal.has_value());
        CHECK(r.proposal->name == "crypt");
        CHECK(r.runMs >= 0.0);
        CHECK(pregenPendingCountForTest() == 0);
        // The snapshot was REUSED, not rebuilt: the body carries the payload
        // the job was queued with (REQ-PREGEN-5, REQ-PREGEN-16).
        CHECK(sentBody.find("stone hall") != std::string::npos);
        // The result is also stored, so the key is now a hit.
        CHECK(pregenStateOf(1, "east") == PregenState::Ready);
    }

    // (e) Failure, both shapes: a throwing transport and a non-200 each leave
    // the slot ABSENT — cleared, not poisoned (REQ-PREGEN-9, REQ-PREGEN-10) —
    // yield no proposal, and are called exactly ONCE (no retries).
    {
        pregenResetForTest();
        pregenSubmit(makeJob(1, "east", 7));
        int calls = 0;
        HttpTransport thrower = [&](const std::string&) -> HttpResponse {
            ++calls;
            throw std::runtime_error("socket exploded");
        };
        const PregenResult r = pregenAcquire(1, "east", &thrower);
        CHECK(r.outcome == PregenOutcome::RanQueued);
        CHECK(!r.proposal.has_value());
        CHECK(calls == 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);
        CHECK(pregenPendingCountForTest() == 0);

        // Absent again means re-queueable — the bounded retry of REQ-PREGEN-10.
        pregenSubmit(makeJob(1, "east", 8));
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);

        int badCalls = 0;
        HttpTransport bad = [&](const std::string&) {
            ++badCalls;
            HttpResponse resp;
            resp.status = 500;
            return resp;
        };
        const PregenResult r2 = pregenAcquire(1, "east", &bad);
        CHECK(r2.outcome == PregenOutcome::RanQueued);
        CHECK(!r2.proposal.has_value());
        CHECK(badCalls == 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);
    }

    // (f) REQ-PREGEN-11 / -21: one slot per key, ever. A second submit for a
    // key already known is a no-op, so no room is ever paid for twice.
    {
        pregenResetForTest();
        pregenSubmit(makeJob(1, "east", 1));
        pregenSubmit(makeJob(1, "east", 2));
        pregenSubmit(makeJob(1, "east", 3));
        CHECK(pregenPendingCountForTest() == 1);
        // A different direction off the same room IS a different key.
        pregenSubmit(makeJob(1, "west", 1));
        CHECK(pregenPendingCountForTest() == 2);
    }

    pregenResetForTest();
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// A fake transport the TEST controls the timing of. It blocks inside the call
// until the test releases it, so a job can be pinned in flight and observed —
// the alternative, sleeping and hoping, is what turns a concurrency bug into an
// unbounded retry loop. Every wait below is on a condition the test itself
// satisfies, so nothing here can hang on a slow machine that would not also
// hang on a fast one.
struct BlockingTransport {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    int calls = 0;
    bool concurrentEntry = false;  // set if two calls are ever inside at once
    int inside = 0;
    std::string roomName = "crypt";

    // Blocks until release() is called. Records the call and flags any
    // overlapping entry — the serial-worker assertion (REQ-PREGEN-6).
    HttpResponse operator()(const std::string&) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++calls;
            ++inside;
            if (inside > 1) concurrentEntry = true;
            cv.wait(lock, [this] { return released; });
            --inside;
        }
        return cannedCreateRoom(roomName, "A cold undercroft of grey stone.");
    }

    void release() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }

    int callCount() {
        const std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
};

// Spin (not sleep) until `pred` holds. A hang here is a DESIGN bug to be read
// out of the code, never a timing knob to be tuned — so there is deliberately
// no timeout to raise.
template <typename Pred>
static void spinUntil(Pred pred) {
    while (!pred()) std::this_thread::yield();
}

// --- Step 5, REQ-PREGEN-2/-6/-12: the worker thread, and NOTHING else. The
// tick still never waits on it, so a bug at this step can only show up as a job
// that does not run — never as a hang. That separation is the entire reason
// this is split from the wait in testPregenWait below. ---
static void testPregenWorker() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");

    auto makeJob = [](int64_t room, const std::string& dir) {
        PregenJob job;
        job.room = room;
        job.direction = dir;
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 5;
        return job;
    };

    // (a) REQ-PREGEN-1 / -2: the thread-existence matrix. This is the check the
    // spec insists on by name — "no records in the log" would pass all three
    // rows below and prove nothing.
    {
        // architect on, pregen on (unset) -> a thread exists.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        unsetenv("TEXTWORLD_PREGEN");
        pregenRefreshEnabledForTest();
        CHECK(architectEnabled());
        pregenResetForTest();
        pregenSetWorkerTransportForTest(
            [](const std::string&) { return cannedCreateRoom("x", "y"); });
        pregenStart();
        CHECK(pregenWorkerRunning());
        pregenResetForTest();
        CHECK(!pregenWorkerRunning());

        // pregen off -> NO thread.
        setenv("TEXTWORLD_PREGEN", "0", 1);
        pregenRefreshEnabledForTest();
        pregenStart();
        CHECK(!pregenWorkerRunning());
        pregenResetForTest();

        // architect off (no key) -> NO thread, even with pregen on.
        unsetenv("TEXTWORLD_PREGEN");
        pregenRefreshEnabledForTest();
        unsetenv("ANTHROPIC_API_KEY");
        CHECK(!architectEnabled());
        pregenStart();
        CHECK(!pregenWorkerRunning());
        pregenResetForTest();
    }

    // Both gates on for the rest.
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();

    // (b) REQ-PREGEN-12, the fourth state: Running is OBSERVABLE. The job is
    // pinned inside the transport until this test releases it.
    {
        pregenResetForTest();
        BlockingTransport blocking;
        pregenSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        pregenStart();
        CHECK(pregenWorkerRunning());

        pregenSubmit(makeJob(1, "east"));
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Running; });
        CHECK(pregenStateOf(1, "east") == PregenState::Running);

        blocking.release();
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Ready; });
        CHECK(pregenStateOf(1, "east") == PregenState::Ready);
        CHECK(blocking.callCount() == 1);

        // And the candidate the worker produced is really there.
        const PregenResult r = pregenAcquire(1, "east", nullptr);
        CHECK(r.outcome == PregenOutcome::Hit);
        CHECK(r.proposal.has_value());
        CHECK(r.proposal->name == "crypt");
        CHECK(r.snapshotTurn == 5);
        pregenResetForTest();
    }

    // (c) REQ-PREGEN-6: ONE job at a time. Three jobs submitted at once; the
    // fake flags any overlapping entry, and all three still complete.
    {
        pregenResetForTest();
        std::mutex m;
        std::condition_variable cv;
        int done = 0;
        bool overlapped = false;
        int inside = 0;
        pregenSetWorkerTransportForTest([&](const std::string&) {
            {
                const std::lock_guard<std::mutex> lock(m);
                ++inside;
                if (inside > 1) overlapped = true;
            }
            std::this_thread::yield();  // widen the window a real overlap needs
            {
                const std::lock_guard<std::mutex> lock(m);
                --inside;
                ++done;
            }
            cv.notify_all();
            return cannedCreateRoom("crypt", "A cold undercroft.");
        });
        pregenStart();

        pregenSubmit(makeJob(1, "east"));
        pregenSubmit(makeJob(1, "west"));
        pregenSubmit(makeJob(1, "north"));

        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return done == 3; });
        }
        // `done` counts transport RETURNS; the worker stores each result just
        // after. Wait on the state the assertions actually read, not on the
        // fake's counter — otherwise the last store races this thread.
        spinUntil([] {
            return pregenStateOf(1, "north") == PregenState::Ready;
        });

        CHECK(!overlapped);  // never re-entered concurrently
        CHECK(pregenStateOf(1, "east") == PregenState::Ready);
        CHECK(pregenStateOf(1, "west") == PregenState::Ready);
        CHECK(pregenStateOf(1, "north") == PregenState::Ready);
        CHECK(pregenPendingCountForTest() == 0);
        pregenResetForTest();
    }

    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// --- Step 6, REQ-PREGEN-16/-19/-20: the only place in the codebase that
// blocks a thread. Everything that can hang lives here, so a hang has exactly
// one place to be. Same discipline as Step 5: explicit gates the test releases,
// no sleep-based sequencing anywhere. ---
static void testPregenWait() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();

    auto makeJob = [](int64_t room, const std::string& dir) {
        PregenJob job;
        job.room = room;
        job.direction = dir;
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 5;
        return job;
    };

    // (a) REQ-PREGEN-16 first bullet: the tick walks an exit whose job is
    // RUNNING. It waits for that job — it does NOT start a second call — and
    // then commits that job's own result.
    {
        pregenResetForTest();
        BlockingTransport blocking;
        pregenSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        pregenStart();
        pregenSubmit(makeJob(1, "east"));
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Running; });

        // The acquire blocks, so it runs on a second thread and the test
        // releases the transport from here — the same shape as the real thing,
        // where the tick blocks and the worker finishes underneath it.
        std::promise<PregenResult> promise;
        std::future<PregenResult> future = promise.get_future();
        std::thread tick([&] {
            promise.set_value(pregenAcquire(1, "east", nullptr));
        });

        // Release only once the tick is PROVABLY blocked in the wait.
        // Releasing sooner would let the job land first and turn this into a
        // hit — a flaky test, not different behavior.
        spinUntil([] { return pregenWaitingCountForTest() == 1; });
        blocking.release();
        const PregenResult r = future.get();
        tick.join();

        CHECK(r.outcome == PregenOutcome::Waited);
        CHECK(r.proposal.has_value());
        CHECK(r.proposal->name == "crypt");
        CHECK(r.waitMs >= 0.0);
        CHECK(blocking.callCount() == 1);  // exactly one call IN TOTAL
        pregenResetForTest();
    }

    // (b) REQ-PREGEN-12 / -16 second bullet — the failure this whole design
    // exists to avoid. With the worker stuck inside job A, walking B must NOT
    // wait for A: B is dequeued and run on the calling thread. Asserted by
    // completing B *while A is still blocked*, which a queue-order wait could
    // never do.
    {
        pregenResetForTest();
        BlockingTransport blockingA;
        std::atomic<int> bCalls{0};
        pregenSetWorkerTransportForTest([&](const std::string& body) {
            return blockingA(body);  // the worker only ever gets A
        });
        pregenStart();

        pregenSubmit(makeJob(1, "east"));   // A — the worker will take this
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Running; });
        pregenSubmit(makeJob(1, "west"));   // B — still sitting in the queue
        CHECK(pregenStateOf(1, "west") == PregenState::Queued);

        HttpTransport bTransport = [&](const std::string&) {
            ++bCalls;
            return cannedCreateRoom("vestry", "A narrow robing room.");
        };
        const PregenResult rb = pregenAcquire(1, "west", &bTransport);

        // B completed. A is STILL blocked — proof the tick did not queue behind
        // it (the bound a naive wait would have accepted is queue depth x 8 s).
        CHECK(pregenStateOf(1, "east") == PregenState::Running);
        CHECK(rb.outcome == PregenOutcome::RanQueued);
        CHECK(rb.proposal.has_value());
        CHECK(rb.proposal->name == "vestry");
        CHECK(bCalls.load() == 1);          // exactly one call for B's key
        CHECK(blockingA.callCount() == 1);  // and A was not called again

        blockingA.release();
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Ready; });
        pregenResetForTest();
    }

    // (c) REQ-PREGEN-19 / -20: stop while a job is in flight. The join must
    // complete — this is the assertion that the process does not hang on quit —
    // and a second stop must be a harmless no-op.
    {
        pregenResetForTest();
        BlockingTransport blocking;
        pregenSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        pregenStart();
        CHECK(pregenWorkerRunning());
        pregenSubmit(makeJob(1, "east"));
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Running; });

        // pregenStop() blocks until the worker's current job returns. A real
        // libcurl transfer is torn down by the abort callback (which needs a
        // real transfer, so it belongs to the live run); a fake transport has
        // no such callback, so the test releases it explicitly — the join is
        // still the thing under test.
        std::thread stopper([] { pregenStop(); });
        blocking.release();
        stopper.join();
        CHECK(!pregenWorkerRunning());

        pregenStop();  // idempotent
        CHECK(!pregenWorkerRunning());
        pregenResetForTest();
    }

    // (d) Stop when no thread was ever started — safe, not a crash.
    {
        pregenResetForTest();
        CHECK(!pregenWorkerRunning());
        pregenStop();
        pregenStop();
        CHECK(!pregenWorkerRunning());
    }

    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// --- Step 9, REQ-PREGEN-4 (call sites): playerRoom() is the accessor main()
// needs to name a room for the scheduler. It must agree with the location row
// AND with the subject renderStartup renders, or main would be prefetching the
// exits of a room the player is not standing in. ---
static void testPlayerRoom() {
    const TempDbFile worldPath("textworld_player_room.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // The fixture stands the player (3) in the stone hall (1).
    CHECK(playerRoom(db) == 1);
    CHECK(playerRoom(db) ==
          queryInt(db, "SELECT container FROM location WHERE entity = 3"));
    CHECK(contains(renderStartup(db), "vaulted hall of grey stone"));

    // It TRACKS the player: after a move it names the new room, and still
    // agrees with what startup would render.
    const TurnResult r = runTurn(db, "go north");
    CHECK(r.outcome == TurnOutcome::Ticked);
    CHECK(playerRoom(db) == 2);
    CHECK(playerRoom(db) ==
          queryInt(db, "SELECT container FROM location WHERE entity = 3"));
    CHECK(contains(renderStartup(db), "overgrown walled garden"));

    // Read-only: naming the room neither ticks nor writes an event.
    const int64_t turn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    const int64_t events = queryInt(db, "SELECT COUNT(*) FROM events");
    CHECK(playerRoom(db) == 2);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turn);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == events);
}

// --- Step 8, REQ-PREGEN-3/-4/-5/-10/-13/-18: the scheduler. Read-only, on the
// main thread, and idempotent — that last property is what lets main() call it
// after every single turn without thinking about it. ---
static void testArchitectQueuePregen() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();

    // (a) REQ-PREGEN-3 / -4: one job per LATENT exit, never one for a realized
    // exit. The fixture already maps 1 -north-> 2 (realized); two latents are
    // added alongside it.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_queue_basic.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");

        architectQueuePregen(db, 1);
        CHECK(pregenPendingCountForTest() == 2);
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);
        CHECK(pregenStateOf(1, "up") == PregenState::Queued);
        CHECK(pregenStateOf(1, "north") == PregenState::Absent);  // realized

        // (b) REQ-PREGEN-4, IDEMPOTENCE. Calling again for the same room queues
        // nothing — the property D3's "call it after every turn" rests on.
        architectQueuePregen(db, 1);
        architectQueuePregen(db, 1);
        CHECK(pregenPendingCountForTest() == 2);
    }

    // (c) REQ-PREGEN-5 / -13: the job carries the SNAPSHOT, and the snapshot is
    // genuinely re-read — advance the turn counter between two scheduler calls
    // and the stamp must move with it.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_queue_snapshot.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        db.exec("UPDATE meta SET value = 4 WHERE key = 'turn'");

        // Capture what the worker actually receives by letting the tick run the
        // queued job with a body-recording fake.
        architectQueuePregen(db, 1);
        std::string sentBody;
        HttpTransport recorder = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("crypt", "A cold undercroft.");
        };
        const PregenResult r = pregenAcquire(1, "east", &recorder);
        CHECK(r.outcome == PregenOutcome::RanQueued);

        // The context payload is exactly buildArchitectContext's output, and
        // the enemy menu exactly eligibleEnemyBlurbs' (empty in this fixture,
        // so the body carries no `enemy` property at all).
        const std::string expectedContext = buildArchitectContext(db, 1, "east");
        const nlohmann::json body = nlohmann::json::parse(sentBody);
        CHECK(body["messages"][0]["content"] == expectedContext);
        CHECK(eligibleEnemyBlurbs(db, 1).empty());
        CHECK(!body["tools"][0]["input_schema"]["properties"].contains("enemy"));

        // The stamp is meta.turn. It rides on the stored candidate, and a
        // result carries it only on a HIT — which is the one outcome that
        // reports age_turns — so it is read back the way production reads it.
        CHECK(pregenStateOf(1, "east") == PregenState::Ready);
        CHECK(pregenAcquire(1, "east", &recorder).snapshotTurn == 4);

        // And it MOVES with meta.turn, so the read is genuinely being
        // exercised rather than hard-coded.
        db.exec("UPDATE meta SET value = 9 WHERE key = 'turn'");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");
        architectQueuePregen(db, 1);
        CHECK(pregenAcquire(1, "up", &recorder).outcome == PregenOutcome::RanQueued);
        const PregenResult r2 = pregenAcquire(1, "up", &recorder);
        CHECK(r2.outcome == PregenOutcome::Hit);
        CHECK(r2.snapshotTurn == 9);
    }

    // (d) REQ-PREGEN-18: a room WITH an eligible enemy menu snapshots that menu
    // into the job, so the model sees the same choices the synchronous path
    // would have offered it.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_queue_enemy.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        const std::vector<std::string> expected = eligibleEnemyBlurbs(db, 1);
        CHECK(!expected.empty());

        architectQueuePregen(db, 1);
        std::string sentBody;
        HttpTransport recorder = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("crypt", "A cold undercroft.");
        };
        CHECK(pregenAcquire(1, "east", &recorder).outcome ==
              PregenOutcome::RanQueued);
        const nlohmann::json body = nlohmann::json::parse(sentBody);
        const nlohmann::json& props = body["tools"][0]["input_schema"]["properties"];
        CHECK(props.contains("enemy"));
        CHECK(props["enemy"]["enum"] == nlohmann::json(expected));
    }

    // (e) REQ-PREGEN-10, OCCUPANCY. A failed job clears its slot; standing in
    // the same room must NOT re-queue it, and leaving and returning MUST.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_queue_occupancy.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (2, 'east', NULL)");

        architectQueuePregen(db, 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);

        // Force the job to fail: the slot goes back to Absent.
        HttpTransport err = [](const std::string&) {
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        CHECK(pregenAcquire(1, "east", &err).outcome == PregenOutcome::RanQueued);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);

        // Same room, same occupancy → not retried. This is what bounds retries
        // by room entries rather than by time; a spin here would be a call per
        // turn, forever, during an outage.
        architectQueuePregen(db, 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);
        CHECK(pregenPendingCountForTest() == 0);

        // Leave (room 2) and come back → a NEW occupancy, so it IS re-queued.
        architectQueuePregen(db, 2);
        CHECK(pregenStateOf(2, "east") == PregenState::Queued);
        architectQueuePregen(db, 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);
    }

    // (f) REQ-PREGEN-1 / -2: both gates, each on its own. Off means NOTHING is
    // queued — not a job that is queued and then ignored.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_queue_gates.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        setenv("TEXTWORLD_PREGEN", "0", 1);
        pregenRefreshEnabledForTest();
        architectQueuePregen(db, 1);
        CHECK(pregenPendingCountForTest() == 0);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);

        unsetenv("TEXTWORLD_PREGEN");
        pregenRefreshEnabledForTest();
        unsetenv("ANTHROPIC_API_KEY");  // architectEnabled() false
        CHECK(!architectEnabled());
        architectQueuePregen(db, 1);
        CHECK(pregenPendingCountForTest() == 0);
        CHECK(pregenStateOf(1, "east") == PregenState::Absent);

        // And with both back on, the same call does queue — so the two checks
        // above are really about the gates and not about the fixture.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        architectQueuePregen(db, 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);
    }

    pregenResetForTest();
    architectResetPregenOccupancyForTest();
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// --- Step 2, REQ-PREGEN-14: architectCommitProposal is architectGenerate's
// Phase 2, extracted whole. The refactor's real proof is that every existing
// architect test above passes UNEDITED; this test adds the direct entry point,
// asserting the same rows testArchitectGenerate asserts — reached from a
// hand-built RoomProposal with no transport anywhere in sight, which is
// precisely how the pregen hit path will reach it. ---
static void testArchitectCommitProposal() {
    const int64_t player = 3;
    const TempDbFile worldPath("textworld_arch_commit.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");

    // A proposal as the gate would have produced it, with one declared onward
    // exit so the latent-stub half of Phase 2 is exercised too.
    RoomProposal proposal;
    proposal.name = "crypt";
    proposal.description = "A cold undercroft of grey stone.";
    proposal.exits = {"north"};

    db.begin();
    const int64_t newRoom =
        architectCommitProposal(db, 1, "east", proposal, player);
    db.commit();

    CHECK(newRoom > 5);  // minted beyond the fixture's ids
    // The origin exit is realized to the new room, and the reciprocal planted.
    CHECK(queryInt(db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'") ==
          newRoom);
    CHECK(queryInt(db, ("SELECT dest FROM exits WHERE room = " +
                        std::to_string(newRoom) + " AND direction = 'west'").c_str()) == 1);
    CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                         std::to_string(newRoom)).c_str()) == "crypt");
    CHECK(queryText(db, ("SELECT prose FROM description WHERE entity = " +
                         std::to_string(newRoom)).c_str()) ==
          "A cold undercroft of grey stone.");
    // The declared onward exit is present as a LATENT stub (dest NULL).
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM exits WHERE room = " +
                        std::to_string(newRoom) +
                        " AND direction = 'north' AND dest IS NULL").c_str()) == 1);
    // Exactly one 'generated' event.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'generated'") == 1);
}

// Architect enemy spawning (REQ-COMBAT-31, -35): the deterministic half, driven
// by a fake transport (cannedCreateRoom with an optional enemy blurb). The model
// SELECTS a costume from the engine's eligible enum; the engine mints the
// instance from the catalog. No network. The live half is testCombatLiveSmoke,
// gated behind TEXTWORLD_AI_LIVE_TEST=1.
static void testArchitectSpawn() {
    const int64_t player = 3;

    // Helper: count / read the lone hostile in a room.
    auto hostilesIn = [](Db& db, int64_t room) {
        return queryInt(db, ("SELECT COUNT(*) FROM hostile h JOIN location l "
                             "ON l.entity = h.entity WHERE l.container = " +
                             std::to_string(room)).c_str());
    };

    // --- (a) bootstrap spawn: the model selects the goblin blurb → the room is
    //     made AND a catalog-equal goblin instance is placed; the ledger ticks. ---
    {
        const TempDbFile worldPath("textworld_arch_spawn_ok.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        const std::string goblinBlurb = queryText(
            db, "SELECT blurb FROM bestiary WHERE archetype = 'goblin_grunt'");

        int calls = 0;
        std::string sentBody;
        HttpTransport fake = [&](const std::string& body) {
            ++calls;
            sentBody = body;
            return cannedCreateRoom("breached study", "A study with a broken door.",
                                    {}, goblinBlurb);
        };

        // Generate east off the cell (room 1, distance 0) → new room distance 1,
        // contested; bootstrap (ledger 0) offers only the goblin.
        db.begin();
        const bool ok = architectGenerate(db, 1, "east", player, fake);
        db.commit();
        CHECK(ok);
        CHECK(calls == 1);

        // The request offered an `enemy` enum — blurbs only — containing exactly
        // the one bootstrap choice.
        {
            const nlohmann::json j = nlohmann::json::parse(sentBody);
            const nlohmann::json& props =
                j["tools"][0]["input_schema"]["properties"];
            CHECK(props.contains("enemy"));
            CHECK(props["enemy"]["type"] == "string");
            const nlohmann::json& en = props["enemy"]["enum"];
            CHECK(en.is_array() && en.size() == 1);
            CHECK(std::find(en.begin(), en.end(), goblinBlurb) != en.end());
            // enemy is optional — not in the required set.
            CHECK(j["tools"][0]["input_schema"]["required"] ==
                  nlohmann::json::array({"name", "description"}));
        }

        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(newRoom > 15);  // engine-minted, beyond the seeded ids
        CHECK(hostilesIn(db, newRoom) == 1);

        // The placed instance is a catalog-equal goblin (the model wrote no
        // number — placeEnemy copies every stat from the bestiary).
        CHECK(queryInt(db,
                       ("SELECT (h.archetype = 'goblin_grunt' AND h.chip = b.chip "
                        "AND h.telegraph_period = b.telegraph_period "
                        "AND hp.max = b.health AND hp.current = b.health) "
                        "FROM hostile h JOIN bestiary b ON b.archetype = h.archetype "
                        "JOIN health hp ON hp.entity = h.entity "
                        "JOIN location l ON l.entity = h.entity "
                        "WHERE l.container = " + std::to_string(newRoom)).c_str()) == 1);
        // The bootstrap ledger ticked exactly once.
        CHECK(queryInt(db,
                       "SELECT value FROM meta WHERE key = 'architect_spawn_count'") == 1);
    }

    // --- (b) no enemy selected → room made, no hostile, ledger never created. ---
    {
        const TempDbFile worldPath("textworld_arch_spawn_none.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom("empty study", "A quiet, empty study.");
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();
        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(hostilesIn(db, newRoom) == 0);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM meta WHERE key = 'architect_spawn_count'") == 0);
    }

    // --- (c) ineligible / hallucinated blurb → no spawn, room still made. ---
    {
        const TempDbFile worldPath("textworld_arch_spawn_bad.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom("study", "A study.", {},
                                    "a dragon of pure invention");
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();
        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(hostilesIn(db, newRoom) == 0);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM meta WHERE key = 'architect_spawn_count'") == 0);
    }

    // --- (d) safe-edge target room → the schema offers NO enemy field at all. ---
    // Generate north off the outer hall (room 15, distance 3) → new room distance
    // 4, beyond the front radius: an empty menu, so no `enemy` property exists.
    {
        const TempDbFile worldPath("textworld_arch_spawn_edge.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        std::string sentBody;
        HttpTransport fake = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("far room", "A far, still room.");
        };
        db.begin();
        CHECK(architectGenerate(db, 15, "north", player, fake));
        db.commit();
        const nlohmann::json j = nlohmann::json::parse(sentBody);
        CHECK(!j["tools"][0]["input_schema"]["properties"].contains("enemy"));
    }

    // --- (e) generation FAILURE (transport error) → no room, no enemy, no ledger
    //     (REQ-COMBAT-35): the same silent-fallback boundary as room generation. ---
    {
        const TempDbFile worldPath("textworld_arch_spawn_fail.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        const int64_t hostiles0 = queryInt(db, "SELECT COUNT(*) FROM hostile");
        HttpTransport err = [&](const std::string&) {
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        db.begin();
        CHECK(!architectGenerate(db, 1, "east", player, err));
        db.commit();
        CHECK(queryInt(db, "SELECT COUNT(*) FROM hostile") == hostiles0);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east'") == 0);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM meta WHERE key = 'architect_spawn_count'") == 0);
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

// A canonical dump of everything a generated room writes to canon: the exit
// graph, the minted room's name and prose, and the event log's verbs. Ids are
// deliberately INCLUDED — two worlds built from the same fixture mint the same
// ids, so an identical string is a genuine row-for-row match rather than a
// coincidence of shape.
static std::string canonSnapshot(Db& db) {
    std::string out;
    out += "exits[" +
           queryText(db,
                     "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                     "SELECT room || ' ' || direction || ' -> ' || "
                     "IFNULL(CAST(dest AS TEXT), 'latent') AS s FROM exits "
                     "ORDER BY room, direction)") +
           "]";
    out += " names[" +
           queryText(db,
                     "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                     "SELECT entity || '=' || value AS s FROM name "
                     "ORDER BY entity)") +
           "]";
    out += " prose[" +
           queryText(db,
                     "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                     "SELECT entity || '=' || prose AS s FROM description "
                     "ORDER BY entity)") +
           "]";
    out += " events[" +
           queryText(db,
                     "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                     "SELECT verb || ':' || IFNULL(detail, '') AS s FROM events "
                     "ORDER BY id)") +
           "]";
    out += " rooms[" + std::to_string(queryInt(db, "SELECT COUNT(*) FROM room")) +
           "] entities[" +
           std::to_string(queryInt(db, "SELECT COUNT(*) FROM entities")) + "]";
    return out;
}

// --- Step 7, REQ-PREGEN-14/-15/-17/-18/-23: committing a candidate inside the
// tick. The headline claim is that a pre-generated room and a synchronously
// generated one are INDISTINGUISHABLE in canon — asserted by building both from
// the same proposal and comparing the two worlds row for row, not by eye. ---
static void testPregenCommit() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    const int64_t player = 3;

    // The one proposal both paths will produce, in both of its forms.
    const std::string roomName = "crypt";
    const std::string roomProse = "A cold undercroft of grey stone.";
    const std::vector<std::string> roomExits = {"north", "down"};

    RoomProposal candidate;
    candidate.name = roomName;
    candidate.description = roomProse;
    candidate.exits = roomExits;

    // --- (a) HIT: canon equality with the synchronous path, and zero network.
    std::string syncCanon;
    {
        pregenResetForTest();
        const TempDbFile worldPath("textworld_pregen_sync.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom(roomName, roomProse, roomExits);
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        syncCanon = canonSnapshot(db);
    }
    {
        pregenResetForTest();
        const TempDbFile worldPath("textworld_pregen_hit.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        pregenInjectReadyForTest(1, "east", candidate, /*snapshotTurn=*/1);

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("wrong", "should never be built");
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);

        CHECK(calls == 0);  // REQ-PREGEN-14: no network on the commit turn
        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(newRoom > 5);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == newRoom);
        // Row for row, the same world the synchronous path produced.
        CHECK(canonSnapshot(db) == syncCanon);
    }

    // --- (b) MISS with an empty store: the synchronous path, untouched. This
    // duplicates nothing — testResolveGoGenerate proves it at length and passes
    // UNEDITED, which IS the REQ-PREGEN-15 regression proof. What is added here
    // is the explicit statement that an empty store leaves the wall intact on a
    // declining transport.
    {
        pregenResetForTest();
        const TempDbFile worldPath("textworld_pregen_miss.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        int calls = 0;
        HttpTransport err = [&](const std::string&) {
            ++calls;
            HttpResponse r;
            r.transportError = true;
            return r;
        };
        tickT(db, Action{Verb::Go, 0, "east"}, err, player);
        CHECK(calls == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
              "You can't go that way.");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 "
                           "AND direction = 'east' AND dest IS NULL") == 1);
    }

    // --- (c) RAN_QUEUED / WAITED reach the same commit. A queued job run on the
    // main thread commits exactly as a hit does — the proposal's provenance is
    // invisible to Phase 2.
    {
        pregenResetForTest();
        const TempDbFile worldPath("textworld_pregen_ranqueued.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        PregenJob job;
        job.room = 1;
        job.direction = "east";
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 1;
        pregenSubmit(job);

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom(roomName, roomProse, roomExits);
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        CHECK(calls == 1);  // the queued job, run here — not a second call
        CHECK(canonSnapshot(db) == syncCanon);
    }

    // --- (d) REQ-PREGEN-14 / -18, the enemy half, BOTH ways. Omitting it would
    // silently stop spawning on the pregen path, which no room-shape assertion
    // would ever catch.
    {
        auto hostilesIn = [](Db& db, int64_t room) {
            return queryInt(db, ("SELECT COUNT(*) FROM hostile h JOIN location l "
                                 "ON l.entity = h.entity WHERE l.container = " +
                                 std::to_string(room)).c_str());
        };

        // (d1) an ELIGIBLE blurb on a hit places the enemy and ticks the ledger.
        {
            pregenResetForTest();
            const TempDbFile worldPath("textworld_pregen_enemy_ok.db");
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
            db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
            const std::string goblinBlurb = queryText(
                db, "SELECT blurb FROM bestiary WHERE archetype = 'goblin_grunt'");

            RoomProposal armed = candidate;
            armed.enemyBlurb = goblinBlurb;
            pregenInjectReadyForTest(1, "east", armed, /*snapshotTurn=*/1);

            int calls = 0;
            HttpTransport fake = [&](const std::string&) {
                ++calls;
                return cannedCreateRoom("wrong", "never built");
            };
            tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
            CHECK(calls == 0);

            const int64_t newRoom = queryInt(
                db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
            CHECK(hostilesIn(db, newRoom) == 1);
            CHECK(queryInt(db, "SELECT value FROM meta WHERE key = "
                               "'architect_spawn_count'") == 1);
        }

        // (d2) a STALE / ineligible blurb places nothing and STILL makes the
        // room — the live re-check declining is not a failure (REQ-PREGEN-18).
        {
            pregenResetForTest();
            const TempDbFile worldPath("textworld_pregen_enemy_stale.db");
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
            db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

            RoomProposal stale = candidate;
            stale.enemyBlurb = "a dragon of pure invention";
            pregenInjectReadyForTest(1, "east", stale, /*snapshotTurn=*/1);

            tickT(db, Action{Verb::Go, 0, "east"},
                  [](const std::string&) {
                      return cannedCreateRoom("wrong", "never built");
                  },
                  player);

            const int64_t newRoom = queryInt(
                db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
            CHECK(newRoom > 15);  // the room IS made
            CHECK(hostilesIn(db, newRoom) == 0);
            CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = "
                               "'architect_spawn_count'") == 0);
            CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") ==
                  newRoom);
        }
    }

    pregenResetForTest();
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// --- Step 7, REQ-PREGEN-23: exactly ONE outcome record per latent-exit walk,
// each optional key on its own outcome and nowhere else, and the `generate`
// stage accompanying a MISS alone. That last clause is the one worth a test:
// reusing `generate` for a ran_queued path would give one stage name two
// different spans and quietly skew every miss-versus-ran_queued comparison. ---
static void testPregenOutcomeRecords() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
    const ScopedEnvVar profGuard("TEXTWORLD_PROFILE");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    const int64_t player = 3;

    std::vector<std::string> captured;
    profileSetSink(
        [&captured](const std::string& line) { captured.push_back(line); });
    setenv("TEXTWORLD_PROFILE", "1", 1);
    profileRefreshEnabled();

    // Every kind=pregen record in the capture, parsed.
    auto pregenRecords = [&captured]() {
        std::vector<std::map<std::string, std::string>> out;
        for (const std::string& line : captured) {
            auto kv = parseProfileRecord(line);
            if (kv.at("kind") == "pregen") out.push_back(std::move(kv));
        }
        return out;
    };
    // True iff a stage=generate record was emitted.
    auto sawGenerateStage = [&captured]() {
        for (const std::string& line : captured) {
            const auto kv = parseProfileRecord(line);
            if (kv.at("kind") == "stage" && kv.at("stage") == "generate") return true;
        }
        return false;
    };

    // (a) HIT — age_turns only, and NO generate stage (no synchronous call ran).
    {
        pregenResetForTest();
        captured.clear();
        const TempDbFile worldPath("textworld_pregen_rec_hit.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        // meta.turn is 0 in the fixture and tickT increments it to 1, so a
        // candidate snapshotted at turn 0 is one turn old at commit.
        RoomProposal proposal;
        proposal.name = "crypt";
        proposal.description = "A cold undercroft.";
        pregenInjectReadyForTest(1, "east", proposal, /*snapshotTurn=*/0);

        tickT(db, Action{Verb::Go, 0, "east"},
              [](const std::string&) { return cannedCreateRoom("x", "y"); }, player);

        const auto records = pregenRecords();
        CHECK(records.size() == 1);
        if (records.size() == 1) {
            const auto& r = records[0];
            CHECK(r.at("outcome") == "hit");
            CHECK(r.at("age_turns") == "1");
            CHECK(r.count("wait_ms") == 0);
            CHECK(r.count("run_ms") == 0);
        }
        CHECK(!sawGenerateStage());
    }

    // (b) MISS — no optional key at all, and the `generate` stage DOES appear:
    // the synchronous architectGenerate really ran (D5).
    {
        pregenResetForTest();
        captured.clear();
        const TempDbFile worldPath("textworld_pregen_rec_miss.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        tickT(db, Action{Verb::Go, 0, "east"},
              [](const std::string&) {
                  return cannedCreateRoom("crypt", "A cold undercroft.");
              },
              player);

        const auto records = pregenRecords();
        CHECK(records.size() == 1);
        if (records.size() == 1) {
            const auto& r = records[0];
            CHECK(r.at("outcome") == "miss");
            CHECK(r.count("age_turns") == 0);
            CHECK(r.count("wait_ms") == 0);
            CHECK(r.count("run_ms") == 0);
        }
        CHECK(sawGenerateStage());
    }

    // (c) RAN_QUEUED — run_ms only, and NO generate stage: the cost rides on
    // this record instead, because a queued run covers Phase 1 alone.
    {
        pregenResetForTest();
        captured.clear();
        const TempDbFile worldPath("textworld_pregen_rec_ranq.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        PregenJob job;
        job.room = 1;
        job.direction = "east";
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 0;
        pregenSubmit(job);

        tickT(db, Action{Verb::Go, 0, "east"},
              [](const std::string&) {
                  return cannedCreateRoom("crypt", "A cold undercroft.");
              },
              player);

        const auto records = pregenRecords();
        CHECK(records.size() == 1);
        if (records.size() == 1) {
            const auto& r = records[0];
            CHECK(r.at("outcome") == "ran_queued");
            CHECK(r.count("run_ms") == 1);
            CHECK(r.count("age_turns") == 0);
            CHECK(r.count("wait_ms") == 0);
        }
        CHECK(!sawGenerateStage());
    }

    // (d) WAITED — wait_ms only. The worker is pinned inside job A's transport
    // and the tick walks that very exit, so the tick waits it out.
    {
        pregenResetForTest();
        captured.clear();
        const TempDbFile worldPath("textworld_pregen_rec_wait.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        BlockingTransport blocking;
        pregenSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        pregenStart();
        PregenJob job;
        job.room = 1;
        job.direction = "east";
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 0;
        pregenSubmit(job);
        spinUntil([] { return pregenStateOf(1, "east") == PregenState::Running; });

        // The tick blocks, so it runs on its own thread and this one releases
        // the worker — the same shape as the real thing.
        std::thread releaser([&] {
            // Wait until the tick is provably inside the Running wait, so the
            // outcome is deterministically `waited` and never a lucky `hit`.
            spinUntil([] { return pregenWaitingCountForTest() == 1; });
            blocking.release();
        });
        tickT(db, Action{Verb::Go, 0, "east"},
              [](const std::string&) {
                  return cannedCreateRoom("wrong", "never built");
              },
              player);
        releaser.join();

        const auto records = pregenRecords();
        CHECK(records.size() == 1);
        if (records.size() == 1) {
            const auto& r = records[0];
            CHECK(r.at("outcome") == "waited");
            CHECK(r.count("wait_ms") == 1);
            CHECK(r.count("age_turns") == 0);
            CHECK(r.count("run_ms") == 0);
        }
        // The worker's result was committed, not a second call's.
        CHECK(blocking.callCount() == 1);
        CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                             std::to_string(queryInt(
                                 db, "SELECT dest FROM exits WHERE room = 1 "
                                     "AND direction = 'east'"))).c_str()) == "crypt");
        pregenResetForTest();
    }

    profileSetSink({});
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
}

// --- the nested `generate` stage (REQ-LAT-2) --------------------------------
// Defined here, after cannedCreateRoom and tickT, and reusing the latent-exit
// fixture testResolveGoGenerate walks. The stage is instrumented on the
// INJECTED architectGenerate, so a fake transport proves it — no network.
static void testProfileGenerateStage() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar profGuard("TEXTWORLD_PROFILE");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    setenv("TEXTWORLD_PROFILE", "1", 1);
    profileRefreshEnabled();

    std::vector<std::string> captured;
    profileSetSink(
        [&captured](const std::string& line) { captured.push_back(line); });

    const int64_t player = 3;

    // --- walking a latent exit emits exactly one generate stage, nested in
    // tick, and no network happened (the transport is canned). ---
    {
        const TempDbFile worldPath("textworld_profile_generate.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);

        CHECK(capturedStages(captured) == std::vector<std::string>({"generate"}));
        const auto kv = parseProfileRecord(captured.front());
        CHECK(kv.at("nested_in") == "tick");
        CHECK(!capturedAnyKind(captured, "call"));  // the fake makes no call
        // The exit really was realized — this is a generating turn.
        CHECK(queryInt(db,
                       "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'") > 5);
    }

    // --- a non-movement turn (and a realized-exit move) generates nothing. ---
    {
        captured.clear();
        const TempDbFile worldPath("textworld_profile_nogenerate.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Look, 0, ""}, fake, player);
        tickT(db, Action{Verb::Go, 0, "north"}, fake, player);  // realized exit
        CHECK(capturedStages(captured).empty());
    }

    // --- a gate-failing generation (the wall path) STILL emits one generate
    // stage: it consumed wall-clock. The wall text is unchanged. ---
    {
        captured.clear();
        const TempDbFile worldPath("textworld_profile_generate_fail.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        HttpTransport err = [&](const std::string&) {
            HttpResponse r;
            r.transportError = true;  // Phase-1 failure → wall
            return r;
        };
        tickT(db, Action{Verb::Go, 0, "east"}, err, player);

        CHECK(capturedStages(captured) == std::vector<std::string>({"generate"}));
        CHECK(parseProfileRecord(captured.front()).at("nested_in") == "tick");
        CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
              "You can't go that way.");
        // Latent row untouched — still retryable.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east' "
                       "AND dest IS NULL") == 1);
    }

    profileSetSink({});
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
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

// --- terminal services (src/term.cpp) --------------------------------------

// Is every byte sequence in `s` valid UTF-8? Used to prove the wrapper never
// splits a multi-byte code point (check 13).
static bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;  // a lone continuation byte or an invalid lead
        }
        if (i + extra >= s.size()) return false;  // truncated sequence
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

static bool hasEscapeByte(const std::string& s) {
    return s.find('\x1b') != std::string::npos;
}

// REQ-UI-19/-20/-21/-22/-23: the color gate's full truth table, plus the
// guarantee that a suppressed run emits no escape bytes at all.
static void testTermColorGate() {
    const char* noColorVals[] = {nullptr, "", "1"};
    const char* forceVals[] = {nullptr, "0", "1"};
    const char* clicolorVals[] = {nullptr, "0", "1"};
    const char* termVals[] = {nullptr, "dumb", "xterm"};

    // Check 4: the full cross-product, against REQ-UI-20's table restated in
    // reading order. `set` mirrors the requirement's "empty means unset".
    auto set = [](const char* v) { return v != nullptr && v[0] != '\0'; };
    for (const char* nc : noColorVals) {
        for (const char* cf : forceVals) {
            for (const char* cc : clicolorVals) {
                for (const char* tm : termVals) {
                    for (const bool tty : {false, true}) {
                        const TermStyle got = styleFor(nc, cf, cc, tm, tty);

                        const bool forced = set(cf) && std::string(cf) != "0";
                        bool wantColor = false;
                        // Attributes are a capability test, not a preference:
                        // nothing styled goes to a pipe (check 7).
                        bool wantAttrs = tty || forced;
                        if (set(tm) && std::string(tm) == "dumb") {
                            wantColor = false;
                            wantAttrs = false;  // REQ-UI-21: bold dies too
                        } else if (set(nc)) {
                            wantColor = false;  // REQ-UI-23: bold survives
                        } else if (forced) {
                            wantColor = true;
                        } else if (set(cc) && std::string(cc) == "0") {
                            wantColor = false;
                        } else {
                            wantColor = tty;
                        }
                        CHECK(got.color == wantColor);
                        CHECK(got.attrs == wantAttrs);

                        // Check 5: whenever color is suppressed, the colorize
                        // helpers emit the plain bytes — no empty sequence,
                        // no bare reset (REQ-UI-22).
                        if (!got.color) {
                            CHECK(!hasEscapeByte(colorize("exits", Color::Cyan, got)));
                            CHECK(colorize("exits", Color::Cyan, got) == "exits");
                        }
                        if (!got.attrs) {
                            CHECK(!hasEscapeByte(bolden("x", got)));
                            CHECK(!hasEscapeByte(
                                boldColor("[WINDING UP]", Color::BrightRed, got)));
                        }
                    }
                }
            }
        }
    }

    // Check 6: bold survives NO_COLOR — the telegraph stays visually distinct
    // on a colorless terminal (REQ-UI-23).
    {
        const TermStyle s = styleFor("1", nullptr, nullptr, "xterm", /*isTty=*/true);
        CHECK(!s.color);
        CHECK(s.attrs);
        // ...but NOT into a pipe: NO_COLOR does not resurrect bold there.
        CHECK(!styleFor("1", nullptr, nullptr, "xterm", /*isTty=*/false).attrs);
        const std::string tg = boldColor("[WINDING UP]", Color::BrightRed, s);
        CHECK(contains(tg, "\x1b[1m"));
        CHECK(!contains(tg, "91"));
        CHECK(contains(tg, "[WINDING UP]"));
    }

    // Check 6a: TERM=dumb strips EVERYTHING, bold included — and the telegraph
    // remains identifiable from its text alone (REQ-UI-21).
    {
        const TermStyle s = styleFor(nullptr, "1", "1", "dumb", true);
        CHECK(!s.color);
        CHECK(!s.attrs);
        const std::string tg = boldColor("[WINDING UP]", Color::BrightRed, s);
        CHECK(!hasEscapeByte(tg));
        CHECK(tg == "[WINDING UP]");
    }

    // Basic 16 only (REQ-UI-19): no 256-color or truecolor introducer anywhere.
    {
        const TermStyle on = styleFor(nullptr, "1", nullptr, "xterm", false);
        CHECK(colorize("x", Color::Cyan, on) == "\x1b[36mx\x1b[0m");
        CHECK(colorize("x", Color::BrightBlue, on) == "\x1b[94mx\x1b[0m");
        CHECK(boldColor("x", Color::BrightRed, on) == "\x1b[1;91mx\x1b[0m");
        const std::string src = readFileBytes("src/term.cpp");
        CHECK(!contains(src, "38;5;"));
        CHECK(!contains(src, "38;2;"));
    }

    // Color::None emits nothing even with color fully enabled.
    {
        const TermStyle on = styleFor(nullptr, "1", nullptr, "xterm", false);
        CHECK(colorize("plain", Color::None, on) == "plain");
    }

    // stripSgr is the inverse the later band tests lean on.
    {
        const TermStyle on = styleFor(nullptr, "1", nullptr, "xterm", false);
        CHECK(stripSgr(colorize("exits", Color::Cyan, on)) == "exits");
        CHECK(stripSgr(boldColor("hall", Color::BrightRed, on)) == "hall");
    }

    // currentStyle() tracks the environment across a refresh, the way
    // profileRefreshEnabled() does (REQ-UI-20 applied to the live process).
    {
        const ScopedEnvVar noColorGuard("NO_COLOR");
        const ScopedEnvVar forceGuard("CLICOLOR_FORCE");
        const ScopedEnvVar termGuard("TERM");
        setenv("TERM", "xterm", 1);
        unsetenv("NO_COLOR");
        setenv("CLICOLOR_FORCE", "1", 1);
        termRefreshStyle();
        CHECK(currentStyle().color);
        setenv("NO_COLOR", "1", 1);
        termRefreshStyle();
        CHECK(!currentStyle().color);
        CHECK(currentStyle().attrs);
        setenv("TERM", "dumb", 1);
        termRefreshStyle();
        CHECK(!currentStyle().attrs);
    }
    // Leave the cached style as the suite found it.
    termRefreshStyle();
}

// REQ-UI-26/-27/-28: the width fallback chain and the clamp floor.
static void testTermWidth() {
    // Check 9: the failing-ioctl case (stdout piped under CI) still yields a
    // usable non-zero width — no zero-width or divide-by-zero behavior.
    CHECK(detectWidth() >= kMinWidth);

    // Check 10: fallback order, exercised through the pure predicate so the
    // test does not depend on whether the harness' stdout happens to be a tty.
    CHECK(widthFrom(false, 0, "52") == 52);
    CHECK(widthFrom(false, 0, nullptr) == 80);
    CHECK(widthFrom(false, 0, "") == 80);
    CHECK(widthFrom(false, 0, "banana") == 80);
    CHECK(widthFrom(false, 0, "80x24") == 80);
    CHECK(widthFrom(false, 0, "0") == 80);
    CHECK(widthFrom(false, 0, "-5") == 80);

    // REQ-UI-27: a zero ws_col is a FAILED ioctl, not a zero-width terminal —
    // it must fall through to COLUMNS, which is exactly the piped/CI shape.
    CHECK(widthFrom(true, 0, "52") == 52);
    CHECK(widthFrom(true, 0, nullptr) == 80);

    // A successful ioctl wins over COLUMNS.
    CHECK(widthFrom(true, 100, "52") == 100);

    // Check 11a: the clamp floor. 10, 1, and 0 all yield 20 (REQ-UI-27).
    CHECK(clampWidth(10) == kMinWidth);
    CHECK(clampWidth(1) == kMinWidth);
    CHECK(clampWidth(0) == kMinWidth);
    CHECK(clampWidth(-5) == kMinWidth);
    CHECK(clampWidth(20) == 20);
    CHECK(clampWidth(200) == 200);
    // The clamp applies through the whole chain, not just at the ioctl.
    CHECK(widthFrom(true, 3, nullptr) == kMinWidth);
    CHECK(widthFrom(false, 0, "7") == kMinWidth);

    // REQ-UI-28: queried fresh each call, and no SIGWINCH handler is installed.
    {
        const std::string src = readFileBytes("src/term.cpp");
        CHECK(!contains(src, "SIGWINCH"));
        CHECK(contains(src, "TIOCGWINSZ"));
    }
}

// REQ-UI-30/-31/-32: word-boundary wrapping that counts code points and
// preserves paragraph structure.
static void testTermWrap() {
    // utf8Length counts code points, not bytes (REQ-UI-31).
    CHECK(utf8Length("abc") == 3);
    CHECK(utf8Length("a—b") == 3);          // em-dash
    CHECK(utf8Length("“quoted”") == 8);  // curly quotes

    // Check 13: a paragraph of multi-byte punctuation wraps NEAR the width,
    // never early, and no multi-byte sequence is split.
    {
        const std::string para =
            "The hall breathes cold — a slow, patient cold — and the "
            "“lantern” gutters against it, throwing shapes that will "
            "not hold still on the worn flagstones underfoot.";
        const int width = 40;
        const std::string wrapped = wrapProse(para, width);

        std::vector<std::string> lines;
        {
            std::string cur;
            for (const char c : wrapped) {
                if (c == '\n') {
                    lines.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            lines.push_back(cur);
        }
        CHECK(lines.size() > 1);
        for (const std::string& l : lines) {
            CHECK(utf8Length(l) <= static_cast<size_t>(width));
            CHECK(isValidUtf8(l));  // no split code point
        }
        // Not wrapped EARLY: every line but the last is full enough that the
        // next line's first word would not have fit. This is the property a
        // byte-counting wrapper violates on multi-byte input.
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            const std::string& next = lines[i + 1];
            const size_t sp = next.find(' ');
            const std::string firstWord =
                sp == std::string::npos ? next : next.substr(0, sp);
            if (firstWord.empty()) continue;
            CHECK(utf8Length(lines[i]) + 1 + utf8Length(firstWord) >
                  static_cast<size_t>(width));
        }

        // Check 14: no word is broken. The word sequence survives the wrap.
        auto words = [](const std::string& s) {
            std::vector<std::string> w;
            std::string cur;
            for (const char c : s) {
                if (c == ' ' || c == '\n') {
                    if (!cur.empty()) w.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) w.push_back(cur);
            return w;
        };
        CHECK(words(para) == words(wrapped));
    }

    // Check 15: two paragraphs in, two paragraphs out — the blank line between
    // them survives and no two source lines are joined (REQ-UI-32).
    {
        const std::string two =
            "First paragraph, long enough that it must wrap at least once at "
            "this width.\n\nSecond paragraph, likewise long enough to wrap.\n";
        const std::string wrapped = wrapProse(two, 30);
        CHECK(contains(wrapped, "\n\n"));
        CHECK(wrapped.back() == '\n');  // the trailing newline survives
        // The paragraph boundary is still where it was: nothing from the second
        // paragraph reached the first.
        const size_t brk = wrapped.find("\n\n");
        CHECK(!contains(wrapped.substr(0, brk), "Second"));
        CHECK(!contains(wrapped.substr(brk), "First"));
    }

    // A word longer than the width overflows onto its own line rather than
    // being split mid-word (REQ-UI-30; the single case REQ-UI-27 accepts).
    {
        const std::string longWord(40, 'x');
        const std::string wrapped = wrapProse("tiny " + longWord + " tail", 20);
        CHECK(contains(wrapped, longWord));  // intact, not split
        CHECK(contains(wrapped, "\n" + longWord + "\n"));
    }

    // Degenerate widths do not hang or corrupt.
    CHECK(wrapProse("", 40).empty());
    CHECK(wrapProse("one two", 0) == "one two");
}

// --- the status band (src/band.cpp) ----------------------------------------

// Color fully suppressed / fully forced. The band tests drive these explicitly
// rather than the process style, so they neither depend on nor disturb the
// developer's terminal.
static const TermStyle kBandPlain{false, false};
static const TermStyle kBandColor{true, true};

static std::vector<std::string> splitOnNewline(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// The band's frame: layout, hanging indent, ASCII-only rules, conditional rows.
// Pure — no database touches this test (REQ-UI-8, -9, -29, -33, -33a, -33b).
static void testBandLayout() {
    // A synthetic row set with a long exit list and many objects — the shape
    // check 11 names. Every word fits the 10-column content field at width 20,
    // so nothing here relies on the over-long-word overflow case.
    auto listRow = [](const char* label, const std::vector<std::string>& items) {
        BandRow row{label, {}};
        for (size_t i = 0; i < items.size(); ++i) {
            row.spans.push_back({items[i], Color::None, false,
                                 i + 1 == items.size() ? "" : ", "});
        }
        return row;
    };
    const std::vector<BandRow> rows = {
        listRow("Exits", {"north", "south", "east", "west", "up", "down"}),
        listRow("Objects",
                {"lantern", "key", "rope", "flask", "chalk", "coin", "map"}),
        BandRow{"Enemy",
                {{"goblin", Color::None, false, "  "},
                 {"HP: 6/9", Color::None, false, "  "},
                 {"dot 2", Color::None, false, ""}}},
        BandRow{"You", {{"HP: 11/12", Color::None, false, ""}}},
    };

    for (const int width : {20, 40, 80, 200}) {
        const std::string band = layoutBand("stone hall", rows, width, kBandPlain);
        const std::vector<std::string> lines = splitOnNewline(band);
        CHECK(!lines.empty());
        for (const std::string& line : lines) {
            // Check 11: no emitted line exceeds the width, at any width.
            CHECK(utf8Length(line) <= static_cast<size_t>(width));
            // Check 12: every framing byte is ASCII (REQ-UI-29) — no box-drawing
            // and no middle dot, both East Asian Ambiguous.
            for (const char c : line) CHECK(static_cast<unsigned char>(c) < 0x80);
        }

        // Check 11c: continuation lines are indented to the row's content
        // column (11, 1-based), not to column 0.
        for (const std::string& line : lines) {
            if (line.rfind("          ", 0) == 0) {  // 10 leading spaces
                CHECK(line.size() > static_cast<size_t>(kBandIndent));
                CHECK(line[kBandIndent] != ' ');
            }
        }

        // Check 11b in frame form: the same facts survive every width. Nothing
        // is truncated or elided (REQ-UI-33a).
        const std::string flat = band;
        for (const char* fact : {"north", "down", "lantern", "map", "HP: 6/9",
                                 "HP: 11/12", "dot 2"}) {
            CHECK(contains(flat, fact));
        }
    }

    // At width 40 the object list must actually wrap, or the indent assertions
    // above are vacuous.
    {
        const std::string band = layoutBand("stone hall", rows, 40, kBandPlain);
        bool sawContinuation = false;
        for (const std::string& line : splitOnNewline(band)) {
            if (line.rfind("          ", 0) == 0) sawContinuation = true;
        }
        CHECK(sawContinuation);
    }

    // Header shape: "-- <name> " then dashes to the width, all ASCII.
    {
        const std::string band = layoutBand("stone hall", rows, 60, kBandPlain);
        const std::vector<std::string> lines = splitOnNewline(band);
        CHECK(lines[0] == "-- stone hall ----------------------------------------------");
        CHECK(lines[0].size() == 60);
    }

    // Check 24 (frame half): an empty room name renders the rule with no title
    // and does not throw (REQ-UI-9).
    {
        const std::string band = layoutBand("", rows, 40, kBandPlain);
        const std::vector<std::string> lines = splitOnNewline(band);
        CHECK(lines[0] == std::string(40, '-'));
    }

    // Check 17 (frame half): a row with no content emits nothing at all — the
    // band's height varies with what is present (REQ-UI-8).
    {
        const std::vector<BandRow> sparse = {
            BandRow{"Exits", {}},
            BandRow{"Objects", {{"", Color::None, false, ""}}},
            BandRow{"You", {{"HP: 1/1", Color::None, false, ""}}},
        };
        const std::string band = layoutBand("cell", sparse, 40, kBandPlain);
        CHECK(!contains(band, "Exits"));
        CHECK(!contains(band, "Objects"));
        CHECK(contains(band, "You      HP: 1/1"));
        CHECK(splitOnNewline(band).size() == 2);  // header + one row
    }

    // REQ-UI-33b: when the name leaves no room for even one dash, the RULES are
    // dropped — never the name. Every byte of the name survives.
    {
        const std::string longName = "the impossibly long vaulted hall of echoes";
        const std::string band = layoutBand(longName, {}, 20, kBandPlain);
        for (const std::string& line : splitOnNewline(band)) {
            CHECK(utf8Length(line) <= 20);
        }
        std::string joined;
        for (const std::string& line : splitOnNewline(band)) joined += line + " ";
        for (const char* word : {"impossibly", "vaulted", "echoes"}) {
            CHECK(contains(joined, word));
        }
        CHECK(!contains(band, "--"));  // the rule is what gave way
    }

    // The clamp floor applies inside layout too, so a width below 20 cannot
    // produce a zero-or-negative content field (REQ-UI-27).
    {
        const std::string band = layoutBand("cell", rows, 5, kBandPlain);
        CHECK(!band.empty());
        for (const std::string& line : splitOnNewline(band)) {
            CHECK(utf8Length(line) <= static_cast<size_t>(kMinWidth));
        }
    }
}

// The band's content: rooms, exits, objects, hostiles, the player row, and
// combat legibility — all with color disabled, so a layout bug can never be
// confused with a color bug (REQ-UI-9..-18, -34, -35, -36, -40).
static void testBandContent() {
    // --- steps 5: room name, exits, objects (fixture.sql) ---
    {
        const TempDbFile worldPath("textworld_band_content_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");

        const std::string band = composeBand(db, 60, kBandPlain);
        CHECK(contains(band, "-- stone hall "));
        CHECK(contains(band, " Exits    north"));
        CHECK(contains(band, " Objects  lantern"));

        // REQ-UI-14: the band repeats the room's NAME, never its description.
        CHECK(!contains(band, "vaulted hall of grey stone"));
        CHECK(!contains(band, "flagstones"));

        // The Exits and Objects lists match the room block's exactly.
        const std::string block = renderRoomOf(db, 3);
        CHECK(contains(block, "Exits: north."));
        CHECK(contains(block, "You see: lantern."));

        // Check 17: moving to a room with neither drops BOTH rows entirely.
        db.exec("DELETE FROM location WHERE entity = 4");   // the lantern
        db.exec("DELETE FROM exits WHERE room = 1");
        {
            const std::string bare = composeBand(db, 60, kBandPlain);
            CHECK(!contains(bare, "Exits"));
            CHECK(!contains(bare, "Objects"));
            CHECK(contains(bare, "-- stone hall "));
        }

        // Check 24: a room whose name row is deleted renders without throwing —
        // the rule appears with no title (REQ-UI-9).
        db.exec("DELETE FROM name WHERE entity = 1");
        {
            const std::string nameless = composeBand(db, 40, kBandPlain);
            CHECK(splitOnNewline(nameless)[0] == std::string(40, '-'));
        }
    }

    // --- check 18: latent exits follow the room block's visibility gate ---
    {
        const TempDbFile worldPath("textworld_band_latent_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");

        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");

        // Architect ENABLED: the latent exit lists, and is textually
        // INDISTINGUISHABLE from the realized one — no marker (REQ-UI-10).
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        {
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(contains(band, "north, up"));
        }

        // Architect DISABLED: the latent exit is absent.
        unsetenv("ANTHROPIC_API_KEY");
        setenv("TEXTWORLD_AI", "0", 1);
        {
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(contains(band, " Exits    north"));
            CHECK(!contains(band, "up"));
        }
    }

    // --- steps 6, 7, 8: hostiles, the player row, combat legibility ---
    {
        const TempDbFile worldPath("textworld_band_combat_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

        // Check 19 / REQ-UI-15, -16: out of combat (the cell) the player row is
        // COMPACT — HP is present, spell readiness is not.
        {
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(contains(band, " You      HP: 12/12"));
            CHECK(!contains(band, "Ward:"));
            CHECK(!contains(band, "Stun:"));
            CHECK(!contains(band, "Enemy"));
        }

        // Move into the corridor: one living hostile, so one Enemy row with its
        // current and maximum HP (check 12 / REQ-UI-12).
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        {
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(contains(band, " Enemy    goblin grunt  HP: 8/8"));
            // Check 19 / REQ-UI-17: in combat, HP *and* readiness.
            CHECK(contains(band, " You      HP: 12/12"));
            CHECK(contains(band, "Stun: ready"));
            CHECK(contains(band, "Ward: ready"));

            // Check 20 — THE ORACLE. Every readiness value in the band equals
            // what combatStatusLine() reports for the same state. This is the
            // payoff of keeping that helper alive (REQ-UI-6a): it catches a
            // readiness reimplementation drifting from the original.
            const std::string oracle = combatStatusLine(db, 3);
            CHECK(!oracle.empty());
            size_t pos = 0;
            int pairs = 0;
            while ((pos = oracle.find(" · ", pos)) != std::string::npos) {
                pos += std::string(" · ").size();
                const size_t end = oracle.find(" · ", pos);
                const std::string pair = oracle.substr(
                    pos, end == std::string::npos ? std::string::npos : end - pos);
                const std::string trimmed =
                    pair.empty() || pair.back() != '\n' ? pair
                                                        : pair.substr(0, pair.size() - 1);
                CHECK(contains(band, trimmed));  // e.g. "Ward: ready"
                ++pairs;
            }
            CHECK(pairs == 2);
        }

        // A cooling spell shows the integer turns remaining, not "ready".
        {
            const int64_t now =
                queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
            const std::string sql =
                "INSERT INTO cooldowns(entity, spell, ready_turn) VALUES "
                "(3, 'ward', " + std::to_string(now + 3) + ")";
            db.exec(sql.c_str());
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(contains(band, "Ward: 3"));
            CHECK(contains(band, "Stun: ready"));
            // The oracle again, on the cooling side of the branch.
            CHECK(contains(combatStatusLine(db, 3), "Ward: 3"));
            db.exec("DELETE FROM cooldowns WHERE entity = 3");
        }

        // Check 25 / REQ-UI-34: the telegraph marker appears only when a
        // pending_strike row exists.
        {
            CHECK(!contains(composeBand(db, 60, kBandPlain), "[WINDING UP]"));
            db.exec("INSERT INTO pending_strike(entity, damage, element) "
                    "VALUES (7, 5, NULL)");
            CHECK(contains(composeBand(db, 60, kBandPlain), "[WINDING UP]"));
            db.exec("DELETE FROM pending_strike WHERE entity = 7");
        }

        // Check 26c / REQ-UI-35: kind plus REMAINING turns, never magnitude.
        // The magnitudes here (9, 6) appear nowhere else in the row, so their
        // digits leaking in would be unambiguous.
        {
            db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
                    "VALUES (7, 'dot', 9, 2), (7, 'slow', 6, 1)");
            const std::string band = composeBand(db, 200, kBandPlain);
            std::string enemyLine;
            for (const std::string& line : splitOnNewline(band)) {
                if (line.rfind(" Enemy", 0) == 0) enemyLine = line;
            }
            CHECK(contains(enemyLine, "dot 2"));
            CHECK(contains(enemyLine, "slow 1"));
            CHECK(!contains(enemyLine, "9"));  // magnitude, deliberately absent
            CHECK(!contains(enemyLine, "6"));
            db.exec("DELETE FROM status_effects WHERE entity = 7");
        }

        // REQ-UI-36: the player's own states, notably a held ward.
        {
            db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
                    "VALUES (3, 'ward', 0, 1)");
            const std::string band = composeBand(db, 80, kBandPlain);
            std::string youLine;
            for (const std::string& line : splitOnNewline(band)) {
                if (line.rfind(" You", 0) == 0) youLine = line;
            }
            CHECK(contains(youLine, "ward 1"));
            db.exec("DELETE FROM status_effects WHERE entity = 3");
        }

        // A hostile at 0 HP produces no row (REQ-UI-12: LIVING hostiles only).
        {
            db.exec("UPDATE health SET current = 0 WHERE entity = 7");
            const std::string band = composeBand(db, 60, kBandPlain);
            CHECK(!contains(band, "Enemy"));
            db.exec("UPDATE health SET current = 8 WHERE entity = 7");
        }

        // Check 23 / REQ-UI-13: three hostiles, three rows, in the entity order
        // resolveCombat iterates.
        {
            db.exec("UPDATE location SET container = 11 WHERE entity = 3");
            const std::string band = composeBand(db, 80, kBandPlain);
            int enemyRows = 0;
            for (const std::string& line : splitOnNewline(band)) {
                if (line.rfind(" Enemy", 0) == 0) ++enemyRows;
            }
            CHECK(enemyRows == 3);
            // Same order as combat's swarm query, by construction.
            std::vector<int64_t> combatOrder;
            {
                Stmt s = db.prepare(
                    "SELECT h.entity FROM hostile h "
                    "JOIN location l ON l.entity = h.entity "
                    "WHERE l.container = 11 ORDER BY h.entity");
                while (s.step()) combatOrder.push_back(s.colInt(0));
            }
            CHECK(combatOrder.size() == 3);
            db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        }

        // Check 11b / REQ-UI-33a: the same FACT SET at width 200 and width 20.
        // Only line breaks differ — nothing is truncated or elided.
        {
            db.exec("INSERT INTO pending_strike(entity, damage, element) "
                    "VALUES (7, 5, NULL)");
            db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
                    "VALUES (7, 'dot', 9, 2)");
            const std::string wide = composeBand(db, 200, kBandPlain);
            const std::string narrow = composeBand(db, 20, kBandPlain);
            for (const char* fact : {"goblin", "grunt", "HP:", "8/8",
                                     "[WINDING", "UP]", "dot", "12/12",
                                     "Ward:", "Stun:", "corridor"}) {
                CHECK(contains(wide, fact));
                CHECK(contains(narrow, fact));
            }
            db.exec("DELETE FROM pending_strike WHERE entity = 7");
            db.exec("DELETE FROM status_effects WHERE entity = 7");
        }
    }
}

// Check 16 / REQ-UI-16: golden bands for seven fixed world states, asserted as
// EXACT strings with color disabled. These are the layout's contract — a change
// to spacing, ordering, or row composition has to be made here deliberately.
// Set TW_DUMP_BANDS=1 to print the actuals when intentionally re-baselining.
static void testBandGoldens() {
    const bool dump = std::getenv("TW_DUMP_BANDS") != nullptr;
    auto golden = [&](const char* label, const std::string& actual,
                      const std::string& expected) {
        if (dump) {
            std::printf("--- %s ---\n%s", label, actual.c_str());
            return;
        }
        CHECK(actual == expected);
    };

    const TempDbFile worldPath("textworld_band_golden_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // 1. An empty room: no exits, no objects, no hostiles — just the player.
    db.exec("INSERT INTO entities(id) VALUES (99)");
    db.exec("INSERT INTO room(entity) VALUES (99)");
    db.exec("INSERT INTO name(entity, value) VALUES (99, 'empty vault')");
    db.exec("UPDATE location SET container = 99 WHERE entity = 3");
    golden("empty room", composeBand(db, 60, kBandPlain),
           "-- empty vault ---------------------------------------------\n"
           " You      HP: 12/12\n");

    // 2. A room with objects (the cell holds the wand).
    db.exec("UPDATE location SET container = 1 WHERE entity = 3");
    golden("objects", composeBand(db, 60, kBandPlain),
           "-- cell ----------------------------------------------------\n"
           " Exits    down, north\n"
           " Objects  wand\n"
           " You      HP: 12/12\n");

    // 3. One hostile (the corridor).
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    golden("one hostile", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8\n"
           " You      HP: 12/12  Stun: ready  Ward: ready\n");

    // 4. Three hostiles (the library swarm), one row each in entity order.
    db.exec("UPDATE location SET container = 11 WHERE entity = 3");
    golden("three hostiles", composeBand(db, 60, kBandPlain),
           "-- library -------------------------------------------------\n"
           " Exits    up\n"
           " Enemy    snapping folio  HP: 10/10\n"
           " Enemy    snapping folio  HP: 10/10\n"
           " Enemy    snapping folio  HP: 10/10\n"
           " You      HP: 12/12  Stun: ready  Ward: ready\n");

    // 5. Mid-telegraph: the loudest element in the band.
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    db.exec("INSERT INTO pending_strike(entity, damage, element) VALUES (7, 5, NULL)");
    golden("mid-telegraph", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8  [WINDING UP]\n"
           " You      HP: 12/12  Stun: ready  Ward: ready\n");
    db.exec("DELETE FROM pending_strike WHERE entity = 7");

    // 6. A barriered hostile (the armory's ironhide brute).
    db.exec("UPDATE location SET container = 9 WHERE entity = 3");
    golden("barrier", composeBand(db, 60, kBandPlain),
           "-- armory --------------------------------------------------\n"
           " Exits    down\n"
           " Enemy    ironhide brute  HP: 14/14  barrier\n"
           " You      HP: 12/12  Stun: ready  Ward: ready\n");

    // 7. A warded player: the state that decides whether a telegraphed strike
    // lands (REQ-UI-36).
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
            "VALUES (3, 'ward', 0, 1)");
    golden("player warded", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8\n"
           " You      HP: 12/12  ward 1  Stun: ready  Ward: ready\n");
}

// Color, applied per role (REQ-UI-19, -22, -24, -25). Runs AFTER the plain-text
// goldens above, so a failure here is unambiguously a color bug and never a
// layout bug.
static void testBandColor() {
    const TempDbFile worldPath("textworld_band_color_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    db.exec("INSERT INTO pending_strike(entity, damage, element) VALUES (7, 5, NULL)");
    db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
            "VALUES (3, 'ward', 0, 1)");
    {
        const int64_t now = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const std::string sql =
            "INSERT INTO cooldowns(entity, spell, ready_turn) VALUES "
            "(3, 'ward', " + std::to_string(now + 3) + ")";
        db.exec(sql.c_str());
    }

    const std::string colored = composeBand(db, 200, kBandColor);

    // --- per-role mapping (REQ-UI-24), against the plan's normative table ---
    CHECK(contains(colored, "\x1b[1mcorridor\x1b[0m"));       // header: bold, no color
    CHECK(contains(colored, "\x1b[36msouth\x1b[0m"));         // exits: cyan
    CHECK(contains(colored, "\x1b[32mkey\x1b[0m"));           // objects: green
    CHECK(contains(colored, "\x1b[31mgoblin\x1b[0m"));        // hostile name: red
    CHECK(contains(colored, "\x1b[31m8/8\x1b[0m"));           // hostile HP: red
    CHECK(contains(colored, "\x1b[1;91m[WINDING\x1b[0m"));    // telegraph: bold bright red
    CHECK(contains(colored, "\x1b[94mStun:\x1b[0m"));         // ready spell: bright blue
    CHECK(contains(colored, "\x1b[90mWard:\x1b[0m"));         // cooling spell: grey
    CHECK(contains(colored, "\x1b[35mward\x1b[0m"));          // status effect: magenta

    // The ten treatments are PAIRWISE DISTINCT. This is what makes "one role
    // per color" a test rather than a comment: a mapping that is merely applied
    // but not distinct would pass every assertion above while violating
    // REQ-UI-24.
    {
        const std::vector<std::string> codes = {
            "1",     // room name header (bold, uncolored)
            "36",    // exits
            "32",    // objects
            "31",    // hostile name + HP
            "1;91",  // telegraph
            "33",    // player HP, low
            "94",    // spell ready
            "90",    // spell cooling
            "35",    // status effects
            "95",    // discovered resistance
        };
        CHECK(codes.size() == 10);
        for (size_t i = 0; i < codes.size(); ++i) {
            for (size_t j = i + 1; j < codes.size(); ++j) CHECK(codes[i] != codes[j]);
        }
        // Basic 16 only (REQ-UI-19): every code is bold, or 30-37, or 90-97.
        for (const std::string& c : codes) {
            const std::string tail = c == "1;91" ? "91" : c;
            if (tail == "1") continue;
            const int n = std::atoi(tail.c_str());
            CHECK((n >= 30 && n <= 37) || (n >= 90 && n <= 97));
        }
    }

    // --- low-HP threshold (current * 3 <= max), boundary included -----------
    // The threshold is invented by the implementation, so its boundary is
    // tested rather than assumed.
    {
        db.exec("UPDATE health SET current = 3 WHERE entity = 3");   // 9 < 12
        CHECK(contains(composeBand(db, 200, kBandColor), "\x1b[33m3/12\x1b[0m"));
        db.exec("UPDATE health SET current = 4 WHERE entity = 3");   // 12 == 12
        CHECK(contains(composeBand(db, 200, kBandColor), "\x1b[33m4/12\x1b[0m"));
        db.exec("UPDATE health SET current = 5 WHERE entity = 3");   // 15 > 12
        const std::string healthy = composeBand(db, 200, kBandColor);
        CHECK(!contains(healthy, "\x1b[33m"));
        CHECK(contains(healthy, "HP: 5/12"));  // present, just unstyled
        db.exec("UPDATE health SET current = 12 WHERE entity = 3");
    }

    // --- ordering: color must not enter the width arithmetic ---------------
    // A band that counted escape bytes as columns fails here. This is the single
    // most likely source of a width bug, which is why it gets its own sweep.
    for (const int width : {20, 40, 80, 200}) {
        for (const std::string& line : splitOnNewline(composeBand(db, width, kBandColor))) {
            CHECK(utf8Length(stripSgr(line)) <= static_cast<size_t>(width));
        }
    }

    // --- suppression (check 22 / REQ-UI-22) --------------------------------
    // The colorless run is byte-for-byte the colored run with its sequences
    // stripped — no empty sequences, no stray resets, nothing lost.
    for (const int width : {20, 40, 60, 80, 200}) {
        CHECK(stripSgr(composeBand(db, width, kBandColor)) ==
              composeBand(db, width, kBandPlain));
    }
    // And a suppressed run carries no escape byte at all.
    CHECK(composeBand(db, 60, kBandPlain).find('\x1b') == std::string::npos);
    // NO_COLOR keeps bold (the header) but drops every color (REQ-UI-23).
    {
        const TermStyle noColor{false, true};
        const std::string band = composeBand(db, 60, noColor);
        CHECK(contains(band, "\x1b[1mcorridor\x1b[0m"));
        CHECK(!contains(band, "\x1b[31m"));
        CHECK(!contains(band, "\x1b[36m"));
        // The telegraph keeps its bold, so it stays distinct without color.
        CHECK(contains(band, "\x1b[1m[WINDING\x1b[0m"));
    }

    // --- check 11b: nothing is lost at the narrow width --------------------
    {
        const std::string wide = stripSgr(composeBand(db, 200, kBandColor));
        const std::string narrow = stripSgr(composeBand(db, 20, kBandColor));
        for (const char* fact : {"goblin", "grunt", "8/8", "[WINDING", "UP]",
                                 "ward", "Stun:", "Ward:", "key", "east",
                                 "south", "up", "corridor"}) {
            CHECK(contains(wide, fact));
            CHECK(contains(narrow, fact));
        }
    }
}

// The wiring: one composition site, read-only, on every output-producing turn,
// degrading rather than throwing (REQ-UI-1, -2, -3, -4, -6, -6a, -7, -7a, -30).
static void testBandWiring() {
    // --- check 1: no writes ------------------------------------------------
    // Static, in the same style as the combat surface's no-RNG guard: the
    // band's translation unit contains no write statement at all.
    {
        // Match SQL as it would actually appear — inside a string literal
        // handed to prepare()/exec() — so the contract COMMENT naming the three
        // forbidden verbs does not trip the scan.
        const std::string src = readFileBytes("src/band.cpp");
        CHECK(!contains(src, "\"INSERT"));
        CHECK(!contains(src, "\"UPDATE"));
        CHECK(!contains(src, "\"DELETE"));
        CHECK(!contains(src, "\"REPLACE"));
        CHECK(!contains(src, "\"DROP"));
        // The same scan on render.cpp, whose contract this mirrors.
        const std::string rsrc = readFileBytes("src/render.cpp");
        CHECK(!contains(rsrc, "\"INSERT"));
        CHECK(!contains(rsrc, "\"UPDATE"));
        CHECK(!contains(rsrc, "\"DELETE"));
        // The header states the contract, so the next person to add a query
        // reads it before writing one.
        CHECK(contains(readFileBytes("src/band.hpp"), "READ-ONLY BY CONTRACT"));
    }

    // --- check 1, dynamic: N compositions change nothing --------------------
    {
        const TempDbFile worldPath("textworld_band_wiring_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        const int64_t turn0 = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t events0 = queryInt(db, "SELECT COUNT(*) FROM events");
        for (int i = 0; i < 20; ++i) composeBand(db, 80, kBandPlain);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turn0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == events0);
    }

    // --- check 2 / REQ-UI-6: exactly ONE place emits status text ------------
    {
        // Both former append sites are gone.
        CHECK(!contains(readFileBytes("src/render.cpp"), "combatStatusLine"));
        CHECK(!contains(readFileBytes("src/prose.cpp"), "combatStatusLine"));
        // And the band is composed from exactly one call site in the loop, so
        // the AI path and the template path cannot receive different bytes.
        const std::string loop = readFileBytes("src/loop.cpp");
        size_t sites = 0;
        for (size_t i = loop.find("composeBand("); i != std::string::npos;
             i = loop.find("composeBand(", i + 1)) {
            ++sites;
        }
        CHECK(sites == 1);
        // REQ-UI-6a: the helper itself SURVIVES — removed as an appender only.
        CHECK(contains(readFileBytes("src/combat.cpp"), "std::string combatStatusLine"));
    }

    // --- check 21: no-tick turns still get a band --------------------------
    {
        const TempDbFile worldPath("textworld_band_notick_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        const int64_t before = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");

        // An unparseable line.
        const TurnResult bad = runTurn(db, "xyzzy the frobnitz");
        CHECK(bad.outcome == TurnOutcome::NoTick);
        CHECK(contains(bad.output, "I don't understand that."));
        CHECK(contains(bad.output, "-- cell "));   // the band is there
        CHECK(contains(bad.output, "HP: 12/12"));

        // A denied cast (an unknown spell for this player).
        const TurnResult denied = runTurn(db, "cast fire");
        CHECK(denied.outcome == TurnOutcome::NoTick);
        CHECK(contains(denied.output, "-- cell "));

        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == before);

        // REQ-UI-4: the band is the LAST thing in the output, below the text.
        CHECK(bad.output.find("I don't understand") < bad.output.find("-- cell "));
        CHECK(bad.output.back() == '\n');
    }

    // --- REQ-UI-3 on the EngineError path ----------------------------------
    // A trigger makes the tick's first event INSERT abort, so the turn rolls
    // back — and still reports, with a band, because the band is composed after
    // and outside the transaction.
    {
        const TempDbFile worldPath("textworld_band_engineerr_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        const int64_t before = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        db.exec("CREATE TRIGGER boom BEFORE INSERT ON events "
                "BEGIN SELECT RAISE(ABORT, 'boom'); END");
        const TurnResult r = runTurn(db, "look");
        CHECK(r.outcome == TurnOutcome::EngineError);
        CHECK(contains(r.output, "boom"));
        CHECK(contains(r.output, "-- cell "));  // the band survives the rollback
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == before);
        db.exec("DROP TRIGGER boom");
    }

    // --- the degrade path: a THROWING band must not fail the turn ----------
    // Not specified — see the note on bandOrEmpty in loop.cpp — so it is tested
    // directly rather than trusted.
    {
        const TempDbFile worldPath("textworld_band_degrade_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        db.exec("DELETE FROM player");
        const TurnResult r = runTurn(db, "look");
        // The turn still returns its message; the exception does NOT propagate.
        CHECK(!r.output.empty());
        CHECK(contains(r.output, "player entity"));
        CHECK(!contains(r.output, "-- "));  // no band, but no crash either
    }

    // --- REQ-UI-30: narration and template prose are wrapped ---------------
    {
        const TempDbFile worldPath("textworld_band_wrap_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql");
        termSetWidthOverride(40);
        const TurnResult r = runTurn(db, "look");
        for (const std::string& line : splitOnNewline(r.output)) {
            CHECK(utf8Length(stripSgr(line)) <= 40);
        }
        // The room's canon prose is long enough that this is not vacuous.
        CHECK(splitOnNewline(r.output).size() > 4);
        termSetWidthOverride(80);
    }
}

// Check 22 / REQ-UI-5: the band is printed once at startup too, after the
// read-only room render and before the first prompt.
static void testBandStartup() {
    const TempDbFile worldPath("textworld_band_startup_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");
    const std::string out = renderStartup(db);

    // The band is there, and its header names the STARTING room.
    CHECK(contains(out, "-- stone hall "));
    CHECK(contains(out, " Exits    north"));
    CHECK(contains(out, " Objects  lantern"));
    // Below the room render, not above it (REQ-UI-4's ordering at startup).
    CHECK(out.find("vaulted hall of grey stone") < out.find("-- stone hall "));
    // REQ-UI-30: the startup prose is wrapped to the width too.
    for (const std::string& line : splitOnNewline(out)) {
        CHECK(utf8Length(stripSgr(line)) <= 80);
    }
    // main.cpp stays a single fputs of this string — one band-emitting site.
    CHECK(!contains(readFileBytes("src/main.cpp"), "composeBand"));
}

// The `spells` verb: read-only spell inspection that consumes no turn
// (REQ-UI-37, -38, -39, -39a, -39b).
static void testSpellsVerb() {
    const TempDbFile worldPath("textworld_spells_verb_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // Parsed by the fixed-verb parser as an argument-free verb.
    {
        const auto action = parse(db, "spells");
        CHECK(action.has_value());
        CHECK(action->verb == Verb::Spells);
        CHECK(parse(db, "spells now")->verb == Verb::Spells);  // stray arg ignored
    }

    // Check 26 / REQ-UI-38: known spells only, with element, cooldown, effect.
    {
        const TurnResult r = runTurn(db, "spells");
        CHECK(r.outcome == TurnOutcome::NoTick);
        CHECK(contains(r.output, "ward"));
        CHECK(contains(r.output, "stun"));
        CHECK(contains(r.output, "cooldown: 2"));   // ward
        CHECK(contains(r.output, "cooldown: 3"));   // stun
        CHECK(contains(r.output, "element: none"));  // NULL renders as "none"
        // The catalog is NOT a spoiler list: unlearned spells are absent.
        for (const char* unlearned : {"fire", "frost", "dispel", "ember", "blast"}) {
            CHECK(!contains(r.output, unlearned));
        }
        // Step 10's wrapper still appends the band, so the player sees the
        // fight state alongside the rules.
        CHECK(contains(r.output, "-- cell "));
    }

    // Gloss completeness: every effect keyword the catalog defines renders a
    // real sentence, not a bare keyword and not a blank. `stun` is the one this
    // directly guards — an incomplete table would leave a player who knows it
    // staring at nothing.
    {
        db.exec("INSERT INTO known_spells(entity, spell) "
                "SELECT 3, spell FROM spell_catalog "
                "WHERE spell NOT IN (SELECT spell FROM known_spells WHERE entity = 3)");
        const std::string rules = renderSpellRules(db, 3, kBandPlain);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM spell_catalog") == 7);
        for (const char* spell : {"ward", "stun", "fire", "frost", "dispel",
                                  "ember", "blast"}) {
            CHECK(contains(rules, spell));
        }
        // Each rendered line carries a gloss beyond the keyword itself.
        int lines = 0;
        for (const std::string& line : splitOnNewline(rules)) {
            if (!contains(line, "cooldown: ")) continue;
            ++lines;
            const size_t comma = line.rfind(", ");
            CHECK(comma != std::string::npos);
            const std::string gloss = line.substr(comma + 2);
            CHECK(gloss.size() > 12);          // a sentence, not a keyword
            CHECK(contains(gloss, " "));
        }
        CHECK(lines == 7);
    }

    // Check 26a / REQ-UI-39: GENUINELY no-tick, with a hostile in the room.
    // Turn cost is not observable from the counter alone — the enemy must also
    // not have acted.
    {
        const TempDbFile p2("textworld_spells_notick_tests.db");
        Db db2 = openWorld(p2.string(), "tests/combat_fixture.sql");
        db2.exec("UPDATE location SET container = 2 WHERE entity = 3");

        const int64_t turn0 = queryInt(db2, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t events0 = queryInt(db2, "SELECT COUNT(*) FROM events");
        const int64_t hp0 = queryInt(db2, "SELECT current FROM health WHERE entity = 3");
        const int64_t strikes0 =
            queryInt(db2, "SELECT COUNT(*) FROM pending_strike");

        const TurnResult r = runTurn(db2, "spells");
        CHECK(r.outcome == TurnOutcome::NoTick);
        CHECK(queryInt(db2, "SELECT value FROM meta WHERE key = 'turn'") == turn0);
        CHECK(queryInt(db2, "SELECT COUNT(*) FROM events") == events0);
        CHECK(queryInt(db2, "SELECT current FROM health WHERE entity = 3") == hp0);
        CHECK(queryInt(db2, "SELECT COUNT(*) FROM pending_strike") == strikes0);
    }

    // Check 26b, as scoped: every command that resolves INSIDE THE TICK writes
    // an events row; `spells` is the only SUCCESSFUL path producing output
    // without one. (The two pre-existing no-tick paths are refusals — the
    // precedent REQ-UI-39a itself cites, not violations.)
    {
        const TempDbFile p3("textworld_spells_bounded_tests.db");
        Db db3 = openWorld(p3.string(), "tests/combat_fixture.sql");
        for (const char* line : {"look", "wait", "inventory", "take wand"}) {
            const int64_t before = queryInt(db3, "SELECT COUNT(*) FROM events");
            const TurnResult r = runTurn(db3, line);
            CHECK(r.outcome == TurnOutcome::Ticked);
            CHECK(queryInt(db3, "SELECT COUNT(*) FROM events") > before);
        }
        // REQ-UI-39b is recorded at BOTH sites, so the exception cannot be
        // cited as precedent by someone reading only one of them.
        CHECK(contains(readFileBytes("src/loop.cpp"), "MUST NOT GENERALIZE"));
        CHECK(contains(readFileBytes("src/band.hpp"), "MUST NOT GENERALIZE"));
    }

    // Verb::Spells must never reach the tick — resolve() throws if it does.
    {
        const TempDbFile p4("textworld_spells_routing_tests.db");
        Db db4 = openWorld(p4.string(), "tests/combat_fixture.sql");
        bool threw = false;
        try {
            resolve(db4, Action{Verb::Spells}, 3);
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    // The model-facing verb list and the tool enum are ELEMENT-WISE EQUAL, so
    // the two cannot drift.
    {
        const std::vector<std::string> words = {
            "look", "go", "take", "drop", "inventory", "wait",
            "quit", "attack", "cast", "read", "spells"};
        // Both lists live in nlresolve.cpp; compare them at the source level,
        // since a word present in one and absent from the other is a silent
        // half-wiring rather than a compile error. (The schema's runtime shape
        // is asserted in testNlResolveRequestBody.)
        const std::string src = readFileBytes("src/nlresolve.cpp");
        const size_t enumStart = src.find("{\"enum\", json::array({");
        CHECK(enumStart != std::string::npos);
        const std::string enumBlock =
            src.substr(enumStart, src.find("})}", enumStart) - enumStart);
        for (const std::string& w : words) {
            CHECK(contains(src, "word == \"" + w + "\""));  // verbFromWord
            CHECK(contains(enumBlock, "\"" + w + "\""));    // the tool enum
        }
        // Nothing beyond the eleven: verbFromWord has exactly this many arms.
        size_t arms = 0;
        for (size_t i = src.find("word == \""); i != std::string::npos;
             i = src.find("word == \"", i + 1)) {
            ++arms;
        }
        CHECK(arms == words.size());
        // And the prompt describes all eleven, so the schema can never accept a
        // value the prompt never mentions.
        const std::string p = kResolveSystemPrompt;
        CHECK(contains(p, "exactly eleven verbs"));
        CHECK(!contains(p, "exactly ten verbs"));
        for (const std::string& w : words) CHECK(contains(p, "\n- " + w + ":"));
    }
}

// Group G: resistance discovery, derived from the events transcript
// (REQ-UI-41..-46). Also the gate on whether group G ships at all — checks 31
// and 32 are what REQ-UI-45/-46 turn on.
static void testBandResistance() {
    // --- checks 31 & 32: no schema movement, no shadow store ---------------
    // If either of these fails, group G does not ship: REQ-UI-45 says defer
    // rather than bump, because world.cpp's gate has no migration path and a
    // bump makes every existing world file unopenable.
    {
        // No SCHEMA_VERSION assertion here on purpose. This check was written as
        // "group G bumped nothing", and pinning it to the current value would
        // make every future bump edit a band test for no reason. (It has since
        // moved 5 → 6 — from the bard fact store, REQ-BARD-STORE-1, not from
        // here.) The verbatim table list below carries the real guarantee: any
        // shape group G added would show up in it.
        const TempDbFile p("textworld_resist_schema_tests.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql");
        std::vector<std::string> tables;
        {
            Stmt s = db.prepare(
                "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
            while (s.step()) tables.push_back(s.colText(0));
        }
        // The pre-feature table list, verbatim. A shadow discovery table would
        // satisfy every behavioural check below while violating REQ-UI-46;
        // this is what makes that requirement falsifiable. `catalog` and
        // `motive_catalog` are the bard fact store's (REQ-BARD-STORE-2, -4) —
        // not group G's, and asserted in full by testBardStoreSchema.
        const std::vector<std::string> expected = {
            "barrier", "bestiary", "catalog", "cooldowns", "description",
            "drop_table", "entities", "events", "exits", "grimoire", "health",
            "hostile", "known_spells", "location", "meta", "motive_catalog",
            "name", "pending_strike", "player", "portable", "resistance", "room",
            "spell_catalog", "status_effects"};
        CHECK(tables == expected);
        // And no DDL was added to the band or the mutation helper.
        CHECK(!contains(readFileBytes("src/band.cpp"), "CREATE TABLE"));
        CHECK(!contains(readFileBytes("src/mutations.cpp"), "CREATE TABLE"));
    }

    const TempDbFile worldPath("textworld_resist_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        db.exec("INSERT INTO known_spells(entity, spell) VALUES (3,'fire'),(3,'frost')");
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");

        // Check 27: a fresh world facing an archetype shows NOTHING. The
        // resistance table is never surfaced wholesale (REQ-UI-42).
        CHECK(discoveredResistances(db, "goblin_grunt").empty());
        CHECK(!contains(composeBand(db, 200, kBandPlain), "x1"));
        CHECK(!contains(composeBand(db, 200, kBandPlain), "x2"));

        // Cast fire at the goblin grunt.
        const TurnResult r = runTurn(db, "cast fire");
        CHECK(r.outcome == TurnOutcome::Ticked);

        // --- step 13's gate: the tag is written, and ONLY here --------------
        {
            Stmt s = db.prepare(
                "SELECT detail FROM events WHERE verb = 'burned' "
                "ORDER BY id DESC LIMIT 1");
            CHECK(s.step());
            CHECK(s.colText(0) == "goblin_grunt|fire");
        }
        // The other five damage verbs are not resistance-scaled and stay NULL.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE detail IS NOT NULL "
                       "AND verb IN ('attacked','aoe','dot','struck','chip')") == 0);

        // --- the shield test: the tag reaches the BAND but never the MODEL --
        // The regression guard for handing a raw archetype tag to the narrator,
        // which would invite it into the prose as a noun.
        {
            const int64_t turn =
                queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
            const TurnFacts facts = buildFacts(db, turn);
            CHECK(!contains(facts.payload, "goblin_grunt"));
            CHECK(!contains(facts.payload, "goblin_grunt|fire"));
            CHECK(!contains(facts.payload, "|fire"));
        }

        // Check 28 & 34: fire is now KNOWN. goblin_grunt has no resistance row,
        // so this is a discovered ABSENCE — rendered as an explicit x1, which
        // must read differently from an element never tried (REQ-UI-42a).
        {
            const std::vector<std::string> facts =
                discoveredResistances(db, "goblin_grunt");
            CHECK(facts.size() == 1);
            CHECK(facts[0] == "fire x1");
            const std::string band = composeBand(db, 200, kBandPlain);
            CHECK(contains(band, "fire x1"));
            CHECK(!contains(band, "frost"));  // untested stays hidden
        }

        // A real (non-neutral) resistance renders its ratio. The rime-touched
        // goblin resists frost 1/2 and is weak to fire 2/1.
        {
            db.exec("UPDATE location SET container = 6 WHERE entity = 3");
            db.exec("DELETE FROM cooldowns WHERE entity = 3");
            CHECK(runTurn(db, "cast frost").outcome == TurnOutcome::Ticked);
            const std::vector<std::string> facts =
                discoveredResistances(db, "rime_touched");
            CHECK(facts.size() == 1);
            CHECK(facts[0] == "frost x1/2");
            CHECK(contains(composeBand(db, 200, kBandPlain), "frost x1/2"));
            // Its FIRE resistance is still hidden — discovery is per element.
            CHECK(!contains(composeBand(db, 200, kBandPlain), "fire x2"));
        }

        // Wildcard regression: a LIKE ?||'|%' filter would treat the '_' in
        // 'goblin_grunt' as a single-character wildcard and credit this row to
        // it. The fixtures have only one underscore-bearing archetype, so
        // without this test the bug would ship invisibly.
        {
            db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
                    "VALUES (999, 3, 'burned', 7, 1, 'goblinXgrunt|fire')");
            const std::vector<std::string> facts =
                discoveredResistances(db, "goblin_grunt");
            CHECK(facts.size() == 1);          // still only its own fire
            CHECK(facts[0] == "fire x1");
            db.exec("DELETE FROM events WHERE turn = 999");
        }

        // Check 29: discovery SURVIVES DEFEAT, which deletes the hostile row and
        // with it the entity→archetype link. It is keyed to the archetype, so a
        // NEW instance of the same archetype is already known.
        {
            db.exec("DELETE FROM hostile WHERE entity = 7");
            db.exec("DELETE FROM location WHERE entity = 7");
            db.exec("INSERT INTO entities(id) VALUES (77)");
            db.exec("INSERT INTO hostile(entity, archetype, chip, telegraph_period) "
                    "VALUES (77, 'goblin_grunt', 1, 0)");
            db.exec("INSERT INTO health(entity, current, max) VALUES (77, 8, 8)");
            db.exec("INSERT INTO name(entity, value) VALUES (77, 'goblin grunt')");
            db.exec("INSERT INTO location(entity, container) VALUES (77, 2)");
            db.exec("UPDATE location SET container = 2 WHERE entity = 3");
            CHECK(contains(composeBand(db, 200, kBandPlain), "fire x1"));
        }
    }

    // Check 30: discovery PERSISTS ACROSS RESTARTS — free, because events is on
    // disk and nothing is cached in the process.
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        const std::vector<std::string> facts =
            discoveredResistances(db, "goblin_grunt");
        CHECK(facts.size() == 1);
        CHECK(facts[0] == "fire x1");
        CHECK(discoveredResistances(db, "rime_touched").size() == 1);

        // Check 33: the transcript IS the source of truth. Delete the fight's
        // events and the discovery is GONE. Survival would prove state is being
        // stored somewhere other than the transcript (REQ-UI-46).
        db.exec("DELETE FROM events WHERE verb IN ('burned','froze')");
        CHECK(discoveredResistances(db, "goblin_grunt").empty());
        CHECK(discoveredResistances(db, "rime_touched").empty());
    }

    // Check 35: a LEGACY save — pre-feature damage events carry NULL details,
    // so such a world legitimately starts with nothing discovered and must not
    // crash on the nulls (REQ-UI-44a). No backfill is attempted, and none is
    // possible: defeat already destroyed the entity→archetype link.
    {
        const TempDbFile legacyPath("textworld_resist_legacy_tests.db");
        Db db = openWorld(legacyPath.string(), "tests/combat_fixture.sql");
        db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
                "VALUES (1, 3, 'burned', 7, 4, NULL), "
                "       (1, 3, 'froze',  7, 4, NULL), "
                "       (2, 3, 'attacked', 7, 4, NULL)");
        CHECK(discoveredResistances(db, "goblin_grunt").empty());
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        const std::string band = composeBand(db, 200, kBandPlain);
        CHECK(contains(band, "goblin grunt"));  // composed fine, no crash
        CHECK(!contains(band, "x1"));

        // A malformed detail (no separator, or an empty element) is skipped
        // rather than crashing or producing a blank fact.
        db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
                "VALUES (3, 3, 'burned', 7, 4, 'nobar'), "
                "       (3, 3, 'burned', 7, 4, 'goblin_grunt|')");
        CHECK(discoveredResistances(db, "goblin_grunt").empty());
    }
}

// Check 8 / REQ-UI-25: color is NEVER applied inside narration. Only
// engine-composed text is styled, and no entity name is matched against
// narration output.
static void testBandProseUnstyled() {
    const ScopedEnvVar termGuard("TERM");
    const ScopedEnvVar forceGuard("CLICOLOR_FORCE");
    setenv("TERM", "xterm", 1);
    setenv("CLICOLOR_FORCE", "1", 1);
    termRefreshStyle();
    CHECK(currentStyle().color);  // color really is on for this test

    const TempDbFile worldPath("textworld_prose_unstyled_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql");
    const TurnResult r = runTurn(db, "look");

    // The band begins at the header rule; everything above it is prose.
    const size_t bandStart = r.output.find("-- ");
    CHECK(bandStart != std::string::npos);
    const std::string narration = r.output.substr(0, bandStart);
    // The narration mentions entity names ("lantern") and carries NO escape
    // byte — the band TU is the only styling site, and it never sees prose.
    CHECK(contains(narration, "lantern"));
    CHECK(narration.find('\x1b') == std::string::npos);
    // The band below it IS styled, so the assertion above is not vacuous.
    CHECK(r.output.substr(bandStart).find('\x1b') != std::string::npos);

    // Structural, not incidental: the band never receives narration text.
    // composeBand takes a Db and a width, and nothing else.
    CHECK(contains(readFileBytes("src/band.hpp"), "std::string composeBand(Db& db, int width)"));

    setenv("TERM", "dumb", 1);
    unsetenv("CLICOLOR_FORCE");
    termRefreshStyle();  // restore the suite's pinned plain style
}

// REQ-UI-47: the named non-goals stayed out of scope.
static void testBandNonGoals() {
    std::string src;
    for (const char* f : {"src/term.cpp", "src/band.cpp", "src/loop.cpp",
                          "src/main.cpp"}) {
        src += readFileBytes(f);
    }
    CHECK(!contains(src, "?1049"));    // no alternate screen
    CHECK(!contains(src, "DECSTBM"));
    CHECK(!contains(src, "\x1b[r"));   // no scroll region
    CHECK(!contains(src, "readline"));  // no line editing / history
    CHECK(!contains(src, "SIGWINCH"));
    // No third-party UI dependency was added.
    const std::string cmake = readFileBytes("CMakeLists.txt");
    CHECK(!contains(cmake, "ncurses"));
    CHECK(!contains(cmake, "termbox"));
    CHECK(!contains(cmake, "ftxui"));
    CHECK(contains(cmake, "src/term.cpp"));
    CHECK(contains(cmake, "src/band.cpp"));
}

// --- The bard fact store (specs/bard-fact-store.md). Schema, seed data, and
// mutation helpers only: no AI call, no network, no fixture beyond seed SQL.
// Every test here opens tests/combat_fixture.sql, the one fixture carrying the
// combat constants the truth gate reads. ---

// Steps 1 + 2: the two new tables, the three meta rows, the eight motives, and
// the SCHEMA_VERSION gate. Modeled on testCombatSchema.
static void testBardStoreSchema() {
    const TempDbFile worldPath("textworld_bard_schema_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    // REQ-BARD-STORE-2: catalog, exactly eleven columns, exactly these names.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type='table' AND name='catalog'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('catalog')") == 11);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('catalog') "
                       "WHERE name IN ('id','kind','handle','name','blurb','motive',"
                       "'tier','seeded','entity','fact_archetype','fact_element')") == 11);

    // REQ-BARD-STORE-4: motive_catalog(motive, blurb), exactly two columns.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('motive_catalog')") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('motive_catalog') "
                       "WHERE name IN ('motive','blurb')") == 2);

    // REQ-BARD-STORE-3 stays DEFERRED: no inert binding table ships through the
    // bump. This is the assertion that keeps it deferred.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type='table' AND name='catalog_binding'") == 0);

    // REQ-BARD-STORE-6: three meta ROWS, present at init (spec test 5).
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key IN "
                       "('bard_journal','bard_focus','bard_last_wake_turn')") == 3);
    CHECK(queryText(db, "SELECT value FROM meta WHERE key='bard_journal'").empty());
    CHECK(queryText(db, "SELECT value FROM meta WHERE key='bard_focus'").empty());
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key='bard_last_wake_turn'") == 0);

    // REQ-BARD-STORE-5: exactly eight motives, every blurb non-empty, and the
    // key set is the authored vocabulary — not merely eight of something.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog "
                       "WHERE blurb IS NULL OR blurb = ''") == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog WHERE motive IN "
                       "('curiosity','secrecy','rivalry','obligation','grief',"
                       "'appetite','pride','homesickness')") == 8);

    // REQ-BARD-STORE-7: the verb vocabulary comment names 'materialized'.
    CHECK(contains(readFileBytes("src/world.cpp"), "'materialized'"));
}

// The SHIPPED seed carries the same eight motives (a fixture/seed divergence
// would let every test above pass against a world the game never builds).
static void testBardStoreShippedSeedMotives() {
    const TempDbFile worldPath("textworld_bard_seed_tests.db");
    Db db = openWorld(worldPath.string());  // default seed/base.sql
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog WHERE motive IN "
                       "('curiosity','secrecy','rivalry','obligation','grief',"
                       "'appetite','pride','homesickness')") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key IN "
                       "('bard_journal','bard_focus','bard_last_wake_turn')") == 3);
}

// REQ-BARD-STORE-1 (mechanical check 4): a world file written at the PREVIOUS
// version is refused, and the refusal writes nothing. Same shape as testWorld's
// 999999 case, with the real predecessor value.
static void testBardStoreVersionGate() {
    const TempDbFile worldPath("textworld_bard_version_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key='schema_version'") == 6);
    }
    {
        Db db(worldPath.string());
        db.exec("UPDATE meta SET value = 5 WHERE key = 'schema_version'");
    }
    const std::string bytesBefore = readFileBytes(worldPath);
    CHECK(!bytesBefore.empty());

    bool refused = false;
    try {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    } catch (const SchemaMismatch&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(readFileBytes(worldPath) == bytesBefore);  // nothing written on refusal
}

// Did `fn` throw std::runtime_error? Every writeCatalogEntry refusal is one
// (REQ-BARD-STORE-9, -10), so the throw cases read as one line each.
template <typename Fn>
static bool threwRuntimeError(Fn fn) {
    try {
        fn();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

// Steps 3, 4, 7: writeCatalogEntry's mint and argument validation, the truth
// gate, and markCatalogSeeded (spec tests 6, 7, 8, 12).
static void testBardStoreWrite() {
    const TempDbFile worldPath("textworld_bard_write_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    const auto catalogCount = [&db] {
        return queryInt(db, "SELECT COUNT(*) FROM catalog");
    };
    const auto eventCount = [&db] {
        return queryInt(db, "SELECT COUNT(*) FROM events");
    };

    // --- spec test 6: a valid entry mints a row and appends NO event --------
    const int64_t eventsBefore = eventCount();
    const int64_t id = writeCatalogEntry(db, "character", "cloistered_scribe",
                                         "cloistered scribe",
                                         "a scribe who has not left the annex in years",
                                         "curiosity", 1);
    CHECK(id > 0);
    CHECK(catalogCount() == 1);
    CHECK(eventCount() == eventsBefore);  // a latent entry has not HAPPENED

    // Every column round-trips, and the fact fields are SQL NULL — not "" — so
    // the DDL's "both NULL or both non-NULL" invariant is true in the data.
    {
        Stmt s = db.prepare(
            "SELECT kind, handle, name, blurb, motive, tier, seeded, "
            "entity IS NULL, fact_archetype IS NULL, fact_element IS NULL "
            "FROM catalog WHERE id = ?");
        s.bind(1, id);
        CHECK(s.step());
        CHECK(s.colText(0) == "character");
        CHECK(s.colText(1) == "cloistered_scribe");
        CHECK(s.colText(2) == "cloistered scribe");
        CHECK(s.colText(3) == "a scribe who has not left the annex in years");
        CHECK(s.colText(4) == "curiosity");
        CHECK(s.colInt(5) == 1);
        CHECK(s.colInt(6) == 0);  // seeded defaults to 0
        CHECK(s.colInt(7) == 1);  // entity IS NULL — latent
        CHECK(s.colInt(8) == 1);
        CHECK(s.colInt(9) == 1);
    }

    // Surrounding whitespace is trimmed on the way in, not merely tolerated.
    {
        const int64_t trimmed = writeCatalogEntry(db, "beat", "  spilled_ink  ",
                                                  "  spilled ink  ",
                                                  "  a dark stain, still wet  ",
                                                  "secrecy", 0);
        Stmt s = db.prepare("SELECT handle, name, blurb FROM catalog WHERE id = ?");
        s.bind(1, trimmed);
        CHECK(s.step());
        CHECK(s.colText(0) == "spilled_ink");
        CHECK(s.colText(1) == "spilled ink");
        CHECK(s.colText(2) == "a dark stain, still wet");
    }

    // --- spec test 7: every refusal throws, and writes NOTHING -------------
    const int64_t rows = catalogCount();
    // One idiom for every refusal below, including the truth-gate cases: name
    // the arguments that make this call illegal, assert it throws.
    const auto refused = [&db](const char* kind, const char* handle,
                               const char* name, const char* blurb,
                               const char* motive, int64_t tier,
                               const char* archetype = "", const char* element = "") {
        return threwRuntimeError([&] {
            writeCatalogEntry(db, kind, handle, name, blurb, motive, tier,
                              archetype, element);
        });
    };
    CHECK(refused("place", "h1", "n", "b", "curiosity", 1));
    CHECK(refused("", "h2", "n", "b", "curiosity", 1));
    CHECK(refused("beat", "h3", "n", "b", "envy", 1));  // not in the eight
    CHECK(refused("beat", "", "n", "b", "curiosity", 1));
    CHECK(refused("beat", "   ", "n", "b", "curiosity", 1));  // whitespace-only
    CHECK(refused("beat", "h4", "", "b", "curiosity", 1));
    CHECK(refused("beat", "h5", " \t ", "b", "curiosity", 1));
    CHECK(refused("beat", "h6", "n", "", "curiosity", 1));
    CHECK(refused("beat", "h7", "n", "\n", "curiosity", 1));
    CHECK(refused("beat", "h8", "n", "b", "curiosity", -1));
    // Both fact fields or neither — one alone throws, in either direction.
    CHECK(refused("beat", "h9", "n", "b", "curiosity", 1, "rime_touched", ""));
    CHECK(refused("beat", "h10", "n", "b", "curiosity", 1, "", "fire"));
    CHECK(catalogCount() == rows);  // not one refusal left a row behind

    // --- spec test 8: the truth gate, driven from the SEEDED matchups ------
    // rime_touched is weak to fire (2x) and shrugs off frost (1/2x); both are
    // real resistance rows, so both are admissible knowledge beats.
    CHECK(writeCatalogEntry(db, "beat", "scorched_lectern", "scorched lectern",
                            "a lectern burned black, as if someone learned "
                            "something here the hard way",
                            "curiosity", 1, "rime_touched", "fire") > 0);
    CHECK(writeCatalogEntry(db, "beat", "rimed_margin", "rimed margin",
                            "a margin note about cold things and colder answers",
                            "curiosity", 1, "rime_touched", "frost") > 0);
    const int64_t afterAccepted = catalogCount();

    // Clause c: goblin_grunt/fire has NO resistance row, so the matchup is
    // neutral. A beat about a neutral matchup teaches the player nothing, and
    // the engine cannot inspect the blurb's English claim about it — refusing
    // is the enforceable form of "may not promise a falsehood".
    CHECK(refused("beat", "neutral_beat", "n", "b", "curiosity", 1,
                  "goblin_grunt", "fire"));
    // Clause a: an archetype absent from the bestiary.
    CHECK(refused("beat", "absent_beast", "n", "b", "curiosity", 1,
                  "no_such_beast", "fire"));
    // Clause b: an element absent from spell_catalog…
    CHECK(refused("beat", "acid_beat", "n", "b", "curiosity", 1,
                  "rime_touched", "acid"));
    // …and a SPELL that is not an element. ward/stun/dispel/blast all carry a
    // NULL element, so the live vocabulary is exactly {fire, frost}.
    CHECK(refused("beat", "ward_beat", "n", "b", "curiosity", 1,
                  "rime_touched", "ward"));
    CHECK(catalogCount() == afterAccepted);  // every refusal wrote nothing

    // --- spec test 12: markCatalogSeeded is idempotent ---------------------
    const auto seededOf = [&db](int64_t row) {
        return queryInt(db, ("SELECT seeded FROM catalog WHERE id = " +
                             std::to_string(row)).c_str());
    };
    const int64_t eventsBeforeSeed = eventCount();
    markCatalogSeeded(db, id);
    CHECK(seededOf(id) == 1);
    markCatalogSeeded(db, id);  // a second call is a no-op BY CONSTRUCTION
    CHECK(seededOf(id) == 1);
    CHECK(eventCount() == eventsBeforeSeed);  // event-free bookkeeping
}

// Steps 5, 6, 9: the L0 → L2 latch, its event, placeCatalogEntry, and the
// narrator shield on the handle (spec tests 9, 10 + micro-decision 2).
static void testBardStoreMaterialize() {
    const TempDbFile worldPath("textworld_bard_materialize_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    const int64_t entry = writeCatalogEntry(db, "character", "wandering_proctor",
                                            "wandering proctor",
                                            "a proctor who keeps arriving from "
                                            "the wrong direction",
                                            "obligation", 1);
    const auto entityOf = [&db](int64_t row) {
        return queryInt(db, ("SELECT entity FROM catalog WHERE id = " +
                             std::to_string(row)).c_str());
    };

    // --- spec test 9: the latch and its event ------------------------------
    const int64_t before = queryInt(db, "SELECT COUNT(*) FROM events");
    CHECK(materializeCatalogEntry(db, entry, 4, 3) == true);
    CHECK(entityOf(entry) == 4);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == before + 1);
    {
        Stmt s = db.prepare(
            "SELECT subject, object, detail FROM events WHERE verb = 'materialized'");
        CHECK(s.step());
        CHECK(s.colInt(0) == 4);      // subject = the world entity
        CHECK(s.colInt(1) == entry);  // object  = the catalog id
        CHECK(s.colText(2) == "wandering_proctor");  // detail = the handle
        CHECK(!s.step());             // exactly one
    }

    // A second call changes nothing, appends nothing, returns false — the
    // guarantee is the WHERE clause, not a prior read.
    CHECK(materializeCatalogEntry(db, entry, 5, 3) == false);
    CHECK(entityOf(entry) == 4);  // unchanged
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == before + 1);

    // A nonexistent catalog id is the same no-op, not a throw.
    CHECK(materializeCatalogEntry(db, 99999, 5, 3) == false);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == before + 1);

    // --- spec test 10: placeCatalogEntry -----------------------------------
    const int64_t second = writeCatalogEntry(db, "beat", "moving_stair",
                                             "moving stair",
                                             "a stair that is not where it was",
                                             "rivalry", 1);
    const int64_t entitiesBefore = queryInt(db, "SELECT COUNT(*) FROM entities");
    const int64_t minted = placeCatalogEntry(db, second, 2,
                                             "It ends on a landing that was not "
                                             "there a moment ago.", 3);
    CHECK(minted > 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entitiesBefore + 1);
    CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                         std::to_string(minted)).c_str()) == "moving stair");
    {
        const std::string prose = queryText(
            db, ("SELECT prose FROM description WHERE entity = " +
                 std::to_string(minted)).c_str());
        CHECK(prose == "It ends on a landing that was not there a moment ago.");
        // Explicitly NOT the blurb: the blurb is selection prose the model has
        // already seen, and reusing it would put it in the room twice.
        CHECK(prose != "a stair that is not where it was");
    }
    CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                        std::to_string(minted)).c_str()) == 2);
    CHECK(entityOf(second) == minted);

    // Called twice, the second call returns 0 and mints NO entity — the
    // assertion that catches a mint-then-check ordering bug.
    const int64_t entitiesAfter = queryInt(db, "SELECT COUNT(*) FROM entities");
    CHECK(placeCatalogEntry(db, second, 2, "another landing", 3) == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entitiesAfter);
    // …and an id that does not exist behaves the same way.
    CHECK(placeCatalogEntry(db, 99999, 2, "nowhere", 3) == 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entitiesAfter);

    // --- micro-decision 2: the handle never reaches the narrator -----------
    // Modeled on testGeneratedEventInvisible. `materialized` is a real event —
    // it appears in the payload — but its detail is an engine-internal machine
    // token, so buildFacts must withhold it the way it withholds burned/froze.
    {
        const int64_t turn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const TurnFacts facts = buildFacts(db, turn);
        const nlohmann::json j = nlohmann::json::parse(facts.payload);
        bool sawMaterialized = false;
        for (const auto& e : j["events"]) {
            if (e.value("verb", "") == "materialized") {
                sawMaterialized = true;
                CHECK(!e.contains("detail"));  // the handle is withheld
            }
        }
        CHECK(sawMaterialized);
        // And the handle string appears NOWHERE in the payload — not in a
        // detail, not smuggled through some other key.
        CHECK(!contains(facts.payload, "wandering_proctor"));
        CHECK(!contains(facts.payload, "moving_stair"));
    }
}

// Step 8a: utf8Truncate cuts by code point and never splits a character.
static void testTermTruncate() {
    // Shorter than the cap: returned unchanged.
    CHECK(utf8Truncate("abc", 10) == "abc");
    CHECK(utf8Truncate("abc", 3) == "abc");
    CHECK(utf8Truncate("", 5).empty());
    // A zero cap yields empty, not the input.
    CHECK(utf8Truncate("abc", 0).empty());
    // ASCII longer than the cap: one code point is one byte here.
    CHECK(utf8Truncate("abcdef", 3) == "abc");
    CHECK(utf8Truncate("abcdef", 3).size() == 3);

    // Multi-byte: an em-dash is 3 bytes, 'é' is 2. Cutting mid-string must
    // count CODE POINTS and leave valid UTF-8 behind.
    const std::string wide = "a—éb—éc";  // 7 code points, 12 bytes
    CHECK(utf8Length(wide) == 7);
    for (size_t n = 0; n <= 7; ++n) {
        const std::string cut = utf8Truncate(wide, n);
        CHECK(utf8Length(cut) == n);
        // A byte-prefix of the input…
        CHECK(wide.compare(0, cut.size(), cut) == 0);
        // …cut on a CODE POINT BOUNDARY, which is what keeps it valid UTF-8.
        // The check is on the byte the cut stopped before, not on the result's
        // last byte: a string legitimately ENDING in a multi-byte character has
        // a continuation byte last, so asserting on cut.back() would be wrong.
        if (cut.size() < wide.size()) {
            CHECK((static_cast<unsigned char>(wide[cut.size()]) & 0xC0) != 0x80);
        }
    }
    CHECK(utf8Truncate(wide, 99) == wide);
}

// Step 8b: the two free-rewrite meta lanes (spec tests 13, 13a).
static void testBardStoreMeta() {
    const TempDbFile worldPath("textworld_bard_meta_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    const auto focus = [&db] {
        return queryText(db, "SELECT value FROM meta WHERE key = 'bard_focus'");
    };
    const auto journal = [&db] {
        return queryText(db, "SELECT value FROM meta WHERE key = 'bard_journal'");
    };

    // --- spec test 13a: FREE REWRITE, never append -------------------------
    writeBardJournal(db, "first thought");
    CHECK(journal() == "first thought");
    writeBardJournal(db, "second thought");
    CHECK(journal() == "second thought");  // exact equality: not a concatenation
    writeBardFocus(db, "the proctor is circling");
    CHECK(focus() == "the proctor is circling");
    writeBardFocus(db, "the stair has moved again");
    CHECK(focus() == "the stair has moved again");
    // Exactly one row each, still — an upsert, not an insert-per-call.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = 'bard_focus'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = 'bard_journal'") == 1);

    // --- spec test 13: normalize, then truncate ----------------------------
    // length() counts CHARACTERS in SQLite, which is the right unit for a cap
    // measured in code points.
    writeBardFocus(db, std::string(kBardFocusMaxChars + 200, 'x'));
    CHECK(queryInt(db, "SELECT length(value) FROM meta WHERE key = 'bard_focus'") ==
          static_cast<int64_t>(kBardFocusMaxChars));

    // Line breaks are collapsed, so no stored focus is ever multi-line.
    writeBardFocus(db, "one\ntwo\rthree");
    CHECK(focus() == "one two three");
    CHECK(focus().find('\n') == std::string::npos);
    CHECK(focus().find('\r') == std::string::npos);
    // A RUN of breaks yields exactly ONE space (micro-decision 4) — read
    // per-character, "\r\n" would have produced two.
    writeBardFocus(db, "a\r\nb");
    CHECK(focus() == "a b");
    writeBardFocus(db, "a\n\n\n\rb");
    CHECK(focus() == "a b");
    // Other whitespace is untouched: the requirement names line breaks only.
    writeBardFocus(db, "a\tb  c");
    CHECK(focus() == "a\tb  c");

    // Normalization happens BEFORE the cut, so the cap is spent on the line the
    // architect will actually read.
    writeBardFocus(db, std::string(kBardFocusMaxChars, 'y') + "\n" +
                           std::string(50, 'z'));
    CHECK(focus() == std::string(kBardFocusMaxChars, 'y'));

    // Multi-byte input is cut by code point, and stays valid UTF-8.
    {
        std::string wide;
        for (size_t i = 0; i < kBardFocusMaxChars + 20; ++i) wide += "é";
        writeBardFocus(db, wide);
        CHECK(queryInt(db, "SELECT length(value) FROM meta WHERE key = 'bard_focus'") ==
              static_cast<int64_t>(kBardFocusMaxChars));
        CHECK(utf8Length(focus()) == kBardFocusMaxChars);
    }

    // The journal is NOT truncated — it is private working memory, and only the
    // focus is paid for on every room generation.
    {
        const std::string big(kBardFocusMaxChars * 3, 'j');
        writeBardJournal(db, big);
        CHECK(journal() == big);
    }
    // …nor normalized: the journal is never read by the architect.
    writeBardJournal(db, "line one\nline two");
    CHECK(journal() == "line one\nline two");
}

// Step 10: the append-only guarantee (REQ-BARD-STORE-17, -18), encoded as
// source-text assertions so it survives as a regression guard rather than being
// grepped once by hand — the testCombatFinalSweep no-RNG precedent. Plus the
// transaction test (spec test 14).
static void testBardStoreAppendOnly() {
    // --- REQ-BARD-STORE-18: mutations.cpp is the ONLY writer ---------------
    // The spec states this guarantee as a GLOB over src/*.cpp, so the test
    // globs too rather than naming files: a hardcoded list silently stops
    // covering the next translation unit someone adds, which is exactly the
    // case the guarantee exists for. The count is asserted so a broken path or
    // an empty directory fails loudly instead of vacuously passing.
    int scanned = 0;
    for (const auto& entry : std::filesystem::directory_iterator("src")) {
        if (entry.path().extension() != ".cpp") continue;
        const std::string filename = entry.path().filename().string();
        if (filename == "mutations.cpp") continue;  // the sanctioned writer
        const std::string code = readFileBytes(entry.path());
        CHECK(!code.empty());
        ++scanned;
        std::istringstream lines(code);
        std::string line;
        while (std::getline(lines, line)) {
            const bool writes = contains(line, "INSERT") ||
                                contains(line, "UPDATE") || contains(line, "DELETE");
            if (!writes) continue;
            CHECK(!contains(line, "catalog"));
            // world.cpp is EXCEPTED for the bard_* keys, and only there: its
            // one INSERT names all three and is REQ-BARD-STORE-6's init write.
            if (filename == "world.cpp") continue;
            CHECK(!contains(line, "bard_journal"));
            CHECK(!contains(line, "bard_focus"));
            CHECK(!contains(line, "bard_last_wake_turn"));
        }
    }
    CHECK(scanned >= 15);  // the tree today; a collapse to 0 must not pass

    // --- REQ-BARD-STORE-17: exactly two latches, no edit path --------------
    const std::string mut = readFileBytes("src/mutations.cpp");
    {
        std::istringstream lines(mut);
        std::string line;
        int latches = 0;
        bool sawEntityLatch = false, sawSeededLatch = false;
        while (std::getline(lines, line)) {
            if (!contains(line, "UPDATE catalog SET")) continue;
            ++latches;
            // The guard must be on the SAME source line as the SET: a wrapped
            // SQL string would pass a naive count and lose the guarantee.
            if (contains(line, "entity = ?") && contains(line, "entity IS NULL")) {
                sawEntityLatch = true;
            }
            if (contains(line, "seeded = 1") && contains(line, "seeded = 0")) {
                sawSeededLatch = true;
            }
        }
        CHECK(latches == 2);
        CHECK(sawEntityLatch);
        CHECK(sawSeededLatch);
    }
    // No edit path exists for any authored column. This is the mechanical form
    // of "corrections append a new entry" — the bard cannot rewrite its mind.
    for (const char* column : {"kind", "handle", "name", "blurb", "motive",
                               "tier", "fact_archetype", "fact_element"}) {
        CHECK(!contains(mut, std::string("UPDATE catalog SET ") + column));
    }

    // --- spec test 14: every helper honors the caller's transaction --------
    const TempDbFile worldPath("textworld_bard_txn_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql");

    db.begin();
    const int64_t entry = writeCatalogEntry(db, "character", "rolled_back",
                                            "rolled back", "never happened",
                                            "grief", 1);
    CHECK(entry > 0);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog") == 1);  // visible inside
    db.rollback();
    CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog") == 0);  // and gone after

    // materializeCatalogEntry + its event roll back together — the "one fact"
    // claim has to survive a rollback to mean anything.
    db.begin();
    const int64_t kept = writeCatalogEntry(db, "character", "kept", "kept",
                                           "this one commits", "pride", 1);
    db.commit();
    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
    db.begin();
    CHECK(materializeCatalogEntry(db, kept, 4, 3));
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 1);
    db.rollback();
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
    CHECK(queryInt(db, ("SELECT entity IS NULL FROM catalog WHERE id = " +
                        std::to_string(kept)).c_str()) == 1);  // latch rolled back too

    db.begin();
    writeBardFocus(db, "a focus that never was");
    CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'bard_focus'") ==
          "a focus that never was");
    db.rollback();
    CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'bard_focus'").empty());
}

int main() {
    // libcurl init/shutdown for the whole run (REQ-LAT-7), ABOVE the live
    // smokes: they use the production transports and must run with libcurl
    // explicitly initialized. The default offline run pays one
    // curl_global_init and nothing else — no handle is ever created.
    const AiHttpGuard httpGuard;

    // Live smoke FIRST, while the developer's real environment is still
    // intact: it needs a real ANTHROPIC_API_KEY, and the hermetic unset below
    // would otherwise clobber it (see testProseLiveSmoke's env-ordering note).
    // No-op unless TEXTWORLD_AI_LIVE_TEST=1, so the default run is unaffected
    // and makes no network access here (REQ-PROSE-17, REQ-RESOLVE-16). Both
    // live smokes run here, before the hermetic unset clobbers the real key.
    testProseLiveSmoke();
    testNlResolveLiveSmoke();
    testArchitectLiveSmoke();  // MUST be here — before the hermetic key unset below
    testCombatLiveSmoke();     // likewise: gated live network, before the key unset

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

    // Same discipline for TEXTWORLD_PROFILE (REQ-LAT-1): a developer shell with
    // it set would otherwise spray profiling lines through every runTurn test.
    // profilingEnabled() caches its getenv at static-init time, so unsetting the
    // var is not enough — the cache must be refreshed too.
    const ScopedEnvVar profileGuard("TEXTWORLD_PROFILE");
    unsetenv("TEXTWORLD_PROFILE");
    profileRefreshEnabled();


    // Pin the band width for the whole suite (REQ-UI-26's chain would otherwise
    // measure the developer's real terminal, so a piped run and a tty run would
    // lay out differently and assert differently). 80 is the same value the
    // detection chain falls back to when nothing answers.
    termSetWidthOverride(80);

    // Same discipline for COLOR (REQ-UI-20): a developer shell — or a CI runner
    // — with CLICOLOR_FORCE set would otherwise style every runTurn-based
    // assertion's band and break substring matches on entity names. TERM=dumb
    // is the one gate that suppresses ALL SGR, bold included, so the suite's
    // runTurn output is plain text. The gate's own truth table is exercised by
    // testTermColorGate, which drives styleFor() directly, and the band's colors
    // by testBandColor, which passes an explicit TermStyle — neither depends on
    // the process style. currentStyle() caches, so the vars must be set AND the
    // cache refreshed.
    const ScopedEnvVar noColorGuard("NO_COLOR");
    const ScopedEnvVar clicolorGuard("CLICOLOR");
    const ScopedEnvVar clicolorForceGuard("CLICOLOR_FORCE");
    const ScopedEnvVar termGuard("TERM");
    unsetenv("NO_COLOR");
    unsetenv("CLICOLOR");
    unsetenv("CLICOLOR_FORCE");
    setenv("TERM", "dumb", 1);
    termRefreshStyle();

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
    testCombatElements();
    testCombatDoT();
    testCombatDefenseLock();
    testCombatMultiplicity();
    testCombatLearn();
    testBestiaryCatalog();
    testCombatGating();
    testCombatSetting();
    testCombatDeterminismReplay();
    testCombatFinalSweep();
    testRender();
    testExitDisplayInvariant();
    testLoop();
    testProseFacts();
    testNlResolveContext();
    testNlResolvePrompt();
    testProseTransport();
    testProfileRecords();
    testProfileBackgroundAndDwell();
    testAiHttpWorkerClient();
    testProfileTurnStages();
    testAiRoleModel();
    testAiUsageParse();
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
    testPregenStore();
    testPregenWorker();
    testPregenWait();
    testPlayerRoom();
    testArchitectQueuePregen();
    testArchitectCommitProposal();
    testArchitectSpawn();
    testResolveGoGenerate();
    testPregenCommit();
    testPregenOutcomeRecords();
    testProfileGenerateStage();
    testCombatFlee();
    testGeneratedEventInvisible();
    testTermColorGate();
    testTermWidth();
    testTermWrap();
    testBandLayout();
    testBandContent();
    testBandGoldens();
    testBandColor();
    testBandWiring();
    testBandStartup();
    testSpellsVerb();
    testBandResistance();
    testBandProseUnstyled();
    testBandNonGoals();
    testBardStoreSchema();
    testBardStoreShippedSeedMotives();
    testBardStoreVersionGate();
    testBardStoreWrite();
    testBardStoreMaterialize();
    testTermTruncate();
    testBardStoreMeta();
    testBardStoreAppendOnly();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
