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
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>  // architect live smoke's single bounded judge call (gated)

#include "action.hpp"
#include "aihttp.hpp"
#include "architect.hpp"
#include "bard.hpp"
#include "band.hpp"
#include "combat.hpp"
#include "db.hpp"
#include "log.hpp"
#include "lookup.hpp"  // lookupNoun — the shared recognition rule (Brick 4 Step 9)
#include "loop.hpp"
#include "mutations.hpp"
#include "nlresolve.hpp"
#include "npc.hpp"
#include "bardworker.hpp"
#include "pregen.hpp"
#include "profile.hpp"
#include "prose.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "term.hpp"
#include "world.hpp"

#include "spinner.hpp"

#include "linenoise.h"  // REQ-POLISH-21: the history API this asserts against

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

// Single text-value query helper.
static std::string queryText(Db& db, const char* sql) {
    Stmt s = db.prepare(sql);
    CHECK(s.step());
    return s.colText(0);
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
        OpenedWorld world = openWorld(worldPath.string(), seedPath);
        Db& db = world.db;

        // REQ-BARD-WAKE-1: the once-ever hook. True only on the call that ran
        // initialize(), which is the only launch the overture may run on.
        CHECK(world.created);

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
        OpenedWorld world = openWorld(worldPath.string(), seedPath);
        Db& db = world.db;

        // The SAME path a second time: not created, and the world intact.
        CHECK(!world.created);

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
        Db db = openWorld(worldPath.string(), seedPath).db;
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
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;

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

    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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

    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- examine (REQ-EXAMINE-3, -4, -5, -6) ---
    // A SECOND world, opened from the SHIPPED seed, because the spec's parser
    // items name the candle in the dormitory cell — that is base.sql's world
    // (candle = entity 4), not fixture.sql's (whose entity 4 is the lantern).
    // fixture.sql is deliberately left alone: other tests pin its exact entity
    // and row counts, which is why combat_fixture.sql exists as a separate file.
    {
        const TempDbFile seedPath("textworld_parser_seed_tests.db");
        Db seed = openWorld(seedPath.string(), "seed/base.sql").db;

        // Item 1: both verb words reach the same entity.
        for (const char* line : {"examine candle", "x candle"}) {
            auto a = parse(seed, line);
            CHECK(a.has_value());
            CHECK(a->verb == Verb::Examine);
            CHECK(a->subject == 4);
        }

        // Item 2: bare verb → nullopt (REQ-PROTO-6a).
        CHECK(!parse(seed, "examine"));
        CHECK(!parse(seed, "x"));

        // Item 3: a noun that exists nowhere in the world → nullopt.
        CHECK(!parse(seed, "examine gryphon"));

        // Item 4, REGRESSION (REQ-EXAMINE-4): `look` is untouched. It still
        // ignores any trailing argument and yields a bare Look — examination
        // phrasings are the AI resolver's job, not this parser's.
        for (const char* line : {"look", "look around", "look at the candle"}) {
            auto a = parse(seed, line);
            CHECK(a.has_value());
            CHECK(a->verb == Verb::Look);
            CHECK(a->subject == 0);
        }
    }
}

// --- the speech clause (REQ-NPCTALK-8, -9, -10) -----------------------------
// `say <text>` takes the remainder of the line VERBATIM. The casing case is the
// one that matters: the parser splits `lowered` to find the verb word but must
// slice `trim(line)` to produce the text, or the player's own words reach the
// `said` row lowercased.
static void testParseSay() {
    const TempDbFile worldPath("textworld_parse_say_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    // The plain case: Say, no subject (resolution finds the character — the
    // shape `attack` already uses), text = the remainder.
    {
        auto a = parse(db, "say hello");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Say);
        CHECK(a->subject == 0);
        CHECK(a->text == "hello");
        CHECK(a->direction.empty());
        CHECK(a->spell.empty());
    }

    // THE CASING GUARD. This is what fails if the lowered copy is sliced.
    {
        auto a = parse(db, "say Hello There, Warden!");
        CHECK(a.has_value());
        CHECK(a->text == "Hello There, Warden!");
    }

    // Outer whitespace is trimmed; INTERIOR spacing is the player's and is
    // preserved byte for byte.
    {
        auto a = parse(db, "say   spaced   out  ");
        CHECK(a.has_value());
        CHECK(a->text == "spaced   out");
    }

    // Leading whitespace before the verb word shifts both copies identically,
    // so the offset still lands in the right place.
    {
        auto a = parse(db, "   say  Mind The Gap ");
        CHECK(a.has_value());
        CHECK(a->text == "Mind The Gap");
    }

    // Bare verb → nullopt (REQ-PROTO-6a), whitespace-only argument included.
    CHECK(!parse(db, "say"));
    CHECK(!parse(db, "say   "));

    // A word that merely STARTS with "say" is not the verb — and specifically
    // must not become a Say carrying an empty text.
    CHECK(!parse(db, "sayonara"));

    // Verb recognition is case-insensitive like every other verb, and the text
    // that follows still is not.
    {
        auto a = parse(db, "SAY Yes");
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Say);
        CHECK(a->text == "Yes");
    }

    // REQ-NPCTALK-10: the parser never branches on AI availability — a `say`
    // parses identically with or without a key, and resolution decides what a
    // say with no reachable model produces. Asserted as source text because
    // parse() has no env-dependent branch to drive; the behavioural half is
    // validation item 22's AI-disabled case, in testSayConversation.
    CHECK(readFileBytes("src/parser.cpp").find("aiNarrationEnabled") ==
          std::string::npos);
}

static void testMutations() {
    const TempDbFile worldPath("textworld_mutations_tests.db");

    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- examine: scope, the event row, and the turn (REQ-EXAMINE-7, -8, -13,
    // -16, -28). The player stands in the garden (room 2); the lantern (4) and
    // the key (5) are both on its floor. ---

    // Total rows across every component table — everything except the event log
    // and the turn counter. Reading the table list from sqlite_master rather
    // than naming tables keeps item 12 honest as the schema grows: a future
    // component table is counted without anyone remembering to add it here.
    auto componentRowTotal = [](Db& d) {
        std::vector<std::string> tables;
        {
            Stmt t = d.prepare(
                "SELECT name FROM sqlite_master WHERE type = 'table' "
                "AND name NOT IN ('events', 'meta', 'sqlite_sequence')");
            while (t.step()) tables.push_back(t.colText(0));
        }
        int64_t total = 0;
        for (const std::string& table : tables) {
            total += queryInt(d, ("SELECT COUNT(*) FROM " + table).c_str());
        }
        return total;
    };

    // Item 5 + items 11, 12, 13: examine a thing on the floor of this room.
    {
        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
        const int64_t rowsBefore = componentRowTotal(db);

        tick(db, Action{Verb::Examine, 4, ""});

        // Item 13: the turn advanced by exactly one. Item 11: exactly one row,
        // verb 'examined', the right subject, object 0, detail NULL.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore + 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore + 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'examined' "
                       "AND subject = 4 AND object = 0 AND detail IS NULL") == 1);
        // Item 12 / REQ-EXAMINE-28: no component table gained or lost a row —
        // examine writes the event and nothing else.
        CHECK(componentRowTotal(db) == rowsBefore);
    }

    // Item 6: the same thing once carried (container = the player) is still in
    // scope — REQ-EXAMINE-7's second leg.
    tick(db, Action{Verb::Take, 4, ""});
    CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 3);
    tick(db, Action{Verb::Examine, 4, ""});
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'examined' "
                   "AND subject = 4") == 2);

    // Item 8a / REQ-EXAMINE-9: the player is in scope, because their container
    // is the room and nothing excludes them. (fixture.sql gives the player a
    // name row and deliberately no description row — the text half of this is
    // asserted in testRender.)
    tick(db, Action{Verb::Examine, 3, ""});
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb = 'examined' "
                   "AND subject = 3") == 1);

    // Item 7: a thing in ANOTHER room refuses with exactly resolveTake's
    // string, and STILL costs a turn — a refused examine is a turn the world
    // understood (REQ-EXAMINE-2, -8).
    {
        tick(db, Action{Verb::Drop, 4, ""});          // lantern back on the floor
        tick(db, Action{Verb::Go, 0, "south"});       // stone hall; lantern left behind
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 4") == 2);

        const int64_t turnBefore = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t failedBefore = queryInt(
            db,
            "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
            "AND detail = 'You don''t see that here.'");
        tick(db, Action{Verb::Examine, 4, ""});
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore + 1);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'failed' "
                       "AND detail = 'You don''t see that here.'") == failedBefore + 1);
        // No 'examined' row was appended for the out-of-scope subject.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'examined' "
                       "AND subject = 4") == 2);
    }

    // Only the fixed seven verb strings ever appear in the log — 'examined'
    // joins them, and no eighth string is introduced (REQ-EXAMINE-15).
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM events WHERE verb NOT IN "
                   "('moved','took','dropped','looked','waited','failed',"
                   "'examined')") == 0);

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
        db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'fire')");
        toStudy(db);
        CHECK(runTurn(db, "cast fire").outcome == TurnOutcome::Ticked);
        CHECK(burnedAmt(db) == kSpellDamage * 2);  // rime weak to fire: 2/1
    }

    // --- Frost (wrong element) applies the resist multiplier (1/2) + a slow ---
    {
        const TempDbFile p("textworld_combat_elem_frost.db");
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;

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
    Db db = openWorld(path.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
        Db db2 = openWorld(downPath.string(), "tests/combat_fixture.sql").db;
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

    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
    int64_t turn = 0;

    // --- turn with no events renders as the empty string ---
    CHECK(render(db, 0).empty());
    CHECK(render(db, 999).empty());

    // --- moved: the destination's canon prose, and NOTHING else
    // (REQ-POLISH-5). The `Exits:` and `You see:` lines these three blocks used
    // to assert on now reach the player only through the status band, which
    // prints them from byte-identical queries (REQ-UI-10, REQ-UI-11). ---
    tick(db, Action{Verb::Go, 0, "north"});
    ++turn;
    {
        const std::string out = render(db, turn);
        // Canon prose from the seed's garden description.
        CHECK(contains(out, "An overgrown walled garden"));
        CHECK(!contains(out, "Exits: "));
        // The key (portable, in the garden) is the band's business now.
        CHECK(!contains(out, "You see: "));
    }

    // --- looked with NULL detail: the actor's current room. REQ-POLISH-17 —
    // the garden was arrived in LAST turn, so this second sight of it prints
    // the room's NAME and nothing else. Exits and objects come from the band on
    // every turn either way. ---
    tick(db, Action{Verb::Look, 0, ""});
    ++turn;
    {
        const std::string out = render(db, turn);
        CHECK(out == "garden\n");
        CHECK(!contains(out, "An overgrown walled garden"));
    }

    // Back to the stone hall — which is meta.start_room, so it is seen from
    // turn zero (REQ-POLISH-15) and prints its name even on ARRIVAL.
    tick(db, Action{Verb::Go, 0, "south"});
    ++turn;
    {
        const std::string out = render(db, turn);
        CHECK(out == "stone hall\n");
        CHECK(!contains(out, "A vaulted hall of grey stone"));
        CHECK(!contains(out, "lantern"));
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

    // --- examined: canon prose verbatim, or the engine-authored fallback ---
    // Item 5 / REQ-EXAMINE-10: the description row, byte-exact, with nothing
    // wrapped, trimmed, or added around it. The expected text is read from the
    // db rather than retyped, so this asserts "verbatim" rather than "equal to
    // a string someone copied correctly once".
    tick(db, Action{Verb::Examine, 4, ""});
    ++turn;
    {
        const std::string prose =
            queryText(db, "SELECT prose FROM description WHERE entity = 4");
        CHECK(render(db, turn) == prose + "\n");
    }

    // Item 6: carried is in scope too, and renders the same prose.
    tick(db, Action{Verb::Take, 4, ""});
    ++turn;
    tick(db, Action{Verb::Examine, 4, ""});
    ++turn;
    CHECK(render(db, turn) ==
          queryText(db, "SELECT prose FROM description WHERE entity = 4") + "\n");
    tick(db, Action{Verb::Drop, 4, ""});
    ++turn;

    // Item 8 / REQ-EXAMINE-11: a named entity with NO description row gets the
    // engine-authored safety net — deterministic, and never a generated
    // description (REQ-EXAMINE-27).
    db.exec("INSERT INTO entities(id) VALUES (6)");
    db.exec("INSERT INTO name(entity, value) VALUES (6, 'gargoyle')");
    db.exec("INSERT INTO location(entity, container) VALUES (6, 1)");
    tick(db, Action{Verb::Examine, 6, ""});
    ++turn;
    CHECK(render(db, turn) == "You see nothing special about the gargoyle.\n");

    // Item 8a / REQ-EXAMINE-9: the player is in scope, and reaches that same
    // fallback — base.sql's deliberate omission of a player description is
    // left undisturbed rather than papered over.
    tick(db, Action{Verb::Examine, 3, ""});
    ++turn;
    CHECK(render(db, turn) == "You see nothing special about the player.\n");

    // Item 9 / REQ-EXAMINE-17: examine the seeded goblin and assert the output
    // is BYTE-EQUAL to its description row. Equality, not digit-hunting, is
    // what proves the requirement: nothing derived from health, hostility,
    // resistance, or any status table can be present if the output is the
    // description row exactly.
    {
        const TempDbFile foePath("textworld_render_examine_foe_tests.db");
        Db foe = openWorld(foePath.string(), "tests/combat_fixture.sql").db;
        foe.exec("UPDATE location SET container = 2 WHERE entity = 3");  // the corridor
        tick(foe, Action{Verb::Examine, 7, ""});
        const int64_t foeTurn = queryInt(foe, "SELECT value FROM meta WHERE key = 'turn'");
        const std::string prose =
            queryText(foe, "SELECT prose FROM description WHERE entity = 7");
        CHECK(render(foe, foeTurn) == prose + "\n");
        // The enemy is alive, hostile, and damaged — none of which shows.
        foe.exec("UPDATE health SET current = 3 WHERE entity = 7");
        tick(foe, Action{Verb::Examine, 7, ""});
        CHECK(render(foe, queryInt(foe, "SELECT value FROM meta WHERE key = 'turn'")) ==
              prose + "\n");
    }

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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

static const char* const kExamineGoldenSession = R"GOLDEN(  cell
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  You are carrying nothing.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  You take the wand.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 You      HP: 12/12 [########]
  You are carrying: wand.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 You      HP: 12/12 [########]
  There's nothing to read there.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 You      HP: 12/12 [########]
  You drop the wand.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  Time passes.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
Spells you know:
stun — element: none, cooldown: 3, interrupts a winding-up strike
ward — element: none, cooldown: 2, blocks one telegraphed strike
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  I don't understand that.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  You don't see that here.
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 12/12 [########]
  A long dim corridor.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key
 Enemy    goblin grunt  HP: 8/8 [########]
 You      HP: 12/12 [########]  Stun: ready  Ward: ready
  You strike the goblin grunt for 4 damage.
  The goblin grunt winds up a heavy blow — strike it down or
  brace!
  The goblin grunt wounds you for 1 damage.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key
 Enemy    goblin grunt  HP: 4/8 [####....]  [WINDING UP]
 You      HP: 11/12 [#######.]  Stun: ready  Ward: ready
  You cast ward.
  The goblin grunt's strike breaks against your ward.
  The goblin grunt wounds you for 1 damage.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key
 Enemy    goblin grunt  HP: 4/8 [####....]
 You      HP: 10/12 [#######.]  Stun: ready  Ward: 2
  You strike the goblin grunt for 4 damage.
  The goblin grunt falls. It drops the fire grimoire.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key, fire grimoire
 You      HP: 10/12 [#######.]
  You study the fire grimoire and learn to cast fire.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key, fire grimoire
 You      HP: 10/12 [#######.]
  You study the fire grimoire, but you already know fire.
-- corridor --------------------------------------------------------------------
 Exits    east, south, up
 Objects  key, fire grimoire
 You      HP: 10/12 [#######.]
  cell
-- cell ------------------------------------------------------------------------
 Exits    down, north
 Objects  wand
 You      HP: 10/12 [#######.]
)GOLDEN";

// The pre-examine golden session (spec AI-Validation item 17, REQ-EXAMINE-23).
// A fixed script over the widest fixture, run through the TEMPLATE path with AI
// disabled, with every turn's output concatenated and compared to one literal.
// Captured on unchanged engine code BEFORE the examine verb existed, so it is
// the byte-identity baseline for the other eleven verbs: if adding examine
// disturbs any of them, this literal stops matching.
//
// Re-capture (only when a verb's template output changes ON PURPOSE):
//   TW_DUMP_GOLDEN=1 ./build/tests
// and paste the printed block back into kExamineGoldenSession.
static void testExamineGoldenSession() {
    const TempDbFile worldPath("textworld_examine_golden_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // Every pre-existing verb at least once, plus the tier-a renderError path.
    // `quit` is deliberately absent: it produces no output.
    const char* script[] = {
        "look",           // looked, NULL detail  → room block
        "inventory",      // looked, 'inventory'  → carrying line
        "take wand",      // took
        "inventory",      // carrying line, non-empty
        "read wand",      // failed: nothing to read there
        "drop wand",      // dropped
        "wait",           // waited
        "spells",         // no-tick reference output
        "frobnicate it",  // tier a: renderError, no tick
        "take key",       // failed: not in this room
        "go north",       // moved, into the goblin's corridor
        "attack",         // attacked + the enemy's turn
        "cast ward",      // cast + warded/chip
        "attack",         // attacked again → defeated, drops the grimoire
        "read fire grimoire",  // learned
        "read fire grimoire",  // reread
        "go south",       // moved, back through a realized exit
    };

    std::string actual;
    for (const char* line : script) actual += runTurn(db, line).output;

    if (std::getenv("TW_DUMP_GOLDEN") != nullptr) {
        std::printf("--- examine golden session ---\n%s--- end ---\n",
                    actual.c_str());
        return;
    }

    CHECK(actual == kExamineGoldenSession);
}

// Two cross-system guards examine depends on but does not itself contain
// (spec AI-Validation items 10 and 14). Neither belongs in testSystems: one
// spans the combat lane, the other spans every entity-minting writer there is.
static void testExamineWorldGuards() {
    // --- item 14 / REQ-EXAMINE-16: examining during a fight costs a tick of
    // the chip clock. resolveCombat runs for every ticked action (loop.cpp), so
    // this is an assertion about examine being an ordinary ticked verb, not a
    // change anywhere. ---
    {
        const TempDbFile worldPath("textworld_examine_combat_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        // Into the corridor, where the goblin grunt (7) waits.
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        const int64_t hpBefore =
            queryInt(db, "SELECT current FROM health WHERE entity = 3");

        const TurnResult r = runTurn(db, "examine key");
        CHECK(r.outcome == TurnOutcome::Ticked);
        const int64_t turn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");

        // The player's examine and the enemy's chip landed in the SAME turn —
        // one transaction, one tick of the clock.
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(turn) + " AND verb = 'examined'")
                           .c_str()) == 1);
        CHECK(queryInt(db,
                       ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(turn) + " AND verb = 'chip' AND actor = 7")
                           .c_str()) == 1);
        CHECK(queryInt(db, "SELECT current FROM health WHERE entity = 3") < hpBefore);
    }

    // --- item 10 / REQ-EXAMINE-27: the coverage guard. The spec's controlling
    // finding is that every entity which can appear ALREADY has canon prose,
    // written by one of five places. Build a world holding one of each and
    // assert it. If a sixth writer ever mints a named entity without prose,
    // this fails loudly — which is the only warning anyone will get that
    // examine has started falling back to its safety net. ---
    {
        const TempDbFile worldPath("textworld_examine_coverage_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t player = 3;

        db.begin();
        // (2) dropGrimoire — the defeat drop.
        CHECK(dropGrimoire(db, "goblin_grunt", /*room=*/1) != 0);
        // (3) placeEnemy — an instance cast from the bestiary.
        CHECK(placeEnemy(db, "rime_touched", /*room=*/1) != 0);
        // (4) writeGeneratedRoom — the architect's room.
        const RoomProposal proposal{"chapter house",
                                    "A low vaulted room, its shelves bare.",
                                    {"east"}};
        CHECK(writeGeneratedRoom(db, /*originRoom=*/1, "north", proposal, player) != 0);
        // (5) writeCatalogEntry + placeCatalogEntry — the bard's materialized
        // story entity.
        const int64_t entry = writeCatalogEntry(
            db, "character", "t0_scribe", "scribe",
            "a scribe copying a ledger nobody asked for", "obligation", 0);
        CHECK(placeCatalogEntry(db, entry, /*room=*/1,
                                "A thin scribe bent over a ledger.", player) != 0);
        db.commit();

        // (1) is the seed itself, already in the fixture. Every named entity in
        // the world now has canon prose — with exactly one exemption: the
        // player, whose missing description is deliberate (see base.sql's note
        // "no description: the player is not canon prose"). REQ-EXAMINE-9 leaves
        // that undisturbed, so `examine player` reaches the fallback line
        // instead of prose.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM name n "
                       "LEFT JOIN description d ON d.entity = n.entity "
                       "WHERE d.entity IS NULL "
                       "AND n.entity NOT IN (SELECT entity FROM player)") == 0);
        // …and the exemption is real, not a vacuous filter.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM description WHERE entity = 3") == 0);
    }
}

// --- facts builder (REQ-PROSE-6, REQ-PROSE-7): pure (db, turn) → TurnFacts,
// payload parseable JSON with exactly the REQ-PROSE-7 keys, name-resolved,
// id-free, with validation anchors. No network anywhere. ---

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- turn 9: examine the carried lantern — the new clause-f anchor, and
    // the prose riding INSIDE the event object (REQ-EXAMINE-24) ---
    const std::string lanternProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 4");
    CHECK(runTurn(db, "examine lantern").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 9);
        const json p = json::parse(f.payload);

        // The top-level key set is STILL exactly the REQ-PROSE-7 four: the
        // description rides inside the event, so nothing about REQ-PROSE-7
        // moves for this feature.
        CHECK(p.size() == 4);

        CHECK(p["events"].size() == 1);
        CHECK(p["events"][0]["verb"] == "examined");
        CHECK(p["events"][0]["subject"] == "lantern");  // name, not id 4
        CHECK(p["events"][0]["description"] == lanternProse);  // verbatim
        CHECK(!p["events"][0].contains("object"));
        CHECK(!p["events"][0].contains("detail"));

        // The anchor itself, verbatim from the description table.
        CHECK(f.examinedText == lanternProse);
        // Examine is not room-describing, and refuses nothing.
        CHECK(!f.canonRequired);
        CHECK(f.failedDetails.empty());
    }

    // --- turn 10: examine the player — no description row, so clause f is
    // INACTIVE and the event carries the name only (REQ-EXAMINE-25a) ---
    CHECK(queryInt(db, "SELECT COUNT(*) FROM description WHERE entity = 3") == 0);
    CHECK(runTurn(db, "examine player").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts f = buildFacts(db, 10);
        const json p = json::parse(f.payload);

        CHECK(p["events"].size() == 1);
        CHECK(p["events"][0]["verb"] == "examined");
        CHECK(p["events"][0]["subject"] == "player");
        // Name only: no description key at all, and emphatically NOT the
        // template's fallback line — requiring that verbatim would pin AI
        // output to template wording.
        CHECK(!p["events"][0].contains("description"));
        CHECK(!contains(f.payload, "You see nothing special about"));

        // EMPTY means "the clause does not apply", never "not found".
        CHECK(f.examinedText.empty());
    }

    // --- hygiene sweep over every payload built this session, plus exact
    // entry shape for every recent_events row (with- and without-subject
    // entries both occur across turns 1..10) ---
    for (int64_t t = 1; t <= 10; ++t) {
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
        for (int64_t t = 0; t <= 10; ++t) (void)buildFacts(db, t);
        CHECK(readFileBytes(worldPath) == bytesBefore);
    }
}

// --- AI resolver: scope-context builder (REQ-RESOLVE-6, -7). Pure function of
// (db, line): SELECTs only, exactly the five REQ-RESOLVE-7 fields, no ids, no
// network. Mirrors testProseFacts's payload-shape + hygiene + purity checks. ---
static void testNlResolveContext() {
    using nlohmann::json;

    const TempDbFile worldPath("textworld_nlresolve_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    // Fresh seed: player in the stone hall (1), lantern (4) here, key (5) in
    // the garden (2), inventory empty. The raw line travels verbatim.
    {
        const ResolveContext ctx = buildResolveContext(db, "take the lantern");
        const json p = json::parse(ctx.payload);

        // Top-level keys are EXACTLY the REQ-RESOLVE-7 set plus `things`
        // (REQ-EXAMINE-19) — six, nothing else.
        CHECK(p.is_object());
        CHECK(p.size() == 6);
        CHECK(p.contains("input"));
        CHECK(p.contains("room"));
        CHECK(p.contains("exits"));
        CHECK(p.contains("items"));
        CHECK(p.contains("inventory"));
        CHECK(p.contains("things"));

        CHECK(p["input"] == "take the lantern");  // raw line, verbatim
        CHECK(p["room"] == "stone hall");
        CHECK(p["exits"] == json::array({"north"}));
        CHECK(p["items"] == json::array({"lantern"}));
        CHECK(p["inventory"].empty());
        // `things` is wider than `items` and includes the player, whose
        // container is the room and whom no case excludes (REQ-EXAMINE-9).
        // (The room itself is absent: rooms have no location row, so nothing
        // is "in" itself — which is also why examine on a room refuses.)
        CHECK(p["things"] == json::array({"player", "lantern"}));

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
        CHECK(p.size() == 6);
        CHECK(p["room"] == "garden");
        CHECK(p["exits"] == json::array({"south"}));
        CHECK(p["items"] == json::array({"key"}));
        CHECK(p["inventory"] == json::array({"lantern"}));
        CHECK(p["things"] == json::array({"player", "key"}));
        checkPayloadHygiene(ctx.payload);
    }

    // --- item 22 / REQ-EXAMINE-19: a room with a NON-PORTABLE occupant. The
    // goblin is examinable and cannot be picked up, which is the whole reason
    // `things` exists — and `items` is unchanged by its presence. Item 23:
    // the hygiene sweep runs over this wider body too, so no id, tier, or
    // internal tag rode in with the new key (REQ-EXAMINE-22). ---
    {
        const TempDbFile foePath("textworld_nlresolve_things_tests.db");
        Db foe = openWorld(foePath.string(), "tests/combat_fixture.sql").db;
        foe.exec("UPDATE location SET container = 2 WHERE entity = 3");  // corridor

        const ResolveContext ctx = buildResolveContext(foe, "look at the goblin");
        const json p = json::parse(ctx.payload);
        CHECK(p.size() == 6);
        CHECK(p["room"] == "corridor");
        // items: portables only, exactly what it has always meant.
        CHECK(p["items"] == json::array({"key"}));
        // things: everything present, in entity order — the key, the player,
        // and the goblin that `items` can never carry.
        CHECK(p["things"] == json::array({"player", "key", "goblin grunt"}));
        checkPayloadHygiene(ctx.payload);
        // No engine-internal vocabulary rode along with the wider key.
        for (const char* internal :
             {"goblin_grunt", "tier", "seeded", "archetype", "hostile", "bestiary"}) {
            CHECK(!contains(ctx.payload, internal));
        }
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

    // Every ISA verb is named (attack/cast/read added in the combat brick,
    // examine in the perception one).
    for (const char* verb :
         {"look", "go", "take", "drop", "inventory", "wait", "quit", "attack",
          "cast", "read", "spells", "examine"}) {
        CHECK(sys.find(verb) != std::string::npos);
    }
    // The subject may now be drawn from `things` as well (REQ-EXAMINE-20), and
    // the no-new-nouns rule is unchanged in force.
    CHECK(sys.find("\"things\"") != std::string::npos);
    CHECK(sys.find("\"items\", \"inventory\", or \"things\"") != std::string::npos);

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

// REQ-EXITS-4: the Exits row lists realized exits always, latent exits only
// when the architect is enabled, and a latent exit renders IDENTICALLY to a
// realized one (no marker). Build a room (stone hall, room 1) with one realized
// exit (north→garden, from the fixture) and one latent exit (up, dest NULL),
// then render it with the architect enabled vs disabled. Defined here, after
// ScopedEnvVar, because it toggles ANTHROPIC_API_KEY under a guard.
//
// Re-pointed at the BAND by REQ-POLISH-5, which deleted roomBlock's duplicate
// Exits line. The invariant REQ-EXITS-4 protects is unchanged; only the one
// place it is now stated moved. This is spec check 6 — the regression the
// deletion could plausibly have caused — asserted here rather than only in a
// capture.
static void testExitDisplayInvariant() {
    const TempDbFile worldPath("textworld_exitdisplay_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    // Plant a latent exit on room 1 (the player's room): up, dest NULL.
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");

    // This test owns the AI env vars for its duration (the suite runs hermetic
    // with both unset); the guards restore whatever was there.
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");

    // Plain style, so the assertions read the text and not the escape bytes.
    const TermStyle plain{false, false};

    // --- architect ENABLED: both the realized and the latent exit list ---
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    {
        const std::string band = composeBand(db, 80, plain);
        // Both directions present, ORDER BY direction → north, up.
        CHECK(contains(band, " Exits    north, up"));
        // The latent 'up' carries NO marker distinguishing it from the realized
        // 'north' — both are bare direction words joined identically. The exact
        // row above plus the absence of any decoration on 'up' proves it.
        CHECK(!contains(band, "up*"));
        CHECK(!contains(band, "up?"));
        CHECK(!contains(band, "(up"));

        // REQ-POLISH-5: and the room block says none of it. The band is the one
        // place either fact reaches the player now.
        const std::string block = renderRoomOf(db, 3, false);
        CHECK(!contains(block, "Exits"));
        CHECK(!contains(block, "You see"));
    }

    // --- architect DISABLED (key unset): only the realized exit lists ---
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("TEXTWORLD_AI");
    {
        const std::string band = composeBand(db, 80, plain);
        CHECK(contains(band, " Exits    north"));
        // The latent 'up' is hidden entirely.
        CHECK(!contains(band, "up"));
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

// One log line broken back into the six fields of REQ-LOG-10. The four middle
// fields contain no spaces by construction, so scanning tokens after the
// fixed-width timestamp is exact; whatever follows the fourth is the message,
// with its own internal spacing intact.
struct ParsedLogLine {
    bool ok = false;
    std::string timestamp;
    std::string level;
    std::string thread;
    std::string turn;
    std::string source;
    std::string message;
};

static ParsedLogLine parseLogLine(const std::string& line) {
    ParsedLogLine p;
    // REQ-LOG-11: YYYY-MM-DD HH:MM:SS.mmm is exactly 23 characters.
    if (line.size() < 25) return p;
    p.timestamp = line.substr(0, 23);
    const std::string digits = "0123456789";
    const char* shape = "dddd-dd-dd dd:dd:dd.ddd";
    for (size_t i = 0; i < 23; ++i) {
        const bool isDigit = digits.find(p.timestamp[i]) != std::string::npos;
        if (shape[i] == 'd' ? !isDigit : p.timestamp[i] != shape[i]) return p;
    }

    std::string* const fields[4] = {&p.level, &p.thread, &p.turn, &p.source};
    size_t i = 23;
    for (std::string* field : fields) {
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (start == i) return p;
        *field = line.substr(start, i - start);
    }
    while (i < line.size() && line[i] == ' ') ++i;
    p.message = line.substr(i);

    if (p.level != "ERROR" && p.level != "WARN" && p.level != "INFO" &&
        p.level != "DEBUG") {
        return p;
    }
    if (p.turn.rfind("turn=", 0) != 0) return p;
    p.ok = true;
    return p;
}

// True iff a captured log entry carries a twprof payload. Since REQ-LOG-22 a
// profiling sink IS the logger's sink, so it also hears the ordinary DEBUG
// entries of whatever subsystem a test happens to drive. The profiling tests
// want the timing records alone — exactly the set they captured before the
// move — so every one of them filters on this. Nothing else about them
// changes, which is what REQ-LOG-30 asks for.
static bool isTwprofEntry(const std::string& line) {
    const ParsedLogLine p = parseLogLine(line);
    return p.ok && p.source == "profile";
}

// The sink the profiling tests install: keep the twprof records, drop
// everything else the logger hears. Built here rather than written out at each
// call site — five copies of it had already started to diverge.
static std::function<void(const std::string&)> captureTwprof(
    std::vector<std::string>& out) {
    return [&out](const std::string& line) {
        if (isTwprofEntry(line)) out.push_back(line);
    };
}

// The same sink, serialized — for the one test that emits from two threads.
static std::function<void(const std::string&)> captureTwprofLocked(
    std::vector<std::string>& out, std::mutex& mutex) {
    return [&out, &mutex](const std::string& line) {
        if (!isTwprofEntry(line)) return;
        const std::lock_guard<std::mutex> lock(mutex);
        out.push_back(line);
    };
}

// Strip the six-field log prefix, returning the bare twprof payload — the
// string these assertions were written against, before REQ-LOG-22 made a timing
// record a DEBUG log entry. The record assertions themselves are therefore
// unchanged by that move, which is what makes REQ-LOG-23's byte-for-byte claim
// checkable here rather than only against a real session log.
static std::string twprofPayload(const std::string& line) {
    const ParsedLogLine p = parseLogLine(line);
    CHECK(p.ok);
    if (!p.ok) return line;
    return p.message;
}

// --- the logging mechanism (REQ-LOG-10..-19, -27) ---------------------------
// ORDERING NOTE, the same one profiling has: logEnabled() caches its getenv
// once per process, so every threshold flip below must be followed by a
// logRefreshLevel() or the cache still holds the value static-init installed.
// This test restores the env var (via the guard), the cached threshold, and the
// default writer before returning, so no later test emits a line.
static void testLogFormatAndLevels() {
    const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");

    // (a) the pure formatter (REQ-LOG-10, -11): six fields, fixed order, one
    // line, for every level.
    const LogLevel levels[4] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info,
                                LogLevel::Debug};
    const char* names[4] = {"ERROR", "WARN", "INFO", "DEBUG"};
    for (int i = 0; i < 4; ++i) {
        const std::string line = formatEntry(
            LogEntry{levels[i], "pregen", 7, "prose", "render failed"});
        const ParsedLogLine p = parseLogLine(line);
        CHECK(p.ok);
        CHECK(p.level == names[i]);
        CHECK(p.thread == "pregen");
        CHECK(p.turn == "turn=7");
        CHECK(p.source == "prose");
        CHECK(p.message == "render failed");
        CHECK(line.find('\n') == std::string::npos);  // no trailing newline
    }
    // A message with its own internal double space survives intact — the
    // parser's field scan stops at the fourth field, not at every gap.
    {
        const ParsedLogLine p = parseLogLine(formatEntry(
            LogEntry{LogLevel::Info, "main", 0, "world", "a  b   c"}));
        CHECK(p.ok);
        CHECK(p.message == "a  b   c");
    }
    // A field wider than its column pushes the line out rather than being
    // truncated.
    {
        const ParsedLogLine p = parseLogLine(formatEntry(
            LogEntry{LogLevel::Info, "main", 1234567, "nlresolve", "x"}));
        CHECK(p.ok);
        CHECK(p.source == "nlresolve");
        CHECK(p.turn == "turn=1234567");
        CHECK(p.message == "x");
    }

    // (b) the threshold (REQ-LOG-17, -18, -19). Unset, empty and unrecognized
    // all fall back to INFO, silently — there is no "off" value.
    unsetenv("TEXTWORLD_LOG_LEVEL");
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Info));
    CHECK(!logEnabled(LogLevel::Debug));

    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Debug));
    CHECK(logEnabled(LogLevel::Error));

    setenv("TEXTWORLD_LOG_LEVEL", "DEBUG", 1);  // case-insensitive
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Debug));

    setenv("TEXTWORLD_LOG_LEVEL", "error", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Error));
    CHECK(!logEnabled(LogLevel::Warn));
    CHECK(!logEnabled(LogLevel::Info));

    setenv("TEXTWORLD_LOG_LEVEL", "warn", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Warn));
    CHECK(!logEnabled(LogLevel::Info));

    setenv("TEXTWORLD_LOG_LEVEL", "banana", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Info));
    CHECK(!logEnabled(LogLevel::Debug));

    setenv("TEXTWORLD_LOG_LEVEL", "", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Info));
    CHECK(!logEnabled(LogLevel::Debug));

    // (c) the sink, and the gate governing emission (REQ-LOG-27).
    std::vector<std::string> captured;
    logSetSink(
        [&captured](const std::string& line) { captured.push_back(line); });

    setenv("TEXTWORLD_LOG_LEVEL", "info", 1);
    logRefreshLevel();
    logEmit(LogLevel::Debug, "bard", "below the threshold");
    logEmitf(LogLevel::Debug, "bard", "below %s", "too");
    CHECK(captured.empty());  // nothing under the threshold reaches the sink

    logEmit(LogLevel::Warn, "prose", "fell back to templates");
    CHECK(captured.size() == 1);
    {
        const ParsedLogLine p = parseLogLine(captured.back());
        CHECK(p.ok);
        CHECK(p.level == "WARN");
        CHECK(p.thread == "main");  // the default thread_local label
        CHECK(p.source == "prose");
        CHECK(p.message == "fell back to templates");
    }

    // logEmitf keeps a migrated site's format string verbatim (REQ-LOG-14).
    logEmitf(LogLevel::Error, "pregen", "job failed: %s after %ds", "timeout",
             20);
    CHECK(captured.size() == 2);
    CHECK(parseLogLine(captured.back()).message ==
          "job failed: timeout after 20s");

    // The turn column tracks the process-local counter (REQ-LOG-13), and
    // profile.cpp's forwarders address the same counter.
    {
        captured.clear();
        const int64_t turn = logNextTurn();
        CHECK(logCurrentTurn() == turn);
        CHECK(profileCurrentTurn() == turn);
        CHECK(profileNextTurn() == turn + 1);
        CHECK(logCurrentTurn() == turn + 1);
        logEmit(LogLevel::Info, "world", "stamped");
        CHECK(parseLogLine(captured.back()).turn ==
              "turn=" + std::to_string(turn + 1));
    }

    // (d) REQ-LOG-15: THREE threads, the shape of the 400-record profiling
    // stress widened to the three that actually emit. Every line must arrive
    // complete and well-formed — never interleaved, never truncated, never two
    // entries braided into one.
    {
        std::vector<std::string> lines;
        std::mutex linesMutex;
        logSetSink([&](const std::string& line) {
            const std::lock_guard<std::mutex> lock(linesMutex);
            lines.push_back(line);
        });
        setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
        logRefreshLevel();

        auto emitMany = [](const char* threadName, const char* source) {
            logSetThreadName(threadName);
            for (int i = 0; i < 200; ++i) {
                logEmitf(LogLevel::Debug, source, "record %d of a long "
                         "enough message to be torn if the write were split",
                         i);
            }
        };
        std::thread a([&] { emitMany("main", "bard"); });
        std::thread b([&] { emitMany("pregen", "pregen"); });
        std::thread c([&] { emitMany("bard", "architect"); });
        a.join();
        b.join();
        c.join();

        CHECK(lines.size() == 600);
        bool allWellFormed = true;
        for (const std::string& line : lines) {
            const ParsedLogLine p = parseLogLine(line);
            // A torn write shows up as an unparseable line, as a second
            // timestamp spliced in mid-line, or as a message cut short of the
            // tail the format string always ends with.
            if (!p.ok || line.find(" record ", 1) != line.rfind(" record ") ||
                p.message.rfind("record ", 0) != 0 ||
                p.message.find("the write were split") == std::string::npos) {
                allWellFormed = false;
                break;
            }
        }
        CHECK(allWellFormed);
    }

    // Restore the default writer AND the cached threshold: the guard only
    // restores the env var, and the cache would otherwise outlive this test.
    logSetSink({});
    unsetenv("TEXTWORLD_LOG_LEVEL");
    logRefreshLevel();
    CHECK(!logEnabled(LogLevel::Debug));
}

// --- the session file: creation, naming, retention (REQ-LOG-3..-9) ----------
// NOTE ON SCOPE. logInit() is the one thing that redirects the error channel,
// and the test binary otherwise never calls it (REQ-LOG-28) — main.cpp is not
// linked here. This test calls it deliberately, against a TEMP directory, and
// every path out of it goes through logShutdown(), which points the error
// channel back at the terminal duplicate. Leave that pairing intact or every
// test that follows writes its stderr into a temp file.
static void testLogFile() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / "textworld-log-tests";

    auto matchingLogs = [](const fs::path& dir) {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(dir)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("textworld-", 0) == 0 &&
                name.size() == std::string("textworld-20260805-143022.log")
                                   .size()) {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    };

    // (a) REQ-LOG-4, -5, -6: the directory is created if absent, exactly one
    // file appears, and its name carries the local date and time year-first.
    {
        const fs::path dir = root / "create";
        fs::remove_all(dir);
        CHECK(!fs::exists(dir));

        const LogInit init = logInit(dir);
        logShutdown();

        CHECK(init.fileOpen);
        CHECK(fs::is_directory(dir));
        const std::vector<std::string> names = matchingLogs(dir);
        CHECK(names.size() == 1);
        if (names.size() == 1) {
            CHECK(init.path.filename().string() == names[0]);
            // textworld-YYYYMMDD-HHMMSS.log, checked digit by digit.
            const std::string& n = names[0];
            bool shaped = n.rfind("textworld-", 0) == 0 &&
                          n.substr(n.size() - 4) == ".log" && n[18] == '-';
            for (size_t i = 10; i < n.size() - 4 && shaped; ++i) {
                if (i == 18) continue;
                if (n[i] < '0' || n[i] > '9') shaped = false;
            }
            CHECK(shaped);
        }
        fs::remove_all(dir);
    }

    // (b) REQ-LOG-9: 25 files in, 20 out — the session's own file among them.
    // Sorting by name equals sorting by modification time, which is the whole
    // point of the REQ-LOG-5 naming. A file that does not match the pattern is
    // never eligible for deletion.
    {
        const fs::path dir = root / "retention";
        fs::remove_all(dir);
        fs::create_directories(dir);

        const auto now = fs::file_time_type::clock::now();
        for (int i = 0; i < 25; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "textworld-2026080%d-1200%02d.log",
                          1 + i / 10, i);
            const fs::path p = dir / name;
            { std::ofstream out(p); out << "seeded\n"; }
            // Older names get older mtimes, so name order IS age order.
            fs::last_write_time(p, now - std::chrono::hours(25 - i));
        }
        { std::ofstream out(dir / "notes.txt"); out << "not a log\n"; }

        const std::vector<std::string> before = matchingLogs(dir);
        CHECK(before.size() == 25);

        const LogInit init = logInit(dir);
        logShutdown();
        CHECK(init.fileOpen);

        const std::vector<std::string> after = matchingLogs(dir);
        CHECK(after.size() == 20);  // the new file counts toward the 20
        CHECK(fs::exists(dir / "notes.txt"));  // never eligible
        // The survivors are the newest: the five oldest seeded names are gone,
        // and the session's own file is present.
        bool oldestGone = true;
        for (int i = 0; i < 5; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "textworld-2026080%d-1200%02d.log",
                          1 + i / 10, i);
            if (fs::exists(dir / name)) oldestGone = false;
        }
        CHECK(oldestGone);
        CHECK(fs::exists(init.path));

        // Name order equals mtime order across everything that survived.
        std::vector<std::pair<fs::file_time_type, std::string>> byTime;
        for (const std::string& name : after) {
            byTime.emplace_back(fs::last_write_time(dir / name), name);
        }
        std::sort(byTime.begin(), byTime.end());
        bool sameOrder = true;
        for (size_t i = 0; i < after.size(); ++i) {
            if (byTime[i].second != after[i]) sameOrder = false;
        }
        CHECK(sameOrder);

        fs::remove_all(dir);
    }

    // (c) REQ-LOG-7, -8: an unwritable directory is not an error. logInit
    // reports the file did not open, throws nothing, and — the half that
    // matters — does not leave the error channel aimed at the game screen.
    {
        const fs::path dir = root / "readonly";
        fs::remove_all(dir);
        fs::create_directories(dir);
        fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);

        bool threw = false;
        LogInit init;
        try {
            init = logInit(dir);
        } catch (...) {
            threw = true;
        }
        logShutdown();

        CHECK(!threw);
        CHECK(!init.fileOpen);

        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        fs::remove_all(dir);
    }

    fs::remove_all(root);
}

// --- profiling mechanism (REQ-LAT-1, -4, -5) --------------------------------
// ORDERING NOTE: profilingEnabled() caches its getenv (once per process, so the
// turn path pays only an enum test), which is exactly why the test-only
// profileRefreshEnabled() exists — every gate flip below must be followed by
// one, or the cache still holds the value main() installed. This test restores
// BOTH the env var (via the guard) and the cached threshold + default writer
// before returning, so no later test emits a profiling line.
static void testProfileRecords() {
    const ScopedEnvVar profGuard("TEXTWORLD_LOG_LEVEL");

    // (a) the gate (REQ-LOG-22). Profiling is no longer a switch of its own —
    // it is the DEBUG level of the one log threshold, so `debug` and only
    // `debug` turns it on. There is deliberately no "off" value to test for
    // (REQ-LOG-19): every other setting, valid or not, simply sits above DEBUG.
    // The threshold's own contract is testLogFormatAndLevels's; this is the
    // half that says profiling rides on it.
    unsetenv("TEXTWORLD_LOG_LEVEL");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    profileRefreshEnabled();
    CHECK(profilingEnabled());
    setenv("TEXTWORLD_LOG_LEVEL", "info", 1);
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
    setenv("TEXTWORLD_LOG_LEVEL", "banana", 1);  // unrecognized => INFO
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
    setenv("TEXTWORLD_LOG_LEVEL", "error", 1);
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
    profileSetSink(captureTwprof(captured));

    setenv("TEXTWORLD_LOG_LEVEL", "error", 1);
    profileRefreshEnabled();
    profileEmit(StageRecord{"resolve", 1, 1.0, nullptr});
    profileEmit(ok);
    { const ScopedStage off("total"); }
    CHECK(captured.empty());  // profiling off => the sink hears nothing

    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
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
        const auto kv = parseProfileRecord(twprofPayload(captured.back()));
        CHECK(kv.at("kind") == "stage");
        CHECK(kv.at("stage") == "total");
        CHECK(kv.at("turn") == std::to_string(turn));
    }

    // Restore the default sink AND the cached gate: the guard only restores the
    // env var, and the cache would otherwise outlive this test.
    profileSetSink({});
    unsetenv("TEXTWORLD_LOG_LEVEL");
    profileRefreshEnabled();
    CHECK(!profilingEnabled());
}

// --- Step 1, REQ-PREGEN-22/-24/-25: the three additions pre-generation needs
// from the profiling mechanism, all provable without a thread of pregen's own:
// the background flag's FORMAT, the dwell record's format and CORRECTNESS, and
// serialized emission under genuine concurrent load. ---
static void testProfileBackgroundAndDwell() {
    const ScopedEnvVar profGuard("TEXTWORLD_LOG_LEVEL");

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
    profileSetSink(captureTwprofLocked(captured, capturedMutex));
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
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
        const auto kv = parseProfileRecord(twprofPayload(captured.back()));
        CHECK(kv.at("kind") == "dwell");
        CHECK(kv.at("turn") == std::to_string(turn));
        const double ms = std::stod(kv.at("ms"));
        CHECK(ms >= 55.0);   // tracks the delay
        CHECK(ms < 500.0);   // and is not some unrelated large number
    }

    // (d) REQ-PREGEN-22, now carried by REQ-LOG-15: two threads, 200 records
    // each. Every one of the 400 must arrive as a COMPLETE, well-formed line —
    // never interleaved, never truncated, never two records braided into one.
    // A record is now a DEBUG log entry, so the checks below run on the PAYLOAD
    // after twprofPayload strips the six-field prefix; a second `twprof` marker
    // anywhere past position 0 of that payload is exactly what a torn write
    // looks like, so that is what is asserted.
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
        for (const std::string& entry : captured) {
            const std::string line = twprofPayload(entry);
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
    unsetenv("TEXTWORLD_LOG_LEVEL");
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
        const auto kv = parseProfileRecord(twprofPayload(line));
        if (kv.at("kind") == "stage") stages.push_back(kv.at("stage"));
    }
    return stages;
}

// True iff any captured record is of `kind`.
static bool capturedAnyKind(const std::vector<std::string>& captured,
                            const std::string& kind) {
    for (const std::string& line : captured) {
        if (parseProfileRecord(twprofPayload(line)).at("kind") == kind) {
            return true;
        }
    }
    return false;
}

// --- turn phase timers (REQ-LAT-2, REQ-LAT-6, REQ-LAT-1) --------------------
// Runs with AI OFF (the suite is hermetic), so resolve and narrate take the
// parser/template paths — which is the point: the stages are SEMANTIC, so they
// are emitted whether or not a network call happened, and no kind=call record
// appears at all (REQ-LAT-6).
static void testProfileTurnStages() {
    const ScopedEnvVar profGuard("TEXTWORLD_LOG_LEVEL");
    std::vector<std::string> captured;
    profileSetSink(captureTwprof(captured));

    // (d) identity check (REQ-LAT-1): the same first turn on two identically
    // seeded worlds, profiling off vs on. With profiling OFF the sink — which
    // is installed the whole time — must hear nothing at all.
    std::string offOutput;
    std::string onOutput;
    {
        const TempDbFile worldPath("textworld_profile_off_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        unsetenv("TEXTWORLD_LOG_LEVEL");
        profileRefreshEnabled();
        offOutput = runTurn(db, "look").output;
    }
    CHECK(captured.empty());

    {
        const TempDbFile worldPath("textworld_profile_on_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
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
        const auto first = parseProfileRecord(twprofPayload(captured.front()));
        for (const std::string& line : captured) {
            CHECK(parseProfileRecord(twprofPayload(line)).at("turn") ==
                  first.at("turn"));
        }
    }

    const TempDbFile worldPath("textworld_profile_stages_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
    unsetenv("TEXTWORLD_LOG_LEVEL");
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
    // The fourth role (REQ-BARD-WAKE-5): a bard wake must be distinguishable
    // from a pregen room job in a profile log, which is the whole reason it is
    // not a reuse of Generate.
    CHECK(std::string(roleName(AiRole::Bard)) == "bard");
    // The fifth role (REQ-NPCTALK-16): a talk turn issues NO narrate request,
    // so the dialogue call occupies the slot narration would have used — and a
    // profile log that could not tell them apart would show a talk turn as an
    // ordinary narrated one.
    CHECK(std::string(roleName(AiRole::Speak)) == "speak");

    // Level 2 — per-role defaults: resolve is the cheap one, prose stays Opus.
    unsetenv("TEXTWORLD_MODEL");
    CHECK(modelForRole(AiRole::Resolve) == "claude-haiku-4-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Generate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Bard) == "claude-opus-4-8");
    // Speech is prose: the reply is the turn's one AI output and prints
    // verbatim, so Speak takes the prose default, not the resolver's cheap one.
    CHECK(modelForRole(AiRole::Speak) == "claude-opus-4-8");

    // Level 1 — the global override wins for EVERY role (REQ-LAT-13), so
    // anyone relying on TEXTWORLD_MODEL today is unaffected by the tiering.
    setenv("TEXTWORLD_MODEL", "claude-sonnet-5", 1);
    CHECK(modelForRole(AiRole::Resolve) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Generate) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Bard) == "claude-sonnet-5");
    CHECK(modelForRole(AiRole::Speak) == "claude-sonnet-5");

    // Set-but-EMPTY is not an override — back to the per-role defaults.
    setenv("TEXTWORLD_MODEL", "", 1);
    CHECK(modelForRole(AiRole::Resolve) == "claude-haiku-4-5");
    CHECK(modelForRole(AiRole::Narrate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Generate) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Bard) == "claude-opus-4-8");
    CHECK(modelForRole(AiRole::Speak) == "claude-opus-4-8");
}

// --- the ISA shape (REQ-NPCTALK-5) ------------------------------------------
// The verb set is THIRTEEN. The switch below has no `default:` arm, so a
// fourteenth verb added later fails to COMPILE here rather than silently
// slipping past a runtime count — which is the only kind of guard that survives
// someone widening the enum in a hurry.
static void testSayIsaShape() {
    const auto arity = [](Verb v) -> int {
        switch (v) {
            // Argument-free verbs.
            case Verb::Look:
            case Verb::Inventory:
            case Verb::Wait:
            case Verb::Quit:
            case Verb::Attack:
            case Verb::Spells:
                return 0;
            // Verbs carrying a subject entity.
            case Verb::Take:
            case Verb::Drop:
            case Verb::Read:
            case Verb::Examine:
                return 1;
            // Verbs carrying their own string field.
            case Verb::Go:
            case Verb::Cast:
                return 2;
            // Say carries `text`, which the ENGINE sets and no model ever does
            // (REQ-NPCTALK-6); `subject` stays 0 and resolution finds the
            // character in the room (REQ-NPCTALK-9).
            case Verb::Say:
                return 3;
        }
        return -1;
    };

    // Every member of the enum, named once, so the count is asserted rather
    // than assumed. Verb has no reflection; this list IS the count.
    const std::vector<Verb> all = {
        Verb::Look, Verb::Go,     Verb::Take,    Verb::Drop,    Verb::Inventory,
        Verb::Wait, Verb::Quit,   Verb::Attack,  Verb::Cast,    Verb::Read,
        Verb::Spells, Verb::Examine, Verb::Say};
    CHECK(all.size() == 13);
    for (const Verb v : all) CHECK(arity(v) >= 0);
    CHECK(arity(Verb::Say) == 3);

    // The new field defaults empty for every other verb, so nothing that
    // constructs an Action today gains a stray payload.
    CHECK(Action{Verb::Look}.text.empty());
    CHECK(Action{Verb::Say}.subject == 0);
}

// --- the threading contract, as written down (REQ-BARD-WAKE-17) -------------
// The header's contract block is the only place the "one handle per thread"
// rule is stated, and it has just gone from one sanctioned worker handle to
// two. A stale block is worse than none: the next person adding a thread reads
// it and concludes a second worker is forbidden. Asserted by source text
// because there is no runtime artifact of a comment.
static void testAiHttpThreadingContract() {
    const std::string header = readFileBytes("src/aihttp.hpp");
    CHECK(!header.empty());

    // The old singular claim is gone.
    CHECK(header.find("ONE sanctioned second handle") == std::string::npos);

    // And both worker guards are named, with their shared position rule.
    CHECK(header.find("PregenGuard") != std::string::npos);
    CHECK(header.find("BardGuard") != std::string::npos);
    CHECK(header.find("BELOW AiHttpGuard") != std::string::npos);
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
        // Examined canon verbatim (REQ-EXAMINE-24). Without this rule the model
        // has no instruction to reproduce the prose, clause f would fail on
        // every examine turn, and the AI path would silently degrade to the
        // template.
        CHECK(sys.find("If an event carries a description") != std::string::npos);
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
        // parser word — a silent half-wiring. 'examine' joins it for the same
        // reason, REQ-EXAMINE-18, and 'say' for REQ-NPCTALK-11.)
        CHECK(verb["enum"] ==
              json::array({"look", "go", "take", "drop", "inventory", "wait",
                           "quit", "attack", "cast", "read", "spells",
                           "examine", "say"}));

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
    // Clause c, examine (REQ-EXAMINE-18): the same treatment as take/drop/read
    // — the noun word is lowered to an id by lookupNoun, never read from the
    // model. Recognition only: the lantern is in the room here, but scope is
    // resolveExamine's decision, not this gate's.
    {
        auto a = validateAndLower(cannedToolUse("examine", "lantern"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Examine);
        CHECK(a->subject == 4);
        CHECK(a->direction.empty());
    }
    // A noun the model may now legitimately see in `things` but which is NOT
    // portable still lowers — REQ-EXAMINE-21's no-guard stance, applied at the
    // gate: the player is a name in the world like any other.
    {
        auto a = validateAndLower(cannedToolUse("examine", "player"), db);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Examine);
        CHECK(a->subject == 3);
    }
    // Clause c failures: unknown noun, and missing subject → nullopt.
    CHECK(!validateAndLower(cannedToolUse("take", "zeppelin"), db));
    CHECK(!validateAndLower(cannedToolUse("take"), db));
    CHECK(!validateAndLower(cannedToolUse("examine", "gryphon"), db));
    CHECK(!validateAndLower(cannedToolUse("examine"), db));

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- clause f (REQ-EXAMINE-25): examined canon prose verbatim ---
    // Item 19 is asserted by everything ABOVE this line: clauses a-e keep their
    // canned responses, their order, and their outcomes unmodified. Nothing in
    // this block edits them.
    TurnFacts examinedFacts;
    examinedFacts.examinedText =
        "A brass lantern, dented and smoke-dulled, its flame steady.";

    // Item 18: verbatim inside surrounding prose → accepted.
    {
        const auto out = validateAiResponse(
            cannedResponse("You turn it over. " + examinedFacts.examinedText +
                           " The wick gutters."),
            examinedFacts);
        CHECK(out.has_value());
    }

    // Item 18: a paraphrase (one changed word) → rejected.
    CHECK(!validateAiResponse(
               cannedResponse("A brass lantern, dented and smoke-stained, its "
                              "flame steady."),
               examinedFacts)
               .has_value());

    // Item 18: absent entirely → rejected.
    CHECK(!validateAiResponse(cannedResponse("You look at the lantern."),
                              examinedFacts)
               .has_value());

    // Item 18: the diagnostic names clause f, and no earlier clause. Captured
    // through the log sink at debug level, the way failClause emits it.
    {
        std::vector<std::string> lines;
        logSetSink([&lines](const std::string& line) { lines.push_back(line); });
        const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");
        setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
        logRefreshLevel();

        CHECK(!validateAiResponse(cannedResponse("You look at the lantern."),
                                  examinedFacts)
                   .has_value());

        logSetSink({});
        unsetenv("TEXTWORLD_LOG_LEVEL");
        logRefreshLevel();

        CHECK(lines.size() == 1);
        CHECK(contains(lines[0], "clause f failed"));
        CHECK(contains(lines[0], "examined canon description not present verbatim"));
    }

    // Item 18a / REQ-EXAMINE-25a: an EMPTY examinedText means the clause does
    // not apply — never that the empty string was not found. Driven with a
    // response containing NONE of the template's fallback wording, so a gate
    // that had quietly started requiring template text would fail here.
    {
        TurnFacts noProse;  // examinedText default-empty, like an entity with
                            // no description row
        const auto out = validateAiResponse(
            cannedResponse("Nothing about it holds your attention for long."),
            noProse);
        CHECK(out.has_value());
        CHECK(*out == "Nothing about it holds your attention for long.");
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- moved turn: AI prose and NOTHING appended (REQ-POLISH-5) ---
    // This block used to assert that the AI path re-emitted roomBlock's own
    // "Exits: " / "You see: " tail, character-identical. REQ-POLISH-5 deleted
    // that tail from both emitters, so the two paths agree by having nothing to
    // agree about: the exits and objects reach the player through the band,
    // which runTurn appends to BOTH paths from one composition (REQ-UI-1).
    // REQ-PROSE-14 is unaffected — the band is engine-composed and appended
    // after the prose, which is what it asks for.
    CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);  // turn 1
    {
        cannedText = "You step through the archway. " + gardenProse +
                     " Cool air settles around you.";
        calls = 0;
        const auto out = aiRender(db, 1, fake);
        CHECK(calls == 1);
        CHECK(out.has_value());

        const std::string tmpl = render(db, 1);
        CHECK(!contains(tmpl, "Exits: "));
        CHECK(!contains(tmpl, "You see: "));
        CHECK(!contains(*out, "Exits: "));
        CHECK(!contains(*out, "You see: "));
        CHECK(*out == cannedText + "\n");
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
        CHECK(r.output ==
              indentProse(wrapProse(render(db, 5), proseWidth(width)), kProseIndent) +
                  composeBand(db, width));

        // Kill switch through the production path: key present but
        // TEXTWORLD_AI=0 → enabled() is false BEFORE any transport, so this
        // never reaches the network either.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        setenv("TEXTWORLD_AI", "0", 1);
        const TurnResult w = runTurn(db, "wait");  // turn 6
        CHECK(w.outcome == TurnOutcome::Ticked);
        CHECK(contains(w.output, "Time passes."));
        CHECK(w.output ==
              indentProse(wrapProse(render(db, 6), proseWidth(width)), kProseIndent) +
                  composeBand(db, width));
        // REQ-UI-4: the band is LAST — the narration is above it.
        CHECK(w.output.find("Time passes.") < w.output.find("-- "));
        // guards restore both vars here.
    }

    // --- examine turn through the AI path (REQ-EXAMINE-24, -26), on its own
    // world so the turn numbering above is untouched ---
    {
        const TempDbFile examinePath("textworld_airender_examine_tests.db");
        Db ex = openWorld(examinePath.string(), "tests/fixture.sql").db;
        const std::string lanternProse =
            queryText(ex, "SELECT prose FROM description WHERE entity = 4");

        // Turn 1: examine the lantern on the floor of the stone hall.
        CHECK(runTurn(ex, "examine lantern").outcome == TurnOutcome::Ticked);

        std::string text;
        const HttpTransport fakeEx = [&text](const std::string&) {
            return cannedResponse(text);
        };

        // Canon prose verbatim inside the model's own connective prose → the
        // gate accepts, and the prose survives into what the player reads.
        text = "You lift it toward the light. " + lanternProse +
               " The glass is still warm.";
        {
            const auto out = aiRender(ex, 1, fakeEx);
            CHECK(out.has_value());
            CHECK(contains(*out, lanternProse));
        }

        // Item 20: a paraphrase is rejected by clause f, so aiRender returns
        // nullopt and the dispatch falls through to the template — which for
        // this turn is the description row itself. The turn still prints.
        text = "A battered brass lantern, more or less as it was described.";
        CHECK(!aiRender(ex, 1, fakeEx).has_value());
        CHECK(render(ex, 1) == lanternProse + "\n");

        // The same, end to end: with AI off, runTurn's output is non-empty and
        // carries the prose — the same output the clause-f fallback lands on.
        // (runTurn hardwires the production transport, so this is the honest
        // dispatch coverage, exactly as the timeout path above notes.)
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");
        unsetenv("ANTHROPIC_API_KEY");
        unsetenv("TEXTWORLD_AI");
        // The output is the WRAPPED template render plus the band, not the raw
        // prose: runTurn wraps to terminal width, so a description longer than
        // the width is broken across lines by the display layer — exactly as
        // room canon already is. render() itself still emits it verbatim.
        const int exWidth = detectWidth();
        const TurnResult t = runTurn(ex, "examine lantern");  // turn 2
        CHECK(t.outcome == TurnOutcome::Ticked);
        CHECK(!t.output.empty());
        CHECK(render(ex, 2) == lanternProse + "\n");
        CHECK(t.output ==
              indentProse(wrapProse(render(ex, 2), proseWidth(exWidth)), kProseIndent) +
                  composeBand(ex, exWidth));
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
// Defined with the conversation tests further down. Forward-declared here
// because main() runs the live smokes FIRST, before the hermetic env unset.
static int64_t placeCharacterIn(Db& db, int64_t room, const char* handle,
                                const char* name, const char* kind = "character");

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // --- THE NAMED REGRESSION SET (spec validation item 6, REQ-NPCTALK-15).
    //
    // REQ-NPCTALK-14 makes the resolver's absolute "make no tool call" rule
    // CONDITIONAL, and that is the highest-risk change the conversation brick
    // contains. Its failure is QUIET: `take the key` classified as speech reads
    // as the character ignoring you. So the regression that matters is the
    // twelve prior verbs re-run WITH a character present.
    //
    // This eight-phrasing set is the CONTRACT REQ-NPCTALK-15a names. Everything
    // outside it is tuned against real play and is not specified. If one of
    // these resolves to Say, tune the prompt's ordering sentence and re-run —
    // bounded to this fixed set, never opened into a tuning session against
    // invented phrasings.
    //
    // The two standing rules of this smoke still hold: mechanical invariants
    // only, and a nullopt is a clean fallback rather than a failure.
    {
        // combat_fixture, not fixture: this set needs a hostile to attack, a
        // catalogued spell to cast, and the motive vocabulary a catalog
        // character is admitted against. A candle and a grimoire are added by
        // hand, because two of the eight phrasings name nouns no fixture
        // carries — and a phrasing whose noun does not exist is rejected by the
        // gate for the WRONG reason, which would make it pass vacuously.
        const TempDbFile talkPath("textworld_resolve_live_say.db");
        Db talk = openWorld(talkPath.string(), "tests/combat_fixture.sql").db;
        const int64_t room =
            queryInt(talk, "SELECT container FROM location WHERE entity = "
                           "(SELECT entity FROM player LIMIT 1)");
        talk.exec("INSERT INTO entities(id) VALUES (401), (402)");
        talk.exec("INSERT INTO name(entity, value) VALUES "
                  "(401, 'candle'), (402, 'grimoire')");
        talk.exec("INSERT INTO description(entity, prose) VALUES "
                  "(401, 'A guttering candle.'), (402, 'A plain grimoire.')");
        talk.exec(("INSERT INTO location(entity, container) VALUES "
                   "(401, " + std::to_string(room) + "), (402, " +
                   std::to_string(room) + ")").c_str());
        talk.exec("INSERT INTO portable(entity) VALUES (402)");
        placeCharacterIn(talk, room, "gate_warden", "gate warden", "character");

        // Every noun the set names really is recognised, so a nullopt below is
        // the model declining rather than the gate rejecting an absent noun.
        for (const char* noun : {"key", "candle", "grimoire"}) {
            CHECK(lookupNoun(talk, noun) != 0);
        }
        CHECK(!lookupSpell(talk, "ward").empty());

        // The payload really does supply present_character now — otherwise
        // every assertion below would pass vacuously, testing the OLD rule.
        {
            const nlohmann::json p = nlohmann::json::parse(
                buildResolveContext(talk, "hello").payload, nullptr, false);
            CHECK(p.contains("present_character"));
        }

        std::fprintf(stderr,
                     "RESOLVER LIVE SMOKE: the eight-phrasing say/action "
                     "regression set (REQ-NPCTALK-15)...\n");

        struct Phrasing {
            const char* line;
            Verb expected;
        };
        const std::vector<Phrasing> actions = {
            {"take the key", Verb::Take},   {"go north", Verb::Go},
            {"attack", Verb::Attack},       {"cast ward", Verb::Cast},
            {"read grimoire", Verb::Read},  {"examine candle", Verb::Examine},
            {"look", Verb::Look},           {"inventory", Verb::Inventory},
        };
        for (const Phrasing& p : actions) {
            const std::optional<Action> a = aiResolve(talk, p.line);
            if (!a) continue;  // clean decline: a correct fallback, not a failure
            // The one that matters: an ACTION must never become speech.
            CHECK(a->verb != Verb::Say);
            CHECK(a->verb == p.expected);
        }

        // The positive direction: a greeting and a question are speech — or a
        // clean decline, which is still not a wrong action.
        for (const char* line : {"hello there", "who are you"}) {
            const std::optional<Action> a = aiResolve(talk, line);
            if (!a) continue;
            CHECK(a->verb == Verb::Say);
            // The player's own words, from the engine's copy (REQ-NPCTALK-6).
            CHECK(a->text == line);
        }

        // Item 7, live: with NO character present the same question still
        // produces no tool call, so behaviour is unchanged from before this
        // brick. A resolved action would be the regression; Say is impossible.
        {
            const std::optional<Action> a = aiResolve(db, "who are you");
            if (a) CHECK(a->verb != Verb::Say);
        }
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
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;
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
    // Sourced from the BAND since REQ-POLISH-5 made it the one place the exits
    // are stated; the invariant this asserts (displayed == walkable) is
    // unchanged, only where the displayed set is read from.
    auto parseExits = [](const std::string& band) {
        std::vector<std::string> out;
        const size_t p = band.find(" Exits ");
        if (p == std::string::npos) return out;
        const size_t listStart = band.find_first_not_of(' ', p + 7);
        if (listStart == std::string::npos) return out;
        size_t e = band.find('\n', listStart);
        if (e == std::string::npos) e = band.size();
        const std::string list = band.substr(listStart, e - listStart);
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
    const std::vector<std::string> shown =
        parseExits(stripSgr(composeBand(db, 200, TermStyle{false, false})));
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
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "drop lantern").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 3);
    }

    // Reopen the same file: everything preserved.
    {
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(origPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(copyPath.string(), "tests/fixture.sql").db;
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
        Db orig = openWorld(origPath.string(), "tests/fixture.sql").db;
        CHECK(queryInt(orig, "SELECT value FROM meta WHERE key = 'turn'") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 3") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 4") == 3);

        Db copy = openWorld(copyPath.string(), "tests/fixture.sql").db;
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

        Db db = openWorld(worldPath.string(), "tests/fixture.sql", setting.string()).db;

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

        Db db = openWorld(worldPath.string(), "tests/fixture.sql", missing).db;

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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql", setting.string()).db;

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
        Db db2 = openWorld(w2.string(), "tests/fixture.sql", missing).db;

        const nlohmann::json j2 =
            nlohmann::json::parse(buildArchitectContext(db2, 1, "up"));
        CHECK(j2.size() == 4);
        CHECK(j2["setting"] == "");
        CHECK(j2["origin_name"] == "stone hall");
        CHECK(j2["direction"] == "up");
    }
}

// --- Brick 4 Step 2, REQ-BARD-ARCH-1/-2/-3/-4/-17: the context gains `focus`
// (meta.bard_focus, verbatim) and `story_options` (the caller-supplied menu, as
// handle + blurb + motive BLURB), both OMITTED ENTIRELY when empty. The
// four-key payload of testArchitectContext above is the byte-identity proof and
// passes unmodified; this test owns everything the menu adds. No ids, no tier,
// no seeded flag, and no motive KEY may appear. Pure SELECTs; no network. ---
static void testArchitectStoryContext() {
    const TempDbFile worldPath("textworld_arch_story_context.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // Three tiers. The origin (room 1) is the seed at distance 0, so the
    // PROSPECTIVE room one hop out is at distance 1 and its menu is exactly
    // {t0_scribe, t1_ink} — tier 2 stays out, which is what makes "the menu is
    // the prospective room's" assertable rather than assumed.
    writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                      "a scribe who has not left the annex in years",
                      "curiosity", 0);
    writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                      "a dark stain, still wet", "secrecy", 1);
    writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                      "a pilgrim arrived long after the gate closed",
                      "homesickness", 2);

    const std::string focusLine =
        "The scribe's annex has gone quiet; someone has been at the ink.";
    writeBardFocus(db, focusLine);

    const std::vector<CatalogChoice> menu = eligibleCatalogForNewRoom(db, 1);
    CHECK(menu.size() == 2);

    const std::string payload = buildArchitectContext(db, 1, "east", menu);
    const nlohmann::json j = nlohmann::json::parse(payload);

    // (d) both present → the four base fields plus two.
    CHECK(j.is_object());
    CHECK(j.size() == 6);
    CHECK(j.contains("focus"));
    CHECK(j.contains("story_options"));

    // (b) the focus line is verbatim — not summarized, not truncated here.
    CHECK(j["focus"] == focusLine);

    // (a) exactly the eligible handles, in catalog.id order, each with its
    // blurb and its motive BLURB.
    const nlohmann::json& opts = j["story_options"];
    CHECK(opts.is_array());
    CHECK(opts.size() == 2);
    CHECK(opts[0]["handle"] == "t0_scribe");
    CHECK(opts[0]["blurb"] == "a scribe who has not left the annex in years");
    CHECK(opts[0]["motive"] == "wants to know something they have not been told");
    CHECK(opts[1]["handle"] == "t1_ink");
    CHECK(opts[1]["blurb"] == "a dark stain, still wet");
    CHECK(opts[1]["motive"] == menu[1].motiveBlurb);
    // The order is the menu's order, entry for entry.
    for (size_t i = 0; i < menu.size(); ++i) {
        CHECK(opts[i]["handle"] == menu[i].handle);
        CHECK(opts[i]["blurb"] == menu[i].blurb);
        CHECK(opts[i]["motive"] == menu[i].motiveBlurb);
    }

    // (c) the model-facing shape, structurally: THREE keys per option and no
    // fourth can slip in, plus no id key anywhere (REQ-BARD-ARCH-17).
    for (const auto& o : opts) {
        CHECK(o.is_object());
        CHECK(o.size() == 3);
        CHECK(o.contains("handle") && o.contains("blurb") && o.contains("motive"));
    }
    checkNoIdKeys(j);
    CHECK(!contains(payload, "tier"));
    CHECK(!contains(payload, "seeded"));
    CHECK(!contains(payload, "catalog"));
    // The motive KEY is never on the wire — only its blurb (REQ-BARD-SEL-8).
    CHECK(!contains(payload, "curiosity"));
    CHECK(!contains(payload, "secrecy"));
    // Nor the in-world name, nor the tier-2 entry the prospective room cannot
    // reach.
    CHECK(!contains(payload, "cloistered scribe"));
    CHECK(!contains(payload, "t2_pilgrim"));

    // (d) the three remaining corners of REQ-BARD-ARCH-4, driven off ONE world
    // by varying only what is supplied.
    {
        // menu only → 5 keys.
        writeBardFocus(db, "");
        const nlohmann::json m =
            nlohmann::json::parse(buildArchitectContext(db, 1, "east", menu));
        CHECK(m.size() == 5);
        CHECK(!m.contains("focus"));
        CHECK(m.contains("story_options"));

        // focus only → 5 keys.
        writeBardFocus(db, focusLine);
        const nlohmann::json f =
            nlohmann::json::parse(buildArchitectContext(db, 1, "east", {}));
        CHECK(f.size() == 5);
        CHECK(f.contains("focus"));
        CHECK(!f.contains("story_options"));

        // neither → EXACTLY today's four fields. This is the omit-when-empty
        // rule doing the work REQ-BARD-ARCH-15 rests on.
        writeBardFocus(db, "");
        const std::string bare = buildArchitectContext(db, 1, "east", {});
        const nlohmann::json n = nlohmann::json::parse(bare);
        CHECK(n.size() == 4);
        CHECK(!n.contains("focus"));
        CHECK(!n.contains("story_options"));
        CHECK(!contains(bare, "story_options"));
        CHECK(!contains(bare, "focus"));
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

// --- Brick 4 Step 3, REQ-BARD-ARCH-5/-6/-7: the OPTIONAL `story` object on the
// create_room schema. Its `handle` is a schema-enforced enum of the supplied
// catalog handles and nothing else; both its fields are required WITHIN it,
// while `story` itself stays out of the tool's top-level required set. An empty
// menu means the substring "story" never appears in the body at all — the
// non-regression claim, worded as spec test 6 words it. Pure string→string. ---
static void testArchitectStoryRequestBody() {
    const ScopedModelEnv guard;
    unsetenv("TEXTWORLD_MODEL");

    const std::string payload =
        "{\"setting\":\"grey stone\",\"direction\":\"east\"}";
    const std::vector<std::string> handles = {"t0_scribe", "t1_ink"};

    // --- handles supplied: the story object, in full ---
    {
        const nlohmann::json j = nlohmann::json::parse(
            buildArchitectRequestBody(payload, {}, handles));
        const nlohmann::json& schema = j["tools"][0]["input_schema"];
        const nlohmann::json& props = schema["properties"];

        // name, description, exits, story — and no fifth without an enemy menu.
        CHECK(props.size() == 4);
        CHECK(props.contains("story"));
        CHECK(props["story"]["type"] == "object");

        // The enum is EXACTLY the supplied handles, in order. This is the whole
        // of "the model cannot invent a handle".
        CHECK(props["story"]["properties"]["handle"]["enum"] ==
              nlohmann::json(handles));
        CHECK(props["story"]["properties"]["handle"]["type"] == "string");
        CHECK(props["story"]["properties"]["description"]["type"] == "string");
        CHECK(props["story"]["properties"].size() == 2);

        // Both required WITHIN story: a half-filled story is a schema error.
        CHECK(props["story"]["required"] ==
              nlohmann::json::array({"handle", "description"}));

        // But story is NOT top-level required — at most one, possibly none
        // (REQ-BARD-ARCH-7).
        CHECK(schema["required"] == nlohmann::json::array({"name", "description"}));

        // No id, tier, or seeded flag reaches the schema (REQ-BARD-ARCH-17).
        checkNoIdKeys(props["story"]);
    }

    // --- enemy AND story together: independent fields, both present ---
    {
        const nlohmann::json j = nlohmann::json::parse(buildArchitectRequestBody(
            payload, {"a squat grey-skinned thing"}, handles));
        const nlohmann::json& props = j["tools"][0]["input_schema"]["properties"];
        CHECK(props.size() == 5);  // name, description, exits, enemy, story
        CHECK(props.contains("enemy"));
        CHECK(props.contains("story"));
        // Still neither is required — a room may carry both, one, or neither.
        CHECK(j["tools"][0]["input_schema"]["required"] ==
              nlohmann::json::array({"name", "description"}));
    }

    // --- empty handles: the substring is ABSENT from the body entirely
    // (REQ-BARD-ARCH-6, spec test 6). Asserted on the raw string, not the
    // parsed object, because "no story field" and "no story text anywhere in
    // the tool schema" are different claims and the spec makes the stronger one.
    {
        const std::string body = buildArchitectRequestBody(payload, {}, {});
        const nlohmann::json j = nlohmann::json::parse(body);
        CHECK(j["tools"][0]["input_schema"]["properties"].size() == 3);
        CHECK(!j["tools"][0]["input_schema"]["properties"].contains("story"));
        // The system prompt is stripped before the substring check: the story
        // CLAUSE lives in kArchitectPrompt unconditionally (Step 4), which the
        // header states and testArchitectStoryPrompt owns. What must be absent
        // is the story SCHEMA.
        CHECK(j["tools"].dump().find("story") == std::string::npos);
        CHECK(j["messages"].dump().find("story") == std::string::npos);
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

// Brick 4 fixture: the same canned create_room shape, with an arbitrary JSON
// value spliced in as `story`. Deliberately takes a raw nlohmann::json rather
// than (handle, description) strings, because the gate's whole job here is
// leniency and half of the arms that must be proven are stories that are NOT
// well-formed objects — a null, a string, an object missing a field, an object
// carrying a spurious id. A typed helper could not express them.
static HttpResponse cannedCreateRoomWithStory(
    const std::string& name, const std::string& description,
    const nlohmann::json& story, const std::string& enemy = "") {
    nlohmann::json input;
    input["name"] = name;
    input["description"] = description;
    if (!enemy.empty()) input["enemy"] = enemy;
    // Note: a null `story` is still WRITTEN — "the key is present and useless"
    // is one of the drop reasons, distinct from the key being absent.
    input["story"] = story;

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

// A well-formed story object, the shape the schema requires.
static nlohmann::json storyInput(const std::string& handle,
                                 const std::string& description) {
    return nlohmann::json{{"handle", handle}, {"description", description}};
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

// --- Brick 4 Step 5, REQ-BARD-ARCH-8/-9/-17: `story` extraction is LENIENT. A
// malformed story is DROPPED, never a rejection — the room survives every
// single one of these arms with its name, description and exits intact, which
// is the whole of REQ-BARD-ARCH-8. A story missing EITHER field is dropped IN
// FULL (REQ-BARD-ARCH-9): a handle with no instance prose has nothing to write
// into the world, so half a story is no story. `name` and `description` remain
// the only strict clauses. Pure function of the response; no DB, no network. ---
static void testArchitectStoryGate() {
    // Every arm below asserts this same shape: the ROOM survived. Factored so
    // the eight drop arms cannot quietly stop checking it.
    auto checkRoomIntact = [](const std::optional<RoomProposal>& p) {
        CHECK(p.has_value());
        CHECK(p->name == "hall");
        CHECK(p->description == "A long room of grey stone.");
        CHECK(p->exits == std::vector<std::string>({"north"}));
    };

    // A canned response carrying `story` AND a declared exit, so every drop arm
    // proves the exits half is untouched too.
    auto withStory = [](const nlohmann::json& story) {
        nlohmann::json input;
        input["name"] = "hall";
        input["description"] = "A long room of grey stone.";
        input["exits"] = nlohmann::json::array({"north"});
        input["story"] = story;
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
    };

    // --- positive 1: a well-formed story survives, both fields TRIMMED ---
    {
        auto p = validateRoomProposal(
            withStory(storyInput("  t0_scribe  ",
                                 "\tA scribe bends over a ledger here.\n")));
        checkRoomIntact(p);
        CHECK(p->story.handle == "t0_scribe");
        CHECK(p->story.description == "A scribe bends over a ledger here.");
    }

    // --- the eight drop reasons. Each yields a NON-NULL proposal with an empty
    // story handle and the room fully intact (spec test 15). ---

    // (1) absent — the common case, and the ONLY one that is silent.
    {
        auto p = validateRoomProposal(cannedCreateRoom("hall",
                                                       "A long room of grey stone.",
                                                       {"north"}));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
        CHECK(p->story.description.empty());
    }
    // (2) present but not an object: a bare string, the likeliest malformation
    // (the model answering the enum instead of the object).
    {
        auto p = validateRoomProposal(withStory("t0_scribe"));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
    }
    // (2b) and a null, which is "the key is there and says nothing".
    {
        auto p = validateRoomProposal(withStory(nlohmann::json()));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
    }
    // (3) handle missing entirely.
    {
        auto p = validateRoomProposal(
            withStory(nlohmann::json{{"description", "A scribe bends here."}}));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
        // Dropped IN FULL (REQ-BARD-ARCH-9) — the description does not survive
        // its handle.
        CHECK(p->story.description.empty());
    }
    // (4) handle present but not a string.
    {
        auto p = validateRoomProposal(withStory(
            nlohmann::json{{"handle", 7}, {"description", "A scribe bends here."}}));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
        CHECK(p->story.description.empty());
    }
    // (5) handle blank after trim.
    {
        auto p = validateRoomProposal(
            withStory(storyInput("   \t ", "A scribe bends here.")));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
        CHECK(p->story.description.empty());
    }
    // (6) description missing entirely — the mirror of (3), and the arm that
    // matters most: a handle alone would otherwise place a nameless entry.
    {
        auto p =
            validateRoomProposal(withStory(nlohmann::json{{"handle", "t0_scribe"}}));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
        CHECK(p->story.description.empty());
    }
    // (7) description present but not a string.
    {
        auto p = validateRoomProposal(withStory(
            nlohmann::json{{"handle", "t0_scribe"}, {"description", 42}}));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
    }
    // (8) description blank after trim.
    {
        auto p = validateRoomProposal(withStory(storyInput("t0_scribe", "  \n ")));
        checkRoomIntact(p);
        CHECK(p->story.handle.empty());
    }

    // --- positive 2: a spurious id / catalog field inside the story object is
    // IGNORED, not read (REQ-BARD-ARCH-17). StoryProposal has two fields and no
    // third for an id to land in — the model can put none on the wire.
    {
        nlohmann::json story = storyInput("t0_scribe", "A scribe bends here.");
        story["id"] = 999;
        story["catalog"] = 17;
        story["tier"] = 3;
        auto p = validateRoomProposal(withStory(story));
        checkRoomIntact(p);
        CHECK(p->story.handle == "t0_scribe");
        CHECK(p->story.description == "A scribe bends here.");
    }

    // --- positive 3: a story ALONGSIDE an enemy — both extracted, neither
    // shadowing the other (REQ-BARD-ARCH-7).
    {
        auto p = validateRoomProposal(cannedCreateRoomWithStory(
            "hall", "A long room of grey stone.",
            storyInput("t0_scribe", "A scribe bends here."),
            "a squat grey-skinned thing"));
        CHECK(p.has_value());
        CHECK(p->enemyBlurb == "a squat grey-skinned thing");
        CHECK(p->story.handle == "t0_scribe");
        CHECK(p->story.description == "A scribe bends here.");
    }

    // --- and the strict clauses stay strict: a perfectly good story cannot
    // rescue a blank name or description.
    CHECK(!validateRoomProposal(cannedCreateRoomWithStory(
        "", "prose", storyInput("t0_scribe", "A scribe bends here."))));
    CHECK(!validateRoomProposal(cannedCreateRoomWithStory(
        "hall", "  ", storyInput("t0_scribe", "A scribe bends here."))));
}

// --- Step 6, REQ-ARCH-9 (write) / REQ-ARCH-6: writeGeneratedRoom mints a room
// with engine-chosen id, both reciprocal exits, and one 'generated' event, all
// inside the caller's transaction. Uses an UNMAPPED invertible direction (east
// from the hall — base.sql maps only north/south) so no exit PK collides. ---
static void testWriteGeneratedRoom() {
    const TempDbFile worldPath("textworld_write_gen_room.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

// --- Brick 4 Step 7, REQ-BARD-ARCH-1/-2/-6/-15: the SYNC path, end to end.
// architectGenerate reads the story menu ONCE and spends it twice — blurbs into
// the context, handles into the tool schema's enum.
//
// This test drives architectGenerate rather than buildArchitectRequestBody in
// isolation, and that is forced rather than stylistic: once the menu is HOISTED
// into the caller (micro-decision 1), "the enum is the PROSPECTIVE room's menu
// and not the origin's" stops being a property of the builder and becomes a
// property of the CALL SITE. Only an end-to-end drive with a body-capturing
// transport can observe it. No network. ---
static void testArchitectStoryGenerate() {
    const int64_t player = 3;

    // Room 1 is the seed, at distance 0. So the ORIGIN's own menu stops at tier
    // 0 while the PROSPECTIVE room one hop east reaches tier 1 — the two differ
    // by exactly one entry, and a menu built against the wrong room cannot pass.
    // t2_pilgrim is beyond both, so "the whole catalog" cannot pass either.
    auto seedCatalog = [](Db& db) {
        writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                          "a scribe who has not left the annex in years",
                          "curiosity", 0);
        writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                          "a dark stain, still wet", "secrecy", 1);
        writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                          "a pilgrim arrived long after the gate closed",
                          "homesickness", 2);
    };

    // --- spec test 10: the enum is the PROSPECTIVE menu ---------------------
    {
        const TempDbFile worldPath("textworld_arch_story_gen.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const std::string focusLine = "Someone has been at the ink.";
        writeBardFocus(db, focusLine);

        // The three menus, computed independently of the code under test.
        std::vector<std::string> originMenu;
        for (const char* kind : {"character", "beat"}) {
            for (const CatalogChoice& c : eligibleCatalog(db, 1, kind)) {
                originMenu.push_back(c.handle);
            }
        }
        std::vector<std::string> prospective;
        for (const CatalogChoice& c : eligibleCatalogForNewRoom(db, 1)) {
            prospective.push_back(c.handle);
        }
        CHECK(originMenu == std::vector<std::string>{"t0_scribe"});
        CHECK(prospective == (std::vector<std::string>{"t0_scribe", "t1_ink"}));

        std::string sentBody;
        HttpTransport recorder = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("scriptorium", "A low room of slanted desks.");
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, recorder));
        db.commit();

        const nlohmann::json j = nlohmann::json::parse(sentBody);
        const nlohmann::json& props = j["tools"][0]["input_schema"]["properties"];
        CHECK(props.contains("story"));
        const nlohmann::json& en = props["story"]["properties"]["handle"]["enum"];

        // It IS the prospective menu...
        CHECK(en == nlohmann::json(prospective));
        // ...and it is neither the origin's (one short) nor the whole catalog
        // (one long). Both negatives are asserted, because the prospective set
        // sits BETWEEN them and only stating all three pins it.
        CHECK(en != nlohmann::json(originMenu));
        CHECK(en.size() == 2);
        CHECK(std::find(en.begin(), en.end(), "t1_ink") != en.end());
        CHECK(std::find(en.begin(), en.end(), "t2_pilgrim") == en.end());

        // The CONTEXT agrees with the schema, entry for entry — the whole point
        // of reading the menu once and passing it to both (REQ-BARD-ARCH-1).
        const nlohmann::json ctx =
            nlohmann::json::parse(j["messages"][0]["content"].get<std::string>());
        CHECK(ctx["story_options"].size() == 2);
        CHECK(ctx["story_options"][0]["handle"] == "t0_scribe");
        CHECK(ctx["story_options"][1]["handle"] == "t1_ink");
        CHECK(ctx["focus"] == focusLine);
        // And no id reached either surface.
        checkNoIdKeys(ctx);
    }

    // --- end to end: one call produces a room AND a placed entity ----------
    {
        const TempDbFile worldPath("textworld_arch_story_gen_e2e.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const std::string prose = "A dark stain has dried across the flagstones.";

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             storyInput("t1_ink", prose));
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();
        CHECK(calls == 1);  // still one transport call, no retries

        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        const int64_t entity =
            queryInt(db, "SELECT entity FROM catalog WHERE handle = 't1_ink'");
        CHECK(entity != 0);
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(entity)).c_str()) == newRoom);
        CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                             std::to_string(entity)).c_str()) == "spilled ink");
        CHECK(queryText(db, ("SELECT prose FROM description WHERE entity = " +
                             std::to_string(entity)).c_str()) == prose);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 1);
    }

    // --- REQ-BARD-ARCH-15: an EMPTY catalog leaves the body exactly as it was.
    // testArchitectGenerate above passes unmodified and is the real proof; this
    // states the claim positively, on the wire, in the same world shape the
    // story arms use.
    {
        const TempDbFile worldPath("textworld_arch_story_gen_empty.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        // No catalog, no focus — a pre-bard world.
        std::string sentBody;
        HttpTransport recorder = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("scriptorium", "A low room of slanted desks.");
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, recorder));
        db.commit();

        const nlohmann::json j = nlohmann::json::parse(sentBody);
        CHECK(!j["tools"][0]["input_schema"]["properties"].contains("story"));
        CHECK(j["tools"].dump().find("story") == std::string::npos);
        const nlohmann::json ctx =
            nlohmann::json::parse(j["messages"][0]["content"].get<std::string>());
        CHECK(ctx.size() == 4);
        CHECK(!ctx.contains("story_options"));
        CHECK(!ctx.contains("focus"));
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

    // When set, returned INSTEAD of a room proposal. The bard's worker consumes
    // wake responses, not rooms, and it needs the identical blocking behavior —
    // so the timing machinery is shared and only the payload differs.
    std::optional<HttpResponse> canned;

    // Blocks until release() is called. Records the call and flags any
    // overlapping entry — the serial-worker assertion (REQ-PREGEN-6,
    // REQ-BARD-WAKE-13).
    HttpResponse operator()(const std::string&) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++calls;
            ++inside;
            if (inside > 1) concurrentEntry = true;
            cv.wait(lock, [this] { return released; });
            --inside;
            if (canned) return *canned;
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    // The fixture stands the player (3) in the stone hall (1).
    CHECK(playerRoom(db) == 1);
    CHECK(playerRoom(db) ==
          queryInt(db, "SELECT container FROM location WHERE entity = 3"));
    // REQ-POLISH-16: with an EMPTY events table this is world creation, the one
    // launch where the player has never seen the room, so the paragraph prints.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 0);
    CHECK(contains(renderStartup(db), "vaulted hall of grey stone"));

    // It TRACKS the player: after a move it names the new room, and still
    // agrees with what startup would render. The events table is no longer
    // empty, so the startup render is now the room NAME and the band.
    const TurnResult r = runTurn(db, "go north");
    CHECK(r.outcome == TurnOutcome::Ticked);
    CHECK(playerRoom(db) == 2);
    CHECK(playerRoom(db) ==
          queryInt(db, "SELECT container FROM location WHERE entity = 3"));
    CHECK(contains(renderStartup(db), "garden"));
    CHECK(!contains(renderStartup(db), "overgrown walled garden"));

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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

// --- Brick 4 Step 6, REQ-BARD-ARCH-10/-11/-12/-13/-16: STORY MATERIALIZATION.
// The seam where catalog intent becomes world state. The model SELECTS a handle
// from the engine's menu and writes the instance prose; the engine re-checks the
// handle LIVE against the room that now exists and mints the entity. Driven
// through architectGenerate with canned responses — the menu is not yet threaded
// into the request here (that is Step 7), which is exactly the isolation this
// step wants: the commit-time re-check is proven on its own, independent of what
// was offered. No network. ---
static void testArchitectStoryPlacement() {
    const int64_t player = 3;

    // Seeds the same three-tier catalog into a combat_fixture world. Room 1 is
    // the seed at distance 0, so the room generated one hop east of it is at
    // distance 1 and its live menu is {t0_scribe, t1_ink} — t2_pilgrim is
    // OUT OF REACH, which is what makes "the re-check is a real gate" provable.
    auto seedCatalog = [](Db& db) {
        writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                          "a scribe who has not left the annex in years",
                          "curiosity", 0);
        writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                          "a dark stain, still wet", "secrecy", 1);
        writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                          "a pilgrim arrived long after the gate closed",
                          "homesickness", 2);
    };
    // The architect's INSTANCE prose — deliberately unlike the blurb, so
    // "the description is the architect's, not the catalog's" is assertable by
    // inequality rather than by inspection (REQ-BARD-ARCH-12).
    // NOTE: no apostrophe, deliberately — one arm below inlines this into a SQL
    // literal to prove no stray row carries it.
    const std::string instanceProse =
        "Ink has run off the lectern and dried in a long black tongue across "
        "the flagstones.";

    // --- spec test 11: an ELIGIBLE handle materializes. -------------------
    {
        const TempDbFile worldPath("textworld_arch_story_place.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const int64_t entities0 = queryInt(db, "SELECT COUNT(*) FROM entities");

        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             storyInput("t1_ink", instanceProse));
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();

        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        CHECK(newRoom > 15);

        // ONE entity beyond the room itself: the room and the story entry.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entities0 + 2);

        // The catalog row is LATCHED to the minted entity (REQ-BARD-STORE-13).
        const int64_t entity =
            queryInt(db, "SELECT entity FROM catalog WHERE handle = 't1_ink'");
        CHECK(entity != 0);

        // The parser noun is catalog.name — never the handle, never the blurb.
        CHECK(queryText(db, ("SELECT value FROM name WHERE entity = " +
                             std::to_string(entity)).c_str()) == "spilled ink");

        // The description is the ARCHITECT's instance prose, and is EXPLICITLY
        // not the blurb: the blurb is selection text the model already saw, and
        // writing it into the world would put the menu on the page.
        const std::string prose =
            queryText(db, ("SELECT prose FROM description WHERE entity = " +
                           std::to_string(entity)).c_str());
        CHECK(prose == instanceProse);
        CHECK(prose != "a dark stain, still wet");

        // It is IN the new room.
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(entity)).c_str()) == newRoom);

        // Exactly ONE materialized event, carrying the handle as its detail.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 1);
        CHECK(queryText(db,
                        "SELECT detail FROM events WHERE verb = 'materialized'") ==
              "t1_ink");

        // And the room was made normally alongside it.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'generated'") == 1);
    }

    // --- spec test 12: an OFF-MENU handle places nothing. -----------------
    // Two flavors in one world, because they fail at different gates: a handle
    // that is not in the catalog at all, and one that IS but is out of reach at
    // this distance (t2_pilgrim, tier 2, in a room at distance 1). The second
    // is the one that matters — it proves the re-check is the eligibility
    // menu and not a mere existence check.
    for (const std::string& handle : {std::string("no_such_handle"),
                                      std::string("t2_pilgrim")}) {
        const TempDbFile worldPath("textworld_arch_story_offmenu.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const int64_t entities0 = queryInt(db, "SELECT COUNT(*) FROM entities");

        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             storyInput(handle, instanceProse));
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));  // the room is STILL made
        db.commit();

        CHECK(queryInt(
                  db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'") > 15);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entities0 + 1);  // room only
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE entity IS NOT NULL") == 0);
        // No stray entity carries the prose the model wrote — only the room's
        // own description was added.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM description WHERE prose = '" +
                            instanceProse + "'").c_str()) == 0);
    }

    // --- spec test 13: an entry materialized BETWEEN snapshot and commit. ---
    // The live re-check's reason for existing (REQ-BARD-ARCH-11): a pregen
    // candidate may be many turns old, and the entry it chose may have been
    // placed elsewhere since. It must resolve to 0 — not mint a SECOND copy of
    // a one-of-a-kind entry.
    {
        const TempDbFile worldPath("textworld_arch_story_stale.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);

        // Materialize t1_ink somewhere else first, exactly as another room's
        // commit would have.
        const int64_t catalogId =
            queryInt(db, "SELECT id FROM catalog WHERE handle = 't1_ink'");
        db.begin();
        const int64_t elsewhere =
            placeCatalogEntry(db, catalogId, 2, "A stain on the corridor flags.", player);
        db.commit();
        CHECK(elsewhere != 0);
        const int64_t entities0 = queryInt(db, "SELECT COUNT(*) FROM entities");

        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             storyInput("t1_ink", instanceProse));
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();

        // The room is made; NO second entity is minted; the latch still points
        // at the first placement, in the room it was actually placed in.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entities0 + 1);
        CHECK(queryInt(db, "SELECT entity FROM catalog WHERE handle = 't1_ink'") ==
              elsewhere);
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(elsewhere)).c_str()) == 2);
        // Still exactly one materialized event — the first one.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 1);
    }

    // --- spec test 14: an enemy AND a story in one proposal — both placed. ---
    // The two halves of Phase 2 are independent, and the story is placed AFTER
    // the enemy (REQ-BARD-ARCH-10).
    {
        const TempDbFile worldPath("textworld_arch_story_both.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const std::string goblinBlurb = queryText(
            db, "SELECT blurb FROM bestiary WHERE archetype = 'goblin_grunt'");

        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoomWithStory(
                "scriptorium", "A low room of slanted desks, and something in them.",
                storyInput("t1_ink", instanceProse), goblinBlurb);
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();

        const int64_t newRoom = queryInt(
            db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");
        // One hostile AND one materialized story entry, in the same room.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM hostile h JOIN location l "
                            "ON l.entity = h.entity WHERE l.container = " +
                            std::to_string(newRoom)).c_str()) == 1);
        const int64_t entity =
            queryInt(db, "SELECT entity FROM catalog WHERE handle = 't1_ink'");
        CHECK(entity != 0);
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(entity)).c_str()) == newRoom);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 1);
    }

    // --- spec test 15: a MALFORMED story still creates the room. -----------
    // Inherited from the gate (testArchitectStoryGate owns the eight arms); the
    // point here is that leniency survives all the way to canon rather than
    // stopping at the struct.
    {
        const TempDbFile worldPath("textworld_arch_story_malformed.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        const int64_t entities0 = queryInt(db, "SELECT COUNT(*) FROM entities");

        HttpTransport fake = [&](const std::string&) {
            // A handle with no instance prose: dropped IN FULL at the gate.
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             nlohmann::json{{"handle", "t1_ink"}});
        };
        db.begin();
        CHECK(architectGenerate(db, 1, "east", player, fake));
        db.commit();

        CHECK(queryInt(
                  db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'") > 15);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entities0 + 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'materialized'") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE entity IS NOT NULL") == 0);
    }

    // --- REQ-BARD-ARCH-13: story placement is OUTSIDE the Phase-1 catch. ----
    //
    // Nothing in the spec's other 16 items would fail if this call were moved
    // INSIDE architectGenerate's try — the room would still be made and the
    // story would still not be placed. The difference is the FAILURE MODE: a DB
    // fault during placement must reach runTurn's rollback as an EngineError,
    // not be downgraded to a silent wall. This arm is the only thing that
    // distinguishes the two.
    //
    // ON THE INJECTION, which is NOT the plan's `DROP TABLE location`. That
    // would not have been surgical: systems.cpp:19 reads the player's container
    // from `location` to find the room BEFORE generation is even reached, so a
    // dropped table faults the turn either way and the arm would pass whether
    // or not the call sits outside the catch — a vacuous test. A BEFORE INSERT
    // trigger is surgical for the same intent: moveEntity is an UPDATE
    // (mutations.cpp:105), writeGeneratedRoom writes no location row at all
    // (rooms have no container), and the proposal carries no enemy, so
    // placeCatalogEntry (mutations.cpp:712) is the ONLY insert this turn can
    // make. The control arm below proves that claim rather than asserting it.
    //
    // AND THE TURN IS DRIVEN THROUGH runTurn VIA A PRE-GENERATED CANDIDATE,
    // because only runTurn owns the tick transaction whose rollback is the
    // thing under test, and runTurn has no transport seam to inject a fake
    // into. An injected candidate makes the turn commit-only: pregenAcquire
    // returns it, Phase 2 runs, and NO network call is ever constructed
    // (testPregenCommit proves the zero-call property of this path). It also
    // means this arm covers the pregen path's commit, which is where a stale
    // story is most likely to be found.
    {
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        const ScopedEnvVar aiGuard("TEXTWORLD_AI");
        const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        unsetenv("TEXTWORLD_PREGEN");
        pregenRefreshEnabledForTest();
        pregenResetForTest();
        architectResetPregenOccupancyForTest();

        const TempDbFile worldPath("textworld_arch_story_fault.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'up', NULL)");
        db.exec(
            "CREATE TRIGGER story_fault BEFORE INSERT ON location "
            "BEGIN SELECT RAISE(ABORT, 'injected location fault'); END");

        // CONTROL: the same world, the same trigger, a candidate with NO story.
        // The turn ticks normally — which is what proves the trigger fires on
        // story placement and on nothing else, rather than on the turn at large.
        {
            RoomProposal clean;
            clean.name = "antechamber";
            clean.description = "A bare stone antechamber.";
            pregenInjectReadyForTest(1, "east", clean, /*snapshotTurn=*/1);

            const int64_t turnBefore =
                queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
            CHECK(runTurn(db, "go east").outcome == TurnOutcome::Ticked);
            CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") ==
                  turnBefore + 1);
            CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") > 15);
        }

        // THE ARM. Back to the seed, and walk the OTHER latent exit with a
        // candidate that carries a story.
        db.exec("UPDATE location SET container = 1 WHERE entity = 3");
        architectResetPregenOccupancyForTest();

        RoomProposal storied;
        storied.name = "scriptorium";
        storied.description = "A low room of slanted desks.";
        storied.story.handle = "t1_ink";
        storied.story.description = instanceProse;
        pregenInjectReadyForTest(1, "up", storied, /*snapshotTurn=*/1);

        const int64_t turnBefore =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t roomsBefore = queryInt(db, "SELECT COUNT(*) FROM room");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
        const int64_t entitiesBefore = queryInt(db, "SELECT COUNT(*) FROM entities");

        const TurnResult r = runTurn(db, "go up");

        // EngineError and a FULL rollback — never Ticked with a wall.
        CHECK(r.outcome == TurnOutcome::EngineError);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM room") == roomsBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM entities") == entitiesBefore);
        // Specifically: the room the fault interrupted was rolled back too.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM exits WHERE room = 1 "
                           "AND direction = 'up' AND dest IS NOT NULL") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE entity IS NOT NULL") == 0);

        // The connection survives: drop the trigger and a normal turn ticks,
        // proving the error path left no transaction dangling.
        db.exec("DROP TRIGGER story_fault");
        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        pregenResetForTest();
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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

// --- Brick 4 Step 9, REQ-BARD-ARCH-12 (the noun half): NOUNS MUST EXIST. The
// whole reason this brick exists — a materialized story entry is a real world
// entity with a real parser noun, not a sentence in a room description. What
// this test can prove today, it proves; what it cannot, it names. ---
static void testArchitectStoryNoun() {
    const int64_t player = 3;
    const TempDbFile worldPath("textworld_arch_story_noun.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // A lowercase catalog name, so the noun is typeable — see the second
    // caveat at the bottom of this test for why that is a fixture choice and
    // not a guarantee.
    writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                      "a dark stain, still wet", "secrecy", 1);

    const std::string instanceProse =
        "A dark stain has dried in a long tongue across the flagstones, and "
        "the spilled ink still smells of iron.";
    HttpTransport fake = [&](const std::string&) {
        return cannedCreateRoomWithStory(
            "scriptorium",
            "A low room of slanted desks. A dark stain has dried across the "
            "flagstones by the near wall.",
            storyInput("t1_ink", instanceProse));
    };
    db.begin();
    CHECK(architectGenerate(db, 1, "east", player, fake));
    db.commit();

    const int64_t newRoom = queryInt(
        db, "SELECT dest FROM exits WHERE room = 1 AND direction = 'east'");

    // 1. THE NOUN RESOLVES. lookupNoun is the shared rule BOTH the deterministic
    //    parser and the AI resolver's validation gate go through
    //    (lookup.hpp:16), so this is the real recognition path, not a
    //    test-only query.
    const int64_t entity = lookupNoun(db, "spilled ink");
    CHECK(entity != 0);
    // It is the entity the catalog latched onto — the same one, not a namesake.
    CHECK(queryInt(db, "SELECT entity FROM catalog WHERE handle = 't1_ink'") ==
          entity);

    // 2. IT IS IN THE ROOM.
    CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                        std::to_string(entity)).c_str()) == newRoom);

    // 3. ITS PROSE IS THE ARCHITECT'S INSTANCE PROSE, not the catalog blurb.
    CHECK(queryText(db, ("SELECT prose FROM description WHERE entity = " +
                         std::to_string(entity)).c_str()) == instanceProse);

    // 4. AND THE PLAYER'S SURFACE FOR IT IS THE ROOM'S CANON DESCRIPTION — the
    //    thing the Step 4 prompt clause asks the model to write the entry into.
    //    Asserted as a property of the room's prose, since that is where a
    //    player actually meets it.
    const std::string roomProse =
        queryText(db, ("SELECT prose FROM description WHERE entity = " +
                       std::to_string(newRoom)).c_str());
    CHECK(contains(roomProse, "stain"));

    // WHAT THIS DOES NOT ASSERT, AND WHY. Spec test 17 words this as "the player
    // can examine it." That is not implementable today: there is no examine verb
    // (the ISA is Look/Go/Take/Drop/Inventory/Wait/Quit/Attack/Cast/Read/Spells,
    // action.hpp:11, and Look takes no subject), and BOTH the narrator's facts
    // (prose.cpp:105) and the resolver's scope payload (nlresolve.cpp:166)
    // enumerate only PORTABLE entities in a room — a story entity is neither
    // portable nor hostile, so it reaches neither. The noun genuinely exists and
    // resolves; it cannot yet be acted on. Closing that is a separate brick
    // (scenery-in-scope + an examine verb), deliberately out of this brick's
    // modules.
    //
    // A SECOND caveat on the same claim: lookupNoun matches EXACTLY, and its
    // header notes names are stored lowercase (lookup.hpp:16). writeCatalogEntry
    // only TRIMS `name` (mutations.cpp:581) — it does not lowercase it. A bard
    // that authors "Scorched Lectern" therefore mints a noun the player cannot
    // type. This fixture controls the case, so this test passes either way; the
    // exposure is real bard output, and the fix belongs in brick 1's
    // writeCatalogEntry, not here. The line below is that exposure, made
    // executable rather than merely described — it PASSES today, and it is the
    // bug.
    {
        const TempDbFile w2("textworld_arch_story_noun_case.db");
        Db db2 = openWorld(w2.string(), "tests/combat_fixture.sql").db;
        writeCatalogEntry(db2, "beat", "t1_lectern", "Scorched Lectern",
                          "a reading stand burned down one side", "secrecy", 1);
        HttpTransport fake2 = [&](const std::string&) {
            return cannedCreateRoomWithStory(
                "scriptorium", "A low room of slanted desks.",
                storyInput("t1_lectern", "The lectern is burned down one side."));
        };
        db2.begin();
        CHECK(architectGenerate(db2, 1, "east", player, fake2));
        db2.commit();

        // The entity exists...
        const int64_t burned =
            queryInt(db2, "SELECT entity FROM catalog WHERE handle = 't1_lectern'");
        CHECK(burned != 0);
        // ...and the player cannot type its name. This assertion is documenting
        // a live defect, not endorsing it: when brick 1 lowercases catalog.name,
        // THIS line is the one that will fail, and that failure is the fix
        // landing.
        CHECK(lookupNoun(db2, "scorched lectern") == 0);
        CHECK(lookupNoun(db2, "Scorched Lectern") == burned);
    }
}

// --- Brick 4 Step 8, REQ-BARD-ARCH-14/-15: the PREGEN path. The scheduler
// snapshots the story menu into the job on the MAIN thread, exactly as it does
// the enemy menu, and commit is untouched — architectCommitProposal is already
// the single Phase 2 both paths run, so Step 6 made story placement
// path-identical STRUCTURALLY. This test is what turns that into a proof. ---
static void testArchitectStoryPregen() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar pregenGuard("TEXTWORLD_PREGEN");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    const int64_t player = 3;

    auto seedCatalog = [](Db& db) {
        writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                          "a scribe who has not left the annex in years",
                          "curiosity", 0);
        writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                          "a dark stain, still wet", "secrecy", 1);
        writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                          "a pilgrim arrived long after the gate closed",
                          "homesickness", 2);
    };

    // --- (a) the job carries the snapshot: context blurbs AND schema handles.
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_arch_story_queue.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        writeBardFocus(db, "Someone has been at the ink.");
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        architectQueuePregen(db, 1);
        CHECK(pregenStateOf(1, "east") == PregenState::Queued);

        // Capture what the WORKER actually receives, the way
        // testArchitectQueuePregen does: let the tick run the queued job with a
        // body-recording fake. Asserting on the emitted body rather than on the
        // PregenJob struct is the point — a handle that never reaches the wire
        // is not a snapshot.
        std::string sentBody;
        HttpTransport recorder = [&](const std::string& body) {
            sentBody = body;
            return cannedCreateRoom("scriptorium", "A low room of slanted desks.");
        };
        const PregenResult r = pregenAcquire(1, "east", &recorder);
        CHECK(r.outcome == PregenOutcome::RanQueued);

        const nlohmann::json body = nlohmann::json::parse(sentBody);

        // The context payload is exactly what buildArchitectContext produces
        // for the PROSPECTIVE menu — story_options and focus included.
        const std::string expected =
            buildArchitectContext(db, 1, "east", eligibleCatalogForNewRoom(db, 1));
        CHECK(body["messages"][0]["content"] == expected);
        const nlohmann::json ctx =
            nlohmann::json::parse(body["messages"][0]["content"].get<std::string>());
        CHECK(ctx.contains("story_options"));
        CHECK(ctx["story_options"].size() == 2);
        CHECK(ctx.contains("focus"));

        // And the snapshotted HANDLES became the schema enum — the prospective
        // menu, not the origin's, on the background path too.
        std::vector<std::string> prospective;
        for (const CatalogChoice& c : eligibleCatalogForNewRoom(db, 1)) {
            prospective.push_back(c.handle);
        }
        const nlohmann::json& en =
            body["tools"][0]["input_schema"]["properties"]["story"]["properties"]
                ["handle"]["enum"];
        CHECK(en == nlohmann::json(prospective));
        CHECK(en.size() == 2);
        CHECK(std::find(en.begin(), en.end(), "t2_pilgrim") == en.end());
    }

    // --- (b) spec test 16: PATH EQUIVALENCE. ------------------------------
    // The same canned proposal, committed once through the pregen hit path and
    // once through a direct synchronous architectGenerate, into two worlds built
    // from the same fixture. The rows and the events must be identical — not
    // similar. This is REQ-BARD-ARCH-14 stated as an equality rather than as a
    // claim about shared code.
    const std::string storyProse =
        "A dark stain has dried in a long tongue across the flagstones.";

    // The story rows a world ended up with, as one comparable string. Covers
    // exactly what the spec names: entity, name, description, location, and the
    // catalog latch. Entity IDS are deliberately EXCLUDED — the two worlds mint
    // in different orders and identical ids are not the claim; identical FACTS
    // are.
    auto storySnapshot = [](Db& db) {
        return queryText(db,
                         "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                         "SELECT c.handle || '|' || n.value || '|' || d.prose || "
                         "'|' || (SELECT value FROM name WHERE entity = l.container) "
                         "AS s FROM catalog c "
                         "JOIN name n ON n.entity = c.entity "
                         "JOIN description d ON d.entity = c.entity "
                         "JOIN location l ON l.entity = c.entity "
                         "WHERE c.entity IS NOT NULL ORDER BY c.handle)");
    };
    // The materialization events, likewise id-free: verb + the handle detail.
    auto storyEvents = [](Db& db) {
        return queryText(db,
                         "SELECT IFNULL(group_concat(s, ';'), '') FROM ("
                         "SELECT verb || '|' || IFNULL(detail, '') AS s FROM events "
                         "WHERE verb = 'materialized' ORDER BY id)");
    };

    std::string syncStory, syncEvents, syncCanon;
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_arch_story_sync.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoomWithStory("scriptorium",
                                             "A low room of slanted desks.",
                                             storyInput("t1_ink", storyProse));
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);
        syncStory = storySnapshot(db);
        syncEvents = storyEvents(db);
        syncCanon = canonSnapshot(db);
        CHECK(!syncStory.empty());  // the sync path really did place it
    }
    {
        pregenResetForTest();
        architectResetPregenOccupancyForTest();
        const TempDbFile worldPath("textworld_arch_story_pregen.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        seedCatalog(db);
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");

        // The SAME proposal, as a pre-generated candidate — the exact object the
        // gate would have produced from the canned response above.
        RoomProposal candidate;
        candidate.name = "scriptorium";
        candidate.description = "A low room of slanted desks.";
        candidate.story.handle = "t1_ink";
        candidate.story.description = storyProse;
        pregenInjectReadyForTest(1, "east", candidate, /*snapshotTurn=*/1);

        int calls = 0;
        HttpTransport fake = [&](const std::string&) {
            ++calls;
            return cannedCreateRoom("wrong", "should never be built");
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);

        CHECK(calls == 0);  // no network on the commit turn
        // Row for row and event for event, the same world.
        CHECK(storySnapshot(db) == syncStory);
        CHECK(storyEvents(db) == syncEvents);
        // And the rest of canon too — the story did not perturb anything else.
        CHECK(canonSnapshot(db) == syncCanon);
    }

    pregenResetForTest();
    architectResetPregenOccupancyForTest();
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    const ScopedEnvVar profGuard("TEXTWORLD_LOG_LEVEL");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    const int64_t player = 3;

    std::vector<std::string> captured;
    profileSetSink(captureTwprof(captured));
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    profileRefreshEnabled();

    // Every kind=pregen record in the capture, parsed.
    auto pregenRecords = [&captured]() {
        std::vector<std::map<std::string, std::string>> out;
        for (const std::string& line : captured) {
            auto kv = parseProfileRecord(twprofPayload(line));
            if (kv.at("kind") == "pregen") out.push_back(std::move(kv));
        }
        return out;
    };
    // True iff a stage=generate record was emitted.
    auto sawGenerateStage = [&captured]() {
        for (const std::string& line : captured) {
            const auto kv = parseProfileRecord(twprofPayload(line));
            if (kv.at("kind") == "stage" && kv.at("stage") == "generate") return true;
        }
        return false;
    };

    // (a) HIT — age_turns only, and NO generate stage (no synchronous call ran).
    {
        pregenResetForTest();
        captured.clear();
        const TempDbFile worldPath("textworld_pregen_rec_hit.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
    unsetenv("TEXTWORLD_LOG_LEVEL");
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
    const ScopedEnvVar profGuard("TEXTWORLD_LOG_LEVEL");
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    profileRefreshEnabled();

    std::vector<std::string> captured;
    profileSetSink(captureTwprof(captured));

    const int64_t player = 3;

    // --- walking a latent exit emits exactly one generate stage, nested in
    // tick, and no network happened (the transport is canned). ---
    {
        const TempDbFile worldPath("textworld_profile_generate.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        HttpTransport fake = [&](const std::string&) {
            return cannedCreateRoom("crypt", "A cold undercroft of grey stone.");
        };
        tickT(db, Action{Verb::Go, 0, "east"}, fake, player);

        CHECK(capturedStages(captured) == std::vector<std::string>({"generate"}));
        const auto kv = parseProfileRecord(twprofPayload(captured.front()));
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        db.exec("INSERT INTO exits(room, direction, dest) VALUES (1, 'east', NULL)");
        HttpTransport err = [&](const std::string&) {
            HttpResponse r;
            r.transportError = true;  // Phase-1 failure → wall
            return r;
        };
        tickT(db, Action{Verb::Go, 0, "east"}, err, player);

        CHECK(capturedStages(captured) == std::vector<std::string>({"generate"}));
        CHECK(parseProfileRecord(twprofPayload(captured.front())).at(
                  "nested_in") == "tick");
        CHECK(queryText(db, "SELECT detail FROM events ORDER BY id DESC LIMIT 1") ==
              "You can't go that way.");
        // Latent row untouched — still retryable.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM exits WHERE room = 1 AND direction = 'east' "
                       "AND dest IS NULL") == 1);
    }

    profileSetSink({});
    unsetenv("TEXTWORLD_LOG_LEVEL");
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

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

    // The prose may not promise a creature the engine did not place. The
    // setting names the invaders and tells the model they may be met abroad,
    // so without this the model writes goblins into rooms it was never offered
    // an enemy for - and the player finds nothing to attack. Signs that they
    // passed through are still allowed; a creature standing here is not.
    CHECK(contains(p, "Name no creature you did not place"));
    CHECK(contains(p, "never say a creature is HERE unless you placed it"));
    CHECK(contains(p, "offers no enemy field, this room holds no creature"));
    // It is the exit rule's twin, and both must survive together.
    CHECK(contains(p, "name no opening you did not declare"));
}

// --- Brick 4 Step 4, design Decision 2: the story clause. STRUCTURE ONLY, by
// substring, exactly as testArchitectPrompt above — prompt QUALITY is judged
// against real play, never in the suite, so this must never become a
// tune-and-retry loop. The clause exists because the schema alone cannot say
// that the blurb is SELECTION text and the description is ROOM text; without
// the last two assertions here the model has a field and no statement of what
// to put in it. ---
static void testArchitectStoryPrompt() {
    const std::string p = kArchitectPrompt;

    // Conditional, like the enemy clause: offered or absent, never assumed.
    CHECK(contains(p, "\"story\" field"));
    CHECK(contains(p, "story.handle"));
    CHECK(contains(p, "story.description"));
    // The enum is closed and the model may decline.
    CHECK(contains(p, "exactly one of its listed values"));
    CHECK(contains(p, "Never invent a handle"));
    CHECK(contains(p, "omit story entirely"));
    // Design Decision 2: the catalog supplies WHO, the architect supplies HOW
    // IT LOOKS HERE. These two are the reason the clause exists.
    CHECK(contains(p, "it is not the prose"));
    CHECK(contains(p, "you write the prose"));
    // And it must surface to the player, not merely exist in the database.
    CHECK(contains(p, "let it show in the room description"));

    // The enemy clause is untouched beside it — appending must not have
    // rewritten what was already there.
    CHECK(contains(p, "\"enemy\" field"));
    CHECK(contains(p, "Never invent an enemy"));
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
// --- Terminal visual polish (specs/terminal-visual-polish.md) ---

// Step 2 / REQ-POLISH-1, -2: the prose wrap cap, driven directly so the truth
// table is exercised without a terminal. The indent is SUBTRACTED, which is
// what keeps the 20-column floor at 20 columns once indentProse has run.
static void testTermProseWidth() {
    CHECK(kProseMaxWidth == 66);
    CHECK(kProseIndent == 2);

    // Above the cap: the cap wins, minus the indent.
    CHECK(proseWidth(200) == 64);
    CHECK(proseWidth(80) == 64);
    CHECK(proseWidth(66) == 64);

    // Below the cap: the detected width wins, minus the indent.
    CHECK(proseWidth(40) == 38);
    CHECK(proseWidth(20) == 18);

    // The floor. detectWidth() never returns these, but proseWidth is pure and
    // must not answer with a zero or negative wrap width.
    CHECK(proseWidth(1) == 18);
    CHECK(proseWidth(0) == 18);
    CHECK(proseWidth(-5) == 18);

    // The property that matters: wrap width plus indent never exceeds the
    // terminal, at every width from the floor upward.
    for (int w = kMinWidth; w <= 120; ++w) {
        CHECK(proseWidth(w) + kProseIndent <= w);
        CHECK(proseWidth(w) > 0);
    }
}

// Step 3 / REQ-POLISH-3, -3a: the indent. A normal line, a blank line between
// paragraphs, the trailing newline wrapProse preserves, and the empty string.
// Step 7 / REQ-POLISH-13: background colour, gated exactly as colorize is.
static void testTermBackgroundColor() {
    const TermStyle full{true, true};
    const TermStyle noColorAttrs{false, true};
    const TermStyle nothing{false, false};

    // The shape the research verified: escape bytes around the spaces, and the
    // spaces alone once they are stripped.
    CHECK(bgColorize("   ", Color::Red, full) == "\x1b[41m   \x1b[0m");
    CHECK(stripSgr(bgColorize("   ", Color::Red, full)) == "   ");
    CHECK(utf8Length(stripSgr(bgColorize("   ", Color::Red, full))) == 3);

    // REQ-UI-22: suppressed means the plain bytes, never an empty sequence and
    // never a bare reset. Both suppressed styles, and Color::None under colour.
    CHECK(bgColorize("   ", Color::Red, noColorAttrs) == "   ");
    CHECK(bgColorize("   ", Color::Red, nothing) == "   ");
    CHECK(bgColorize("   ", Color::Red, noColorAttrs).find('\x1b') ==
          std::string::npos);
    CHECK(bgColorize("   ", Color::Red, nothing).find('\x1b') == std::string::npos);
    CHECK(bgColorize("   ", Color::None, full) == "   ");

    // It is a COLOUR effect, so it follows style.color and not style.attrs —
    // the opposite of bolden, which survives NO_COLOR.
    CHECK(bgColorize("x", Color::Red, TermStyle{true, false}) == "\x1b[41mx\x1b[0m");

    // REQ-UI-19: all sixteen, each a DISTINCT parameter in the 40-47 / 100-107
    // ranges. No 256-colour, no truecolor — nothing here emits a 38 or 48.
    const Color all[] = {Color::Black,        Color::Red,         Color::Green,
                         Color::Yellow,       Color::Blue,        Color::Magenta,
                         Color::Cyan,         Color::White,       Color::BrightBlack,
                         Color::BrightRed,    Color::BrightGreen, Color::BrightYellow,
                         Color::BrightBlue,   Color::BrightMagenta,
                         Color::BrightCyan,   Color::BrightWhite};
    std::vector<std::string> seen;
    for (const Color c : all) {
        const std::string out = bgColorize("x", c, full);
        CHECK(stripSgr(out) == "x");
        CHECK(!contains(out, "\x1b[38"));
        CHECK(!contains(out, "\x1b[48"));
        for (const std::string& prior : seen) CHECK(prior != out);
        seen.push_back(out);
    }
    CHECK(seen.size() == 16);

    // No reverse video anywhere in the module (REQ-POLISH-12).
    CHECK(!contains(readFileBytes("src/term.cpp"), "\x1b[7m"));
}

static void testTermIndent() {
    CHECK(indentProse("hello", 2) == "  hello");

    // REQ-POLISH-3a: the blank line between paragraphs stays EMPTY. Two stray
    // spaces there would be a whitespace-only line, which check 3 forbids.
    CHECK(indentProse("one\n\ntwo", 2) == "  one\n\n  two");

    // A trailing newline survives as a trailing newline, not as a line of pad.
    CHECK(indentProse("line\n", 2) == "  line\n");
    CHECK(indentProse("", 2) == "");
    CHECK(indentProse("\n", 2) == "\n");

    // Zero and negative are the identity, so a caller that computes its indent
    // cannot accidentally shift text by a negative amount.
    CHECK(indentProse("hello", 0) == "hello");
    CHECK(indentProse("hello", -3) == "hello");

    // The combination that matters: wrapped then indented, no line exceeds the
    // terminal, and no line is whitespace-only.
    const std::string prose =
        "The corridor runs on into the dark, and the lamps have all gone "
        "out.\n\nSomething moves at the far end of it.";
    for (const int w : {20, 40, 66, 80, 200}) {
        const std::string laid =
            indentProse(wrapProse(prose, proseWidth(w)), kProseIndent);
        size_t pos = 0;
        while (pos <= laid.size()) {
            const size_t nl = laid.find('\n', pos);
            const std::string line =
                laid.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            CHECK(static_cast<int>(utf8Length(line)) <= w);
            // No whitespace-only line anywhere (REQ-POLISH-3a).
            CHECK(line.empty() || line.find_first_not_of(' ') != std::string::npos);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
}

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

        const std::string band = composeBand(db, 60, kBandPlain);
        CHECK(contains(band, "-- stone hall "));
        CHECK(contains(band, " Exits    north"));
        CHECK(contains(band, " Objects  lantern"));

        // REQ-UI-14: the band repeats the room's NAME, never its description.
        CHECK(!contains(band, "vaulted hall of grey stone"));
        CHECK(!contains(band, "flagstones"));

        // REQ-POLISH-5: the room block no longer states either fact. The band
        // above is the one place both reach the player.
        const std::string block = renderRoomOf(db, 3, false);
        CHECK(contains(block, "vaulted hall of grey stone"));
        CHECK(!contains(block, "Exits"));
        CHECK(!contains(block, "You see"));
        CHECK(!contains(block, "lantern"));

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
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // 1. An empty room: no exits, no objects, no hostiles — just the player.
    db.exec("INSERT INTO entities(id) VALUES (99)");
    db.exec("INSERT INTO room(entity) VALUES (99)");
    db.exec("INSERT INTO name(entity, value) VALUES (99, 'empty vault')");
    db.exec("UPDATE location SET container = 99 WHERE entity = 3");
    golden("empty room", composeBand(db, 60, kBandPlain),
           "-- empty vault ---------------------------------------------\n"
           " You      HP: 12/12 [########]\n");

    // 2. A room with objects (the cell holds the wand).
    db.exec("UPDATE location SET container = 1 WHERE entity = 3");
    golden("objects", composeBand(db, 60, kBandPlain),
           "-- cell ----------------------------------------------------\n"
           " Exits    down, north\n"
           " Objects  wand\n"
           " You      HP: 12/12 [########]\n");

    // 3. One hostile (the corridor).
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    golden("one hostile", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8 [########]\n"
           " You      HP: 12/12 [########]  Stun: ready  Ward: ready\n");

    // 4. Three hostiles (the library swarm), one row each in entity order.
    db.exec("UPDATE location SET container = 11 WHERE entity = 3");
    golden("three hostiles", composeBand(db, 60, kBandPlain),
           "-- library -------------------------------------------------\n"
           " Exits    up\n"
           " Enemy    snapping folio  HP: 10/10 [########]\n"
           " Enemy    snapping folio  HP: 10/10 [########]\n"
           " Enemy    snapping folio  HP: 10/10 [########]\n"
           " You      HP: 12/12 [########]  Stun: ready  Ward: ready\n");

    // 5. Mid-telegraph: the loudest element in the band.
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    db.exec("INSERT INTO pending_strike(entity, damage, element) VALUES (7, 5, NULL)");
    golden("mid-telegraph", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8 [########]  [WINDING UP]\n"
           " You      HP: 12/12 [########]  Stun: ready  Ward: ready\n");
    db.exec("DELETE FROM pending_strike WHERE entity = 7");

    // 6. A barriered hostile (the armory's ironhide brute).
    db.exec("UPDATE location SET container = 9 WHERE entity = 3");
    golden("barrier", composeBand(db, 60, kBandPlain),
           "-- armory --------------------------------------------------\n"
           " Exits    down\n"
           " Enemy    ironhide brute  HP: 14/14 [########]  barrier\n"
           " You      HP: 12/12 [########]  Stun: ready  Ward: ready\n");

    // 7. A warded player: the state that decides whether a telegraphed strike
    // lands (REQ-UI-36).
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");
    db.exec("INSERT INTO status_effects(entity, kind, magnitude, remaining) "
            "VALUES (3, 'ward', 0, 1)");
    golden("player warded", composeBand(db, 60, kBandPlain),
           "-- corridor ------------------------------------------------\n"
           " Exits    east, south, up\n"
           " Objects  key\n"
           " Enemy    goblin grunt  HP: 8/8 [########]\n"
           " You      HP: 12/12 [########]  ward 1  Stun: ready  Ward:\n"
           "          ready\n");
}

// Color, applied per role (REQ-UI-19, -22, -24, -25). Runs AFTER the plain-text
// goldens above, so a failure here is unambiguously a color bug and never a
// layout bug.
static void testBandColor() {
    const TempDbFile worldPath("textworld_band_color_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    //
    // With ONE deliberate exception, REQ-POLISH-12's two-step degrade: a health
    // bar is ten background-coloured SPACES under colour and `[####....]`
    // without it, so the colourless bar is not the coloured bar stripped. Both
    // occupy the same ten columns (REQ-POLISH-10), which is the property that
    // matters, so the plain band's bars are mapped back to ten spaces and the
    // byte-for-byte identity is asserted on everything else.
    const auto barsToSpaces = [](const std::string& band) {
        std::string out;
        size_t i = 0;
        while (i < band.size()) {
            if (band[i] == '[' && i + kHealthBarWidth <= band.size() &&
                band[i + kHealthBarWidth - 1] == ']' &&
                band.find_first_not_of("#.", i + 1) == i + kHealthBarWidth - 1) {
                out.append(static_cast<size_t>(kHealthBarWidth), ' ');
                i += kHealthBarWidth;
                continue;
            }
            out += band[i];
            ++i;
        }
        return out;
    };
    for (const int width : {20, 40, 60, 80, 200}) {
        CHECK(stripSgr(composeBand(db, width, kBandColor)) ==
              barsToSpaces(composeBand(db, width, kBandPlain)));
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.exec("DELETE FROM player");
        const TurnResult r = runTurn(db, "look");
        // The turn still returns its message; the exception does NOT propagate.
        CHECK(!r.output.empty());
        CHECK(contains(r.output, "player entity"));
        CHECK(!contains(r.output, "-- "));  // no band, but no crash either
    }

    // --- REQ-POLISH-6 / -6a: the fallback exits line, and its own failure ---
    // Since REQ-POLISH-5 the band is the ONLY route the exits reach the player,
    // so a band failure must not silently take them with it. Spec check 7.
    {
        const TempDbFile worldPath("textworld_band_fallback_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

        std::vector<std::string> logLines;
        logSetSink([&logLines](const std::string& line) { logLines.push_back(line); });
        const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");
        setenv("TEXTWORLD_LOG_LEVEL", "info", 1);
        logRefreshLevel();

        // Make composeBand throw for a reason the FALLBACK does not share: the
        // fallback needs player, location and exits, and nothing else. Dropping
        // a table only the band reads separates the two paths, which the plan's
        // "no player row" case cannot — that denies the fallback its room too.
        db.exec("DROP TABLE barrier");
        bool threw = false;
        try {
            (void)composeBand(db, 80, kBandPlain);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);  // the premise of everything below

        const TurnResult r = runTurn(db, "look");
        CHECK(r.outcome == TurnOutcome::Ticked);
        // The narration is still there (the room NAME — the stone hall is
        // meta.start_room, so REQ-POLISH-17 applies from turn zero), and so are
        // the exits, as plain text at column 0 since the styled composition is
        // what just failed.
        CHECK(contains(r.output, "stone hall"));
        CHECK(contains(r.output, "Exits: north.\n"));
        CHECK(!contains(r.output, "-- "));  // no band

        // REQ-POLISH-6: a warn entry naming the failure, which this path used
        // to write nothing at all.
        bool warned = false;
        for (const std::string& line : logLines) {
            if (contains(line, "WARN") && contains(line, "band")) warned = true;
        }
        CHECK(warned);

        // REQ-POLISH-6a: now break the FALLBACK too. The turn still prints its
        // narration and the game continues — nothing in this spec may turn a
        // display failure into a failed turn.
        logLines.clear();
        db.exec("DROP TABLE exits");
        const TurnResult r2 = runTurn(db, "look");
        CHECK(r2.outcome == TurnOutcome::Ticked);
        CHECK(contains(r2.output, "stone hall"));
        CHECK(!contains(r2.output, "Exits"));
        CHECK(!contains(r2.output, "-- "));

        logSetSink({});
        logRefreshLevel();
    }

    // --- REQ-UI-30: narration and template prose are wrapped ---------------
    {
        const TempDbFile worldPath("textworld_band_wrap_tests.db");
        Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
        termSetWidthOverride(40);
        // FIRST sight of the garden, so the canon paragraph is what is wrapped.
        // A repeat `look` would print the room name alone (REQ-POLISH-17) and
        // make the length assertion below vacuous.
        const TurnResult r = runTurn(db, "go north");
        for (const std::string& line : splitOnNewline(r.output)) {
            CHECK(utf8Length(stripSgr(line)) <= 40);
        }
        // The room's canon prose is long enough that this is not vacuous.
        CHECK(contains(r.output, "overgrown walled garden"));
        CHECK(splitOnNewline(r.output).size() > 4);
        termSetWidthOverride(80);
    }
}

// Step 6 / REQ-POLISH-7, -7a: a refusal is dimmed, and still indented. Spec
// check 8, driven through runTurn so the assertion is on what the player sees.
// Step 8 / REQ-POLISH-9, -10, -10a, -12: the bar as a pure function. Spec
// checks 9 and 10, at the level where the arithmetic lives.
// Step 10 / REQ-POLISH-15's anchor: meta.start_room, the row the derivation in
// step 11 needs because the starting room never gets a `moved` event.
// Step 11 / REQ-POLISH-15: "have I been here", derived from the transcript.
// Driven against a hand-built events table so every clause is exercised
// directly rather than through a turn.
static void testRoomSeen() {
    const TempDbFile worldPath("textworld_room_seen_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    const int64_t start = queryInt(db, "SELECT value FROM meta WHERE key = 'start_room'");
    CHECK(start == 1);

    // The starting room is seen at turn 0, with no events at all — the whole
    // reason meta.start_room exists.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 0);
    CHECK(roomSeen(db, 1, 0));

    // A room with no `moved` event naming it is unseen.
    CHECK(!roomSeen(db, 2, 0));
    CHECK(!roomSeen(db, 2, 5));

    // A `moved` at an EARLIER turn makes it seen.
    db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
            "VALUES (3, 3, 'moved', 0, 2, NULL)");
    CHECK(roomSeen(db, 2, 4));
    CHECK(roomSeen(db, 2, 99));

    // A `moved` at the SAME turn does NOT: the arrival turn's own event must
    // not mark the room seen before the turn reporting it has printed.
    CHECK(!roomSeen(db, 2, 3));
    // Nor does one at a later turn.
    CHECK(!roomSeen(db, 2, 2));

    // A different verb naming the room does not count, and neither does a
    // `moved` naming a different room.
    db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
            "VALUES (3, 3, 'downed', 3, 7, NULL)");
    CHECK(!roomSeen(db, 7, 9));

    // READ-ONLY: twenty calls move neither the turn counter nor the event count.
    {
        const int64_t turnBefore =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");
        for (int i = 0; i < 20; ++i) {
            (void)roomSeen(db, 1, 5);
            (void)roomSeen(db, 2, 5);
            (void)roomSeen(db, 99, 5);
        }
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
    }

    // A world with NO start_room row answers from `moved` events alone and does
    // not throw — the pre-existing-world case REQ-POLISH-15's amendment names.
    {
        db.exec("DELETE FROM meta WHERE key = 'start_room'");
        CHECK(!roomSeen(db, 1, 0));   // the starting room reads as unseen
        CHECK(roomSeen(db, 2, 4));    // and the events still answer
    }
}

// Steps 12 + 13 / REQ-POLISH-14, -16, -17, -18: first sight. Spec checks 11
// (first three clauses), 12 and 13, driven through runTurn over the combat
// fixture so respawn is reachable.
// Step 14 / REQ-POLISH-14 (explicit request), REQ-POLISH-19, and the amendment
// to REQ-EXAMINE-7. Spec check 11's last clause.
static void testExamineRoom() {
    const TempDbFile worldPath("textworld_examine_room_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const std::string cellProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 1");
    const std::string cellName = queryText(db, "SELECT value FROM name WHERE entity = 1");

    // A `look` in the starting room prints its NAME alone (REQ-POLISH-17)...
    {
        const TurnResult r = runTurn(db, "look");
        CHECK(!contains(r.output, cellProse.substr(0, 30)));
    }

    // ...and `x <room name>` then prints the FULL paragraph. This is the clause
    // that makes REQ-POLISH-17 acceptable: the reread is always one command away.
    {
        const int64_t before = queryInt(db, "SELECT COUNT(*) FROM events "
                                            "WHERE verb = 'examined'");
        const TurnResult r = runTurn(db, "x " + cellName);
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(contains(r.output, cellProse.substr(0, 30)));
        // One `examined` event, naming the room — no new verb (REQ-POLISH-19).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'examined'") ==
              before + 1);
        CHECK(queryInt(db, "SELECT subject FROM events WHERE verb = 'examined' "
                           "ORDER BY id DESC LIMIT 1") == 1);
    }

    // The `examined` branch prints the description row verbatim, which after
    // REQ-POLISH-5 is byte-identical to what roomBlock emits for an UNSEEN room.
    // Confirmed rather than assumed, which is what lets that branch stay as it is.
    {
        const int64_t turn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(render(db, turn) == renderRoomOf(db, 3, false));
    }

    // A room the player is NOT in stays out of scope and keeps the existing
    // refusal — no new wording enters the game (REQ-EXAMINE-8).
    {
        const std::string other = queryText(db, "SELECT value FROM name WHERE entity = 2");
        const TurnResult r = runTurn(db, "x " + other);
        CHECK(contains(r.output, "You don't see that here."));
    }

    // REQ-POLISH-19: no new verb and no two-word command form. The reread is
    // the EXISTING Examine verb with the room's name as its single argument —
    // a name that happens to contain a space is an argument, not a second verb
    // word. Asserted behaviourally, and against the verb list itself.
    {
        CHECK(!contains(readFileBytes("src/action.hpp"), "Room"));

        // combat_fixture names room 1 "cell"; base.sql names it "dormitory
        // cell". The two-word case is the one that would tempt a two-word verb.
        const TempDbFile seedPath("textworld_examine_room_seed_tests.db");
        Db seeded = openWorld(seedPath.string(), "seed/base.sql").db;
        const std::string twoWord =
            queryText(seeded, "SELECT value FROM name WHERE entity = 1");
        CHECK(contains(twoWord, " "));  // the premise
        const std::optional<Action> a = parse(seeded, "x " + twoWord);
        CHECK(a.has_value());
        CHECK(a->verb == Verb::Examine);
        CHECK(a->subject == 1);
    }
}

static void testFirstSight() {
    const TempDbFile worldPath("textworld_first_sight_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const std::string cellProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 1");
    const std::string corridorProse =
        queryText(db, "SELECT prose FROM description WHERE entity = 2");
    CHECK(!cellProse.empty());
    CHECK(!corridorProse.empty());

    // Spec check 12 / REQ-POLISH-16: at world creation the events table is
    // empty, so the courtesy render prints the paragraph.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == 0);
    CHECK(contains(renderStartup(db), cellProse.substr(0, 30)));

    // REQ-POLISH-17: `look` in the starting room prints its NAME, not the
    // paragraph — the cell is meta.start_room, so it is seen from turn zero.
    {
        const TurnResult r = runTurn(db, "look");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(contains(r.output, "cell"));
        CHECK(!contains(r.output, cellProse.substr(0, 30)));
    }

    // REQ-POLISH-14: FIRST sight of the corridor prints its paragraph.
    {
        const TurnResult r = runTurn(db, "go north");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(contains(r.output, corridorProse.substr(0, 30)));
    }

    // A `look` there does not print it again.
    {
        const TurnResult r = runTurn(db, "look");
        CHECK(!contains(r.output, corridorProse.substr(0, 30)));
        CHECK(contains(r.output, "corridor"));
    }

    // Move away and back: it does not print on RETURN either.
    {
        CHECK(runTurn(db, "go south").outcome == TurnOutcome::Ticked);
        const TurnResult r = runTurn(db, "go north");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(!contains(r.output, corridorProse.substr(0, 30)));
        CHECK(contains(r.output, "corridor"));
    }

    // Spec check 12, the other half: relaunching against the SAME world file —
    // a non-empty events table — shows the room name and the band, no
    // paragraph. renderStartup is what a relaunch calls.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") > 0);
    {
        const std::string startup = renderStartup(db);
        CHECK(contains(startup, "corridor"));
        CHECK(!contains(startup, corridorProse.substr(0, 30)));
        CHECK(contains(startup, "-- "));  // and the band is still there
    }

    // Spec check 13 / REQ-POLISH-18: get downed, wake in the cell, NO paragraph.
    // Respawn writes `downed`, not `moved` (mutations.cpp:429), so this is the
    // case most likely to expose a wrong derivation — and the case
    // meta.start_room exists for, since no event would ever mark the cell seen.
    {
        db.exec("UPDATE health SET current = 1 WHERE entity = 3");
        bool downed = false;
        for (int i = 0; i < 20 && !downed; ++i) {
            const TurnResult r = runTurn(db, "wait");
            if (contains(r.output, "The world tips and goes black")) {
                downed = true;
                CHECK(!contains(r.output, cellProse.substr(0, 30)));
                CHECK(contains(r.output, "cell"));
            }
        }
        CHECK(downed);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'downed'") > 0);
    }
}

// Step 15 / REQ-POLISH-29, -30, -31: the title screen. Spec check 19.
// Step 17 / REQ-POLISH-21, -21a, -21b: the history file. The guard itself lives
// in main.cpp and is not linkable from here, so what this asserts is the
// vendored history API's own contract plus main.cpp's structure — the two
// things that decide whether history survives a bad exit.
// Step 19 / REQ-POLISH-26, -28, and the mechanism half of -25. Spec check 18.
// Everything here is OFFLINE: no network, no live call, no token.
static void testSpinner() {
    // Frames are captured rather than written to the terminal.
    std::mutex sinkMutex;
    std::string captured;
    Spinner::setSink([&](std::string_view text) {
        const std::lock_guard<std::mutex> lock(sinkMutex);
        captured += text;
    });
    const auto drain = [&] {
        const std::lock_guard<std::mutex> lock(sinkMutex);
        return captured;
    };
    const auto reset = [&] {
        const std::lock_guard<std::mutex> lock(sinkMutex);
        captured.clear();
    };

    const auto sleepFrames = [](int n) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(Spinner::frameIntervalMs() * n));
    };

    // REQ-POLISH-26: no terminal, no thread and no byte. The counter is what
    // makes "no thread starts" checkable rather than merely likely.
    {
        const int64_t before = Spinner::threadsStarted();
        {
            const Spinner spinner(TermStyle{false, false}, true);
            sleepFrames(4);
        }
        CHECK(Spinner::threadsStarted() == before);
        CHECK(drain().empty());
    }

    // NO_COLOR on a terminal keeps attrs, so the spinner still runs — it is not
    // a colour effect. TERM=dumb clears attrs and suppresses it.
    {
        const int64_t before = Spinner::threadsStarted();
        {
            const Spinner spinner(TermStyle{false, true}, true);
            sleepFrames(3);
        }
        CHECK(Spinner::threadsStarted() == before + 1);
        CHECK(!drain().empty());
        reset();
    }

    // REQ-POLISH-28: template mode starts no thread at all. There is no network
    // call there and nothing to wait for.
    {
        const int64_t before = Spinner::threadsStarted();
        {
            const Spinner spinner(TermStyle{true, true}, false);
            sleepFrames(4);
        }
        CHECK(Spinner::threadsStarted() == before);
        CHECK(drain().empty());
    }

    // REQ-POLISH-27: it erases itself completely. The destructor's clear is the
    // LAST thing written, and every byte it ever wrote is ASCII.
    {
        {
            const Spinner spinner(TermStyle{true, true}, true);
            sleepFrames(3);
        }
        const std::string out = drain();
        CHECK(!out.empty());
        CHECK(out.size() >= 3);
        CHECK(out.substr(out.size() - 3) == "\r \r");
        for (const char ch : out) {
            CHECK(static_cast<unsigned char>(ch) < 0x80);
        }
        // Frames are ASCII spinner characters, never braille or block drawing —
        // those are the widths a terminal is allowed to disagree about
        // (REQ-UI-29), and a half-erased column is what REQ-POLISH-27 forbids.
        for (const char ch : out) {
            CHECK(ch == '\r' || ch == ' ' || ch == '|' || ch == '/' ||
                  ch == '-' || ch == '\\');
        }
        reset();
    }

    // A spinner destroyed before its first frame writes NOTHING — not even the
    // clear. An unconditional erase would move the cursor on a turn that
    // returned instantly.
    {
        {
            const Spinner spinner(TermStyle{true, true}, true);
        }
        CHECK(drain().empty());
    }

    // The destructor does not sit through a whole frame interval: a turn that
    // returns in microseconds must not be delayed 100 ms by its own spinner.
    {
        const auto start = std::chrono::steady_clock::now();
        {
            const Spinner spinner(TermStyle{true, true}, true);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        CHECK(elapsed < Spinner::frameIntervalMs());
        reset();
    }

    // Two at once do not interfere: the state is per-object, not a file-static.
    {
        const int64_t before = Spinner::threadsStarted();
        {
            const Spinner a(TermStyle{true, true}, true);
            const Spinner b(TermStyle{true, true}, true);
            sleepFrames(3);
        }
        CHECK(Spinner::threadsStarted() == before + 2);
        reset();
    }

    Spinner::setSink({});

    // REQ-POLISH-25's mechanism is scoped to exactly the blocking call, and its
    // gates are checked in the CONSTRUCTOR, so there is no call site that can
    // forget one.
    {
        const std::string src = readFileBytes("src/loop.cpp");
        CHECK(contains(src, "const Spinner spinner(currentStyle(), aiNarrationEnabled())"));
        const std::string spin = readFileBytes("src/spinner.cpp");
        CHECK(contains(spin, "if (!style.attrs || !aiEnabled) return;"));
    }
}

static void testHistoryFile() {
    // REQ-POLISH-21: bounded, beside world.db, and gitignored.
    const std::string mainSrc = readFileBytes("src/main.cpp");
    CHECK(contains(mainSrc, ".textworld_history"));
    CHECK(contains(mainSrc, "linenoiseHistorySetMaxLen"));
    CHECK(contains(mainSrc, "linenoiseHistoryLoad"));
    CHECK(contains(mainSrc, "linenoiseHistoryAdd"));
    CHECK(contains(readFileBytes(".gitignore"), ".textworld_history"));

    // REQ-POLISH-21a: saved from a DESTRUCTOR, which is what reaches the quit
    // verb, EOF, and the fatal-error path alike. A save called at the bottom of
    // the loop would miss two of the three.
    CHECK(contains(mainSrc, "~HistoryGuard"));
    CHECK(contains(mainSrc, "linenoiseHistorySave"));
    {
        // And the guard is declared BEFORE the turn loop, so an exception
        // thrown inside the loop unwinds through it.
        const size_t guard = mainSrc.find("const HistoryGuard historyGuard");
        const size_t loop = mainSrc.find("while (true)");
        CHECK(guard != std::string::npos);
        CHECK(loop != std::string::npos);
        CHECK(guard < loop);
    }

    // REQ-POLISH-21b: an unusable path is not an error. The API reports failure
    // by return code, which is what the guard logs rather than throws on.
    {
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "textworld_history_tests";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        const std::string missing = (dir / "absent").string();
        CHECK(linenoiseHistoryLoad(missing.c_str()) != 0);  // reported, not thrown

        // A round trip through a usable path: what a second session loads.
        const std::string path = (dir / "history").string();
        linenoiseHistorySetMaxLen(500);
        CHECK(linenoiseHistoryAdd("look") == 1);
        CHECK(linenoiseHistoryAdd("go north") == 1);
        CHECK(linenoiseHistorySave(path.c_str()) == 0);
        const std::string saved = readFileBytes(path);
        CHECK(contains(saved, "look"));
        CHECK(contains(saved, "go north"));
        CHECK(linenoiseHistoryLoad(path.c_str()) == 0);

        // An UNWRITABLE path reports failure rather than throwing or aborting.
        const std::string unwritable = (dir / "nosuchdir" / "history").string();
        CHECK(linenoiseHistorySave(unwritable.c_str()) != 0);

        std::filesystem::remove_all(dir, ec);
    }
}

static void testTitleScreen() {
    const TempDbFile worldPath("textworld_title_tests.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;

    // The seed row exists, and it is a ROW — no DDL, no version bump.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = 'title_art'") == 1);
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") == 8);

    const std::string art = queryText(db, "SELECT value FROM meta WHERE key = 'title_art'");
    CHECK(!art.empty());

    // REQ-POLISH-30: plain ASCII only. No box drawing, no ambiguous-width
    // character, nothing that lets the terminal decide the column count.
    for (const char ch : art) {
        CHECK(static_cast<unsigned char>(ch) < 0x80);
        CHECK(ch == '\n' || (ch >= 0x20 && ch < 0x7f));
    }

    // Its natural width, measured rather than assumed.
    size_t widest = 0;
    for (const std::string& line : splitOnNewline(art)) {
        widest = std::max(widest, utf8Length(line));
    }
    CHECK(widest > 0);

    // At and above its natural width the art prints, and no line overflows.
    for (const int w : {static_cast<int>(widest), 80, 200}) {
        const std::string screen = titleScreen(db, w);
        CHECK(contains(screen, "#"));
        CHECK(screen.back() == '\n');
        for (const std::string& line : splitOnNewline(screen)) {
            CHECK(utf8Length(line) <= static_cast<size_t>(w));
        }
    }

    // REQ-POLISH-31: one column short of its natural width, and at REQ-UI-27's
    // 20-column floor, it degrades to the bare name — never wrapped into rubble.
    for (const int w : {static_cast<int>(widest) - 1, 20, 1}) {
        const std::string screen = titleScreen(db, w);
        CHECK(screen == "TextWorld\n");
        CHECK(!contains(screen, "#"));
    }

    // A world file created BEFORE this row existed takes the same path.
    {
        db.exec("DELETE FROM meta WHERE key = 'title_art'");
        CHECK(titleScreen(db, 200) == "TextWorld\n");
    }

    // REQ-POLISH-29: printed FIRST, before the template-mode notice and before
    // the startup render. Source order is what makes that structural.
    {
        const std::string src = readFileBytes("src/main.cpp");
        const size_t title = src.find("titleScreen(db");
        const size_t notice = src.find("AI narration off");
        const size_t startup = src.find("renderStartup(db)");
        CHECK(title != std::string::npos);
        CHECK(notice != std::string::npos);
        CHECK(startup != std::string::npos);
        CHECK(title < notice);
        CHECK(title < startup);
    }

    // No renderer, no font files, no runtime dependency (REQ-POLISH-29).
    CHECK(!contains(readFileBytes("CMakeLists.txt"), "figlet"));
    CHECK(!contains(readFileBytes("src/world.cpp"), "figlet"));
}

static void testWorldStartRoom() {
    // Derived from the seed's own location row, so it is right for every seed
    // and fixture without any of them naming a number.
    for (const char* seed : {"seed/base.sql", "tests/fixture.sql",
                             "tests/combat_fixture.sql"}) {
        const TempDbFile worldPath("textworld_start_room_tests.db");
        Db db = openWorld(worldPath.string(), seed).db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key = 'start_room'") == 1);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'start_room'") ==
              queryInt(db, "SELECT container FROM location WHERE entity = "
                           "(SELECT entity FROM player LIMIT 1)"));
        // All three happen to start the player in room 1 today. Asserted as the
        // derivation above, not as the number, so a fixture that moves the
        // player elsewhere still passes.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'start_room'") == 1);

        // REQ-POLISH-32: a ROW, not a shape. No DDL, no version bump.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'schema_version'") == 8);
    }

    // Written INSIDE initialize()'s transaction: a seed that throws leaves no
    // half-seeded world, and therefore no orphan start_room row either.
    {
        const TempDbFile worldPath("textworld_start_room_rollback_tests.db");
        bool threw = false;
        try {
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                              "seed/setting.txt",
                              {{"broken.txt", "handle: h\nname n\n\nbody\n"}})
                        .db;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        Db raw(worldPath.string());
        CHECK(queryInt(raw, "SELECT COUNT(*) FROM sqlite_master "
                            "WHERE type = 'table'") == 0);
    }
}

static void testBandHealthBar() {
    const TermStyle colored{true, true};
    const TermStyle plain{false, false};

    // The width of a bar's TEXT, in code points, once the escape bytes are gone.
    const auto barWidth = [](const std::vector<BandSpan>& spans) {
        size_t n = 0;
        for (const BandSpan& span : spans) n += utf8Length(span.text);
        return n;
    };
    const auto barText = [](const std::vector<BandSpan>& spans) {
        std::string out;
        for (const BandSpan& span : spans) out += span.text;
        return out;
    };

    const struct {
        int64_t current;
        int64_t max;
    } living[] = {{12, 12}, {6, 12}, {1, 12}, {1, 10}, {3, 4}, {11, 12}, {1, 100}};

    for (const auto& c : living) {
        const std::vector<BandSpan> lit = healthBarSpans(c.current, c.max,
                                                         Color::Red, colored);
        const std::vector<BandSpan> ascii = healthBarSpans(c.current, c.max,
                                                           Color::Red, plain);

        // REQ-POLISH-10 / -10a: exactly ten columns, in BOTH modes, whatever the
        // value shown — so a row's width never moves as health drops, and a bar
        // costs a row at most eleven columns including its separator.
        CHECK(barWidth(lit) == static_cast<size_t>(kHealthBarWidth));
        CHECK(barWidth(ascii) == static_cast<size_t>(kHealthBarWidth));

        // REQ-POLISH-9: spaces, never a block character, and every byte ASCII.
        for (const std::string& text : {barText(lit), barText(ascii)}) {
            for (const char ch : text) {
                CHECK(static_cast<unsigned char>(ch) < 0x80);
            }
        }
        CHECK(barText(lit) == std::string(kHealthBarWidth, ' '));

        // REQ-POLISH-12: the degraded form carries the delimiters and the fill
        // characters, and neither form ever emits reverse video.
        CHECK(barText(ascii).front() == '[');
        CHECK(barText(ascii).back() == ']');
        CHECK(contains(barText(ascii), "#"));

        // REQ-POLISH-10's floor: a LIVING body never shows an empty bar.
        for (const BandSpan& span : lit) {
            if (span.color == Color::Red) CHECK(!span.text.empty());
        }
    }

    // 1/10 is the floor case in both modes: one filled cell, not zero.
    {
        const std::vector<BandSpan> lit = healthBarSpans(1, 10, Color::Red, colored);
        CHECK(lit.size() == 2);
        CHECK(lit[0].text == " ");          // one filled column
        CHECK(lit[0].color == Color::Red);  // the row's own colour
        CHECK(lit[0].background);
        CHECK(utf8Length(lit[1].text) == 9);
        CHECK(lit[1].color == Color::BrightBlack);
        CHECK(barText(healthBarSpans(1, 10, Color::Red, plain)) == "[#.......]");
        // round(8 * 1/10) is 1 by the floor, not 1 by the rounding.
        CHECK(barText(healthBarSpans(1, 100, Color::Red, plain)) == "[#.......]");
    }

    // Full and empty.
    CHECK(barText(healthBarSpans(12, 12, Color::Red, plain)) == "[########]");
    CHECK(barText(healthBarSpans(0, 12, Color::Red, plain)) == "[........]");
    {
        // A dead body shows no filled column at all — the floor is for the
        // LIVING, which is the whole point of it.
        const std::vector<BandSpan> lit = healthBarSpans(0, 12, Color::Red, colored);
        CHECK(lit.size() == 1);
        CHECK(lit[0].color == Color::BrightBlack);
        CHECK(barWidth(lit) == static_cast<size_t>(kHealthBarWidth));
    }

    // Rounding, at the resolution each mode has.
    CHECK(barText(healthBarSpans(6, 12, Color::Red, plain)) == "[####....]");
    CHECK(utf8Length(healthBarSpans(6, 12, Color::Red, colored)[0].text) == 5);
    CHECK(utf8Length(healthBarSpans(3, 4, Color::Red, colored)[0].text) == 8);

    // max <= 0: no bar at all, and no division by zero.
    CHECK(healthBarSpans(0, 0, Color::Red, colored).empty());
    CHECK(healthBarSpans(0, 0, Color::Red, plain).empty());
    CHECK(healthBarSpans(5, -1, Color::Red, colored).empty());

    // current > max cannot overrun the width.
    CHECK(barWidth(healthBarSpans(20, 12, Color::Red, colored)) ==
          static_cast<size_t>(kHealthBarWidth));
    CHECK(barText(healthBarSpans(20, 12, Color::Red, plain)) == "[########]");

    // REQ-POLISH-12: no reverse-video step, anywhere in the module.
    CHECK(!contains(readFileBytes("src/band.cpp"), "\x1b[7m"));
    CHECK(!contains(readFileBytes("src/band.cpp"), "[7m"));

    // The spans survive the layout: a spans-only-spaces span used to be erased
    // by the tokenizer, which splits span text on spaces. It is now one atomic
    // token, so the bar reaches the line intact and measures ten columns there.
    {
        const BandRow row{"Enemy", healthBarSpans(6, 12, Color::Red, colored)};
        const std::string laid = layoutBand("cell", {row}, 60, colored);
        bool found = false;
        for (const std::string& line : splitOnNewline(laid)) {
            const std::string bare = stripSgr(line);
            if (bare.rfind(" Enemy", 0) == 0) {
                found = true;
                CHECK(utf8Length(bare) == static_cast<size_t>(kBandIndent) +
                                              kHealthBarWidth);
            }
        }
        CHECK(found);
    }
}

// Step 9 / REQ-POLISH-8, -8a, -10a, -11: the bar inside a real band row. Spec
// checks 9 and 10, driven through composeBand rather than the pure builder.
static void testBandBarsInRows() {
    const TempDbFile worldPath("textworld_band_bars_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    db.exec("UPDATE location SET container = 2 WHERE entity = 3");  // the corridor

    const TermStyle colored{true, true};
    const TermStyle plain{false, false};

    // The column count of every row, after stripSgr.
    const auto rowWidths = [](const std::string& band) {
        std::vector<size_t> widths;
        for (const std::string& line : splitOnNewline(band)) {
            if (!line.empty()) widths.push_back(utf8Length(stripSgr(line)));
        }
        return widths;
    };

    // Spec check 9: every row has an identical column count at full health and
    // at low health — the BAR's width never moves as health drops. Compared at
    // two values with the same digit count, so the only thing that could differ
    // is the bar; "12/12" against "1/12" would differ by the number itself,
    // which is not what this asserts.
    db.exec("UPDATE health SET current = 12, max = 12 WHERE entity = 3");
    db.exec("UPDATE health SET current = 10, max = 10 WHERE entity = 7");
    const std::vector<size_t> full = rowWidths(composeBand(db, 80, colored));
    db.exec("UPDATE health SET current = 11 WHERE entity = 3");
    db.exec("UPDATE health SET current = 10 WHERE entity = 7");
    const std::vector<size_t> hurt = rowWidths(composeBand(db, 80, colored));
    CHECK(full == hurt);
    CHECK(full.size() >= 4);  // header, exits, objects, enemy, you

    // And the same across the whole range, with the number's own width taken
    // out: a row's width is the bar's ten columns plus the digits, never more.
    {
        size_t baseline = 0;
        for (const int64_t hp : {12, 10, 6, 3, 1}) {
            const std::string sql = "UPDATE health SET current = " +
                                    std::to_string(hp) + " WHERE entity = 3";
            db.exec(sql.c_str());
            for (const std::string& line :
                 splitOnNewline(composeBand(db, 80, colored))) {
                const std::string bare = stripSgr(line);
                if (bare.rfind(" You", 0) != 0) continue;
                const size_t normalized =
                    utf8Length(bare) - std::to_string(hp).size();
                if (baseline == 0) baseline = normalized;
                CHECK(normalized == baseline);
            }
        }
        CHECK(baseline > 0);
        db.exec("UPDATE health SET current = 1 WHERE entity = 3");
    }

    // A living enemy at 1/10 shows ONE filled column, never zero.
    {
        db.exec("UPDATE health SET current = 1, max = 10 WHERE entity = 7");
        const std::string band = composeBand(db, 80, plain);
        CHECK(contains(band, "HP: 1/10 [#.......]"));
    }

    // REQ-POLISH-8: alongside the numbers, never instead of them.
    {
        const std::string band = composeBand(db, 80, plain);
        CHECK(contains(band, "HP: 1/10"));
        CHECK(contains(band, "HP: 1/12"));
    }

    // No bar byte is outside ASCII, in either mode.
    for (const TermStyle style : {colored, plain}) {
        const std::string band = stripSgr(composeBand(db, 80, style));
        for (const char ch : band) {
            if (static_cast<unsigned char>(ch) >= 0x80) {
                // The only non-ASCII the band may carry is a room NAME or an
                // enemy name from the world, never a framing or bar byte.
                CHECK(false);
            }
        }
    }

    // Spec check 10: health is readable with colour and without, the plain form
    // carries `[` and `#`, and neither contains reverse video.
    {
        const std::string lit = composeBand(db, 80, colored);
        const std::string ascii = composeBand(db, 80, plain);
        CHECK(contains(stripSgr(lit), "HP: 1/10"));
        CHECK(contains(ascii, "HP: 1/10"));
        CHECK(contains(ascii, "["));
        CHECK(contains(ascii, "#"));
        CHECK(!contains(lit, "\x1b[7m"));
        CHECK(!contains(ascii, "\x1b[7m"));
        // Under colour the bar is background SGR around spaces.
        CHECK(contains(lit, "\x1b[41m"));  // the hostile's, red
    }

    // A HEALTHY player's bar must still be a bar. The HP number is Color::None
    // while health is fine (REQ-UI-16), and bgColorize with Color::None emits
    // nothing — so a bar that borrowed the number's colour came out as ten plain
    // spaces. Found in a real fight; pinned here.
    {
        db.exec("UPDATE health SET current = 12, max = 12 WHERE entity = 3");
        const std::string lit = composeBand(db, 80, colored);
        CHECK(contains(lit, "\x1b[42m          \x1b[0m"));  // ten green columns
        // And when health IS low it turns yellow, agreeing with the number.
        db.exec("UPDATE health SET current = 3 WHERE entity = 3");
        const std::string low = composeBand(db, 80, colored);
        CHECK(contains(low, "\x1b[43m"));   // the bar
        CHECK(contains(low, "\x1b[33m"));   // the number
        CHECK(!contains(low, "\x1b[42m"));
        // The empty remainder is BrightBlack in both.
        CHECK(contains(low, "\x1b[100m"));
    }

    // REQ-POLISH-8a: cooldowns get NO bar. The spell spans are text only, so a
    // band in combat carries exactly as many bars as it has health rows.
    {
        db.exec("UPDATE cooldowns SET ready_turn = 999 WHERE entity = 3");
        const std::string ascii = composeBand(db, 80, plain);
        size_t bars = 0;
        for (size_t i = 0; i + 1 < ascii.size(); ++i) {
            if (ascii[i] == '[' && (ascii[i + 1] == '#' || ascii[i + 1] == '.')) ++bars;
        }
        CHECK(bars == 2);  // one enemy, one player — and none for the two spells
        CHECK(contains(ascii, "Stun: "));
    }

    // REQ-POLISH-10a: the bar costs a row at most eleven columns. Measured as
    // the difference the bar makes to the widest row, not asserted by eye.
    {
        const std::string with = stripSgr(composeBand(db, 200, plain));
        size_t widest = 0;
        for (const std::string& line : splitOnNewline(with)) {
            const size_t n = utf8Length(line);
            if (line.rfind(" Enemy", 0) == 0 && n > widest) widest = n;
        }
        // Remove each bar AND the single space REQ-POLISH-10 buys it. Anchored
        // on "HP: n/m" so a two-space gap here would leave one behind and the
        // arithmetic below would come out at 12, not 11 — this must not pass
        // whichever separator was used.
        std::string without = with;
        for (size_t i = 0; (i = without.find(" [")) != std::string::npos;) {
            CHECK(without[i - 1] != ' ');  // exactly ONE space before the bar
            without.erase(i, kHealthBarWidth + 1);
        }
        size_t widestWithout = 0;
        for (const std::string& line : splitOnNewline(without)) {
            const size_t n = utf8Length(line);
            if (line.rfind(" Enemy", 0) == 0 && n > widestWithout) widestWithout = n;
        }
        CHECK(widest - widestWithout == static_cast<size_t>(kHealthBarWidth) + 1);
    }
}

static void testErrorStyling() {
    const TempDbFile worldPath("textworld_error_style_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    // The band is styled too, so every assertion below reads the NARRATION —
    // everything above the header rule.
    const auto narrationOf = [](const std::string& out) {
        const size_t bandStart = out.find("-- ");
        CHECK(bandStart != std::string::npos);
        return out.substr(0, bandStart);
    };

    std::string colored;
    std::string noColor;
    std::string dumb;

    // The three captures are taken inside the guards' scope; the assertions are
    // made outside it, with the developer's environment already restored.
    {
        const ScopedEnvVar termGuard("TERM");
        const ScopedEnvVar forceGuard("CLICOLOR_FORCE");
        const ScopedEnvVar noColorGuard("NO_COLOR");

        setenv("TERM", "xterm", 1);
        setenv("CLICOLOR_FORCE", "1", 1);
        unsetenv("NO_COLOR");
        termRefreshStyle();
        CHECK(currentStyle().color);
        colored = narrationOf(runTurn(db, "xyzzy the frobnitz").output);

        setenv("NO_COLOR", "1", 1);
        termRefreshStyle();
        CHECK(!currentStyle().color);
        noColor = narrationOf(runTurn(db, "xyzzy the frobnitz").output);

        unsetenv("NO_COLOR");
        setenv("TERM", "dumb", 1);
        termRefreshStyle();
        CHECK(!currentStyle().color);
        CHECK(!currentStyle().attrs);
        dumb = narrationOf(runTurn(db, "xyzzy the frobnitz").output);
    }
    termRefreshStyle();

    // REQ-POLISH-7: dimmed with BrightBlack under colour.
    CHECK(contains(colored, "\x1b[90m"));
    // Suppressed means the plain bytes, not an empty sequence (REQ-UI-22).
    CHECK(noColor.find('\x1b') == std::string::npos);
    CHECK(dumb.find('\x1b') == std::string::npos);
    CHECK(noColor == dumb);

    // The escape bytes sit OUTSIDE the two-space indent, so stripSgr yields the
    // same string in all three — which is also what keeps the wrap honest, since
    // wrapProse would have counted those bytes as columns.
    CHECK(stripSgr(colored) == noColor);
    CHECK(contains(colored, "  \x1b[90m"));

    // REQ-POLISH-7a: indented like prose in all three.
    for (const std::string& capture : {colored, noColor, dumb}) {
        CHECK(capture.rfind("  ", 0) == 0);
        CHECK(contains(stripSgr(capture), "  I don't understand that."));
    }
}

// Check 22 / REQ-UI-5: the band is printed once at startup too, after the
// read-only room render and before the first prompt.
static void testBandStartup() {
    const TempDbFile worldPath("textworld_band_startup_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
        Db db2 = openWorld(p2.string(), "tests/combat_fixture.sql").db;
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
        Db db3 = openWorld(p3.string(), "tests/combat_fixture.sql").db;
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
        Db db4 = openWorld(p4.string(), "tests/combat_fixture.sql").db;
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
            "look", "go",     "take", "drop", "inventory", "wait",  "quit",
            "attack", "cast", "read", "spells", "examine", "say"};
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
        // Nothing beyond the thirteen: verbFromWord has exactly this many arms.
        size_t arms = 0;
        for (size_t i = src.find("word == \""); i != std::string::npos;
             i = src.find("word == \"", i + 1)) {
            ++arms;
        }
        CHECK(arms == words.size());
        // And the prompt describes all thirteen, so the schema can never accept
        // a value the prompt never mentions.
        const std::string p = kResolveSystemPrompt;
        CHECK(contains(p, "exactly thirteen verbs"));
        // The STALE-NUMERAL GUARD (REQ-NPCTALK-11). The count at the top of the
        // prompt is a sentence the MODEL READS, not a comment — a stale one is
        // a wrong instruction, not a stale note. "seven single actions" was
        // exactly that: a leftover from when the ISA had seven verbs, which the
        // conversation brick's rewrite drops rather than corrects, so the
        // numeral cannot go stale again.
        CHECK(!contains(p, "exactly twelve verbs"));
        CHECK(!contains(p, "exactly eleven verbs"));
        CHECK(!contains(p, "exactly ten verbs"));
        CHECK(!contains(p, "seven single actions"));
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
        Db db = openWorld(p.string(), "tests/combat_fixture.sql").db;
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
        // `catalog_profile` and `npc_memory` are the NPC memory store's
        // (REQ-NPCSTORE-6, -7), asserted in full by testNpcStoreSchema. This
        // list is also what keeps REQ-NPCSTORE-3 falsifiable: a table created
        // to hold conversation lines would show up here. `condition_catalog`
        // and `story_step` are the story arc store's (REQ-ARC-STORE-3, -4),
        // asserted in full by testStoryStoreSchema.
        const std::vector<std::string> expected = {
            "barrier", "bestiary", "catalog", "catalog_profile",
            "condition_catalog", "cooldowns", "description", "drop_table",
            "entities", "events", "exits", "grimoire", "health", "hostile",
            "known_spells", "location", "meta", "motive_catalog", "name",
            "npc_memory", "pending_strike", "player", "portable", "resistance",
            "room", "spell_catalog", "status_effects", "story_step"};
        CHECK(tables == expected);
        // And no DDL was added to the band or the mutation helper.
        CHECK(!contains(readFileBytes("src/band.cpp"), "CREATE TABLE"));
        CHECK(!contains(readFileBytes("src/mutations.cpp"), "CREATE TABLE"));
    }

    const TempDbFile worldPath("textworld_resist_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
        Db db = openWorld(legacyPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;
    const TurnResult r = runTurn(db, "look");

    // The band begins at the header rule; everything above it is prose.
    const size_t bandStart = r.output.find("-- ");
    CHECK(bandStart != std::string::npos);
    const std::string narration = r.output.substr(0, bandStart);
    // The narration carries NO escape byte — the band TU is the only styling
    // site, and it never sees prose. ("lantern" used to be asserted here as the
    // narration's own entity name; REQ-POLISH-5 moved the objects line into the
    // band, so the room's canon prose is what stands in for it.)
    // (The room's canon paragraph used to stand in for "narration mentioning an
    // entity name". REQ-POLISH-17 prints the room NAME on a repeat look, and the
    // stone hall is meta.start_room, so the name is what appears here.)
    CHECK(contains(narration, "stone hall"));
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
    // REQ-POLISH-20 REVERSES the "no line editing / history" non-goal
    // deliberately: input is read through vendored linenoise on a terminal. The
    // readline scan itself stays and still passes — it was always about GPL,
    // which is exactly why linenoise (BSD-2) is the one that was vendored.
    CHECK(!contains(src, "readline"));
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string()).db;  // default seed/base.sql
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM motive_catalog WHERE motive IN "
                       "('curiosity','secrecy','rivalry','obligation','grief',"
                       "'appetite','pride','homesickness')") == 8);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key IN "
                       "('bard_journal','bard_focus','bard_last_wake_turn')") == 3);
}

// REQ-BARD-STORE-1 (mechanical check 4): a world file written at an OLDER
// version is refused, and the refusal writes nothing. Same shape as testWorld's
// 999999 case, with a real predecessor value.
//
// The fresh-world assertion tracks SCHEMA_VERSION rather than a literal: this
// is a bard test, and every later brick that bumps the schema would otherwise
// have to edit it (REQ-NPCSTORE-10's bump did). The literal 5 below stays a
// literal on purpose — it is "some version that is not this one", which is
// what the refusal path is about, and what testNpcStoreSchema pins with 6.
static void testBardStoreVersionGate() {
    const TempDbFile worldPath("textworld_bard_version_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key='schema_version'") ==
              SCHEMA_VERSION);
    }
    {
        Db db(worldPath.string());
        db.exec("UPDATE meta SET value = 5 WHERE key = 'schema_version'");
    }
    const std::string bytesBefore = readFileBytes(worldPath);
    CHECK(!bytesBefore.empty());

    bool refused = false;
    try {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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

    // --- the wake stamp (REQ-BARD-WAKE-11, REQ-BARD-STORE-18) --------------
    // The third free-rewrite meta lane, and the one the trigger's ceiling and
    // event window are both measured against.
    const auto wakeTurn = [&db] {
        return queryInt(db,
                        "SELECT value FROM meta WHERE key = 'bard_last_wake_turn'");
    };

    // A fresh world starts at 0 — world.cpp seeds the row, so no reader ever
    // has to branch on its absence.
    CHECK(wakeTurn() == 0);

    writeBardWakeTurn(db, 7);
    CHECK(wakeTurn() == 7);

    // Free rewrite, not append, and not monotone-by-construction: the value is
    // simply whatever was stamped last.
    writeBardWakeTurn(db, 12);
    CHECK(wakeTurn() == 12);
    CHECK(queryInt(db,
                   "SELECT COUNT(*) FROM meta WHERE key = 'bard_last_wake_turn'") == 1);

    // Stored as an INTEGER, not as the text of one: every reader treats this
    // as a number, and world.cpp seeds it as one.
    CHECK(queryText(db,
                    "SELECT typeof(value) FROM meta WHERE key = 'bard_last_wake_turn'") ==
          "integer");

    // The helper never begins or commits — it writes inside the CALLER's
    // transaction, so a rollback takes the stamp with it (spec test 14's
    // shape). This is what makes "stamp, then submit" safe: a stamp that could
    // not be rolled back would strand the trigger window on any later fault.
    db.begin();
    writeBardWakeTurn(db, 99);
    db.rollback();
    CHECK(wakeTurn() == 12);
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
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

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

// --- The NPC memory store (specs/npc-memory-store.md). Schema, write helpers,
// read helpers, and the hand-authored profile loader: no AI call, no network,
// no fixture beyond seed SQL. Every test here opens tests/combat_fixture.sql,
// the one fixture carrying the combat constants writeCatalogEntry's truth gate
// reads — the same rule the bard fact store's tests follow. ---

// Step 1: the two new tables (shapes AND defaults), the version gate, and the
// two new verbs documented rather than merely used. Modeled on
// testBardStoreSchema.
static void testNpcStoreSchema() {
    const TempDbFile worldPath("textworld_npc_schema_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        // REQ-NPCSTORE-6: catalog_profile, exactly two columns, exactly these
        // names. Spec check 1.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                           "WHERE type='table' AND name='catalog_profile'") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('catalog_profile')") == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('catalog_profile') "
                           "WHERE name IN ('catalog','profile')") == 2);

        // REQ-NPCSTORE-7: npc_memory, exactly three columns — and the DEFAULTS,
        // not just the names. The upsert (REQ-NPCSTORE-9) never supplies them,
        // so a missing default is a NOT NULL failure at the first write.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('npc_memory')") == 3);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('npc_memory') "
                           "WHERE name IN ('entity','summary','summary_turn')") == 3);
        CHECK(queryText(db, "SELECT dflt_value FROM pragma_table_info('npc_memory') "
                            "WHERE name = 'summary'") == "''");
        CHECK(queryText(db, "SELECT dflt_value FROM pragma_table_info('npc_memory') "
                            "WHERE name = 'summary_turn'") == "0");

        // REQ-NPCSTORE-10: the bump landed. Tracks SCHEMA_VERSION rather than
        // the literal 7 it was written with, for the reason testBardStoreVersionGate
        // already gives: this is an NPC test, and every later brick that bumps
        // the schema would otherwise have to edit it. The story arc store's
        // 7 -> 8 bump is what made that concrete.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key='schema_version'") ==
              SCHEMA_VERSION);
    }

    // Spec check 2: a world file at the PREVIOUS version is refused, and the
    // refusal writes nothing. Same shape as testBardStoreVersionGate's.
    {
        Db db(worldPath.string());
        db.exec("UPDATE meta SET value = 6 WHERE key = 'schema_version'");
    }
    const std::string bytesBefore = readFileBytes(worldPath);
    CHECK(!bytesBefore.empty());
    bool refused = false;
    try {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    } catch (const SchemaMismatch&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(readFileBytes(worldPath) == bytesBefore);

    // REQ-NPCSTORE-1, -2: the two new verbs are DOCUMENTED, not merely used —
    // asserted the way testBardStoreSchema already asserts 'materialized'. The
    // no-write-verb line is the thing a future reader checks a new verb
    // against, so all six names are pinned (spec check 31a).
    const std::string world = readFileBytes("src/world.cpp");
    CHECK(contains(world, "'said'"));
    CHECK(contains(world, "'spoke'"));
    const std::string mut = readFileBytes("src/mutations.hpp");
    for (const char* verb : {"'looked'", "'waited'", "'failed'", "'examined'",
                             "'said'", "'spoke'"}) {
        CHECK(contains(mut, verb));
    }
}

// A string of `n` code points built from MULTI-BYTE characters, so a
// byte-truncating cap implementation fails the length assertions below and a
// code-point one passes (spec checks 7, 12, 27). 'é' is 2 bytes, '—' is 3.
static std::string multiByteOfLength(size_t n) {
    static const char* const cycle[] = {"é", "—", "ñ", "☾"};
    std::string out;
    for (size_t i = 0; i < n; ++i) out += cycle[i % 4];
    return out;
}

// Steps 2 + 4: writeCatalogProfile's write-once latch, its orphan refusal, its
// cap, and npcProfile's read half. Spec checks 4-9 and 13.
static void testNpcStoreProfile() {
    const TempDbFile worldPath("textworld_npc_profile_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const int64_t warden = writeCatalogEntry(db, "character", "gate_warden",
                                             "gate warden", "a warden at a gate",
                                             "obligation", 0);
    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

    // Check 4: a first call on a fresh catalog entry returns true and stores.
    CHECK(writeCatalogProfile(db, warden, "She counts everyone who passes."));
    CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
          "She counts everyone who passes.");

    // Check 5: a second call with DIFFERENT text returns false and the stored
    // value is byte-identical to the first. Assert the value, not just the
    // return — a latch that returned false while writing anyway would pass a
    // return-only check (REQ-NPCSTORE-11).
    CHECK(!writeCatalogProfile(db, warden, "She waves everyone through."));
    CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
          "She counts everyone who passes.");
    CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 1);

    // Check 6: a catalog id that does not exist is refused, and adds NO row
    // (REQ-NPCSTORE-14) — no orphan.
    CHECK(!writeCatalogProfile(db, 9999, "nobody's profile"));
    CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 1);

    // Check 7: the cap counts CODE POINTS. SQL length() counts characters on a
    // TEXT column, which is the right unit; a byte-truncating implementation
    // stores ~2000 characters here and fails.
    const int64_t runner = writeCatalogEntry(db, "character", "ash_runner",
                                             "ash runner", "a runner in ash",
                                             "grief", 0);
    CHECK(writeCatalogProfile(db, runner, multiByteOfLength(5000)));
    CHECK(queryInt(db, ("SELECT length(profile) FROM catalog_profile "
                        "WHERE catalog = " + std::to_string(runner)).c_str()) ==
          static_cast<int64_t>(kProfileCap));
    // And it is still valid UTF-8: the byte count is an exact multiple of the
    // cycle's character widths, never a character short of one.
    CHECK(queryText(db, ("SELECT profile FROM catalog_profile WHERE catalog = " +
                         std::to_string(runner)).c_str()) ==
          multiByteOfLength(kProfileCap));

    // Check 8: event-free (REQ-NPCSTORE-13). None of the above appended a row.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);

    // Check 9: the caller owns the transaction boundary (REQ-NPCSTORE-16) —
    // rolled back, nothing persists, and the helper began nothing of its own.
    const int64_t bell = writeCatalogEntry(db, "character", "bell_ringer",
                                           "bell ringer", "a ringer of bells",
                                           "pride", 0);
    db.begin();
    CHECK(writeCatalogProfile(db, bell, "He rings at the wrong hours."));
    db.rollback();
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM catalog_profile WHERE catalog = " +
                        std::to_string(bell)).c_str()) == 0);

    // --- Step 4: npcProfile, the catalog-keyed read (check 13) ---
    //
    // The profile above was written BEFORE the entry materialized, which is the
    // whole point of keying by catalog id: it still reads back through the
    // entity once one exists.
    db.begin();
    const int64_t wardenEntity =
        placeCatalogEntry(db, warden, 1, "A warden at the gate.", 3);
    db.commit();
    CHECK(wardenEntity != 0);
    CHECK(npcProfile(db, wardenEntity) == "She counts everyone who passes.");

    // An entity that is not a catalog character at all: empty, not an error.
    CHECK(npcProfile(db, 3).empty());   // the player
    CHECK(npcProfile(db, 999).empty());  // no such entity

    // A catalog character with NO profile row: also empty. This is the normal,
    // common state of a minor character before its first conversation
    // (REQ-NPCSTORE-19), and it must not be distinguishable from an error.
    const int64_t mute = writeCatalogEntry(db, "character", "mute_sexton",
                                           "mute sexton", "a sexton who says little",
                                           "secrecy", 0);
    db.begin();
    const int64_t muteEntity =
        placeCatalogEntry(db, mute, 1, "A sexton, silent.", 3);
    db.commit();
    CHECK(muteEntity != 0);
    CHECK(npcProfile(db, muteEntity).empty());
}

// Steps 3 + 4: writeNpcMemory's free rewrite, its turn stamp, its cap, and
// npcMemory's read half. Spec checks 10-13.
static void testNpcStoreMemory() {
    const TempDbFile worldPath("textworld_npc_memory_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

    // Check 13 FIRST, before any row exists: no row is not an error, and it is
    // observed on a world where nothing has ever written one (REQ-NPCSTORE-20).
    {
        const NpcMemory none = npcMemory(db, 7);
        CHECK(none.summary.empty());
        CHECK(none.summaryTurn == 0);
    }

    // Check 10: a first call on an entity with no row CREATES one — the row is
    // not pre-created at materialisation (REQ-NPCSTORE-9).
    db.exec("UPDATE meta SET value = 4 WHERE key = 'turn'");
    writeNpcMemory(db, 7, "The player asked about the vault.");
    CHECK(queryInt(db, "SELECT COUNT(*) FROM npc_memory WHERE entity = 7") == 1);
    CHECK(queryText(db, "SELECT summary FROM npc_memory WHERE entity = 7") ==
          "The player asked about the vault.");
    // The stamp is the turn the summary COVERS TO — the turn BEFORE this one
    // (REQ-NPCTALK-29a), because the summary was composed from lines handed
    // over before this turn's own exchange existed.
    CHECK(queryInt(db, "SELECT summary_turn FROM npc_memory WHERE entity = 7") == 3);

    // Check 10 (second half) + 11: the second call REPLACES outright — exact
    // equality, never a concatenation — and the stamp moves with meta.turn.
    // The turn is advanced BETWEEN the two writes, so a stamp that was computed
    // once and cached, or passed in by the caller, fails here.
    db.exec("UPDATE meta SET value = 9 WHERE key = 'turn'");
    writeNpcMemory(db, 7, "The player left without answering.");
    CHECK(queryInt(db, "SELECT COUNT(*) FROM npc_memory WHERE entity = 7") == 1);
    CHECK(queryText(db, "SELECT summary FROM npc_memory WHERE entity = 7") ==
          "The player left without answering.");
    CHECK(queryInt(db, "SELECT summary_turn FROM npc_memory WHERE entity = 7") == 8);

    // The read helper agrees with the raw SQL above (REQ-NPCSTORE-20).
    {
        const NpcMemory m = npcMemory(db, 7);
        CHECK(m.summary == "The player left without answering.");
        CHECK(m.summaryTurn == 8);
    }

    // Check 12: 900 code points of multi-byte text store at exactly 800.
    writeNpcMemory(db, 8, multiByteOfLength(900));
    CHECK(queryInt(db, "SELECT length(summary) FROM npc_memory WHERE entity = 8") ==
          static_cast<int64_t>(kSummaryCap));
    CHECK(npcMemory(db, 8).summary == multiByteOfLength(kSummaryCap));

    // Event-free, like the profile writer.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);

    // Ambient transaction only (REQ-NPCSTORE-16).
    db.begin();
    writeNpcMemory(db, 10, "a memory that never was");
    db.rollback();
    CHECK(queryInt(db, "SELECT COUNT(*) FROM npc_memory WHERE entity = 10") == 0);
}

// --- the off-by-one that silently loses a conversation ----------------------
// REQ-NPCTALK-29a, at the helper level (spec validation item 19a's lower half;
// its end-to-end form lives in testSayConversation). A summary is composed by
// the model from the lines it was HANDED, in the same call that produces this
// turn's reply — so it cannot cover this turn's own exchange. Stamping the
// current turn would hide that exchange from every future npcLinesSince read,
// forever, with nothing failing.
//
// This test is written to FAIL under the old behaviour on purpose, and it was
// observed failing before the one-line fix landed (see the implementation
// notes). A guard never seen to fail is not a guard.
static void testNpcMemoryStampCoversPrevious() {
    const TempDbFile worldPath("textworld_npc_stamp_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const int64_t player = 3;
    const int64_t npc = 7;

    db.exec("UPDATE meta SET value = 7 WHERE key = 'turn'");
    appendEvent(db, player, "said", npc, 0, "what is behind the gate");
    appendEvent(db, npc, "spoke", player, 0, "nothing you would want");
    writeNpcMemory(db, npc, "the player asked about the gate");

    // The stamp is turn - 1…
    CHECK(npcMemory(db, npc).summaryTurn == 6);
    // …so THIS turn's exchange is still visible to the next conversation. Under
    // a summary_turn of 7 the filter `turn > summary_turn` would drop both rows
    // and the character would have no memory of what was just said to it.
    {
        const std::vector<SpeechLine> lines = npcLinesSince(db, npc);
        CHECK(lines.size() == 2);
        if (lines.size() == 2) {
            CHECK(lines[0].verb == "said");
            CHECK(lines[0].detail == "what is behind the gate");
            CHECK(lines[1].verb == "spoke");
            CHECK(lines[1].detail == "nothing you would want");
        }
    }

    // The NEXT fold consumes them: written a turn later, it stamps 7 and the
    // pair falls out of the read. Visible until then, never longer.
    db.exec("UPDATE meta SET value = 8 WHERE key = 'turn'");
    writeNpcMemory(db, npc, "the player asked about the gate; I refused");
    CHECK(npcMemory(db, npc).summaryTurn == 7);
    CHECK(npcLinesSince(db, npc).empty());

    // Clamped at 0: a write on turn 0 cannot stamp -1, which no turn column can
    // ever be less than and which would make the filter meaningless.
    db.exec("UPDATE meta SET value = 0 WHERE key = 'turn'");
    writeNpcMemory(db, 8, "a memory written before the first turn");
    CHECK(npcMemory(db, 8).summaryTurn == 0);
}

// Step 5: npcLinesSince — the bounded, pair-safe raw-line read. Spec checks
// 14-18c. Driven with appendEvent and meta.turn advanced between pairs, so the
// events are exactly the shape the conversation brick will write.
static void testNpcStoreLines() {
    const TempDbFile worldPath("textworld_npc_lines_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const int64_t player = 3;
    const int64_t npc = 7;
    const int64_t other = 8;
    int64_t turn = 0;
    // One exchange: the player's line (actor = player, subject = npc) and the
    // character's reply (actor = npc, subject = player), in ONE turn — the pair
    // REQ-NPCSTORE-21a is about.
    const auto exchange = [&](int64_t who, const char* said, const char* spoke) {
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        appendEvent(db, player, "said", who, 0, said);
        if (spoke) appendEvent(db, who, "spoke", player, 0, spoke);
    };
    const auto verbs = [](const std::vector<SpeechLine>& lines) {
        std::vector<std::string> out;
        for (const SpeechLine& l : lines) out.push_back(l.verb);
        return out;
    };
    const auto details = [](const std::vector<SpeechLine>& lines) {
        std::vector<std::string> out;
        for (const SpeechLine& l : lines) out.push_back(l.detail);
        return out;
    };

    // Check 14: six rows, a summary written after the third, exactly the last
    // three back, OLDEST FIRST.
    exchange(npc, "who are you", "the warden");
    turn = 2;
    db.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
    appendEvent(db, player, "said", npc, 0, "what is behind the gate");
    // The summary is written a turn LATER than the last line it covers, which
    // is the real shape: a fold stamps turn - 1 (REQ-NPCTALK-29a), so covering
    // "everything up to and including the third line" means writing it at
    // meta.turn 3. The fixture's INTENT is unchanged; only the turn the write
    // happens on moves, because the stamp is now derived rather than equal.
    db.exec("UPDATE meta SET value = 3 WHERE key = 'turn'");
    writeNpcMemory(db, npc, "asked who I am, and what is behind the gate");
    CHECK(npcMemory(db, npc).summaryTurn == 2);
    exchange(npc, "will you open it", "no");
    CHECK(details(npcLinesSince(db, npc)) ==
          (std::vector<std::string>{"will you open it", "no"}));

    // Check 18a: every returned detail is BYTE-EQUAL to what was appended.
    // Filtering and ordering are worthless if the content is not what was
    // stored (REQ-NPCSTORE-1, -5).
    CHECK(verbs(npcLinesSince(db, npc)) ==
          (std::vector<std::string>{"said", "spoke"}));

    // Check 16: a conversation with a DIFFERENT character is excluded, even in
    // the same turn range.
    exchange(other, "and you", "a different voice entirely");
    {
        const std::vector<std::string> d = details(npcLinesSince(db, npc));
        CHECK(std::find(d.begin(), d.end(), "and you") == d.end());
        CHECK(std::find(d.begin(), d.end(), "a different voice entirely") == d.end());
        CHECK(details(npcLinesSince(db, other)) ==
              (std::vector<std::string>{"and you", "a different voice entirely"}));
    }

    // Check 18: other verbs in the same turn range are excluded — including
    // 'looked' and 'failed', which are no-write verbs like speech, and 'moved',
    // which carries the character as its subject.
    ++turn;
    db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
             " WHERE key = 'turn'").c_str());
    appendEvent(db, player, "looked", npc, 0, "looked at the warden");
    appendEvent(db, player, "failed", npc, 0, "could not reach the warden");
    appendEvent(db, npc, "moved", npc, 2, nullptr);
    {
        const std::vector<std::string> d = details(npcLinesSince(db, npc));
        CHECK(d == (std::vector<std::string>{"will you open it", "no"}));
    }

    // Check 17: a `said` where the character is the SUBJECT (every exchange
    // above) and a `spoke` where it is the ACTOR are both included — asserted
    // by the pair coming back together, which the checks above already show.
    // Here the reverse orientation: the character as `actor` on a `said` and as
    // `subject` on a `spoke`, which the OR in the predicate must also admit.
    ++turn;
    db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
             " WHERE key = 'turn'").c_str());
    appendEvent(db, npc, "said", player, 0, "orientation reversed");
    CHECK(details(npcLinesSince(db, npc)).back() == "orientation reversed");

    // Check 18c: a TRAILING `said` with no `spoke` — a failed reply — is
    // PRESENT. That is what the character being spoken to and not answering
    // looks like, and hiding it would make it unaware it was addressed.
    CHECK(npcLinesSince(db, npc).back().verb == "said");

    // --- The cap. A fresh character, so the counts above do not interfere. ---
    const int64_t capped = 12;

    // Check 15: kLineCap + 10 qualifying rows return exactly kLineCap, and the
    // first returned row is NOT the oldest qualifying row — the recent TAIL.
    for (int64_t i = 0; i < kLineCap + 10; ++i) {
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        appendEvent(db, player, "said", capped, 0,
                    ("line " + std::to_string(i)).c_str());
    }
    {
        const std::vector<SpeechLine> lines = npcLinesSince(db, capped);
        CHECK(static_cast<int64_t>(lines.size()) == kLineCap);
        CHECK(lines.front().detail != "line 0");
        CHECK(lines.front().detail == "line 10");   // the recent tail
        CHECK(lines.back().detail == "line " + std::to_string(kLineCap + 9));
    }

    // Check 18b: exactly kLineCap + 1 qualifying rows whose oldest SURVIVOR
    // would be a `spoke` — the cap cut that reply's question. kLineCap - 1 rows
    // return, and the first is a `said` (REQ-NPCSTORE-21a).
    const int64_t orphan = 13;
    {
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        // Row 1 is the `said` the cap will cut; row 2 is its orphaned `spoke`.
        appendEvent(db, player, "said", orphan, 0, "the cut question");
        appendEvent(db, orphan, "spoke", player, 0, "the orphaned reply");
        for (int64_t i = 0; i < kLineCap - 1; ++i) {
            ++turn;
            db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                     " WHERE key = 'turn'").c_str());
            appendEvent(db, player, "said", orphan, 0,
                        ("filler " + std::to_string(i)).c_str());
        }
        const std::vector<SpeechLine> lines = npcLinesSince(db, orphan);
        CHECK(static_cast<int64_t>(lines.size()) == kLineCap - 1);
        CHECK(lines.front().verb == "said");
        CHECK(lines.front().detail == "filler 0");
    }

    // The case micro-decision 4 exists for: FEWER than kLineCap qualifying rows
    // beginning with a `spoke` KEEPS that `spoke`. Its paired `said` predates
    // summary_turn rather than having been cut, which is legitimate — and a cap
    // that guessed from the row count alone would wrongly drop it.
    const int64_t legit = 14;
    {
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        appendEvent(db, player, "said", legit, 0, "before the summary");
        // Written a turn LATER than the line it covers, because the stamp is
        // turn - 1 (REQ-NPCTALK-29a). The fixture still means "the summary
        // covers the first `said` and nothing after it"; only the turn the
        // write happens on moves.
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        writeNpcMemory(db, legit, "we had begun talking");
        ++turn;
        db.exec(("UPDATE meta SET value = " + std::to_string(turn) +
                 " WHERE key = 'turn'").c_str());
        appendEvent(db, legit, "spoke", player, 0, "a legitimate leading reply");
        appendEvent(db, player, "said", legit, 0, "and then");
        const std::vector<SpeechLine> lines = npcLinesSince(db, legit);
        CHECK(lines.size() == 2);
        CHECK(lines.front().verb == "spoke");
        CHECK(lines.front().detail == "a legitimate leading reply");
    }
}

// A well-formed overture entry, for the NPC store's admission tests. A local
// copy of bardProposal, which is defined further down the file with the Brick 2
// tests; duplicating five assignments is cheaper than hoisting it and reordering
// a block that is not this brick's.
static CatalogEntryProposal npcProposal(const std::string& handle) {
    CatalogEntryProposal e;
    e.kind = "character";
    e.handle = handle;
    e.name = "cloistered scribe";
    e.blurb = "a scribe who has not left the annex in years";
    e.motive = "curiosity";
    e.tier = 1;
    return e;
}

// Step 6: the helper admits kind = 'major', the model-facing check still does
// not, and a handle a major already owns drops one entry rather than the batch.
// Spec checks 3, 22, 30b.
static void testNpcStoreMajorKind() {
    const TempDbFile worldPath("textworld_npc_major_kind_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // Check 3: the ENGINE's write path takes it (REQ-NPCSTORE-23)…
    const int64_t major =
        writeCatalogEntry(db, "major", "thornmere_abbot", "abbot",
                          "the abbot, who has not left the abbey", "obligation", 0);
    CHECK(major > 0);
    CHECK(queryText(db, "SELECT kind FROM catalog WHERE handle = 'thornmere_abbot'") ==
          "major");

    // …and a fourth kind is still refused, so the guard was widened by exactly
    // one value rather than removed.
    CHECK(threwRuntimeError([&] {
        writeCatalogEntry(db, "wanderer", "stray", "stray", "a stray", "grief", 0);
    }));

    // Check 22: the MODEL-FACING check diverges on purpose (REQ-NPCSTORE-24).
    // The wording is asserted as a substring, so a reworded refusal is caught —
    // this refusal is what a model reads when it guesses at a third kind.
    {
        CatalogEntryProposal e = npcProposal("model_major");
        e.kind = "major";
        const std::string refusal = catalogEntryRefusal(db, e, {});
        CHECK(contains(refusal, "kind must be 'character' or 'beat'"));
    }

    // And its SIBLINGS in the same batch still admit — one entry drops, not the
    // response.
    {
        OvertureProposal proposal;
        proposal.entries.push_back(npcProposal("sibling_before"));
        CatalogEntryProposal bad = npcProposal("model_major");
        bad.kind = "major";
        proposal.entries.push_back(bad);
        proposal.entries.push_back(npcProposal("sibling_after"));
        int admitted = -1;
        CHECK(!threwRuntimeError([&] {
            admitted = admitOvertureProposal(db, proposal);
        }));
        CHECK(admitted == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle = 'model_major'") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle IN "
                           "('sibling_before','sibling_after')") == 2);
    }

    // Check 30b (REQ-NPCSTORE-31b): the overture proposes a handle the
    // hand-authored major already owns. catalog.handle is UNIQUE, so an
    // unguarded insert would throw and roll the whole batch back —
    // catalogEntryRefusal's existing pre-existing-handle check is what makes
    // this one dropped entry instead. This needs no new code; the test is what
    // keeps it true.
    {
        OvertureProposal proposal;
        proposal.entries.push_back(npcProposal("collide_before"));
        proposal.entries.push_back(npcProposal("thornmere_abbot"));  // the major's
        proposal.entries.push_back(npcProposal("collide_after"));
        int admitted = -1;
        CHECK(!threwRuntimeError([&] {
            admitted = admitOvertureProposal(db, proposal);
        }));
        CHECK(admitted == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle IN "
                           "('collide_before','collide_after')") == 2);
        // The major's own row is UNTOUCHED — still one row, still 'major',
        // still its authored name, not the proposal's.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog "
                           "WHERE handle = 'thornmere_abbot'") == 1);
        CHECK(queryText(db, "SELECT kind FROM catalog "
                            "WHERE handle = 'thornmere_abbot'") == "major");
        CHECK(queryText(db, "SELECT name FROM catalog "
                            "WHERE handle = 'thornmere_abbot'") == "abbot");
    }
}

// Step 7: THE REGRESSION THAT MATTERS. The room generator's menu is a stated
// list, so a major character is invisible to it — while staying visible to the
// bard, which is what decides when a major arrives. Spec checks 19 and 21.
static void testNpcStoreGeneratorMenu() {
    const TempDbFile worldPath("textworld_npc_menu_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // Both at tier 0, so BOTH are eligible by every gate the menu applies —
    // the major is excluded by its KIND and by nothing else. A tier that
    // happened to exclude it would make this test pass for the wrong reason.
    writeCatalogEntry(db, "major", "thornmere_abbot", "abbot",
                      "the abbot, who has not left the abbey", "obligation", 0);
    writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                      "a scribe who has not left the annex in years",
                      "curiosity", 0);

    // Check 19: the generator sees the character and NOT the major. Written to
    // fail if REQ-NPCSTORE-25's explicit pair is ever reverted to an empty
    // kind, which would silently readmit every kind including this one.
    {
        std::vector<std::string> handles;
        for (const CatalogChoice& c : eligibleCatalogForNewRoom(db, 1)) {
            handles.push_back(c.handle);
        }
        CHECK(handles == std::vector<std::string>{"t0_scribe"});
    }

    // The existing-room menu never spanned kinds, and still does not: asking
    // for characters does not smuggle the major in under 'character'.
    {
        std::vector<std::string> handles;
        for (const CatalogChoice& c : eligibleCatalog(db, 1, "character")) {
            handles.push_back(c.handle);
        }
        CHECK(handles == std::vector<std::string>{"t0_scribe"});
    }

    // The major IS reachable through an explicit kind — which is what proves
    // the exclusion above is the KIND FILTER doing the work and not some other
    // gate quietly rejecting the entry. Nothing in the tree asks for this kind;
    // the assertion exists so the previous one cannot pass vacuously.
    {
        const std::vector<CatalogChoice> majors = eligibleCatalog(db, 1, "major");
        CHECK(majors.size() == 1);
        CHECK(majors.front().handle == "thornmere_abbot");
    }

    // Check 21 (REQ-NPCSTORE-26): the bard's wake context carries the FULL
    // catalog, so the major is visible to it. Invisible to the generator,
    // visible to the bard — the two facts together are the design.
    CHECK(contains(buildWakeContext(db), "thornmere_abbot"));

    // And the all-kinds path is GONE from the source, not merely unused: a
    // future caller cannot reuse a branch that no longer exists (plan
    // micro-decision 6). The explicit pair is pinned by name.
    const std::string bard = readFileBytes("src/bard.cpp");
    CHECK(!contains(bard, "/*kind=*/\"\""));
    CHECK(!contains(bard, "kind.empty()"));
    CHECK(contains(bard, "{\"character\", \"beat\"}"));
}

// Step 8a: the profile-file parser, called DIRECTLY with no world at all. Four
// of the six ways a file fails world creation are parser failures, and they
// cost a string each here instead of a world creation in the loader test. Spec
// check 24, and the syntax halves of 25 and 30a.
static void testNpcStoreProfileParse() {
    // A well-formed file: all four header values, and a body byte-exact from
    // the character after the blank line to the last byte.
    {
        MajorProfile p;
        CHECK(parseMajorProfile(
                  "handle: thornmere_abbot\n"
                  "name: abbot\n"
                  "motive: obligation\n"
                  "tier: 2\n"
                  "\n"
                  "He has not left the abbey in thirty years.\n"
                  "He speaks in short sentences.\n",
                  p)
                  .empty());
        CHECK(p.handle == "thornmere_abbot");
        CHECK(p.name == "abbot");
        CHECK(p.motive == "obligation");
        CHECK(p.tier == 2);
        CHECK(p.profile ==
              "He has not left the abbey in thirty years.\n"
              "He speaks in short sentences.\n");
    }

    // Check 24: the header ends at the first blank line and NEVER resumes. A
    // `key: value` line AFTER it is body text — including one whose key the
    // header would have recognised, which is the case that would silently
    // reparse under a line-by-line header scanner.
    {
        MajorProfile p;
        CHECK(parseMajorProfile(
                  "handle: gate_warden\nname: warden\nmotive: obligation\ntier: 0\n"
                  "\n"
                  "She keeps a list.\n"
                  "motive: she will not say\n"
                  "goal: nor this\n",
                  p)
                  .empty());
        CHECK(p.motive == "obligation");  // NOT "she will not say"
        CHECK(p.profile ==
              "She keeps a list.\n"
              "motive: she will not say\n"
              "goal: nor this\n");
    }

    // A body opening with its OWN blank line survives intact: the separator is
    // the FIRST blank line, and everything after it is body, blank or not.
    {
        MajorProfile p;
        CHECK(parseMajorProfile(
                  "handle: h\nname: n\nmotive: grief\ntier: 0\n\n\nIndented after a gap.\n",
                  p)
                  .empty());
        CHECK(p.profile == "\nIndented after a gap.\n");
    }

    // The five parser failures. Each returns a NON-EMPTY reason naming the
    // offending key or line — and none of them contains a file name, which the
    // caller owns (REQ-NPCSTORE-31).
    {
        const std::pair<const char*, const char*> cases[] = {
            // no blank line at all
            {"handle: h\nname: n\nmotive: grief\ntier: 0\nbody with no gap\n",
             "blank line"},
            // a missing required key
            {"handle: h\nname: n\ntier: 0\n\nbody\n", "motive"},
            // an UNRECOGNISED key — the concrete case REQ-NPCSTORE-31a exists
            // for: `goal:` is a field this format deliberately does not carry
            // (REQ-NPCSTORE-30), and a silent skip would give the author a
            // world where the line quietly did nothing.
            {"handle: h\nname: n\nmotive: grief\ntier: 0\ngoal: deeper\n\nbody\n",
             "goal"},
            // a non-integer tier
            {"handle: h\nname: n\nmotive: grief\ntier: soon\n\nbody\n", "tier"},
            // a header line that does not parse
            {"handle: h\nname n\nmotive: grief\ntier: 0\n\nbody\n", "colon"},
        };
        for (const auto& [text, needle] : cases) {
            MajorProfile p;
            const std::string reason = parseMajorProfile(text, p);
            CHECK(!reason.empty());
            CHECK(contains(reason, needle));
            // The caller prefixes the file name; the parser is pure and knows
            // nothing about files.
            CHECK(!contains(reason, ".txt"));
        }
    }

    // A tier that parses as a PREFIX is not an integer: whole-string, so
    // `tier: 2 or 3` is refused rather than quietly read as 2.
    {
        MajorProfile p;
        CHECK(!parseMajorProfile(
                   "handle: h\nname: n\nmotive: grief\ntier: 2 or 3\n\nbody\n", p)
                   .empty());
    }
}

// Steps 8b + 8c + 9: the loader, its six loud failures, and the shipped tree's
// absent seed/majors/. Spec checks 23, 25, 26, 27, 28, 30, 30a.
static void testNpcStoreMajorFiles() {
    const auto file = [](const char* name, const std::string& text) {
        MajorProfileFile f;
        f.name = name;
        f.text = text;
        return f;
    };
    const std::string abbot =
        "handle: thornmere_abbot\n"
        "name: abbot\n"
        "motive: obligation\n"
        "tier: 2\n"
        "\n"
        "\n"                                    // the body opens with a blank line…
        "He has not left the abbey in years.\n"  // …so THIS is the blurb
        "He speaks in short sentences.\n";

    // Check 23: one catalog row carrying the header, one profile row carrying
    // the body byte-exact, and a blurb equal to the body's first NON-EMPTY line.
    {
        const TempDbFile worldPath("textworld_npc_majors_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                          "seed/setting.txt", {file("abbot.txt", abbot)})
                    .db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE kind = 'major'") == 1);
        CHECK(queryText(db, "SELECT handle FROM catalog WHERE kind = 'major'") ==
              "thornmere_abbot");
        CHECK(queryText(db, "SELECT name FROM catalog WHERE kind = 'major'") == "abbot");
        CHECK(queryText(db, "SELECT motive FROM catalog WHERE kind = 'major'") ==
              "obligation");
        CHECK(queryInt(db, "SELECT tier FROM catalog WHERE kind = 'major'") == 2);
        CHECK(queryText(db, "SELECT blurb FROM catalog WHERE kind = 'major'") ==
              "He has not left the abbey in years.");
        // REQ-NPCSTORE-28: byte-exact. Nothing the engine owns is injected into
        // the stored text — the engine's rules live in the prompt, not here.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 1);
        CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
              "\nHe has not left the abbey in years.\nHe speaks in short sentences.\n");

        // Check 28: the rows exist the instant openWorld returns, which is
        // before main() can reach bardOverture. The latent entry is unmaterialized.
        CHECK(queryInt(db, "SELECT entity IS NULL FROM catalog WHERE kind = 'major'") == 1);
    }

    // Check 27: the cap applies to hand-authored files too (REQ-NPCSTORE-17).
    {
        const TempDbFile worldPath("textworld_npc_majors_cap_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                          "seed/setting.txt",
                          {file("long.txt",
                                "handle: h\nname: n\nmotive: grief\ntier: 0\n\n" +
                                    multiByteOfLength(5000))})
                    .db;
        CHECK(queryInt(db, "SELECT length(profile) FROM catalog_profile") ==
              static_cast<int64_t>(kProfileCap));
    }

    // Two majors in ONE call both land, in FILE ORDER, with ascending catalog
    // ids — the order the loader is handed is the order the world records.
    {
        const TempDbFile worldPath("textworld_npc_majors_two_tests.db");
        const std::string warden =
            "handle: gate_warden\nname: warden\nmotive: secrecy\ntier: 1\n\nShe keeps a list.\n";
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                          "seed/setting.txt",
                          {file("a_abbot.txt", abbot), file("b_warden.txt", warden)})
                    .db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE kind = 'major'") == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 2);
        CHECK(queryInt(db, "SELECT id FROM catalog WHERE handle = 'thornmere_abbot'") <
              queryInt(db, "SELECT id FROM catalog WHERE handle = 'gate_warden'"));
    }

    // Check 26: ZERO files creates a world successfully with no major rows and
    // NO DIAGNOSTIC AT ANY LEVEL. The absence of a cast is not a fault
    // (REQ-NPCSTORE-32), so this is asserted against a captured log sink at
    // debug — the level is raised so the assertion cannot pass vacuously by the
    // entry simply being below the bar.
    {
        const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");
        setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
        logRefreshLevel();
        CHECK(logEnabled(LogLevel::Debug));  // the capture is not vacuous

        std::vector<std::string> entries;
        logSetSink([&entries](const std::string& line) { entries.push_back(line); });
        const TempDbFile worldPath("textworld_npc_majors_none_tests.db");
        {
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
            CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE kind = 'major'") == 0);
            CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 0);
        }
        logSetSink({});
        unsetenv("TEXTWORLD_LOG_LEVEL");
        logRefreshLevel();
        for (const std::string& line : entries) {
            CHECK(!contains(line, "major"));
            CHECK(!contains(line, "profile"));
        }
    }

    // --- Step 8c: the SIX ways a profile file fails world creation loudly ---
    //
    // Each throws std::runtime_error out of initialize(), and each what()
    // CONTAINS THE FILE NAME (REQ-NPCSTORE-31) — asserted as a substring, once
    // per case, so a generic message fails. And after any of them the world
    // file has no rows at all: the whole initialize() transaction rolled back
    // rather than leaving a half-seeded world (spec check 30).
    {
        const std::pair<const char*, std::string> broken[] = {
            // 1. a missing required header key
            {"missing_key.txt", "handle: h\nname: n\ntier: 0\n\nbody\n"},
            // 2. an UNRECOGNISED header key — `goal:` by name, since it is the
            //    concrete reason REQ-NPCSTORE-31a exists.
            {"goal_key.txt",
             "handle: h\nname: n\nmotive: grief\ntier: 0\ngoal: deeper\n\nbody\n"},
            // 3. a motive absent from motive_catalog — the loader's own case,
            //    caught from writeCatalogEntry and re-thrown with the file name.
            {"bad_motive.txt",
             "handle: h\nname: n\nmotive: vengeance\ntier: 0\n\nbody\n"},
            // 4. a non-integer tier
            {"bad_tier.txt", "handle: h\nname: n\nmotive: grief\ntier: soon\n\nbody\n"},
            // 5. a header line that does not parse
            {"no_colon.txt", "handle: h\nname n\nmotive: grief\ntier: 0\n\nbody\n"},
        };
        for (const auto& [name, text] : broken) {
            const TempDbFile worldPath("textworld_npc_majors_broken_tests.db");
            std::string message;
            bool threw = false;
            try {
                Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                                  "seed/setting.txt", {file(name, text)})
                            .db;
            } catch (const std::runtime_error& e) {
                threw = true;
                message = e.what();
            }
            CHECK(threw);
            CHECK(contains(message, name));

            // Spec check 30: NO rows at all. Reopened raw with Db, so this
            // reads the file itself rather than asking openWorld again — a
            // half-seeded world would have a meta table here.
            Db raw(worldPath.string());
            CHECK(queryInt(raw, "SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table' AND name = 'meta'") == 0);
            CHECK(queryInt(raw, "SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table'") == 0);
        }

        // 6. a handle duplicating ANOTHER PROFILE FILE's handle — scoped to the
        //    files, because the catalog is empty when they are written
        //    (REQ-NPCSTORE-31b). The SECOND file's name is the one named.
        {
            const TempDbFile worldPath("textworld_npc_majors_dup_tests.db");
            const std::string one =
                "handle: twin\nname: first\nmotive: grief\ntier: 0\n\nbody\n";
            const std::string two =
                "handle: twin\nname: second\nmotive: grief\ntier: 0\n\nbody\n";
            std::string message;
            bool threw = false;
            try {
                Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql",
                                  "seed/setting.txt",
                                  {file("first.txt", one), file("second.txt", two)})
                            .db;
            } catch (const std::runtime_error& e) {
                threw = true;
                message = e.what();
            }
            CHECK(threw);
            CHECK(contains(message, "second.txt"));
            CHECK(contains(message, "twin"));
            Db raw(worldPath.string());
            CHECK(queryInt(raw, "SELECT COUNT(*) FROM sqlite_master "
                                "WHERE type = 'table'") == 0);
        }
    }

    // Step 9's half: the SHIPPED tree has no seed/majors/ directory
    // (plan micro-decision 2 — zero profile files ship), so a default
    // openWorld creates zero major rows. This is the assertion that keeps the
    // "no cast is a valid world" path exercised by the game itself.
    CHECK(!std::filesystem::exists("seed/majors"));
    {
        const TempDbFile worldPath("textworld_npc_majors_shipped_tests.db");
        Db db = openWorld(worldPath.string(), "seed/base.sql").db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE kind = 'major'") == 0);
    }
}

// Does any line of `code` contain BOTH `a` and `b`? The line is the unit
// because a file legitimately holds an INSERT and, elsewhere, the name of a
// table it never writes — REQ-NPCSTORE-37 is about the two meeting.
static bool anyLineHasBoth(const std::string& code, const std::string& a,
                           const std::string& b) {
    std::istringstream in(code);
    std::string line;
    while (std::getline(in, line)) {
        if (contains(line, a) && contains(line, b)) return true;
    }
    return false;
}

// Step 11: the invariants. The spec states these as greps; encoding them in the
// binary is what makes them survive as regression guards rather than being run
// once by hand (REQ-NPCSTORE-38, the testCombatFinalSweep precedent). Spec
// checks 31, 31a, 32, and the behavioural half of 16-18.
static void testNpcStoreInvariants() {
    // (1) REQ-NPCSTORE-36: no line of any src/*.cpp contains
    // "UPDATE catalog_profile". Write-once is a property of the SOURCE TEXT,
    // not a convention — there is no helper to edit a profile, and this is what
    // keeps it that way. writeCatalogProfile's single INSERT OR IGNORE makes it
    // true by construction; this makes it stay true.
    std::vector<std::filesystem::path> sources;
    for (const auto& entry : std::filesystem::directory_iterator("src")) {
        if (entry.path().extension() == ".cpp") sources.push_back(entry.path());
    }
    std::sort(sources.begin(), sources.end());
    CHECK(sources.size() >= 15);  // the sweep is not vacuous
    for (const std::filesystem::path& src : sources) {
        CHECK(!contains(readFileBytes(src), "UPDATE catalog_profile"));
    }

    // (2) REQ-NPCSTORE-37: mutations.cpp is the ONLY writer of either table.
    // Enumerated by directory walk rather than by hand, so a new translation
    // unit is covered the day it lands. world.cpp needs NO exception: its DDL
    // is CREATE TABLE, and the major-profile loader calls the mutation helpers
    // rather than writing SQL. If an exception is ever needed here, the loader
    // has been written wrong.
    for (const std::filesystem::path& src : sources) {
        if (src.filename() == "mutations.cpp") continue;
        const std::string code = readFileBytes(src);
        for (const char* verb : {"INSERT", "UPDATE", "DELETE"}) {
            CHECK(!anyLineHasBoth(code, verb, "catalog_profile"));
            CHECK(!anyLineHasBoth(code, verb, "npc_memory"));
        }
    }
    // And the guard is not vacuous: mutations.cpp DOES write both.
    {
        const std::string mut = readFileBytes("src/mutations.cpp");
        CHECK(anyLineHasBoth(mut, "INSERT", "catalog_profile"));
        CHECK(anyLineHasBoth(mut, "INSERT", "npc_memory"));
    }

    // (3) REQ-NPCSTORE-4 / REQ-NPCTALK-37: speech does NOT wake the bard. The
    // wake predicate still names exactly the irreversible verbs, verbatim, and
    // src/bard.cpp mentions neither speech verb. The list gained a FIFTH,
    // 'advanced', with the story arc store (REQ-ARC-STORE-22); pinning it in
    // full is also what asserts the other four survived (REQ-ARC-STORE-23). Asserted as source text
    // because hasTriggeringEvent is file-local and the behavioural surface needs
    // the bard enabled and a transport — the wrong price for a one-line
    // guarantee.
    //
    // This guard was written by the memory-store brick, which added the two
    // verbs to the schema; the CONVERSATION brick is what made them reachable,
    // so this is the point at which it stops being hypothetical. A waking bard
    // does see conversations in its recent-events context (REQ-NPCTALK-36) —
    // but seeing them is not being woken by them, and nothing acts on that yet.
    {
        const std::string bard = readFileBytes("src/bard.cpp");
        CHECK(contains(bard,
                       "verb IN ('generated','defeated','learned','materialized','advanced')"));
        CHECK(!contains(bard, "'said'"));
        CHECK(!contains(bard, "'spoke'"));
    }

    // (4) REQ-NPCSTORE-22: the three read helpers write NOTHING. Behavioural,
    // not textual, because they live in mutations.cpp and the grep above
    // deliberately exempts that file. Every table's row count is snapshotted,
    // each helper is called on a POPULATED entity and on one with no rows at
    // all, and every count must be unchanged — `events` included.
    {
        const TempDbFile worldPath("textworld_npc_invariants_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        const int64_t id = writeCatalogEntry(db, "character", "gate_warden",
                                             "gate warden", "a warden at a gate",
                                             "obligation", 0);
        writeCatalogProfile(db, id, "She counts everyone who passes.");
        db.begin();
        const int64_t entity = placeCatalogEntry(db, id, 1, "A warden.", 3);
        db.commit();
        appendEvent(db, 3, "said", entity, 0, "who are you");
        appendEvent(db, entity, "spoke", 3, 0, "the warden");
        writeNpcMemory(db, entity, "the player asked who I am");

        std::vector<std::string> tables;
        {
            Stmt s = db.prepare("SELECT name FROM sqlite_master "
                                "WHERE type = 'table' ORDER BY name");
            while (s.step()) tables.push_back(s.colText(0));
        }
        CHECK(tables.size() >= 26);  // the snapshot is not vacuous
        const auto counts = [&db, &tables] {
            std::vector<int64_t> out;
            for (const std::string& t : tables) {
                out.push_back(queryInt(db, ("SELECT COUNT(*) FROM \"" + t + "\"").c_str()));
            }
            return out;
        };
        const std::vector<int64_t> before = counts();

        // The populated character…
        CHECK(!npcProfile(db, entity).empty());
        CHECK(!npcMemory(db, entity).summary.empty());
        CHECK(npcLinesSince(db, entity).empty());  // the summary covers them
        // …and an entity with no rows of any of these kinds at all.
        CHECK(npcProfile(db, 5).empty());
        CHECK(npcMemory(db, 5).summary.empty());
        CHECK(npcLinesSince(db, 5).empty());

        CHECK(counts() == before);
    }
}

// --- NPC conversation (specs/npc-conversation.md) ---------------------------

// Place a talkable (or, with kind = "beat", a NON-talkable) catalog entity in
// `room` and return its entity id. The shared fixture every say test builds on:
// combat_fixture.sql carries no catalog entities, so each test mints its own.
static int64_t placeCharacterIn(Db& db, int64_t room, const char* handle,
                                const char* name, const char* kind) {
    const int64_t catalog =
        writeCatalogEntry(db, kind, handle, name,
                          "someone standing where the light does not reach",
                          "obligation", 0);
    db.begin();
    const int64_t entity =
        placeCatalogEntry(db, catalog, room, "A still figure.", /*actor=*/3);
    db.commit();
    return entity;
}

// The two refusals and the shape of a reachable talk turn, driven through
// runTurn against a real world — spec validation items 1, 2, 3, 3a, 3b, 30.
// The suite is AI-disabled, so a reachable conversation takes REQ-NPCTALK-31's
// no-reply shape here; that is exactly what an AI-off session does permanently.
static void testSayRefusals() {
    // (a) Item 1: no character in the room → the exact no-one-here line, plus
    // the status band composed as on any other turn (item 30), and meta.turn
    // advanced by one. The turn is CONSUMED, as it is for every refusal the
    // world understands (REQ-NPCTALK-3).
    {
        const TempDbFile worldPath("textworld_say_nobody_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        const int64_t before =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const TurnResult r = runTurn(db, "say hello");
        CHECK(r.outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") ==
              before + 1);
        CHECK(r.output ==
              indentProse(wrapProse(std::string(kNoOneToTalkTo) + "\n",
                                    proseWidth(80)),
                          kProseIndent) +
                  composeBand(db, 80));
        // Nothing was said: there is nobody to have said it to.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'said'") == 0);
    }

    // (b) Item 2: a character AND a hostile → the exact hostile-present line,
    // the turn advanced. The goblin's chip lands in the same tick, so the
    // refusal is asserted on the event row rather than on the whole output.
    {
        const TempDbFile worldPath("textworld_say_hostile_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        placeCharacterIn(db, 2, "gate_warden", "gate warden");
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");

        const int64_t before =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const TurnResult r = runTurn(db, "say hello");
        CHECK(r.outcome == TurnOutcome::Ticked);
        const int64_t turn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(turn == before + 1);
        CHECK(queryText(db, ("SELECT detail FROM events WHERE verb = 'failed' "
                             "AND turn = " + std::to_string(turn))
                                .c_str()) == kNoTalkingInCombat);
        CHECK(contains(r.output, kNoTalkingInCombat));
        // Refused before any exchange: no line was spoken into a fight.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'said'") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'spoke'") == 0);
    }

    // (c) Item 3: a room holding only a `beat` catalog entity → the no-one-here
    // line. Beats are objects: a scorched lectern is examinable, not talkable
    // (REQ-NPCTALK-2).
    {
        const TempDbFile worldPath("textworld_say_beat_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t lectern =
            placeCharacterIn(db, 1, "scorched_lectern", "lectern", "beat");
        CHECK(lectern != 0);  // the beat really is present and really is placed
        CHECK(characterInRoom(db, 1) == 0);

        const TurnResult r = runTurn(db, "say hello");
        CHECK(r.output ==
              indentProse(wrapProse(std::string(kNoOneToTalkTo) + "\n",
                                    proseWidth(80)),
                          kProseIndent) +
                  composeBand(db, 80));
    }

    // (d) Item 3a / REQ-NPCTALK-4a: no character but a hostile present → the
    // NO-ONE-HERE line, not the hostile one. With nobody present, "no time for
    // talk in a fight" would imply there was someone to talk to.
    {
        const TempDbFile worldPath("textworld_say_order_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        CHECK(hostileInRoom(db, 2) == 7);  // the goblin really is there

        const TurnResult r = runTurn(db, "say hello");
        const int64_t turn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(queryText(db, ("SELECT detail FROM events WHERE verb = 'failed' "
                             "AND turn = " + std::to_string(turn))
                                .c_str()) == kNoOneToTalkTo);
        CHECK(contains(r.output, kNoOneToTalkTo));
        CHECK(!contains(r.output, kNoTalkingInCombat));
    }

    // (e) Item 3b: the reachable shape. The `said` row's `turn` column equals
    // the INCREMENTED meta.turn — which is what makes REQ-NPCTALK-7's ordering
    // claim observable rather than a statement about code structure.
    {
        const TempDbFile worldPath("textworld_say_reachable_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");

        const int64_t before =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const TurnResult r = runTurn(db, "say who are you");
        const int64_t turn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(turn == before + 1);
        CHECK(queryInt(db, "SELECT turn FROM events WHERE verb = 'said'") == turn);
        CHECK(queryInt(db, "SELECT subject FROM events WHERE verb = 'said'") ==
              warden);
        CHECK(queryText(db, "SELECT detail FROM events WHERE verb = 'said'") ==
              "who are you");
        // AI disabled: the authored no-reply line, never fabricated dialogue.
        CHECK(r.output ==
              indentProse(wrapProse(std::string(kNoReply) + "\n", proseWidth(80)),
                          kProseIndent) +
                  composeBand(db, 80));
    }
}

// Items 27 and 28's template half (REQ-NPCTALK-26): `spoke` prints its detail
// byte-exact; `said` prints NOTHING, because the player already saw what they
// typed. Driven on hand-written event rows so the renderer is the only thing
// under test.
static void testSayRenderBranches() {
    const TempDbFile worldPath("textworld_say_render_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");

    db.exec("UPDATE meta SET value = 5 WHERE key = 'turn'");
    const char* reply =
        "I am the warden.\n\"And you?\" — she does not look up.\n\nAsk again.";
    appendEvent(db, 3, "said", warden, 0, "who are you");
    appendEvent(db, warden, "spoke", 3, 0, reply);

    // Exactly the reply, exactly once, with its internal newlines and
    // punctuation intact — and not one byte of the player's own line.
    CHECK(render(db, 5) == std::string(reply) + "\n");

    // A `said` alone renders nothing at all, rather than falling through to
    // some default.
    db.exec("UPDATE meta SET value = 6 WHERE key = 'turn'");
    appendEvent(db, 3, "said", warden, 0, "hello?");
    CHECK(render(db, 6).empty());
}

// The prompt halves (REQ-NPCTALK-20..-24), driven directly with no transport.
// Spec validation items 11, 12, 13, 14.
static void testSpeakPrompt() {
    const TempDbFile worldPath("textworld_speak_prompt_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    db.exec("INSERT INTO meta(key, value) VALUES ('setting', "
            "'A drowned school, still half-lit.') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value");

    const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
    const int64_t catalog =
        queryInt(db, ("SELECT id FROM catalog WHERE entity = " +
                      std::to_string(warden)).c_str());
    writeCatalogProfile(db, catalog, "She counts everyone who passes the gate.");

    // --- Item 11: the system block is BYTE-IDENTICAL across two conversations
    // with the same character whose memory differs. This is the cache prefix,
    // asserted as a property rather than intended as one. ---
    const std::string first = buildSpeakSystem(db, warden);
    db.exec("UPDATE meta SET value = 4 WHERE key = 'turn'");
    appendEvent(db, 3, "said", warden, 0, "what is behind the gate");
    appendEvent(db, warden, "spoke", 3, 0, "nothing you would want");
    writeNpcMemory(db, warden, "the player asked about the gate");
    const std::string second = buildSpeakSystem(db, warden);
    CHECK(first == second);

    // --- Item 12: content lands on the right side of the cache boundary. ---
    const std::string user =
        buildSpeakUser(db, warden, "will you open it", SpeakAsks{false, false});

    // System: the engine rules, this character's profile, the setting — and
    // (the documented departure from REQ-NPCTALK-20's table) who the character
    // is, so a first contact writes a profile matching the figure on screen.
    CHECK(contains(first, kSpeakRulesPrompt));
    CHECK(contains(first, "She counts everyone who passes the gate."));
    CHECK(contains(first, "A drowned school, still half-lit."));
    CHECK(contains(first, "gate warden"));

    // User: the summary, the lines, and the input line.
    CHECK(contains(user, "the player asked about the gate"));
    CHECK(contains(user, "what is behind the gate"));
    CHECK(contains(user, "nothing you would want"));
    CHECK(contains(user, "will you open it"));

    // …and NONE of those three volatile strings is in the system block, which
    // is what makes item 11 hold rather than happen to hold.
    CHECK(!contains(first, "the player asked about the gate"));
    CHECK(!contains(first, "what is behind the gate"));
    CHECK(!contains(first, "will you open it"));

    // The user message is well-formed JSON carrying the ask bits, so the field
    // the engine reads and the field it asked for are the same bit.
    {
        const nlohmann::json j =
            nlohmann::json::parse(user, nullptr, /*allow_exceptions=*/false);
        CHECK(!j.is_discarded());
        CHECK(j["input"] == "will you open it");
        CHECK(j["write_profile"] == false);
        CHECK(j["write_summary"] == false);
        // Speaker labels, not ids or names: the model IS the character.
        CHECK(j["recent"][0]["speaker"] == "player");
        CHECK(j["recent"][1]["speaker"] == "you");
    }

    // --- Item 14: each of REQ-NPCTALK-23's three prohibitions is
    // spot-checkable by substring in the git-versioned constant. ---
    CHECK(contains(kSpeakRulesPrompt, "Never explain a mechanic"));
    CHECK(contains(kSpeakRulesPrompt, "Never volunteer background unprompted"));
    CHECK(contains(kSpeakRulesPrompt,
                   "Never name a place, a person, or an object that has not "
                   "already been established"));

    // --- Item 13: the id shield (REQ-NPCTALK-24). A world whose character
    // carries deliberately distinctive machine values, swept over BOTH
    // builders. Entity id is the first thing REQ-NPCTALK-24 names and the
    // easiest to leak, since every helper in this unit takes one as an
    // argument; the ids are in the hundreds so the probes cannot collide with
    // the fixture's prose. ---
    {
        const TempDbFile sweepPath("textworld_speak_shield_tests.db");
        Db s = openWorld(sweepPath.string(), "tests/combat_fixture.sql").db;

        // Force the catalog row to id 317 and the entity to 419.
        s.exec("INSERT INTO entities(id) VALUES (419)");
        s.exec("INSERT INTO catalog(id, kind, handle, name, blurb, motive, "
               "tier, seeded, entity) VALUES (317, 'character', "
               "'scorched_lectern', 'lectern keeper', 'a keeper of a burnt "
               "lectern', 'obligation', 3, 1, 419)");
        s.exec("INSERT INTO name(entity, value) VALUES (419, 'lectern keeper')");
        s.exec("INSERT INTO description(entity, prose) VALUES "
               "(419, 'A keeper beside a burnt lectern.')");
        s.exec("INSERT INTO location(entity, container) VALUES (419, 1)");
        writeCatalogProfile(s, 317, "She keeps what the fire left.");
        s.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
        appendEvent(s, 3, "said", 419, 0, "what burned here");
        appendEvent(s, 419, "spoke", 3, 0, "the lectern, and the rest");

        const std::string sweep =
            buildSpeakSystem(s, 419) +
            buildSpeakUser(s, 419, "who are you", SpeakAsks{true, true});

        CHECK(!contains(sweep, "317"));              // catalog id
        CHECK(!contains(sweep, "419"));              // ENTITY id
        CHECK(!contains(sweep, "tier"));             // the placement gate
        CHECK(!contains(sweep, "seeded"));           // the hinted flag
        CHECK(!contains(sweep, "scorched_lectern")); // the model-facing handle
        CHECK(!contains(sweep, "catalog"));          // no table name either
        // …and the sweep is not vacuous: the world's own words DID travel.
        CHECK(contains(sweep, "She keeps what the fire left."));
        CHECK(contains(sweep, "what burned here"));
        CHECK(contains(sweep, "who are you"));
    }
}

// The request body and the emit_reply tool (REQ-NPCTALK-18, -18a, -19).
// Spec validation items 19 and 19b, plus the body's exact shape.
static void testSpeakRequestBody() {
    const TempDbFile worldPath("textworld_speak_body_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // --- the body shape, the same way testNlResolveRequestBody pins the
    // resolver's: an EXACT top-level key set, so a stray thinking / stream /
    // cache_control key cannot appear unnoticed. ---
    {
        const std::string raw = buildSpeakRequestBody("SYSTEM", "USER");
        const nlohmann::json b =
            nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        CHECK(!b.is_discarded());

        std::vector<std::string> keys;
        for (auto it = b.begin(); it != b.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        CHECK(keys == (std::vector<std::string>{"max_tokens", "messages", "model",
                                                "system", "tool_choice", "tools"}));
        CHECK(b["system"] == "SYSTEM");
        CHECK(b["messages"].size() == 1);
        CHECK(b["messages"][0]["role"] == "user");
        CHECK(b["messages"][0]["content"] == "USER");

        // No caching, no streaming, no extended thinking — anywhere.
        CHECK(!contains(raw, "cache_control"));
        CHECK(!contains(raw, "thinking"));
        CHECK(!contains(raw, "stream"));

        // One tool, named for the house pattern, with exactly three properties
        // and only `reply` required. profile and summary are NEVER clauses:
        // a bad part must not cost the whole turn (REQ-NPCTALK-32).
        CHECK(b["tools"].size() == 1);
        const nlohmann::json& tool = b["tools"][0];
        CHECK(tool["name"] == "emit_reply");
        const nlohmann::json& props = tool["input_schema"]["properties"];
        CHECK(props.size() == 3);
        CHECK(props.contains("reply"));
        CHECK(props.contains("profile"));
        CHECK(props.contains("summary"));
        CHECK(tool["input_schema"]["required"] ==
              nlohmann::json::array({"reply"}));

        // FORCED, unlike the resolver's "auto": a talk turn always wants a
        // reply, so "no tool call" is a failure, not a designed path.
        CHECK(b["tool_choice"]["type"] == "tool");
        CHECK(b["tool_choice"]["name"] == "emit_reply");

        // The Speak role's model, under the unchanged TEXTWORLD_MODEL
        // precedence (REQ-NPCTALK-16).
        CHECK(b["model"] == "claude-opus-4-8");
    }
    {
        const ScopedModelEnv guard;
        setenv("TEXTWORLD_MODEL", "claude-sonnet-5", 1);
        const nlohmann::json b = nlohmann::json::parse(
            buildSpeakRequestBody("S", "U"), nullptr, false);
        CHECK(b["model"] == "claude-sonnet-5");
    }

    // --- Item 19: the fold threshold is 20 unsummarised lines. At 19 the
    // summary is not asked for; at 20 it is. ---
    {
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        db.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
        for (size_t i = 0; i < kFoldThreshold - 1; ++i) {
            appendEvent(db, 3, "said", warden, 0, "a line");
        }
        CHECK(npcLinesSince(db, warden).size() == kFoldThreshold - 1);
        CHECK(!speakAsksFor(db, warden).summary);
        {
            const nlohmann::json u = nlohmann::json::parse(
                buildSpeakUser(db, warden, "x", speakAsksFor(db, warden)),
                nullptr, false);
            CHECK(u["write_summary"] == false);
        }

        appendEvent(db, 3, "said", warden, 0, "the twentieth line");
        CHECK(npcLinesSince(db, warden).size() == kFoldThreshold);
        CHECK(speakAsksFor(db, warden).summary);
        {
            const nlohmann::json u = nlohmann::json::parse(
                buildSpeakUser(db, warden, "x", speakAsksFor(db, warden)),
                nullptr, false);
            CHECK(u["write_summary"] == true);
        }
    }

    // --- Item 19b: the profile condition is over the ROW, not over `kind`
    // (REQ-NPCTALK-18a). A major with a profile is not asked; the SAME major
    // with its catalog_profile row deleted IS asked, through the same branch.
    // Asserted by driving the real condition, never by reading `kind`. ---
    {
        const TempDbFile majorPath("textworld_speak_major_tests.db");
        Db m = openWorld(majorPath.string(), "tests/combat_fixture.sql").db;
        const int64_t major =
            placeCharacterIn(m, 1, "the_archivist", "archivist", "major");
        const int64_t catalog =
            queryInt(m, ("SELECT id FROM catalog WHERE entity = " +
                         std::to_string(major)).c_str());
        CHECK(queryText(m, ("SELECT kind FROM catalog WHERE id = " +
                            std::to_string(catalog)).c_str()) == "major");

        // With a profile row — as a major has from world creation — no ask.
        writeCatalogProfile(m, catalog, "He has read everything and says little.");
        CHECK(!speakAsksFor(m, major).profile);
        {
            const nlohmann::json u = nlohmann::json::parse(
                buildSpeakUser(m, major, "x", speakAsksFor(m, major)), nullptr,
                false);
            CHECK(u["write_profile"] == false);
        }

        // Row gone: the same major takes the SAME branch a minor character on
        // first contact takes, rather than a special case of its own.
        m.exec(("DELETE FROM catalog_profile WHERE catalog = " +
                std::to_string(catalog)).c_str());
        CHECK(speakAsksFor(m, major).profile);
        {
            const nlohmann::json u = nlohmann::json::parse(
                buildSpeakUser(m, major, "x", speakAsksFor(m, major)), nullptr,
                false);
            CHECK(u["write_profile"] == true);
        }
    }
}

// A canned 200 response carrying one emit_reply tool_use block whose `input` is
// exactly `input`. The Anthropic content[] shape, built rather than pasted so a
// test can vary one field at a time.
static HttpResponse speakResponse(const nlohmann::json& input) {
    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = "toolu_test";
    block["name"] = "emit_reply";
    block["input"] = input;

    nlohmann::json body;
    body["id"] = "msg_test";
    body["type"] = "message";
    body["role"] = "assistant";
    body["stop_reason"] = "tool_use";
    body["content"] = nlohmann::json::array({block});

    HttpResponse r;
    r.status = 200;
    r.body = body.dump();
    return r;
}

// validateSpeech: one gate, its four clauses, and the leniency that is
// deliberately NOT a clause (REQ-NPCTALK-31, -32). Pure — no db, no network.
static void testValidateSpeech() {
    // --- the happy path, so every rejection below is a real rejection. ---
    {
        const auto got = validateSpeech(speakResponse({{"reply", "I am here."}}));
        CHECK(got.has_value());
        if (got) {
            CHECK(got->reply == "I am here.");
            CHECK(got->profile.empty());
            CHECK(got->summary.empty());
        }
    }

    // --- clause a: HTTP. Status 500 and a transport error (status 0) leave by
    // the same door, which is what makes them two of the eight cases rather
    // than two code paths. ---
    {
        HttpResponse r = speakResponse({{"reply", "unreachable"}});
        r.status = 500;
        CHECK(!validateSpeech(r));
    }
    {
        HttpResponse r;
        r.transportError = true;  // status stays 0
        CHECK(!validateSpeech(r));
    }
    // A timeout arrives as exactly this shape, by different code.
    {
        HttpResponse r;
        r.transportError = true;
        r.status = 0;
        r.body = "";
        CHECK(!validateSpeech(r));
    }

    // --- clause b: the body is not a JSON object, or has no content array. ---
    {
        HttpResponse r;
        r.status = 200;
        r.body = "not json at all {{{";
        CHECK(!validateSpeech(r));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = R"({"id":"msg","role":"assistant"})";
        CHECK(!validateSpeech(r));
    }

    // --- clause c: the tool call. ZERO blocks is a FAILURE here, unlike the
    // resolver where zero is the designed no-action path. ---
    {
        HttpResponse r;
        r.status = 200;
        r.body =
            R"({"content":[{"type":"text","text":"I would rather just talk."}]})";
        CHECK(!validateSpeech(r));
    }
    {
        // Two blocks: one opcode per line, here as there.
        nlohmann::json block;
        block["type"] = "tool_use";
        block["name"] = "emit_reply";
        block["input"] = {{"reply", "twice"}};
        nlohmann::json body;
        body["content"] = nlohmann::json::array({block, block});
        HttpResponse r;
        r.status = 200;
        r.body = body.dump();
        CHECK(!validateSpeech(r));
    }

    // --- clause d: reply missing, empty, whitespace-only, or not a string. ---
    CHECK(!validateSpeech(speakResponse({{"profile", "no reply here"}})));
    CHECK(!validateSpeech(speakResponse({{"reply", ""}})));
    CHECK(!validateSpeech(speakResponse({{"reply", "   \n\t "}})));
    CHECK(!validateSpeech(speakResponse({{"reply", 42}})));
    CHECK(!validateSpeech(speakResponse({{"reply", nlohmann::json::array()}})));

    // --- the leniency half (items 20 and 21). A malformed profile or summary
    // is DROPPED to "" and the reply still lands: a bad part never costs the
    // whole turn. ---
    {
        const auto got = validateSpeech(
            speakResponse({{"reply", "She looks up."}, {"profile", 42}}));
        CHECK(got.has_value());
        if (got) {
            CHECK(got->reply == "She looks up.");
            CHECK(got->profile.empty());
        }
    }
    {
        const auto got = validateSpeech(speakResponse(
            {{"reply", "She looks up."}, {"summary", nlohmann::json::object()}}));
        CHECK(got.has_value());
        if (got) {
            CHECK(got->reply == "She looks up.");
            CHECK(got->summary.empty());
        }
    }
    // Both fields well-formed: both come through untouched.
    {
        const auto got = validateSpeech(speakResponse({{"reply", "Yes."},
                                                       {"profile", "A warden."},
                                                       {"summary", "We spoke."}}));
        CHECK(got.has_value());
        if (got) {
            CHECK(got->profile == "A warden.");
            CHECK(got->summary == "We spoke.");
        }
    }

    // --- never throws. A deliberately hostile body: deeply nested, wrong types
    // throughout, and a `content` array full of things that are not blocks. ---
    {
        nlohmann::json hostile;
        hostile["content"] = nlohmann::json::array(
            {1, "two", nlohmann::json::array({3}), nullptr,
             {{"type", 7}, {"name", nlohmann::json::object()}},
             {{"type", "tool_use"}, {"name", "emit_reply"}, {"input", "a string"}}});
        nlohmann::json deep = hostile;
        for (int i = 0; i < 50; ++i) deep = nlohmann::json::array({deep});
        hostile["nested"] = deep;

        HttpResponse r;
        r.status = 200;
        r.body = hostile.dump();
        bool threw = false;
        try {
            CHECK(!validateSpeech(r));
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
    }
}

// One tick with an INJECTED transport, run exactly the way loop.cpp runs one:
// begin, increment meta.turn, resolve, the enemy turn, commit — and roll back
// on any throw. runTurn binds production transports itself and has no injectable
// overload, so this is how a talk turn is driven offline with a fake. Returns
// false iff the tick rolled back (the tier-c engine-error path).
static bool sayTick(Db& db, const std::string& text,
                    const HttpTransport& transport) {
    const int64_t player = 3;
    db.begin();
    try {
        const int64_t startRoom = queryInt(
            db, "SELECT container FROM location WHERE entity = 3");
        db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
        Action a{Verb::Say};
        a.text = text;
        resolve(db, a, player, transport);
        resolveCombat(db, player, startRoom);
        db.commit();
    } catch (const std::exception&) {
        db.rollback();
        return false;
    }
    return true;
}

// The rendered output of the current turn, the way runTurnCore's template path
// composes it. No band: the band is composed by runTurn, and testSayRefusals
// already pins it (item 30).
static std::string sayOutput(Db& db) {
    return render(db, queryInt(db, "SELECT value FROM meta WHERE key = 'turn'"));
}

// A transport that counts calls and records the last body it was handed.
struct CountingTransport {
    int calls = 0;
    std::string lastBody;
    HttpResponse canned;

    HttpTransport fn() {
        return [this](const std::string& body) {
            ++calls;
            lastBody = body;
            return canned;
        };
    }
};

// The orchestration end to end (REQ-NPCTALK-6, -17, -25, -29, -29a, -30, -31,
// -33, -34, -35). Spec validation items 4, 15, 17, 18, 19a, 22, 23, 24, 26, 28.
static void testSayConversation() {
    // AI on for this whole test: the eight-case sweep below needs seven of its
    // eight cases to actually reach the transport.
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");

    // --- Items 4, 15, 28: a character present → ONE request is issued, and the
    // reply reaches the rendered turn byte for byte, punctuation and internal
    // newlines included. ---
    {
        setenv("ANTHROPIC_API_KEY", "test-key", 1);
        unsetenv("TEXTWORLD_AI");

        const TempDbFile worldPath("textworld_say_reply_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");

        const char* reply =
            "\"Behind it?\"  She does not look up.\n\nNothing you would want.\n"
            "  — and nothing I will open.";
        CountingTransport t;
        t.canned = speakResponse({{"reply", reply}});

        const int64_t before =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(sayTick(db, "what is behind the gate", t.fn()));
        const int64_t turn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(turn == before + 1);

        // EXACTLY ONE request, no retry, and it went out as the Speak role.
        CHECK(t.calls == 1);
        {
            const nlohmann::json b =
                nlohmann::json::parse(t.lastBody, nullptr, false);
            CHECK(!b.is_discarded());
            CHECK(b["model"] == modelForRole(AiRole::Speak));
            CHECK(b["tools"][0]["name"] == "emit_reply");
        }

        // Byte for byte, exactly as returned (REQ-NPCTALK-25). `said` prints
        // nothing, so the reply is the whole of the turn's output.
        CHECK(sayOutput(db) == std::string(reply) + "\n");

        // `said` before `spoke`, in that order, in one transaction
        // (REQ-NPCTALK-29) — a reply preceding its question would be read back
        // by npcLinesSince as one.
        {
            std::vector<std::string> verbs;
            Stmt s = db.prepare(
                "SELECT verb FROM events WHERE verb IN ('said','spoke') ORDER BY id");
            while (s.step()) verbs.push_back(s.colText(0));
            CHECK(verbs == (std::vector<std::string>{"said", "spoke"}));
        }
        // The player's words came from the ENGINE, never from the response.
        CHECK(queryText(db, "SELECT detail FROM events WHERE verb = 'said'") ==
              "what is behind the gate");
        CHECK(queryInt(db, "SELECT actor FROM events WHERE verb = 'spoke'") ==
              warden);
    }

    // --- Item 2, the half testSayRefusals cannot assert: with a hostile
    // present, NO MODEL CALL IS ISSUED. That test runs AI-disabled, where no
    // call happens on any path, so the claim is only real here — AI on, a
    // character present, a counting transport, and zero calls. ---
    {
        setenv("ANTHROPIC_API_KEY", "test-key", 1);
        unsetenv("TEXTWORLD_AI");

        const TempDbFile worldPath("textworld_say_nocall_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        placeCharacterIn(db, 2, "gate_warden", "gate warden");
        db.exec("UPDATE location SET container = 2 WHERE entity = 3");
        CHECK(hostileInRoom(db, 2) == 7);
        CHECK(characterInRoom(db, 2) != 0);  // both present: the refused shape

        CountingTransport t;
        t.canned = speakResponse({{"reply", "never asked for"}});
        CHECK(sayTick(db, "hello", t.fn()));
        CHECK(t.calls == 0);
        // The control: the SAME world without the hostile does spend the call,
        // so the zero above is the refusal and not a broken fixture.
        db.exec("DELETE FROM hostile WHERE entity = 7");
        CHECK(sayTick(db, "hello", t.fn()));
        CHECK(t.calls == 1);
    }

    // --- Item 22: all EIGHT of REQ-NPCTALK-31's cases produce `said` +
    // `failed`, meta.turn advanced, and BYTE-IDENTICAL output — asserted as a
    // set of one. Item 23: that fallback detail is the engine constant, so no
    // failure path can surface as fabricated dialogue. ---
    {
        // Each case is (label, how the transport behaves, whether AI is on).
        struct Case {
            const char* label;
            bool aiOn;
            HttpResponse response;
        };

        HttpResponse status500 = speakResponse({{"reply", "unreachable"}});
        status500.status = 500;

        HttpResponse transportErr;
        transportErr.transportError = true;

        HttpResponse timeout;  // arrives by different code, same shape
        timeout.transportError = true;
        timeout.status = 0;

        HttpResponse unparseable;
        unparseable.status = 200;
        unparseable.body = "<html>gateway</html>";

        HttpResponse noToolCall;
        noToolCall.status = 200;
        noToolCall.body = R"({"content":[{"type":"text","text":"hello"}]})";

        const std::vector<Case> cases = {
            {"ai disabled", false, speakResponse({{"reply", "never asked"}})},
            {"transport error", true, transportErr},
            {"timeout", true, timeout},
            {"status 500", true, status500},
            {"unparseable body", true, unparseable},
            {"no tool call", true, noToolCall},
            {"missing reply", true, speakResponse({{"profile", "no reply"}})},
            {"empty reply", true, speakResponse({{"reply", ""}})},
        };
        CHECK(cases.size() == 8);

        std::set<std::string> outputs;
        for (const Case& c : cases) {
            if (c.aiOn) {
                setenv("ANTHROPIC_API_KEY", "test-key", 1);
                unsetenv("TEXTWORLD_AI");
            } else {
                unsetenv("ANTHROPIC_API_KEY");
            }

            const TempDbFile worldPath("textworld_say_failure_tests.db");
            Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
            placeCharacterIn(db, 1, "gate_warden", "gate warden");

            CountingTransport t;
            t.canned = c.response;
            const int64_t before =
                queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
            CHECK(sayTick(db, "what is behind the gate", t.fn()));
            const int64_t turn =
                queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");

            // The turn still ticks. It always ticks.
            CHECK(turn == before + 1);
            // With AI off nothing is even built; otherwise exactly one attempt.
            CHECK(t.calls == (c.aiOn ? 1 : 0));

            const std::string sql =
                "SELECT COUNT(*) FROM events WHERE turn = " + std::to_string(turn);
            CHECK(queryInt(db, (sql + " AND verb = 'said'").c_str()) == 1);
            CHECK(queryInt(db, (sql + " AND verb = 'spoke'").c_str()) == 0);
            // Item 23: the ENGINE's line, the constant — never model text.
            CHECK(queryText(db, ("SELECT detail FROM events WHERE verb = 'failed' "
                                 "AND turn = " + std::to_string(turn))
                                    .c_str()) == kNoReply);
            // Nothing permanent was written on the way past.
            CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 0);
            CHECK(queryInt(db, "SELECT COUNT(*) FROM npc_memory") == 0);

            outputs.insert(sayOutput(db));
        }
        // Byte-identical across all eight: the player cannot tell a timeout
        // from a malformed field, which is the point.
        CHECK(outputs.size() == 1);
        CHECK(*outputs.begin() == std::string(kNoReply) + "\n");
    }

    setenv("ANTHROPIC_API_KEY", "test-key", 1);
    unsetenv("TEXTWORLD_AI");

    // --- Items 17, 18, 35: first contact stores the profile and still prints
    // the reply; the second conversation does not ask, and a profile supplied
    // anyway leaves the stored text unchanged (the write-once latch). ---
    {
        const TempDbFile worldPath("textworld_say_profile_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        const int64_t catalog =
            queryInt(db, ("SELECT id FROM catalog WHERE entity = " +
                          std::to_string(warden)).c_str());

        CountingTransport first;
        first.canned = speakResponse(
            {{"reply", "I am the warden."}, {"profile", "She counts everyone."}});
        CHECK(sayTick(db, "who are you", first.fn()));

        // The prompt DID ask, the profile WAS stored, and the reply still
        // printed — three things one call had to do at once.
        CHECK(contains(first.lastBody, "\\\"write_profile\\\":true"));
        CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
              "She counts everyone.");
        CHECK(sayOutput(db) == "I am the warden.\n");

        // The second conversation does not ask…
        CountingTransport second;
        second.canned = speakResponse({{"reply", "Still here."},
                                       {"profile", "A COMPLETELY DIFFERENT PERSON"}});
        CHECK(sayTick(db, "still here", second.fn()));
        CHECK(contains(second.lastBody, "\\\"write_profile\\\":false"));
        // …and a profile supplied anyway changes NOTHING, silently
        // (REQ-NPCTALK-35): writeCatalogProfile is a one-way latch and the
        // engine does not re-check.
        CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
              "She counts everyone.");
        // One row, still — the latch did not append a second.
        CHECK(queryInt(db, ("SELECT COUNT(*) FROM catalog_profile WHERE catalog = " +
                            std::to_string(catalog)).c_str()) == 1);
    }

    // --- Item 26 / REQ-NPCTALK-34: a FAILED first contact writes no profile,
    // and the next conversation asks for one again. Nothing permanent is lost
    // by a failure. ---
    {
        const TempDbFile worldPath("textworld_say_failed_first_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        placeCharacterIn(db, 1, "gate_warden", "gate warden");

        CountingTransport failed;
        failed.canned.status = 500;
        CHECK(sayTick(db, "who are you", failed.fn()));
        CHECK(contains(failed.lastBody, "\\\"write_profile\\\":true"));
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 0);

        CountingTransport retry;
        retry.canned = speakResponse(
            {{"reply", "I am the warden."}, {"profile", "She counts everyone."}});
        CHECK(sayTick(db, "who are you", retry.fn()));
        CHECK(contains(retry.lastBody, "\\\"write_profile\\\":true"));  // asked AGAIN
        CHECK(queryText(db, "SELECT profile FROM catalog_profile") ==
              "She counts everyone.");
    }

    // --- Item 19a, end to end: after a turn that WRITES a summary,
    // npcLinesSince still returns that turn's `said` and `spoke`. This is the
    // off-by-one that would otherwise lose the last exchange of every
    // conversation, forever, with nothing failing (REQ-NPCTALK-29a). ---
    {
        const TempDbFile worldPath("textworld_say_fold_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        writeCatalogProfile(
            db, queryInt(db, ("SELECT id FROM catalog WHERE entity = " +
                              std::to_string(warden)).c_str()),
            "She counts everyone.");

        // Push past the fold threshold so the engine asks for a summary.
        db.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
        for (size_t i = 0; i < kFoldThreshold; ++i) {
            appendEvent(db, 3, "said", warden, 0, "an earlier line");
        }
        CHECK(speakAsksFor(db, warden).summary);

        CountingTransport t;
        t.canned = speakResponse({{"reply", "Enough of that."},
                                  {"summary", "the player asked many things"}});
        CHECK(sayTick(db, "and one more thing", t.fn()));
        CHECK(contains(t.lastBody, "\\\"write_summary\\\":true"));

        const int64_t turn =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        CHECK(npcMemory(db, warden).summary == "the player asked many things");
        CHECK(npcMemory(db, warden).summaryTurn == turn - 1);

        // THE ASSERTION THIS TEST EXISTS FOR: this turn's exchange survives the
        // fold that was written in the same transaction.
        const std::vector<SpeechLine> lines = npcLinesSince(db, warden);
        CHECK(lines.size() == 2);
        if (lines.size() == 2) {
            CHECK(lines[0].detail == "and one more thing");
            CHECK(lines[1].detail == "Enough of that.");
        }
    }

    // --- Item 24: FAULT INJECTION. A database error writing npc_memory rolls
    // the TURN back — no `said`, no `spoke`, meta.turn unchanged — rather than
    // degrading to the no-reply line. That distinction is where the try block
    // ends, and nothing but this test can see it. ---
    {
        const TempDbFile worldPath("textworld_say_fault_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        writeCatalogProfile(
            db, queryInt(db, ("SELECT id FROM catalog WHERE entity = " +
                              std::to_string(warden)).c_str()),
            "She counts everyone.");
        db.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
        for (size_t i = 0; i < kFoldThreshold; ++i) {
            appendEvent(db, 3, "said", warden, 0, "an earlier line");
        }
        db.exec("CREATE TRIGGER npc_memory_fault BEFORE INSERT ON npc_memory "
                "BEGIN SELECT RAISE(ABORT, 'injected npc_memory fault'); END");

        const int64_t turnBefore =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

        CountingTransport t;
        t.canned = speakResponse({{"reply", "Enough of that."},
                                  {"summary", "the player asked many things"}});
        CHECK(!sayTick(db, "and one more thing", t.fn()));  // rolled back

        // The world is as if the prompt never happened.
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") ==
              turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'spoke'") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM npc_memory") == 0);
        // Emphatically NOT the no-reply line: that would report a world which
        // did not change as one that did.
        {
            Stmt s = db.prepare("SELECT COUNT(*) FROM events WHERE detail = ?");
            s.bind(1, std::string(kNoReply));
            CHECK(s.step());
            CHECK(s.colInt(0) == 0);
        }

        // THE CONTROL ARM: the injection is still installed, but this turn
        // writes no memory — so it commits normally. That is what proves the
        // trigger touches nothing else, and that the rollback above was caused
        // by the memory write rather than by the injection's mere presence.
        db.exec("UPDATE meta SET value = 2 WHERE key = 'turn'");
        db.exec("DELETE FROM events WHERE verb = 'said'");  // back under the fold
        CHECK(!speakAsksFor(db, warden).summary);

        CountingTransport control;
        control.canned = speakResponse({{"reply", "Enough of that."}});
        CHECK(sayTick(db, "and one more thing", control.fn()));
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'spoke'") == 1);
        CHECK(sayOutput(db) == "Enough of that.\n");
    }
}

// The resolver's half of the conversation brick (REQ-NPCTALK-11, -12, -13,
// -14, -15a). Spec validation items 9, 10, 7 and 8's offline halves — the
// BEHAVIOURAL halves of 6/7/8 need a live model and live in
// testNlResolveLiveSmoke, behind TEXTWORLD_AI_LIVE_TEST=1.
static void testSayResolver() {
    // --- Item 9: `present_character` is ABSENT in a room with no character —
    // not present-and-empty, so the prompt's conditional rule turns on the
    // key's existence rather than on a sentinel value. ---
    {
        const TempDbFile worldPath("textworld_say_scope_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        {
            const nlohmann::json p = nlohmann::json::parse(
                buildResolveContext(db, "hello").payload, nullptr, false);
            CHECK(!p.is_discarded());
            CHECK(!p.contains("present_character"));
        }

        // With one present the key holds that character's NOUN and no id.
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        {
            const std::string raw = buildResolveContext(db, "hello").payload;
            const nlohmann::json p = nlohmann::json::parse(raw, nullptr, false);
            CHECK(p.contains("present_character"));
            CHECK(p["present_character"] == "gate warden");
            CHECK(!contains(raw, std::to_string(warden)));
            CHECK(!contains(raw, "gate_warden"));  // the handle stays home
        }

        // A `beat` is not a character: the key stays absent (REQ-NPCTALK-2).
        {
            const TempDbFile beatPath("textworld_say_scope_beat_tests.db");
            Db b = openWorld(beatPath.string(), "tests/combat_fixture.sql").db;
            placeCharacterIn(b, 1, "scorched_lectern", "lectern", "beat");
            const nlohmann::json p = nlohmann::json::parse(
                buildResolveContext(b, "hello").payload, nullptr, false);
            CHECK(!p.contains("present_character"));
        }
    }

    // --- Item 10: a `say` tool call carrying a subject AND a text argument is
    // ACCEPTED, and both arguments are IGNORED — the spoken text in the
    // resulting `said` row is the player's raw line, byte-equal. The model's
    // `text` reaching that row is the failure this asserts against. ---
    {
        const TempDbFile worldPath("textworld_say_lower_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");

        HttpResponse canned;
        {
            nlohmann::json block;
            block["type"] = "tool_use";
            block["name"] = "emit_action";
            block["input"] = {{"verb", "say"},
                              {"subject", "gate warden"},
                              {"text", "PARAPHRASED BY THE MODEL"}};
            nlohmann::json body;
            body["content"] = nlohmann::json::array({block});
            canned.status = 200;
            canned.body = body.dump();
        }

        // The gate accepts it and drops the arguments (clause e).
        {
            const auto lowered = validateAndLower(canned, db);
            CHECK(lowered.has_value());
            if (lowered) {
                CHECK(lowered->verb == Verb::Say);
                CHECK(lowered->subject == 0);   // resolution finds the character
                CHECK(lowered->text.empty());   // validateAndLower never sets it
            }
        }

        // aiResolve then fills `text` from the ENGINE's copy of the raw line.
        const char* raw = "Warden, What Is Behind The Gate?";
        const auto action = aiResolve(db, raw, [&canned](const std::string&) {
            return canned;
        });
        CHECK(action.has_value());
        if (action) {
            CHECK(action->verb == Verb::Say);
            CHECK(action->text == raw);  // byte-equal, casing intact
        }

        // …and it is the raw line, not the model's paraphrase, that lands in
        // the `said` row.
        {
            const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
            unsetenv("ANTHROPIC_API_KEY");  // AI off: the no-reply shape
            db.begin();
            db.exec("UPDATE meta SET value = value + 1 WHERE key = 'turn'");
            resolve(db, *action, 3);
            db.commit();
        }
        CHECK(queryText(db, "SELECT detail FROM events WHERE verb = 'said'") == raw);
        CHECK(!contains(queryText(db, "SELECT detail FROM events WHERE verb = 'said'"),
                        "PARAPHRASED"));
        CHECK(queryInt(db, "SELECT subject FROM events WHERE verb = 'said'") ==
              warden);
    }

    // --- Items 7 and 8, offline halves. The resolver prompt is the only input
    // that changed, so what offline can decide is what the prompt SAYS: it
    // still carries the unconditional no-call sentence for the absent case, and
    // it carries the conditional rule and its ordering for the present one.
    // The behavioural halves are item 6's job, live-gated. ---
    {
        const std::string p = kResolveSystemPrompt;
        // The unconditional rule, unchanged for the absent case (item 7).
        CHECK(contains(p, "make no tool call at all"));
        CHECK(contains(p, "When in doubt, make no call."));
        // The rule becomes conditional, and states the ORDERING (item 8).
        CHECK(contains(p, "This rule changes when \"present_character\" is supplied."));
        CHECK(contains(p, "if the line clearly means one of the other twelve "
                          "actions, emit that action; only if it does not, emit say"));
        CHECK(contains(p, "A question, a greeting, or chatter is speech."));
        CHECK(contains(p, "A line naming an item, a direction, or a spell is an "
                          "action, not speech, even when it is phrased politely."));
        // The scope-facts paragraph names the new key, so the model is told
        // when it exists rather than having to infer it.
        CHECK(contains(p, "\"present_character\""));

        // REQ-NPCTALK-15a is recorded IN THE CODE, not only in the spec — a
        // later reader must not mistake a passing suite for a settled rule.
        const std::string src = readFileBytes("src/nlresolve.cpp");
        CHECK(contains(src, "PROMPT-TUNING TERRITORY"));
        CHECK(contains(src, "REQ-NPCTALK-15a"));
    }

    // --- REQ-NPCTALK-6, structurally: the emit_action schema has NO text
    // property, so a paraphrase has no wire to travel on in the first place. ---
    {
        const nlohmann::json b = nlohmann::json::parse(
            buildResolveRequestBody("{}"), nullptr, false);
        const nlohmann::json& props = b["tools"][0]["input_schema"]["properties"];
        CHECK(!props.contains("text"));
        CHECK(props.size() == 3);  // verb, subject, direction — and nothing else
    }
}

// The whole-spec gates, encoded (REQ-NPCTALK-24, -27, -33, -36, -37).
// Spec validation items 31 and 32, plus the structural half of item 15.
// Follows testNpcStoreInvariants: properties of the SOURCE TEXT where a
// behavioural surface would cost more than the guarantee is worth.
static void testSayInvariants() {
    // --- Item 31: every write goes through mutations.cpp. The conversation
    // translation unit contains no raw write statement of any kind — the
    // discipline architect.cpp and bard.cpp already live under, and the reason
    // "a conversation cannot write to the world" is structural rather than a
    // rule someone has to remember. ---
    {
        const std::string npc = readFileBytes("src/npc.cpp");
        CHECK(npc.size() > 1000);  // the sweep is not vacuous: the file is real
        CHECK(contains(npc, "appendEvent"));  // …and it really does write
        for (const char* verb : {"INSERT", "UPDATE", "DELETE"}) {
            CHECK(!contains(npc, verb));
        }
    }

    // --- The structural half of item 15 (REQ-NPCTALK-27): NO narrate request
    // is issued on a talk turn. runTurn binds production transports itself and
    // has no injectable overload, so nothing offline can count what it sent —
    // this is asserted as a source-text invariant instead, the
    // testBardOvertureContract / testNpcStoreInvariants precedent.
    //
    // It is SUFFICIENT, not best-effort: aiRender is the only AiRole::Narrate
    // call site in the binary and it is called exactly once, immediately inside
    // the guard pinned below. Gating that one call site gates every narrate
    // request there is. ---
    {
        const std::string loop = readFileBytes("src/loop.cpp");
        CHECK(contains(loop, "action->verb != Verb::Say && aiNarrationEnabled()"));
        CHECK(contains(loop, "aiRender"));  // not vacuous: the call is still there

        // And it really is the only one, on both halves of the claim: exactly
        // one place in the binary BINDS a narrate transport, and exactly one
        // place CALLS aiRender — the guarded line above. (AiRole::Narrate
        // itself appears in aihttp.cpp's two role switches, which is the
        // routing table, not a call site.)
        int binds = 0;
        int callers = 0;
        for (const auto& entry : std::filesystem::directory_iterator("src")) {
            if (entry.path().extension() != ".cpp") continue;
            const std::string code = readFileBytes(entry.path());
            for (size_t i = code.find("makeAnthropicTransport(AiRole::Narrate)");
                 i != std::string::npos;
                 i = code.find("makeAnthropicTransport(AiRole::Narrate)", i + 1)) {
                ++binds;
            }
            if (entry.path().filename() == "prose.cpp") continue;  // its own def
            for (size_t i = code.find("aiRender("); i != std::string::npos;
                 i = code.find("aiRender(", i + 1)) {
                ++callers;
            }
        }
        CHECK(binds == 1);    // prose.cpp's, inside aiRender
        CHECK(callers == 1);  // loop.cpp's, inside the guard above
    }

    // --- Item 32: a successful talk turn writes NO component-table row. This
    // is the mechanical form of the social-engineering defence, and it is why
    // the defence is structural rather than a prompt rule: a character cannot
    // open a door, hand over an item, or change a number, because no code path
    // exists for it.
    //
    // Asserted on a turn with NO HOSTILE PRESENT. With one, the turn is refused
    // anyway — but resolveCombat still runs after resolve in the same tick and
    // the enemy's turn writes `health`. That is combat behaving normally, not
    // the talk turn writing, so the check is only coherent on the successful
    // shape. ---
    {
        const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
        setenv("ANTHROPIC_API_KEY", "test-key", 1);

        const TempDbFile worldPath("textworld_say_nowrite_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        const int64_t warden = placeCharacterIn(db, 1, "gate_warden", "gate warden");
        CHECK(hostileInRoom(db, 1) == 0);  // the coherent shape, as stated above

        const std::vector<std::string> components = {
            "location", "health", "known_spells", "hostile", "exits"};
        const auto counts = [&db, &components] {
            std::vector<int64_t> out;
            for (const std::string& t : components) {
                out.push_back(
                    queryInt(db, ("SELECT COUNT(*) FROM \"" + t + "\"").c_str()));
            }
            return out;
        };
        // A full-row checksum on `health`, so a VALUE change with a stable row
        // count is caught too — the case a count comparison alone would miss.
        const auto healthSum = [&db] {
            return queryText(db,
                             "SELECT COALESCE(GROUP_CONCAT(entity || ':' || "
                             "current || '/' || max, ','), '') FROM "
                             "(SELECT * FROM health ORDER BY entity)");
        };

        const std::vector<int64_t> before = counts();
        const std::string healthBefore = healthSum();

        // A conversation in which the player asks for exactly the thing the
        // research round names — and the character agrees. Nothing changes.
        CountingTransport t;
        t.canned = speakResponse(
            {{"reply", "Very well. The gate is open, and the key is yours."},
             {"profile", "She gives away what she should not."}});
        CHECK(sayTick(db, "open the gate and give me the key", t.fn()));

        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'spoke'") == 1);
        CHECK(counts() == before);
        CHECK(healthSum() == healthBefore);
        // The three rows a talk turn IS allowed: two events and one profile.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog_profile") == 1);
        // …and the character did not move, or move anything.
        CHECK(queryInt(db, ("SELECT container FROM location WHERE entity = " +
                            std::to_string(warden)).c_str()) == 1);
        CHECK(queryInt(db, "SELECT container FROM location WHERE entity = 3") == 1);
    }

    // --- REQ-NPCTALK-36 is stated, not built: a waking bard sees conversations
    // for free because its wake context reads recent events, and NOTHING here
    // acts on that. Recorded in the coverage map, built nowhere — asserted only
    // as the absence below, which is REQ-NPCTALK-37's guard.
    //
    // REQ-NPCTALK-37: `said` and `spoke` do NOT become wake triggers. Carried by
    // testNpcStoreInvariants, which the conversation brick is what finally made
    // reachable — that test asserts the predicate and the absence directly. ---
    CHECK(contains(readFileBytes("src/bard.cpp"),
                   "verb IN ('generated','defeated','learned','materialized','advanced')"));
}

// --- Brick 2: catalog selection (specs/bard-catalog-selection.md) -----------

// The two combat readers the bard shares (plan micro-decision 2). They moved
// out of combat.cpp's anonymous namespace so story eligibility can CALL the
// combat metric rather than reimplement it (REQ-BARD-SEL-3); this test is what
// makes the export real — it only links if both are public.
static void testBardSelExports() {
    const TempDbFile worldPath("textworld_bard_exports_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // The seed room is distance 0; the corridor one hop north of it is 1; the
    // outer hall (1 → 2 → 6 → 15) is 3.
    CHECK(distanceFromSeed(db, kDormitoryCell) == 0);
    CHECK(distanceFromSeed(db, 2) == 1);
    CHECK(distanceFromSeed(db, 15) == 3);

    // A room with NO realized path from the seed is maximally far, not near
    // (the sentinel every tier gate must guard before comparing).
    db.exec("INSERT INTO entities(id) VALUES (20)");
    db.exec("INSERT INTO room(entity) VALUES (20)");
    db.exec("INSERT INTO name(entity, value) VALUES (20, 'sealed vault')");
    CHECK(distanceFromSeed(db, 20) == INT64_MAX);

    // eligibleArchetypesForNewRoom is the archetype layer under the blurb menu
    // the architect already sees: the two must agree entry for entry.
    for (const int64_t origin : {kDormitoryCell, int64_t{2}, int64_t{15}}) {
        const std::vector<std::string> archetypes =
            eligibleArchetypesForNewRoom(db, origin);
        const std::vector<std::string> blurbs = eligibleEnemyBlurbs(db, origin);
        CHECK(archetypes.size() == blurbs.size());
        for (size_t i = 0; i < archetypes.size(); ++i) {
            Stmt s = db.prepare("SELECT blurb FROM bestiary WHERE archetype = ?");
            s.bind(1, archetypes[i]);
            CHECK(s.step());
            CHECK(s.colText(0) == blurbs[i]);
        }
    }
}

// Handles of a choice list, in the order offered — the shape most eligibility
// assertions below compare against.
static std::vector<std::string> handlesOf(const std::vector<CatalogChoice>& choices) {
    std::vector<std::string> out;
    for (const CatalogChoice& c : choices) out.push_back(c.handle);
    return out;
}

// The four composing gates of REQ-BARD-SEL-2, plus ordering and the
// empty-is-normal contract (REQ-BARD-SEL-4). Every catalog row is seeded
// through writeCatalogEntry, never raw SQL, so the menu can never drift from
// the admission path Brick 1 shipped.
static void testBardSelEligible() {
    const TempDbFile worldPath("textworld_bard_eligible_tests.db");

    // Fixture distances from the seed (room 1): corridor 2 → 1, frost study
    // 6 → 2, armory 9 → 2, library 11 → 1, outer hall 15 → 3.
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        // An empty catalog is a normal answer, not an error — asserted BEFORE
        // anything is seeded, so the empty path is exercised for real.
        CHECK(eligibleCatalog(db, 1, "character").empty());

        writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                          "a scribe who has not left the annex in years",
                          "curiosity", 0);
        writeCatalogEntry(db, "beat", "t0_ink", "spilled ink",
                          "a dark stain, still wet", "secrecy", 0);
        writeCatalogEntry(db, "character", "t1_warden", "under-warden",
                          "a warden who counts the doors twice a night",
                          "obligation", 1);
        writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                          "a pilgrim arrived long after the gate closed",
                          "homesickness", 2);
        writeCatalogEntry(db, "beat", "t3_rumor", "carried rumor",
                          "a rumor that outran the road it came by", "rivalry", 3);

        // --- gate (b): tier <= distance, and deeper rooms offer strictly more
        CHECK(handlesOf(eligibleCatalog(db, 1, "character")) ==
              std::vector<std::string>{"t0_scribe"});
        CHECK(handlesOf(eligibleCatalog(db, 2, "character")) ==
              (std::vector<std::string>{"t0_scribe", "t1_warden"}));
        CHECK(handlesOf(eligibleCatalog(db, 6, "character")) ==
              (std::vector<std::string>{"t0_scribe", "t1_warden", "t2_pilgrim"}));

        // --- gate (c): a kind filter excludes the other kind entirely
        CHECK(handlesOf(eligibleCatalog(db, 15, "beat")) ==
              (std::vector<std::string>{"t0_ink", "t3_rumor"}));
        for (const CatalogChoice& c : eligibleCatalog(db, 15, "beat")) {
            CHECK(c.handle != "t0_scribe" && c.handle != "t1_warden");
        }

        // --- the model-facing shape: handle + blurb + motive BLURB, never the
        // motive key, the name, the tier, or an id (REQ-BARD-SEL-1, -8).
        {
            const std::vector<CatalogChoice> menu = eligibleCatalog(db, 1, "character");
            CHECK(menu.size() == 1);
            CHECK(menu[0].handle == "t0_scribe");
            CHECK(menu[0].blurb == "a scribe who has not left the annex in years");
            CHECK(menu[0].motiveBlurb ==
                  "wants to know something they have not been told");
            CHECK(menu[0].motiveBlurb != "curiosity");  // the blurb, never the key
            // No field carries the in-world name or the tier value.
            for (const CatalogChoice& c : menu) {
                CHECK(!contains(c.handle + c.blurb + c.motiveBlurb,
                                "cloistered scribe"));
            }
        }

        // --- gate (a): a materialized entry leaves the menu ------------------
        {
            const int64_t id = queryInt(
                db, "SELECT id FROM catalog WHERE handle = 't1_warden'");
            db.exec("INSERT INTO entities(id) VALUES (30)");
            CHECK(materializeCatalogEntry(db, id, 30, 3));
            CHECK(handlesOf(eligibleCatalog(db, 2, "character")) ==
                  std::vector<std::string>{"t0_scribe"});
        }

        // --- determinism within one process (REQ-BARD-SEL-4) ----------------
        CHECK(handlesOf(eligibleCatalog(db, 6, "character")) ==
              handlesOf(eligibleCatalog(db, 6, "character")));

        // --- a room with no realized path from the seed offers nothing, and
        // does NOT throw: the INT64_MAX sentinel must not read as "very deep".
        db.exec("INSERT INTO entities(id) VALUES (20)");
        db.exec("INSERT INTO room(entity) VALUES (20)");
        CHECK(eligibleCatalog(db, 20, "character").empty());
        CHECK(eligibleCatalog(db, 20, "beat").empty());
    }

    // --- determinism across a close and reopen of the world -----------------
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        CHECK(handlesOf(eligibleCatalog(db, 6, "character")) ==
              (std::vector<std::string>{"t0_scribe", "t2_pilgrim"}));
        CHECK(handlesOf(eligibleCatalog(db, 15, "beat")) ==
              (std::vector<std::string>{"t0_ink", "t3_rumor"}));
    }
}

// The eight seeded motive keys, and the blurb each must carry to the wire.
static const std::vector<std::pair<std::string, std::string>>& fixtureMotives() {
    static const std::vector<std::pair<std::string, std::string>> motives = {
        {"curiosity", "wants to know something they have not been told"},
        {"secrecy", "has something to keep hidden, and is arranging for it to stay that way"},
        {"rivalry", "wants to be first, or to be seen to be first"},
        {"obligation", "is bound by a duty they did not choose"},
        {"grief", "is holding on to someone or something already gone"},
        {"appetite", "wants to take and carry off"},
        {"pride", "would rather be wrong than corrected"},
        {"homesickness", "does not belong here yet, and feels it"},
    };
    return motives;
}

// The overture context (REQ-BARD-SEL-9): exactly two keys, and the motive
// vocabulary reaching the wire as key AND blurb (spec test 11a).
static void testBardSelContext() {
    const TempDbFile worldPath("textworld_bard_context_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    db.exec("UPDATE meta SET value = 'a drowned abbey above a breached vault' "
            "WHERE key = 'setting'");

    {
        const std::string payload = buildOvertureContext(db);
        const nlohmann::json j = nlohmann::json::parse(payload);
        CHECK(j.is_object());
        CHECK(j.size() == 2);
        CHECK(j.contains("setting"));
        CHECK(j.contains("motives"));
        CHECK(j["setting"] == "a drowned abbey above a breached vault");

        // All eight keys AND all eight blurbs, or the model is choosing between
        // opaque tokens.
        CHECK(j["motives"].is_array());
        CHECK(j["motives"].size() == 8);
        for (const auto& [key, blurb] : fixtureMotives()) {
            CHECK(contains(payload, key));
            CHECK(contains(payload, blurb));
        }
    }

    // An empty setting still yields a well-formed object with both keys.
    {
        db.exec("UPDATE meta SET value = '' WHERE key = 'setting'");
        const nlohmann::json j = nlohmann::json::parse(buildOvertureContext(db));
        CHECK(j.is_object());
        CHECK(j.size() == 2);
        CHECK(j["setting"] == "");
        CHECK(j["motives"].size() == 8);
    }

    // --- Spec check 29 (REQ-NPCSTORE-35): the major cast, the third key ------
    //
    // Additive: every assertion above ran on a world with no majors and still
    // sees exactly two keys, which is the byte-identity half of the check — the
    // third element is ABSENT, not present-and-empty. Now the other half, on
    // the same world.
    {
        const std::string beforeAnyMajor = buildOvertureContext(db);

        const int64_t abbot =
            writeCatalogEntry(db, "major", "thornmere_abbot", "the abbot",
                              "he has not left the abbey", "obligation", 0);
        writeCatalogProfile(db, abbot, "He answers questions with questions.");
        const int64_t warden =
            writeCatalogEntry(db, "major", "gate_warden", "the warden",
                              "she keeps a list", "secrecy", 1);
        writeCatalogProfile(db, warden, "She counts everyone who passes.");

        const std::string payload = buildOvertureContext(db);
        const nlohmann::json j = nlohmann::json::parse(payload);
        CHECK(j.size() == 3);
        CHECK(j.contains("majors"));
        CHECK(j["majors"].size() == 2);
        // Both names AND both profiles, in catalog.id order.
        CHECK(j["majors"][0]["name"] == "the abbot");
        CHECK(j["majors"][0]["profile"] == "He answers questions with questions.");
        CHECK(j["majors"][1]["name"] == "the warden");
        CHECK(j["majors"][1]["profile"] == "She counts everyone who passes.");

        // Still NO IDS (REQ-BARD-SEL-8): neither catalog id appears anywhere in
        // the payload, and neither does the handle, which is the model-facing
        // selection token for an entry the overture cannot select.
        CHECK(!contains(payload, std::to_string(abbot)));
        CHECK(!contains(payload, std::to_string(warden)));

        // And the no-cast payload is what it was before any major existed —
        // byte-identical, which is what "absent, not present-and-empty" means
        // in practice.
        db.exec("DELETE FROM catalog_profile");
        db.exec("DELETE FROM catalog WHERE kind = 'major'");
        CHECK(buildOvertureContext(db) == beforeAnyMajor);
        CHECK(nlohmann::json::parse(buildOvertureContext(db)).size() == 2);
    }
}

// The overture request body and the write_catalog schema (REQ-BARD-SEL-11,
// -12, -14), modeled on testArchitectRequestBody. The artifact under test is
// the EMITTED BODY, never the builders in isolation.
static void testBardSelRequestBody() {
    const ScopedModelEnv guard;
    unsetenv("TEXTWORLD_MODEL");

    const TempDbFile worldPath("textworld_bard_body_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const std::string payload = buildOvertureContext(db);

    {
        const nlohmann::json j = nlohmann::json::parse(
            buildOvertureRequestBody(payload, motiveKeys(db)));

        // Story authoring is quality work: the GENERATE role's model, and a
        // budget large enough for a whole cast rather than one room.
        CHECK(j["model"] == "claude-opus-4-8");
        CHECK(j["max_tokens"] == 4096);

        // Exact top-level set — no thinking, no stream, no cache-control key.
        CHECK(j.size() == 6);
        CHECK(!j.contains("thinking"));
        CHECK(!j.contains("stream"));
        CHECK(j["system"] == std::string(kBardOverturePrompt));
        CHECK(j["messages"].size() == 1);
        CHECK(j["messages"][0]["role"] == "user");
        CHECK(j["messages"][0]["content"] == payload);

        // AUTO, not required: a response with no tool call is a valid outcome.
        CHECK(j["tool_choice"]["type"] == "auto");
        CHECK(!j["tool_choice"].contains("name"));

        // Exactly one tool, and it is write_catalog.
        CHECK(j["tools"].size() == 1);
        const nlohmann::json& tool = j["tools"][0];
        CHECK(tool["name"] == "write_catalog");

        const nlohmann::json& schema = tool["input_schema"];
        CHECK(schema["type"] == "object");
        CHECK(schema["required"] ==
              nlohmann::json::array({"entries", "journal"}));
        CHECK(schema["properties"].size() == 2);
        CHECK(schema["properties"]["entries"]["type"] == "array");
        CHECK(schema["properties"]["journal"]["type"] == "string");
        // No maxLength on the journal: it is stored verbatim and uncapped, and
        // adding the constraint later is one line here (plan micro-decision 9).
        CHECK(!schema["properties"]["journal"].contains("maxLength"));

        // The entry schema: the six required fields, and `fact` optional but
        // both-or-neither when present.
        const nlohmann::json& entry = schema["properties"]["entries"]["items"];
        CHECK(entry["required"] ==
              nlohmann::json::array({"kind", "handle", "name", "blurb",
                                     "motive", "tier"}));
        CHECK(entry["properties"]["kind"]["enum"] ==
              nlohmann::json::array({"character", "beat"}));
        CHECK(entry["properties"]["tier"]["type"] == "integer");
        CHECK(entry["properties"]["fact"]["required"] ==
              nlohmann::json::array({"archetype", "element"}));
        CHECK(entry["properties"]["motive"]["enum"].size() == 8);
    }

    // --- spec test 11: the motive enum is DATA-DRIVEN ----------------------
    // A ninth motive_catalog row changes the enum in the emitted body, with no
    // code change — which is what proves it is not hardcoded in the builder.
    {
        db.exec("INSERT INTO motive_catalog(motive, blurb) VALUES "
                "('vengeance', 'is owed something, and means to collect')");
        const std::vector<std::string> keys = motiveKeys(db);
        CHECK(keys.size() == 9);
        const nlohmann::json j =
            nlohmann::json::parse(buildOvertureRequestBody(payload, keys));
        const nlohmann::json& motiveEnum =
            j["tools"][0]["input_schema"]["properties"]["entries"]["items"]
             ["properties"]["motive"]["enum"];
        CHECK(motiveEnum.size() == 9);
        bool sawNew = false;
        for (const nlohmann::json& m : motiveEnum) {
            if (m == "vengeance") sawNew = true;
        }
        CHECK(sawNew);
        db.exec("DELETE FROM motive_catalog WHERE motive = 'vengeance'");
    }

    // --- spec test 10: no ids on the wire ----------------------------------
    {
        writeCatalogEntry(db, "character", "t9_stranger", "far stranger",
                          "someone who has not arrived yet", "rivalry", 9);
        db.exec("INSERT INTO entities(id) VALUES (5000)");
        const std::string body =
            buildOvertureRequestBody(buildOvertureContext(db), motiveKeys(db));
        CHECK(!contains(body, "5000"));
        CHECK(!contains(body, "t9_stranger"));  // no catalog at overture time
        CHECK(!contains(body, "\"id\""));
        CHECK(!contains(body, "\"seeded\""));
        // Positively: the setting and the vocabulary DO reach the wire.
        CHECK(contains(body, "curiosity"));
        CHECK(contains(body, "wants to know something they have not been told"));
    }
}

// The wake request body (REQ-BARD-SEL-13, -14, -15): four small tools, and an
// `entry` schema that is byte-equal to the overture's — the assertion that
// catches the two drifting apart.
static void testBardSelWakeRequestBody() {
    const ScopedModelEnv guard;
    unsetenv("TEXTWORLD_MODEL");

    const TempDbFile worldPath("textworld_bard_wake_body_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    writeBardJournal(db, "the scribe is the one to watch");
    writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                      "a scribe who has not left the annex in years",
                      "curiosity", 0);
    // Ids well clear of the single-digit trap, for the sweep below.
    db.exec("INSERT INTO entities(id) VALUES (5000)");
    db.exec("UPDATE meta SET value = 6 WHERE key = 'turn'");
    CHECK(materializeCatalogEntry(
        db, queryInt(db, "SELECT id FROM catalog WHERE handle = 't0_scribe'"),
        5000, 3));

    const std::string wakePayload = buildWakeContext(db);
    const std::string body = buildWakeRequestBody(wakePayload, motiveKeys(db));
    const nlohmann::json j = nlohmann::json::parse(body);

    CHECK(j.size() == 6);
    CHECK(j["model"] == "claude-opus-4-8");
    CHECK(j["max_tokens"] == 1024);  // a line and a paragraph, not a cast
    CHECK(j["system"] == std::string(kBardWakePrompt));
    CHECK(j["messages"][0]["content"] == wakePayload);
    CHECK(j["tool_choice"]["type"] == "auto");

    // Exactly four tools, with exactly those names.
    CHECK(j["tools"].size() == 4);
    std::vector<std::string> names;
    for (const nlohmann::json& tool : j["tools"]) {
        names.push_back(tool["name"].get<std::string>());
    }
    CHECK(names == (std::vector<std::string>{"write_focus", "write_journal",
                                             "append_catalog", "mark_seeded"}));

    // append_catalog takes ONE entry — no entries array, no journal.
    const nlohmann::json* append = nullptr;
    for (const nlohmann::json& tool : j["tools"]) {
        if (tool["name"] == "append_catalog") append = &tool;
    }
    CHECK(append != nullptr);
    const nlohmann::json& appendSchema = (*append)["input_schema"];
    CHECK(appendSchema["required"] == nlohmann::json::array({"entry"}));
    CHECK(appendSchema["properties"].size() == 1);
    CHECK(!appendSchema["properties"].contains("entries"));
    CHECK(!appendSchema["properties"].contains("journal"));

    // The anti-drift assertion: append_catalog's `entry` and write_catalog's
    // `entries` items serialize IDENTICALLY, because one builder makes both.
    {
        const nlohmann::json overture = nlohmann::json::parse(
            buildOvertureRequestBody(buildOvertureContext(db), motiveKeys(db)));
        const nlohmann::json& overtureEntry =
            overture["tools"][0]["input_schema"]["properties"]["entries"]["items"];
        CHECK(appendSchema["properties"]["entry"].dump() == overtureEntry.dump());
    }

    // The other two tools take one string each.
    for (const nlohmann::json& tool : j["tools"]) {
        if (tool["name"] == "write_focus" || tool["name"] == "write_journal") {
            CHECK(tool["input_schema"]["required"] ==
                  nlohmann::json::array({"text"}));
            CHECK(tool["input_schema"]["properties"]["text"]["type"] == "string");
        }
        if (tool["name"] == "mark_seeded") {
            CHECK(tool["input_schema"]["required"] ==
                  nlohmann::json::array({"handle"}));
        }
    }

    // --- spec test 10 on the fuller body: no ids, positively the handles ----
    CHECK(!contains(body, "5000"));
    CHECK(!contains(body, "\"id\""));
    CHECK(!contains(body, "\"seeded\""));
    CHECK(contains(body, "t0_scribe"));  // the handle IS model-facing
    CHECK(contains(body, "a scribe who has not left the annex in years"));

    // There is no place_catalog tool anywhere in the body (REQ-BARD-SEL-15).
    CHECK(!contains(body, "place_catalog"));
}

// --- canned responses for the two bard gates -------------------------------
// The Anthropic shape the gates navigate: content[] carrying tool_use blocks.

static nlohmann::json bardToolUse(const char* name, nlohmann::json input) {
    nlohmann::json block;
    block["type"] = "tool_use";
    block["id"] = "toolu_bard";
    block["name"] = name;
    block["input"] = std::move(input);
    return block;
}

static HttpResponse bardCanned(nlohmann::json content) {
    nlohmann::json j;
    j["stop_reason"] = "tool_use";
    j["content"] = std::move(content);
    HttpResponse r;
    r.status = 200;
    r.body = j.dump();
    return r;
}

// A well-formed entry, which each per-entry test then breaks in exactly one way.
static nlohmann::json bardEntry(const std::string& handle) {
    nlohmann::json e;
    e["kind"] = "character";
    e["handle"] = handle;
    e["name"] = "cloistered scribe";
    e["blurb"] = "a scribe who has not left the annex in years";
    e["motive"] = "curiosity";
    e["tier"] = 1;
    return e;
}

static HttpResponse bardOvertureCanned(nlohmann::json entries,
                                       const std::string& journal = "a note") {
    nlohmann::json input;
    input["entries"] = std::move(entries);
    input["journal"] = journal;
    return bardCanned(nlohmann::json::array({bardToolUse("write_catalog", input)}));
}

// Every log entry the bard emits while `fn` runs, at DEBUG. Used where the
// ABSENCE of a diagnostic is the assertion.
//
// This used to capture the error channel by reopening it onto a temp file.
// Since REQ-LOG-1 nothing writes there directly any more, so that capture would
// come back empty whatever happened — a test that passes because the mechanism
// it watches no longer exists. The capture is the REQ-LOG-27 sink instead, and
// the level is RAISED to debug for the duration: at the default INFO threshold
// the bard's rejection entries are below the bar and the absence assertion
// would be vacuous a second time (REQ-LOG-28).
template <typename Fn>
static std::vector<std::string> bardCapturedEntries(Fn fn) {
    const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    logRefreshLevel();
    CHECK(logEnabled(LogLevel::Debug));  // the capture is not vacuous

    std::vector<std::string> entries;
    logSetSink([&entries](const std::string& line) {
        const ParsedLogLine p = parseLogLine(line);
        if (p.ok && p.source == std::string("bard")) entries.push_back(line);
    });
    fn();
    logSetSink({});

    unsetenv("TEXTWORLD_LOG_LEVEL");
    logRefreshLevel();
    return entries;
}

// The overture gate (REQ-BARD-SEL-16, -17, -18): strict per response, lenient
// per entry. Every case below is called OUTSIDE a try/catch — the gate never
// throws, and that is the point of calling them this way.
static void testBardSelGateOverture() {
    const std::vector<std::string> motives = {
        "curiosity", "secrecy", "rivalry",  "obligation",
        "grief",     "appetite", "pride",   "homesickness"};

    // --- happy path ---------------------------------------------------------
    {
        auto p = validateOvertureResponse(
            bardOvertureCanned(nlohmann::json::array({bardEntry("one"),
                                                      bardEntry("two")}),
                               "watching the annex"),
            motives);
        CHECK(p.has_value());
        CHECK(p->entries.size() == 2);
        CHECK(p->entries[0].handle == "one");
        CHECK(p->entries[0].kind == "character");
        CHECK(p->entries[0].tier == 1);
        CHECK(p->journal == "watching the annex");
    }

    // A well-formed fact survives intact; an absent one leaves both halves empty.
    {
        nlohmann::json withFact = bardEntry("lore");
        withFact["kind"] = "beat";
        withFact["fact"] = {{"archetype", "rime_touched"}, {"element", "fire"}};
        auto p = validateOvertureResponse(
            bardOvertureCanned(nlohmann::json::array({withFact})), motives);
        CHECK(p.has_value());
        CHECK(p->entries.size() == 1);
        CHECK(p->entries[0].factArchetype == "rime_touched");
        CHECK(p->entries[0].factElement == "fire");
    }

    // --- lenient per entry: every REQ-BARD-SEL-17 failure mode --------------
    // Three entries, the middle one broken: TWO survive, and the response is
    // never rejected.
    {
        const auto broken = [&](const nlohmann::json& bad) {
            auto p = validateOvertureResponse(
                bardOvertureCanned(nlohmann::json::array(
                    {bardEntry("first"), bad, bardEntry("third")})),
                motives);
            CHECK(p.has_value());  // dropping an entry NEVER rejects
            if (!p.has_value()) return;
            CHECK(p->entries.size() == 2);
            CHECK(p->entries[0].handle == "first");
            CHECK(p->entries[1].handle == "third");
        };

        nlohmann::json e = bardEntry("bad");
        e["handle"] = "   ";
        broken(e);
        e = bardEntry("bad");
        e["name"] = "";
        broken(e);
        e = bardEntry("bad");
        e["blurb"] = "\t\n ";
        broken(e);
        e = bardEntry("bad");
        e["kind"] = "place";
        broken(e);
        e = bardEntry("bad");
        e["motive"] = "vengeance";  // outside the vocabulary
        broken(e);
        e = bardEntry("bad");
        e["tier"] = -1;
        broken(e);
        e = bardEntry("bad");
        e["tier"] = "deep";  // not an integer
        broken(e);
        e = bardEntry("bad");
        e["tier"] = 1.5;  // still not an integer
        broken(e);
        e = bardEntry("bad");
        e["fact"] = {{"archetype", "rime_touched"}};  // element missing
        broken(e);
        e = bardEntry("bad");
        e["fact"] = {{"element", "fire"}};  // archetype missing
        broken(e);
        broken(nlohmann::json("not an object"));
    }

    // Two write_catalog blocks: the FIRST is the overture, and the extra is
    // noted rather than treated as a rejection — REQ-BARD-SEL-18 lists the only
    // three clauses that reject in full, and this is not one of them.
    {
        nlohmann::json firstInput;
        firstInput["entries"] = nlohmann::json::array({bardEntry("kept")});
        firstInput["journal"] = "the first";
        nlohmann::json secondInput;
        secondInput["entries"] = nlohmann::json::array({bardEntry("ignored")});
        secondInput["journal"] = "the second";
        auto p = validateOvertureResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("write_catalog", firstInput),
                 bardToolUse("write_catalog", secondInput)})),
            motives);
        CHECK(p.has_value());
        CHECK(p->entries.size() == 1);
        CHECK(p->entries[0].handle == "kept");
        CHECK(p->journal == "the first");
    }

    // An EMPTY vocabulary means "unchecked here" — admission still refuses a
    // motive with no motive_catalog row.
    {
        nlohmann::json e = bardEntry("odd");
        e["motive"] = "vengeance";
        auto p = validateOvertureResponse(
            bardOvertureCanned(nlohmann::json::array({e})), {});
        CHECK(p.has_value());
        CHECK(p->entries.size() == 1);
    }

    // An overture with zero entries is well formed, not a rejection.
    {
        auto p = validateOvertureResponse(
            bardOvertureCanned(nlohmann::json::array()), motives);
        CHECK(p.has_value());
        CHECK(p->entries.empty());
    }

    // --- strict per response (REQ-BARD-SEL-18) ------------------------------
    {
        HttpResponse r = bardOvertureCanned(
            nlohmann::json::array({bardEntry("one")}));
        r.status = 500;
        CHECK(!validateOvertureResponse(r, motives));
    }
    {
        HttpResponse r;  // transport error: status 0
        r.transportError = true;
        CHECK(!validateOvertureResponse(r, motives));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "}{ not json";
        CHECK(!validateOvertureResponse(r, motives));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "[1, 2, 3]";  // valid JSON, but an array
        CHECK(!validateOvertureResponse(r, motives));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "{\"stop_reason\":\"end_turn\"}";  // no content array
        CHECK(!validateOvertureResponse(r, motives));
    }
    {
        // A 200 whose only tool_use is some OTHER tool.
        CHECK(!validateOvertureResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("write_focus", {{"text", "a line"}})})),
            motives));
    }
    {
        // Text only, no tool call at all.
        CHECK(!validateOvertureResponse(
            bardCanned(nlohmann::json::array(
                {{{"type", "text"}, {"text", "I decline."}}})),
            motives));
    }
}

// The wake gate (REQ-BARD-SEL-16, -14, -20): four tools accumulated, and NO
// tool call at all as a success.
static void testBardSelGateWake() {
    const std::vector<std::string> motives = {
        "curiosity", "secrecy", "rivalry",  "obligation",
        "grief",     "appetite", "pride",   "homesickness"};

    // --- all four tools in one response ------------------------------------
    {
        nlohmann::json entryInput;
        entryInput["entry"] = bardEntry("appended_one");
        auto p = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("write_focus", {{"text", "the annex is watching"}}),
                 bardToolUse("write_journal", {{"text", "a longer note"}}),
                 bardToolUse("append_catalog", entryInput),
                 bardToolUse("mark_seeded", {{"handle", "t0_scribe"}})})),
            motives);
        CHECK(p.has_value());
        CHECK(p->hasFocus);
        CHECK(p->focus == "the annex is watching");
        CHECK(p->hasJournal);
        CHECK(p->journal == "a longer note");
        CHECK(p->appended.size() == 1);
        CHECK(p->appended[0].handle == "appended_one");
        CHECK(p->seededHandles == std::vector<std::string>{"t0_scribe"});
    }

    // --- a partial wake: journal only ---------------------------------------
    {
        auto p = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("write_journal", {{"text", "only this"}})})),
            motives);
        CHECK(p.has_value());
        CHECK(p->hasJournal);
        CHECK(!p->hasFocus);
        CHECK(p->focus.empty());
        CHECK(p->appended.empty());
        CHECK(p->seededHandles.empty());
    }

    // An EMPTY focus and NO focus are different facts — the flag, not the
    // string, is what admission reads.
    {
        auto p = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("write_focus", {{"text", ""}})})),
            motives);
        CHECK(p.has_value());
        CHECK(p->hasFocus);
        CHECK(p->focus.empty());
    }

    // --- spec test 15: no tool call at all is a SUCCESSFUL, EMPTY wake ------
    {
        std::optional<WakeProposal> p;
        const std::vector<std::string> diagnostics = bardCapturedEntries([&] {
            p = validateWakeResponse(
                bardCanned(nlohmann::json::array(
                    {{{"type", "text"}, {"text", "Nothing has changed."}}})),
                motives);
        });
        CHECK(p.has_value());  // NOT nullopt
        CHECK(!p->hasFocus);
        CHECK(!p->hasJournal);
        CHECK(p->appended.empty());
        CHECK(p->seededHandles.empty());
        // And the log carries no claim of failure: declining to act is normal.
        // Asserted with DEBUG active, so an entry WOULD have been recorded.
        CHECK(diagnostics.empty());
    }

    // --- accumulation, and per-entry leniency inside a wake -----------------
    {
        nlohmann::json first;
        first["entry"] = bardEntry("appended_one");
        nlohmann::json second;
        second["entry"] = bardEntry("appended_two");
        auto p = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("append_catalog", first),
                 bardToolUse("append_catalog", second)})),
            motives);
        CHECK(p.has_value());
        CHECK(p->appended.size() == 2);

        nlohmann::json broken;
        broken["entry"] = bardEntry("appended_two");
        broken["entry"]["kind"] = "place";
        auto q = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("append_catalog", first),
                 bardToolUse("append_catalog", broken)})),
            motives);
        CHECK(q.has_value());
        CHECK(q->appended.size() == 1);
        CHECK(q->appended[0].handle == "appended_one");
    }

    // Two mark_seeded calls accumulate; a blank handle is dropped here, while
    // an UNKNOWN one is admission's problem (it is not detectable without a DB).
    {
        auto p = validateWakeResponse(
            bardCanned(nlohmann::json::array(
                {bardToolUse("mark_seeded", {{"handle", "one"}}),
                 bardToolUse("mark_seeded", {{"handle", "  "}}),
                 bardToolUse("mark_seeded", {{"handle", "two"}})})),
            motives);
        CHECK(p.has_value());
        CHECK(p->seededHandles == (std::vector<std::string>{"one", "two"}));
    }

    // --- rejection is transport/parse only, and never a throw ---------------
    {
        HttpResponse r = bardCanned(nlohmann::json::array(
            {bardToolUse("write_focus", {{"text", "a line"}})}));
        r.status = 429;
        CHECK(!validateWakeResponse(r, motives));
    }
    {
        HttpResponse r;
        r.transportError = true;
        CHECK(!validateWakeResponse(r, motives));
    }
    {
        HttpResponse r;
        r.status = 200;
        r.body = "not json at all";
        CHECK(!validateWakeResponse(r, motives));
    }
}

// A proposal in the shape admission takes, built field by field so each test
// below can break exactly one thing.
static CatalogEntryProposal bardProposal(const std::string& handle,
                                         const std::string& motive = "curiosity",
                                         int64_t tier = 1) {
    CatalogEntryProposal e;
    e.kind = "character";
    e.handle = handle;
    e.name = "cloistered scribe";
    e.blurb = "a scribe who has not left the annex in years";
    e.motive = motive;
    e.tier = tier;
    return e;
}

// Admission (REQ-BARD-SEL-19, -20, -24): the only place in the unit that
// causes a write, and it causes them only through Brick 1's helpers.
static void testBardSelAdmit() {
    // --- a false fact drops its WHOLE entry, siblings admitted (test 14) ----
    {
        const TempDbFile worldPath("textworld_bard_admit_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        OvertureProposal proposal;
        proposal.entries.push_back(bardProposal("first"));
        // The neutral matchup: goblin_grunt has no resistance row for fire, so
        // this beat would promise a falsehood about the rules.
        CatalogEntryProposal liar = bardProposal("liar");
        liar.kind = "beat";
        liar.factArchetype = "goblin_grunt";
        liar.factElement = "fire";
        proposal.entries.push_back(liar);
        proposal.entries.push_back(bardProposal("third"));
        proposal.journal = "the annex is the thread";

        CHECK(admitOvertureProposal(db, proposal) == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog") == 2);
        // ZERO rows for the false entry — not a row minus its fact.
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM catalog WHERE handle = 'liar'") == 0);
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM catalog WHERE handle IN "
                       "('first','third')") == 2);
        // And the journal still lands.
        CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'bard_journal'") ==
              "the annex is the thread");

        // A TRUE fact is admitted with both halves intact.
        OvertureProposal truthful;
        CatalogEntryProposal lore = bardProposal("rime_lore");
        lore.kind = "beat";
        lore.factArchetype = "rime_touched";
        lore.factElement = "fire";
        truthful.entries.push_back(lore);
        CHECK(admitOvertureProposal(db, truthful) == 1);
        CHECK(queryText(db, "SELECT fact_archetype FROM catalog "
                            "WHERE handle = 'rime_lore'") == "rime_touched");
    }

    // --- duplicate handles inside ONE overture ------------------------------
    // Exactly one is admitted, and admission does NOT throw — the case that
    // would otherwise surface as a UNIQUE violation and, under a catch, be
    // indistinguishable from a disk fault.
    {
        const TempDbFile worldPath("textworld_bard_admit_dup_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        OvertureProposal proposal;
        proposal.entries.push_back(bardProposal("twin"));
        proposal.entries.push_back(bardProposal("twin"));
        int admitted = -1;
        CHECK(!threwRuntimeError([&] {
            admitted = admitOvertureProposal(db, proposal);
        }));
        CHECK(admitted == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle = 'twin'") == 1);
    }

    // --- the equivalence guard (plan micro-decision 6a) ---------------------
    // The pre-flight refuses IFF the helper throws, for the same arguments.
    // Both are driven from ONE loop, so the duplicated predicate cannot drift
    // without this test going red.
    {
        const TempDbFile worldPath("textworld_bard_equiv_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        const auto agree = [&db](const CatalogEntryProposal& e) {
            const bool refused = !catalogEntryRefusal(db, e, {}).empty();
            const bool threw = threwRuntimeError([&] {
                writeCatalogEntry(db, e.kind, e.handle, e.name, e.blurb, e.motive,
                                  e.tier, e.factArchetype, e.factElement);
            });
            CHECK(refused == threw);
            return refused;
        };

        CatalogEntryProposal e = bardProposal("bad_kind");
        e.kind = "place";
        CHECK(agree(e));

        e = bardProposal("bad_motive", "vengeance");
        CHECK(agree(e));

        e = bardProposal("   ");
        CHECK(agree(e));

        e = bardProposal("blank_name");
        e.name = "  ";
        CHECK(agree(e));

        e = bardProposal("blank_blurb");
        e.blurb = "";
        CHECK(agree(e));

        e = bardProposal("neg_tier", "curiosity", -1);
        CHECK(agree(e));

        e = bardProposal("half_fact_a");
        e.factArchetype = "rime_touched";
        CHECK(agree(e));

        e = bardProposal("half_fact_b");
        e.factElement = "fire";
        CHECK(agree(e));

        // The three truth-gate clauses, one at a time.
        e = bardProposal("no_such_beast");
        e.factArchetype = "wyrm";
        e.factElement = "fire";
        CHECK(agree(e));

        e = bardProposal("not_an_element");
        e.factArchetype = "rime_touched";
        e.factElement = "ward";  // a spell, not an element
        CHECK(agree(e));

        e = bardProposal("neutral_pair");
        e.factArchetype = "goblin_grunt";
        e.factElement = "fire";  // no resistance row: teaches nothing
        CHECK(agree(e));

        // A VALID entry: both paths accept, which is the other half of "iff".
        e = bardProposal("taken");
        CHECK(!agree(e));  // no refusal, no throw — and the row is now written
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle = 'taken'") == 1);

        // Duplicate handle: refused by the pre-flight, thrown by the helper.
        CHECK(agree(bardProposal("taken")));
    }

    // --- the wake path (REQ-BARD-SEL-20, -24) -------------------------------
    {
        const TempDbFile worldPath("textworld_bard_wake_apply_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        // An entry ineligible EVERYWHERE: tier 9 is above any reachable room's
        // distance, so catalogForHandle would refuse it in every room.
        const int64_t deep =
            writeCatalogEntry(db, "character", "t9_stranger", "far stranger",
                              "someone who has not arrived yet", "rivalry", 9);
        CHECK(catalogForHandle(db, 1, "t9_stranger") == 0);

        WakeProposal wake;
        wake.hasFocus = true;
        wake.focus = "the annex is watching the stair";
        wake.appended.push_back(bardProposal("appended_one"));
        wake.seededHandles.push_back("t9_stranger");
        wake.seededHandles.push_back("no_such_handle");

        const int applied = applyWakeProposal(db, wake);
        CHECK(applied == 3);  // focus + one entry + one seeding; the unknown is
                              // ignored, not counted and not fatal
        CHECK(queryText(db, "SELECT value FROM meta WHERE key = 'bard_focus'") ==
              "the annex is watching the stair");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog "
                           "WHERE handle = 'appended_one'") == 1);
        // The ineligible-everywhere entry IS seeded — the assertion that catches
        // catalogForHandle being wired here instead of catalogIdForHandle.
        CHECK(queryInt(db, ("SELECT seeded FROM catalog WHERE id = " +
                            std::to_string(deep)).c_str()) == 1);
        // The unknown handle latched nothing anywhere.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE seeded = 1") == 1);
    }

    // --- the transaction shape Brick 3 relies on (REQ-BARD-WAKE-7) ----------
    // Admission neither begins nor commits, so the caller's rollback discards
    // everything it wrote.
    {
        const TempDbFile worldPath("textworld_bard_admit_txn_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        OvertureProposal proposal;
        proposal.entries.push_back(bardProposal("rolled_back"));
        proposal.journal = "never committed";

        db.begin();
        CHECK(admitOvertureProposal(db, proposal) == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog") == 1);
        db.rollback();
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog") == 0);
        CHECK(queryText(db,
                        "SELECT value FROM meta WHERE key = 'bard_journal'").empty());
    }
}

// The unit's contract, encoded as source-text assertions rather than greps run
// once by hand (the testCombatFinalSweep precedent). Each of these is a
// mechanical check from the spec's AI Validation section, kept as a standing
// regression guard.
static void testBardSelContract() {
    const std::string bard = readFileBytes("src/bard.cpp");
    CHECK(!bard.empty());

    // REQ-BARD-SEL-21: the bard's TU performs only SELECTs. This is the spec's
    // own grep, encoded — every write goes through the mutations helpers, which
    // contain the SQL.
    CHECK(!contains(bard, "INSERT"));
    CHECK(!contains(bard, "UPDATE"));
    CHECK(!contains(bard, "DELETE"));
    // It DOES call the helpers — otherwise the check above passes vacuously.
    CHECK(contains(bard, "writeCatalogEntry"));
    CHECK(contains(bard, "writeBardJournal"));
    CHECK(contains(bard, "writeBardFocus"));
    CHECK(contains(bard, "markCatalogSeeded"));

    // Plan micro-decision 6a: ADMISSION catches NOTHING. A catch there could
    // not tell a validation refusal from a disk fault (db.hpp raises the same
    // type for both), and would swallow the fault Brick 3 rolls back on.
    //
    // This was a whole-file "no catch" check while bard.cpp was inert. Brick 3
    // put two TOTAL entry points in this file (bardOverture, bardAfterTurn),
    // each of which must catch everything — REQ-BARD-WAKE-6 and micro-decision
    // 15 — so the check is now scoped to the functions it was always about.
    // Widening it back would forbid the degradation guarantee.
    for (const char* fn : {"int admitOvertureProposal(", "int applyWakeProposal("}) {
        const size_t start = bard.find(fn);
        CHECK(start != std::string::npos);
        if (start == std::string::npos) continue;
        // To the start of the next top-level definition: every function in this
        // file closes on a column-zero brace, so that is the body's end.
        const size_t end = bard.find("\n}\n", start);
        CHECK(end != std::string::npos);
        CHECK(!contains(bard.substr(start, end - start), "catch"));
    }

    // REQ-BARD-SEL-15: there is no place_catalog tool, anywhere under src/.
    // The bard cannot express a room; placement is the architect's.
    for (const auto& entry : std::filesystem::directory_iterator("src")) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        CHECK(!contains(readFileBytes(entry.path()), "place_catalog"));
    }

    // REQ-BARD-SEL-7: no eligibility gate reads a per-entry unlock condition,
    // and no such column exists. The storylet/time-cave line, asserted rather
    // than remembered.
    CHECK(!contains(bard, "unlock"));
    CHECK(!contains(readFileBytes("src/world.cpp"), "unlock"));

    // REQ-BARD-SEL-23, asserted again HERE as well as in testBardSelPrompt:
    // this is the sweep a future reworder runs.
    for (const std::string& prompt :
         {std::string(kBardOverturePrompt), std::string(kBardWakePrompt)}) {
        CHECK(contains(prompt, "situations, not urgency"));
        CHECK(contains(prompt, "No deadlines"));
    }
}

// The two system prompts (REQ-BARD-SEL-22, -23), modeled on
// testArchitectPrompt: STRUCTURE only, pinned by substring. Nothing here judges
// wording — that is a live concern, and Brick 3's.
static void testBardSelPrompt() {
    const std::string overture(kBardOverturePrompt);
    const std::string wake(kBardWakePrompt);
    CHECK(!overture.empty());
    CHECK(!wake.empty());

    // Each names its own tools, and only its own.
    CHECK(contains(overture, "write_catalog"));
    CHECK(!contains(overture, "mark_seeded"));
    for (const char* tool : {"write_focus", "write_journal", "append_catalog",
                             "mark_seeded"}) {
        CHECK(contains(wake, tool));
    }
    CHECK(!contains(wake, "write_catalog"));

    // Each names the context keys it will be reading, so the model knows what
    // it is looking at.
    for (const char* key : {"setting", "motives"}) {
        CHECK(contains(overture, key));
        CHECK(contains(wake, key));
    }
    for (const char* key : {"journal", "events", "catalog"}) {
        CHECK(contains(wake, key));
    }

    // The urgency prohibition, in BOTH (spec mechanical check 4). Asserted on
    // distinctive phrases rather than whole sentences, so a rewording that
    // keeps the rule keeps the test.
    for (const std::string& prompt : {overture, wake}) {
        CHECK(contains(prompt, "situations, not urgency"));
        CHECK(contains(prompt, "No deadlines"));
        CHECK(contains(prompt, "countdowns"));
        CHECK(contains(prompt, "spatial"));
    }

    // Neither invites the model to emit an engine identifier. `tier` IS
    // discussed — it is a field the model fills — but only as DEPTH, never as
    // danger, which is the misreading the prompt exists to prevent.
    for (const std::string& prompt : {overture, wake}) {
        CHECK(contains(prompt, "Invent no numbers or identifiers"));
    }
    CHECK(contains(overture, "Tier is distance, not danger"));
    CHECK(contains(wake, "tier is DEPTH"));

    // The overture's two load-bearing clauses: what a false fact costs, and
    // that nothing exists yet. The wake's: declining to act is legitimate.
    CHECK(contains(overture, "WHOLE entry"));
    CHECK(contains(overture, "Nothing exists yet"));
    CHECK(contains(wake, "Doing nothing is a legitimate turn"));
}

// The wake context (REQ-BARD-SEL-10): five keys, the tag shield on the event
// lines, no ids anywhere, and the event cap.
static void testBardSelWakeContext() {
    {
        const TempDbFile worldPath("textworld_bard_wake_ctx_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.exec("UPDATE meta SET value = 'a drowned abbey' WHERE key = 'setting'");
        writeBardJournal(db, "the scribe is the one to watch");

        const int64_t scribe =
            writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                              "a scribe who has not left the annex in years",
                              "curiosity", 0);
        writeCatalogEntry(db, "character", "t9_stranger", "far stranger",
                          "someone who has not arrived yet", "rivalry", 9);

        // Ids well clear of the single-digit trap: a room and an entity whose
        // decimal forms cannot collide with a turn number or a tier.
        db.exec("INSERT INTO entities(id) VALUES (4321), (5000)");
        db.exec("INSERT INTO room(entity) VALUES (4321)");
        db.exec("INSERT INTO name(entity, value) VALUES (5000, 'quiet copyist')");
        db.exec("INSERT INTO events(turn, actor, verb, subject, object, detail) "
                "VALUES (5, 3, 'moved', 3, 4321, NULL)");
        // The wake carries events SINCE the last wake, and a fresh world sits
        // at turn 0 — so the turn has to advance before an event the helpers
        // stamp with meta.turn can qualify at all.
        db.exec("UPDATE meta SET value = 6 WHERE key = 'turn'");
        CHECK(materializeCatalogEntry(db, scribe, 5000, 3));

        const std::string payload = buildWakeContext(db);
        const nlohmann::json j = nlohmann::json::parse(payload);

        // --- exactly five keys ---------------------------------------------
        CHECK(j.is_object());
        CHECK(j.size() == 5);
        for (const char* key : {"setting", "motives", "journal", "events", "catalog"}) {
            CHECK(j.contains(key));
        }
        CHECK(j["setting"] == "a drowned abbey");
        CHECK(j["journal"] == "the scribe is the one to watch");

        // --- the vocabulary reaches the wire, keys AND blurbs (test 11a) ----
        CHECK(j["motives"].size() == 8);
        for (const auto& [key, blurb] : fixtureMotives()) {
            CHECK(contains(payload, key));
            CHECK(contains(payload, blurb));
        }

        // --- the FULL catalog: known handle + blurb, materialized marked -----
        CHECK(j["catalog"].size() == 2);
        for (const nlohmann::json& entry : j["catalog"]) {
            CHECK(entry.size() == 4);  // handle, blurb, motive, materialized
            CHECK(entry.contains("handle"));
            CHECK(entry.contains("blurb"));
            CHECK(entry.contains("motive"));
            CHECK(entry.contains("materialized"));
            CHECK(!entry.contains("id"));
            CHECK(!entry.contains("tier"));
            CHECK(!entry.contains("seeded"));
            CHECK(!entry.contains("entity"));
        }
        CHECK(j["catalog"][0]["handle"] == "t0_scribe");
        CHECK(j["catalog"][0]["blurb"] ==
              "a scribe who has not left the annex in years");
        CHECK(j["catalog"][0]["materialized"] == true);   // it has been cast
        CHECK(j["catalog"][1]["handle"] == "t9_stranger");
        CHECK(j["catalog"][1]["materialized"] == false);  // still latent, and
                                                          // still carried

        // --- the no-ids sweep (REQ-BARD-SEL-8) ------------------------------
        // The room id an event's `object` column carried, and the entity id its
        // `subject` resolved from, appear NOWHERE — while the subject's NAME
        // does, which is the point of resolving it.
        CHECK(!contains(payload, "4321"));
        CHECK(!contains(payload, "5000"));
        CHECK(contains(payload, "quiet copyist"));
        // Nor the tier of the entry that is nine deep.
        CHECK(!contains(payload, "\"tier\""));
        CHECK(!contains(payload, "\"seeded\""));

        // --- the tag shield on 'materialized' (plan micro-decision 8) -------
        // The event line names the verb, never the catalog HANDLE its detail
        // carries — the handle is a machine token, and it is already in the
        // catalog block as a selection field.
        {
            bool sawMaterialized = false;
            for (const nlohmann::json& line : j["events"]) {
                const std::string text = line.get<std::string>();
                if (contains(text, "materialized")) {
                    sawMaterialized = true;
                    CHECK(!contains(text, "t0_scribe"));
                }
                CHECK(!contains(text, "4321"));  // the object column, dropped
            }
            CHECK(sawMaterialized);
        }
    }

    // --- the event cap: the most recent kBardWakeEventLimit, oldest-first ---
    {
        const TempDbFile worldPath("textworld_bard_wake_cap_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.exec("DELETE FROM events");
        for (int i = 1; i <= 200; ++i) {
            Stmt s = db.prepare(
                "INSERT INTO events(turn, actor, verb, subject, object, detail) "
                "VALUES (?, 3, 'looked', 0, 0, ?)");
            s.bind(1, static_cast<int64_t>(i));
            s.bind(2, "ev" + std::to_string(i));
            CHECK(!s.step());
        }

        const nlohmann::json j = nlohmann::json::parse(buildWakeContext(db));
        CHECK(j["events"].size() == static_cast<size_t>(kBardWakeEventLimit));
        // The RECENT tail, not the oldest: ev1..ev80 are dropped.
        CHECK(contains(j["events"][0].get<std::string>(), "ev81"));
        CHECK(contains(j["events"][119].get<std::string>(), "ev200"));
        CHECK(!contains(buildWakeContext(db), "ev1)"));
        // Rendered oldest-first, so the narrative order survives the cap.
        CHECK(contains(j["events"][0].get<std::string>(), "turn 81:"));
        CHECK(contains(j["events"][1].get<std::string>(), "turn 82:"));

        // Events at or before the last wake are not carried at all.
        db.exec("UPDATE meta SET value = 199 WHERE key = 'bard_last_wake_turn'");
        const nlohmann::json after = nlohmann::json::parse(buildWakeContext(db));
        CHECK(after["events"].size() == 1);
        CHECK(contains(after["events"][0].get<std::string>(), "ev200"));
    }
}

// The two handle lookups (REQ-BARD-SEL-6, -24). They answer different
// questions, and the assertions below are what catch mark_seeded being wired to
// the gated one.
static void testBardSelHandle() {
    const TempDbFile worldPath("textworld_bard_handle_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const int64_t scribe =
        writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                          "a scribe who has not left the annex in years",
                          "curiosity", 0);
    const int64_t ink = writeCatalogEntry(db, "beat", "t0_ink", "spilled ink",
                                          "a dark stain, still wet", "secrecy", 0);
    // Tier 9: above every reachable room's distance, so it is ineligible
    // EVERYWHERE — the case mark_seeded must still resolve.
    const int64_t deep =
        writeCatalogEntry(db, "character", "t9_stranger", "far stranger",
                          "someone who has not arrived yet", "rivalry", 9);

    // --- catalogForHandle: the live re-check --------------------------------
    CHECK(catalogForHandle(db, 1, "t0_scribe") == scribe);
    CHECK(catalogForHandle(db, 1, "t0_ink") == ink);  // both kinds are searched
    CHECK(catalogForHandle(db, 1, "no_such_handle") == 0);
    CHECK(catalogForHandle(db, 1, "") == 0);
    CHECK(catalogForHandle(db, 1, "t9_stranger") == 0);  // tier above distance

    // The stale-snapshot case: eligible when the menu was built, materialized
    // before the proposal committed.
    const std::vector<CatalogChoice> snapshot = eligibleCatalog(db, 1, "character");
    CHECK(handlesOf(snapshot) == std::vector<std::string>{"t0_scribe"});
    db.exec("INSERT INTO entities(id) VALUES (30)");
    CHECK(materializeCatalogEntry(db, scribe, 30, 3));
    CHECK(catalogForHandle(db, 1, "t0_scribe") == 0);  // the snapshot is stale

    // --- catalogIdForHandle: room-free and gate-free ------------------------
    // Non-zero for BOTH handles catalogForHandle just refused.
    CHECK(catalogIdForHandle(db, "t0_scribe") == scribe);   // materialized
    CHECK(catalogIdForHandle(db, "t9_stranger") == deep);   // ineligible everywhere
    CHECK(catalogIdForHandle(db, "no_such_handle") == 0);
    CHECK(catalogIdForHandle(db, "") == 0);
}

// The prospective-room menu (REQ-BARD-SEL-5): one hop past the origin, both
// kinds in one call.
static void testBardSelEligibleNewRoom() {
    const TempDbFile worldPath("textworld_bard_newroom_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    writeCatalogEntry(db, "character", "t0_scribe", "cloistered scribe",
                      "a scribe who has not left the annex in years",
                      "curiosity", 0);
    writeCatalogEntry(db, "beat", "t1_ink", "spilled ink",
                      "a dark stain, still wet", "secrecy", 1);
    writeCatalogEntry(db, "character", "t2_pilgrim", "late pilgrim",
                      "a pilgrim arrived long after the gate closed",
                      "homesickness", 2);

    // The origin (the seed room) is at distance 0, so its OWN menu stops at
    // tier 0 while the prospective room's reaches tier 1 — the two differ by
    // exactly one tier, so a menu built against the wrong distance cannot pass.
    CHECK(handlesOf(eligibleCatalog(db, 1, "character")) ==
          std::vector<std::string>{"t0_scribe"});
    CHECK(eligibleCatalog(db, 1, "beat").empty());
    CHECK(handlesOf(eligibleCatalogForNewRoom(db, 1)) ==
          (std::vector<std::string>{"t0_scribe", "t1_ink"}));

    // Both kinds arrive in that ONE call — a character and a beat.
    CHECK(handlesOf(eligibleCatalogForNewRoom(db, 1)).size() == 2);

    // It equals the menu at a REAL room of distance d+1 (the corridor), taken
    // over both kinds and still in catalog.id order.
    {
        std::vector<std::string> real =
            handlesOf(eligibleCatalog(db, 2, "character"));
        for (const std::string& h : handlesOf(eligibleCatalog(db, 2, "beat"))) {
            real.push_back(h);
        }
        std::sort(real.begin(), real.end());
        std::vector<std::string> prospective = handlesOf(eligibleCatalogForNewRoom(db, 1));
        std::sort(prospective.begin(), prospective.end());
        CHECK(real == prospective);
    }

    // An origin BFS cannot reach yields empty, and the +1 does not overflow the
    // sentinel into a very small (very permissive) distance.
    db.exec("INSERT INTO entities(id) VALUES (20)");
    db.exec("INSERT INTO room(entity) VALUES (20)");
    CHECK(distanceFromSeed(db, 20) == INT64_MAX);
    CHECK(eligibleCatalogForNewRoom(db, 20).empty());
}

// Gate (d): a knowledge beat is offered only where its subject is LIVE — the
// archetype appears in eligibleArchetypes for this room or for one a realized
// exit away (REQ-BARD-SEL-2d, -3). Both states are driven from ONE world by
// changing only the room argument, so the test cannot pass by fixture
// divergence.
static void testBardSelEligibleFact() {
    const TempDbFile worldPath("textworld_bard_fact_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // Take combat out of bootstrap and teach the player fire, so rime_touched
    // (weak to fire) enters the eligible menu of every CONTESTED room — the
    // only archetype the fixture can carry a TRUE fact about.
    db.exec("INSERT INTO meta(key, value) VALUES ('architect_spawn_count', 1)");
    db.exec("INSERT INTO known_spells(entity, spell) VALUES (3, 'fire')");
    {
        const std::vector<std::string> here = eligibleArchetypes(db, 6);
        CHECK(std::find(here.begin(), here.end(), "rime_touched") != here.end());
        CHECK(eligibleArchetypes(db, 15).empty());  // distance 3: a safe edge
    }

    // Room 21: a second safe-edge room one hop PAST the outer hall (distance
    // 4), so neither it nor its only neighbour offers anything. Room 15's exits
    // are rewritten so the EMPTY neighbour is scanned first — a
    // first-neighbour-only implementation of the union would withhold the beat.
    db.exec("INSERT INTO entities(id) VALUES (21)");
    db.exec("INSERT INTO room(entity) VALUES (21)");
    db.exec("INSERT INTO name(entity, value) VALUES (21, 'dust stair')");
    db.exec("DELETE FROM exits WHERE room = 15");
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (15, 'north', 21)");
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (15, 'west', 6)");
    db.exec("INSERT INTO exits(room, direction, dest) VALUES (21, 'south', 15)");
    CHECK(distanceFromSeed(db, 15) == 3);
    CHECK(distanceFromSeed(db, 21) == 4);

    // One knowledge beat and one ordinary entry, both tier 0 so gate (b) can
    // never be the reason either is missing.
    writeCatalogEntry(db, "beat", "rime_lore", "rime lore",
                      "frost-bitten things burn faster than they look",
                      "curiosity", 0, "rime_touched", "fire");
    writeCatalogEntry(db, "beat", "plain_beat", "plain beat",
                      "a door left open onto nothing", "secrecy", 0);

    // Withheld where the subject is nowhere live; offered one room closer,
    // where a NEIGHBOUR (not this room) has it. Only the argument changes.
    CHECK(handlesOf(eligibleCatalog(db, 21, "beat")) ==
          std::vector<std::string>{"plain_beat"});
    CHECK(handlesOf(eligibleCatalog(db, 15, "beat")) ==
          (std::vector<std::string>{"rime_lore", "plain_beat"}));

    // And offered in a room whose OWN menu carries it.
    CHECK(handlesOf(eligibleCatalog(db, 6, "beat")) ==
          (std::vector<std::string>{"rime_lore", "plain_beat"}));

    // The factless entry is unaffected in every state above — gate (d) applies
    // to knowledge beats only.
    for (const int64_t room : {int64_t{6}, int64_t{15}, int64_t{21}}) {
        const std::vector<std::string> menu = handlesOf(eligibleCatalog(db, room, "beat"));
        CHECK(std::find(menu.begin(), menu.end(), "plain_beat") != menu.end());
    }

    // Forget fire and the subject stops being live everywhere — the beat is
    // withheld even in its own room, because eligibleArchetypes says so.
    db.exec("DELETE FROM known_spells WHERE entity = 3 AND spell = 'fire'");
    CHECK(handlesOf(eligibleCatalog(db, 6, "beat")) ==
          std::vector<std::string>{"plain_beat"});
}

// --- Brick 3: the overture and wake scheduling ------------------------------

// Step 4, REQ-BARD-WAKE-16/-19/-20: the worker's LIFECYCLE, and nothing else.
// It parks and stops; it processes nothing yet. Modeled on testPregenWorker's
// (a) block, and for the same reason the spec insists on it by name — "no bard
// records in the log" would pass every row of the matrix below and prove
// nothing, because a thread that was never created and a thread sitting idle
// produce identical logs.
static void testBardWorkerLifecycle() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    // A transport that must never be called at this step — the skeleton makes
    // no request. Installing one anyway is what makes "no thread" and "a thread
    // that did nothing" distinguishable from the transport's side too.
    std::atomic<int> calls{0};
    const auto counting = [&calls](const std::string&) {
        ++calls;
        HttpResponse r;
        r.status = 200;
        return r;
    };

    // --- (a) the thread-existence matrix (REQ-BARD-WAKE-20) ----------------
    {
        // bard on (unset), AI on -> a thread exists.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        unsetenv("TEXTWORLD_AI");
        unsetenv("TEXTWORLD_BARD");
        bardRefreshEnabledForTest();
        CHECK(bardEnabled());
        CHECK(aiNarrationEnabled());
        bardResetForTest();
        bardSetWorkerTransportForTest(counting);
        bardStart();
        CHECK(bardWorkerRunning());
        bardResetForTest();
        CHECK(!bardWorkerRunning());

        // TEXTWORLD_BARD=0 -> NO thread, asserted through the hook rather than
        // through an absent log line.
        setenv("TEXTWORLD_BARD", "0", 1);
        bardRefreshEnabledForTest();
        CHECK(!bardEnabled());
        bardSetWorkerTransportForTest(counting);
        bardStart();
        CHECK(!bardWorkerRunning());
        bardResetForTest();

        // The convention, not a truthiness test: "00" is not the kill switch,
        // exactly as TEXTWORLD_AI and TEXTWORLD_PREGEN read it.
        setenv("TEXTWORLD_BARD", "00", 1);
        bardRefreshEnabledForTest();
        CHECK(bardEnabled());
        setenv("TEXTWORLD_BARD", "1", 1);
        bardRefreshEnabledForTest();
        CHECK(bardEnabled());

        // AI off (no key) -> NO thread, even with the bard on.
        unsetenv("TEXTWORLD_BARD");
        bardRefreshEnabledForTest();
        unsetenv("ANTHROPIC_API_KEY");
        CHECK(!aiNarrationEnabled());
        bardSetWorkerTransportForTest(counting);
        bardStart();
        CHECK(!bardWorkerRunning());
        bardResetForTest();
    }

    // Both gates on for the rest.
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    // --- (b) idempotence, in both directions --------------------------------
    {
        bardResetForTest();
        bardSetWorkerTransportForTest(counting);

        bardStart();
        CHECK(bardWorkerRunning());
        bardStart();  // a second start creates no second thread
        CHECK(bardWorkerRunning());

        bardStop();
        CHECK(!bardWorkerRunning());
        bardStop();  // and a second stop completes rather than hanging on a
                     // thread that is already joined
        CHECK(!bardWorkerRunning());
    }

    // bardStop() with no thread EVER started: the shutdown path main() takes
    // when the bard is off, and the one a missing guard would deadlock on.
    {
        bardResetForTest();
        bardStop();
        CHECK(!bardWorkerRunning());
    }

    // --- (c) the guard: a scope starts and joins ----------------------------
    // This is the shape main() relies on, so it is asserted as a shape rather
    // than as a sequence of calls.
    {
        bardResetForTest();
        bardSetWorkerTransportForTest(counting);
        {
            const BardGuard guard;
            CHECK(bardWorkerRunning());
        }
        CHECK(!bardWorkerRunning());
    }

    // No wake was ever submitted above, so no request was ever made — the
    // lifecycle is asserted entirely through bardWorkerRunning(), never through
    // the transport's silence.
    CHECK(calls.load() == 0);

    bardResetForTest();
}

// The eight motive keys the shipped seed carries, as the wake gate wants them.
static const std::vector<std::string>& bardTestMotives() {
    static const std::vector<std::string> motives = {
        "curiosity", "secrecy", "rivalry",  "obligation",
        "grief",     "appetite", "pride",   "homesickness"};
    return motives;
}

// A wake response that writes a focus and a journal — enough to tell "the
// proposal arrived intact" from "something arrived".
static HttpResponse cannedWake(const std::string& focus) {
    return bardCanned(nlohmann::json::array(
        {bardToolUse("write_focus", {{"text", focus}}),
         bardToolUse("write_journal", {{"text", "a note the bard kept"}})}));
}

static BardJob bardTestJob(int64_t snapshotTurn) {
    BardJob job;
    job.requestBody = R"({"model":"test","messages":[]})";
    job.motives = bardTestMotives();
    job.snapshotTurn = snapshotTurn;
    return job;
}

// Step 5, REQ-BARD-WAKE-13/-14/-16/-19/-25: the job, the state machine, and the
// worker body — against fake transports and hand-built jobs, with no database
// and no engine wiring anywhere. Modeled on testPregenWorker, and every wait
// below is on a condition this test itself satisfies: no sleeps, so a hang here
// is a design bug to be read out of the code rather than a timing knob.
static void testBardWorkerJobs() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    // --- (a) one wake, end to end -------------------------------------------
    {
        bardResetForTest();
        bardSetWorkerTransportForTest(
            [](const std::string&) { return cannedWake("the annex is watching"); });
        bardStart();
        CHECK(bardStateNow() == BardState::Idle);

        CHECK(bardSubmit(bardTestJob(11)));
        spinUntil([] { return bardStateNow() == BardState::Ready; });

        // Taken ONCE: the proposal is handed over, not copied out.
        const std::optional<WakeProposal> first = bardTakeReady();
        CHECK(first.has_value());
        CHECK(first->hasFocus);
        CHECK(first->focus == "the annex is watching");
        CHECK(first->hasJournal);
        CHECK(bardStateNow() == BardState::Idle);

        // …and only once. A second take finds nothing rather than the same
        // wake again, which is what stops one wake committing twice.
        CHECK(!bardTakeReady().has_value());
        CHECK(bardStateNow() == BardState::Idle);
    }

    // --- (b) singleness, and (c) the dirty flag -----------------------------
    // REQ-BARD-WAKE-13: at most ONE call in flight, process-wide. Held mid-call
    // so the claim is tested against a wake that is genuinely running, not
    // against one that happened to finish first.
    {
        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("held");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        CHECK(bardSubmit(bardTestJob(20)));
        spinUntil([&blocking] { return blocking.callCount() == 1; });
        CHECK(bardStateNow() == BardState::Running);

        // The flag starts clear, so the three refusals below are what set it.
        CHECK(!bardTakeDirty());

        CHECK(!bardSubmit(bardTestJob(21)));
        CHECK(!bardSubmit(bardTestJob(22)));
        CHECK(!bardSubmit(bardTestJob(23)));

        // (c) REQ-BARD-WAKE-14: three refusals, ONE flag. It is a flag and not
        // a counter on purpose — the bard owes the world one further look,
        // however many triggers it missed.
        CHECK(bardTakeDirty());
        CHECK(!bardTakeDirty());  // consumed

        // (d) The drain hook: release only once a waiter is PROVABLY inside the
        // wait. Releasing before that would let the wake finish first and turn
        // this into a test of scheduling luck.
        std::thread drain([] { bardWaitForIdleForTest(); });
        spinUntil([] { return bardWaitingCountForTest() == 1; });
        CHECK(bardWaitingCountForTest() == 1);
        CHECK(bardStateNow() == BardState::Running);  // still held

        blocking.release();
        drain.join();
        CHECK(bardWaitingCountForTest() == 0);

        spinUntil([] { return bardStateNow() == BardState::Ready; });
        // Exactly one call, and never two at once: the three refused submits
        // cost nothing, and the worker is serial.
        CHECK(blocking.callCount() == 1);
        CHECK(!blocking.concurrentEntry);
        CHECK(bardTakeReady().has_value());
    }

    // --- (e) a failing wake poisons nothing ---------------------------------
    // Both failure modes leave the slot Idle, the thread alive, and the next
    // wake acceptable (REQ-BARD-WAKE-23). A wake that fails must cost the
    // session nothing but the call.
    {
        // A transport that THROWS. There is no caller on that thread to catch
        // it, so an escape here would kill the process.
        bardResetForTest();
        bardSetWorkerTransportForTest([](const std::string&) -> HttpResponse {
            throw std::runtime_error("transport exploded");
        });
        bardStart();
        CHECK(bardSubmit(bardTestJob(30)));
        spinUntil([] { return bardStateNow() == BardState::Idle; });
        CHECK(bardWorkerRunning());
        CHECK(!bardTakeReady().has_value());
        CHECK(bardSubmit(bardTestJob(31)));  // accepted again
        bardResetForTest();
    }
    {
        // A NON-200. The gate rejects it, which is a rejection and not a fault.
        bardResetForTest();
        bardSetWorkerTransportForTest([](const std::string&) {
            HttpResponse r;
            r.status = 503;
            r.body = "upstream unavailable";
            return r;
        });
        bardStart();
        CHECK(bardSubmit(bardTestJob(40)));
        spinUntil([] { return bardStateNow() == BardState::Idle; });
        CHECK(bardWorkerRunning());
        CHECK(!bardTakeReady().has_value());
        CHECK(bardSubmit(bardTestJob(41)));
        bardResetForTest();
    }

    // A response calling NO tool is a SUCCESSFUL, EMPTY wake — the bard
    // declining to act. It reaches Ready like any other, and committing it is a
    // no-op. Treating this as a failure is the single most likely misreading.
    {
        bardResetForTest();
        bardSetWorkerTransportForTest([](const std::string&) {
            return bardCanned(nlohmann::json::array());
        });
        bardStart();
        CHECK(bardSubmit(bardTestJob(45)));
        spinUntil([] { return bardStateNow() == BardState::Ready; });
        const std::optional<WakeProposal> empty = bardTakeReady();
        CHECK(empty.has_value());
        CHECK(!empty->hasFocus);
        CHECK(!empty->hasJournal);
        CHECK(empty->appended.empty());
        CHECK(empty->seededHandles.empty());
        bardResetForTest();
    }

    // --- (f) stopping with a wake in flight ---------------------------------
    // In production g_stopping is the worker client's ABORT flag, so libcurl
    // abandons the transfer at its next progress callback rather than waiting
    // out the timeout (REQ-BARD-WAKE-20). A hermetic suite has no libcurl to
    // abort, so what is asserted here is the half that is ours: bardStop()
    // COMPLETES rather than deadlocking against a worker that needs the mutex
    // to finish its iteration. stopWorker takes no lock around the join for
    // exactly this reason, and this is the test that fails if that changes.
    {
        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("in flight at shutdown");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();
        CHECK(bardSubmit(bardTestJob(50)));
        spinUntil([&blocking] { return blocking.callCount() == 1; });

        std::thread stopper([] { bardStop(); });
        blocking.release();  // stands in for libcurl abandoning the transfer
        stopper.join();      // returns => the stop completed
        CHECK(!bardWorkerRunning());
    }

    // --- (g) a Ready result is DISCARDED at stop (REQ-BARD-WAKE-25) ----------
    // Nothing is persisted across a session boundary. The next session's first
    // irreversible event queues a fresh wake against a world that has meanwhile
    // moved on, which is a better wake than this stale one.
    {
        bardResetForTest();
        bardSetWorkerTransportForTest(
            [](const std::string&) { return cannedWake("never committed"); });
        bardStart();
        CHECK(bardSubmit(bardTestJob(60)));
        spinUntil([] { return bardStateNow() == BardState::Ready; });

        bardStop();
        CHECK(bardStateNow() == BardState::Idle);
        CHECK(!bardTakeReady().has_value());
    }

    // With the bard OFF, a submit is refused outright — no slot movement, no
    // thread, nothing to take (REQ-BARD-WAKE-20).
    {
        bardResetForTest();
        setenv("TEXTWORLD_BARD", "0", 1);
        bardRefreshEnabledForTest();
        CHECK(!bardSubmit(bardTestJob(70)));
        CHECK(bardStateNow() == BardState::Idle);
        unsetenv("TEXTWORLD_BARD");
        bardRefreshEnabledForTest();
    }

    bardResetForTest();
}

// A well-formed overture response: two entries and a journal.
static HttpResponse cannedOverture(const std::vector<std::string>& handles) {
    nlohmann::json input;
    input["entries"] = nlohmann::json::array();
    for (const std::string& h : handles) input["entries"].push_back(bardEntry(h));
    input["journal"] = "the school, as I first imagined it";
    return bardCanned(
        nlohmann::json::array({bardToolUse("write_catalog", input)}));
}

// Step 6, REQ-BARD-WAKE-2..-7: the overture. One blocking call on the main
// thread, run once per world file, that can fail in any way at all without
// costing the session anything but an empty catalog.
static void testBardOverture() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    const auto catalogCount = [](Db& db) {
        return queryInt(db, "SELECT COUNT(*) FROM catalog");
    };
    const auto journalOf = [](Db& db) {
        return queryText(db, "SELECT value FROM meta WHERE key = 'bard_journal'");
    };

    // --- spec test 6: ONCE per world file ----------------------------------
    {
        const TempDbFile worldPath("textworld_bard_overture_tests.db");

        int calls = 0;
        const HttpTransport counting = [&calls](const std::string&) {
            ++calls;
            return cannedOverture({"t0_scribe", "t0_bellringer"});
        };

        {
            OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
            CHECK(world.created);
            bardOverture(world.db, &counting);
            CHECK(calls == 1);
            CHECK(catalogCount(world.db) == 2);
            CHECK(journalOf(world.db) == "the school, as I first imagined it");
            CHECK(queryInt(world.db,
                           "SELECT COUNT(*) FROM catalog WHERE handle = 't0_scribe'") == 1);
        }

        // Reopening the SAME path: not created, so main() never calls the
        // overture, and the transport is invoked ZERO further times. The
        // condition is driven off OpenedWorld::created exactly as main() does.
        {
            OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
            CHECK(!world.created);
            if (world.created) bardOverture(world.db, &counting);
            CHECK(calls == 1);              // still one, for the file's lifetime
            CHECK(catalogCount(world.db) == 2);  // and the cast survived
        }
    }

    // --- spec test 7: every failure mode leaves an empty catalog ------------
    // Four ways for the call to fail, and none of them may throw or write.
    {
        struct Arm {
            const char* name;
            HttpTransport transport;
        };
        std::vector<Arm> arms;
        arms.push_back({"non-200", [](const std::string&) {
                            HttpResponse r;
                            r.status = 503;
                            r.body = "upstream unavailable";
                            return r;
                        }});
        arms.push_back({"throws", [](const std::string&) -> HttpResponse {
                            throw std::runtime_error("transport exploded");
                        }});
        arms.push_back({"unparseable", [](const std::string&) {
                            HttpResponse r;
                            r.status = 200;
                            r.body = "not json at all {{{";
                            return r;
                        }});
        arms.push_back({"no tool call", [](const std::string&) {
                            return bardCanned(nlohmann::json::array());
                        }});

        for (const Arm& arm : arms) {
            const TempDbFile worldPath("textworld_bard_overture_fail.db");
            OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
            Db& db = world.db;

            bardOverture(db, &arm.transport);  // must not throw

            CHECK(catalogCount(db) == 0);
            CHECK(journalOf(db).empty());

            // And the game still plays. Driven with tickT + a fake transport,
            // which is what makes this a statement about the SESSION rather
            // than about bardOverture's return.
            const int64_t turnBefore = queryInt(
                db, "SELECT value FROM meta WHERE key = 'turn'");
            const HttpTransport fake = [](const std::string&) {
                return cannedCreateRoom("unused", "unused");
            };
            tickT(db, Action{Verb::Look, 0, ""}, fake);
            CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") ==
                  turnBefore + 1);
            CHECK(!render(db, turnBefore + 1).empty());
        }
    }

    // --- spec test 8: ONE transaction, all or nothing (REQ-BARD-WAKE-7) -----
    // Admission is made to throw MID-WAY by a trigger that aborts on the second
    // entry's handle — a genuine engine fault, which is the only thing that can
    // reach past catalogEntryRefusal's pre-flight. All three entries must be
    // absent afterwards, not merely the one that failed.
    {
        const TempDbFile worldPath("textworld_bard_overture_txn.db");
        OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        Db& db = world.db;
        db.exec(
            "CREATE TRIGGER bard_overture_boom BEFORE INSERT ON catalog "
            "WHEN NEW.handle = 'boom' BEGIN SELECT RAISE(ABORT, 'boom'); END");

        const HttpTransport t = [](const std::string&) {
            return cannedOverture({"first_ok", "boom", "third_ok"});
        };
        bardOverture(db, &t);

        CHECK(catalogCount(db) == 0);  // not one, not two — zero
        CHECK(journalOf(db).empty());

        // bardOverture RETURNED rather than merely not crashing: asserted by
        // executing a statement after the call, on the same handle.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta") > 0);
    }

    // --- micro-decision 15: the BUILDERS are inside the guard ----------------
    // Not merely the transport call. With motive_catalog gone, motiveKeys and
    // buildOvertureContext are ordinary db.hpp callers that throw — and an
    // escape from here would reach main()'s catch and end the session on world
    // creation, which is the exact inverse of the degradation claim.
    {
        const TempDbFile worldPath("textworld_bard_overture_builder.db");
        OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        Db& db = world.db;
        db.exec("DROP TABLE motive_catalog");

        int calls = 0;
        const HttpTransport counting = [&calls](const std::string&) {
            ++calls;
            return cannedOverture({"never_built"});
        };
        bardOverture(db, &counting);  // must not throw

        CHECK(catalogCount(db) == 0);
        CHECK(journalOf(db).empty());
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta") > 0);  // returned
    }

    // --- spec test 12b: disabled means NO CALL AT ALL -----------------------
    // Not "a call whose result is discarded" — the transport is never even
    // constructed, and the counting transport below proves it was never run.
    {
        const TempDbFile worldPath("textworld_bard_overture_off.db");
        OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
        Db& db = world.db;

        int calls = 0;
        const HttpTransport counting = [&calls](const std::string&) {
            ++calls;
            return cannedOverture({"never_asked"});
        };

        // AI off (no key), bard on.
        unsetenv("ANTHROPIC_API_KEY");
        bardRefreshEnabledForTest();
        CHECK(!aiNarrationEnabled());
        bardOverture(db, &counting);
        CHECK(calls == 0);
        CHECK(catalogCount(db) == 0);

        // AI on, bard off.
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
        setenv("TEXTWORLD_BARD", "0", 1);
        bardRefreshEnabledForTest();
        CHECK(!bardEnabled());
        bardOverture(db, &counting);
        CHECK(calls == 0);
        CHECK(catalogCount(db) == 0);

        unsetenv("TEXTWORLD_BARD");
        bardRefreshEnabledForTest();
    }
}

// Advance `n` turns with no qualifying event, so the rate ceiling stops
// masking whatever the caller is actually testing. tickT + a fake transport,
// never runTurn: the bard requires aiNarrationEnabled(), and runTurn with
// narration on reaches the PRODUCTION resolver and prose transports, which the
// suite forbids.
static void bardAdvanceTurns(Db& db, int n) {
    const HttpTransport fake = [](const std::string&) {
        return cannedCreateRoom("unused", "unused");
    };
    for (int i = 0; i < n; ++i) tickT(db, Action{Verb::Look, 0, ""}, fake);
}

// Step 7, REQ-BARD-WAKE-8..-12: trigger evaluation — the ceiling, the query,
// the snapshot, the stamp, and the submit, in that order, because the order IS
// the requirement.
static void testBardTrigger() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    const auto wakeStamp = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'bard_last_wake_turn'");
    };
    const auto turnOf = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    };

    // --- spec test 9: the non-qualifying verbs queue NOTHING ---------------
    // The whole reversible half of the vocabulary. If any of these ever wakes
    // the bard, the ceiling is the only thing standing between the player and a
    // wake on every turn.
    {
        const TempDbFile worldPath("textworld_bard_trigger_quiet.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        std::atomic<int> calls{0};
        bardSetWorkerTransportForTest([&calls](const std::string&) {
            ++calls;
            return cannedWake("should never happen");
        });
        bardStart();

        bardAdvanceTurns(db, 10);  // well past the ceiling
        // 'examined' belongs here and nowhere else (REQ-EXAMINE-14): observing
        // something changes nothing and is not irreversible, so bard.cpp's wake
        // predicate is deliberately NOT extended for it.
        for (const char* verb :
             {"moved", "took", "looked", "waited", "failed", "examined"}) {
            appendEvent(db, 3, verb, 0, 0, nullptr);
            bardAfterTurn(db);
        }
        CHECK(bardStateNow() == BardState::Idle);
        CHECK(calls.load() == 0);
        CHECK(wakeStamp(db) == 0);  // never stamped, because never queued
        bardResetForTest();
    }

    // --- spec test 10: one sub-case PER qualifying verb ---------------------
    // Four separate worlds, so no verb can pass on another's leftovers.
    for (const char* verb : {"generated", "defeated", "learned", "materialized"}) {
        const TempDbFile worldPath("textworld_bard_trigger_verb.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        std::atomic<int> calls{0};
        bardSetWorkerTransportForTest([&calls](const std::string&) {
            ++calls;
            return cannedWake("woken");
        });
        bardStart();

        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, verb, 0, 0, nullptr);
        const int64_t at = turnOf(db);
        bardAfterTurn(db);

        // Exactly one wake, and the stamp landed on the QUEUEING turn.
        bardWaitForIdleForTest();
        CHECK(calls.load() == 1);
        CHECK(wakeStamp(db) == at);
        bardResetForTest();
    }

    // --- spec test 11: the ceiling, checked FIRST (REQ-BARD-WAKE-9) ---------
    {
        const TempDbFile worldPath("textworld_bard_trigger_ceiling.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        std::atomic<int> calls{0};
        bardSetWorkerTransportForTest([&calls](const std::string&) {
            ++calls;
            return cannedWake("woken");
        });
        bardStart();

        // Inside the gap: turn - bard_last_wake_turn < kBardMinTurnGap, and a
        // fully qualifying event queues NOTHING.
        bardAdvanceTurns(db, static_cast<int>(kBardMinTurnGap) - 1);
        CHECK(turnOf(db) - wakeStamp(db) < kBardMinTurnGap);
        appendEvent(db, 3, "defeated", 0, 0, nullptr);
        bardAfterTurn(db);
        CHECK(calls.load() == 0);
        CHECK(bardStateNow() == BardState::Idle);

        // Past the gap, the SAME event queues — it was deferred, not dropped,
        // because the stamp is still 0 and the query's window still contains it.
        bardAdvanceTurns(db, 2);
        CHECK(turnOf(db) - wakeStamp(db) >= kBardMinTurnGap);
        bardAfterTurn(db);
        bardWaitForIdleForTest();
        CHECK(calls.load() == 1);
        bardResetForTest();
    }

    // --- spec test 12: stamped at QUEUE time, not at completion -------------
    // Asserted while the transport is still INSIDE the call, which is what
    // makes "stamped at queue time" a fact rather than a comment. If the stamp
    // moved to completion, an in-flight wake could re-trigger itself.
    {
        const TempDbFile worldPath("textworld_bard_trigger_stamp.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("held mid-call");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, "generated", 0, 0, nullptr);
        const int64_t at = turnOf(db);
        bardAfterTurn(db);

        spinUntil([&blocking] { return blocking.callCount() == 1; });
        CHECK(bardStateNow() == BardState::Running);
        CHECK(wakeStamp(db) == at);  // ALREADY stamped, with the call in flight

        // And the in-flight wake cannot re-trigger itself off its own window:
        // a further evaluation while Running queues nothing.
        bardAfterTurn(db);
        CHECK(blocking.callCount() == 1);

        blocking.release();
        bardWaitForIdleForTest();
        bardResetForTest();
    }

    // --- spec test 12a: the player's text is COMPLETE before the bard runs --
    // The bard's cost is paid after the turn, never inside it. Captured as the
    // rendered bytes at the moment bardAfterTurn is entered, compared against a
    // world where the bard never existed.
    {
        std::string bardOffBytes;
        {
            const TempDbFile offPath("textworld_bard_trigger_12a_off.db");
            Db db = openWorld(offPath.string(), "tests/combat_fixture.sql").db;
            bardAdvanceTurns(db, 10);
            appendEvent(db, 3, "defeated", 0, 0, nullptr);
            bardOffBytes = render(db, queryInt(
                db, "SELECT value FROM meta WHERE key = 'turn'"));
        }

        const TempDbFile worldPath("textworld_bard_trigger_12a.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("held");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, "defeated", 0, 0, nullptr);

        // The bytes the player has, at the instant the hook is entered…
        const std::string shown = render(db, turnOf(db));
        CHECK(shown == bardOffBytes);
        CHECK(blocking.callCount() == 0);  // …and nothing has been called yet

        bardAfterTurn(db);
        spinUntil([&blocking] { return blocking.callCount() == 1; });
        // The text did not change because the bard ran.
        CHECK(render(db, turnOf(db)) == shown);

        blocking.release();
        bardWaitForIdleForTest();
        bardResetForTest();
    }

    // --- micro-decision 15: a broken evaluation cannot cost a turn ----------
    // With the stamp row deleted the evaluation path still reads it as 0 rather
    // than throwing, so the throw is induced where it can actually happen: the
    // events table itself. Whatever breaks, the turn the player already paid
    // for must survive it.
    {
        const TempDbFile worldPath("textworld_bard_trigger_broken.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        std::atomic<int> calls{0};
        bardSetWorkerTransportForTest([&calls](const std::string&) {
            ++calls;
            return cannedWake("never");
        });
        bardStart();

        bardAdvanceTurns(db, 10);
        const int64_t turnBefore = turnOf(db);
        db.exec("DROP TABLE events");  // the trigger query now throws

        bardAfterTurn(db);  // must not throw

        // The session continues: statements still execute on the same handle,
        // the turn is exactly where the tick left it, and nothing was queued.
        // (render is not called here — it reads `events` too, so it would be
        // asserting the fixture's damage rather than the bard's behavior.)
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta") > 0);
        CHECK(turnOf(db) == turnBefore);
        CHECK(calls.load() == 0);
        CHECK(bardStateNow() == BardState::Idle);
        bardResetForTest();
    }
}

// A wake calling all four tools, so a partial application is visible as a
// partial application rather than as nothing.
static HttpResponse cannedFullWake(const std::string& focus,
                                   const std::string& appendHandle,
                                   const std::string& seedHandle) {
    nlohmann::json entryInput;
    entryInput["entry"] = bardEntry(appendHandle);
    return bardCanned(nlohmann::json::array(
        {bardToolUse("write_focus", {{"text", focus}}),
         bardToolUse("write_journal", {{"text", "what I made of it"}}),
         bardToolUse("append_catalog", entryInput),
         bardToolUse("mark_seeded", {{"handle", seedHandle}})}));
}

// Run one wake all the way to Ready and leave it there, uncommitted.
static void bardRunOneWakeToReady(Db& db, const HttpResponse& response) {
    bardResetForTest();
    bardSetWorkerTransportForTest(
        [response](const std::string&) { return response; });
    bardStart();
    bardAdvanceTurns(db, 10);
    appendEvent(db, 3, "defeated", 0, 0, nullptr);
    bardAfterTurn(db);  // queues
    spinUntil([] { return bardStateNow() == BardState::Ready; });
}

// Step 8, REQ-BARD-WAKE-21..-24: committing a ready result. Its own
// transaction, run before the evaluation, and re-checked against the world as
// it is NOW rather than as it was when the wake was snapshotted.
static void testBardCommit() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    const auto turnOf = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    };
    const auto stampOf = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'bard_last_wake_turn'");
    };
    const auto focusOf = [](Db& db) {
        return queryText(db, "SELECT value FROM meta WHERE key = 'bard_focus'");
    };
    const auto journalOf = [](Db& db) {
        return queryText(db, "SELECT value FROM meta WHERE key = 'bard_journal'");
    };
    // A canonical snapshot of everything a wake may write, compared as ONE
    // string so a failure prints as a diff rather than as a mystery.
    const auto storySnapshot = [&](Db& db) {
        std::string out;
        Stmt s = db.prepare(
            "SELECT id, kind, handle, name, blurb, motive, tier, seeded "
            "FROM catalog ORDER BY id");
        while (s.step()) {
            for (int c = 0; c < 8; ++c) out += s.colText(c) + "\x1f";
            out += "\x1e";
        }
        return out + "focus=" + focusOf(db) + "\x1e" + "journal=" + journalOf(db);
    };

    // --- spec test 16: a wake is NOT a turn (REQ-BARD-WAKE-21, -22) ---------
    {
        const TempDbFile worldPath("textworld_bard_commit_ok.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        // Something for mark_seeded to resolve to.
        const int64_t existing = writeCatalogEntry(db, "character", "t0_seed_me",
                                                   "seed me", "an entry to latch",
                                                   "curiosity", 0);
        CHECK(existing > 0);

        bardRunOneWakeToReady(db, cannedFullWake("the annex is watching",
                                                 "t0_appended", "t0_seed_me"));

        const int64_t turnBefore = turnOf(db);
        const int64_t stampBefore = stampOf(db);
        bardAfterTurn(db);  // commits the ready result

        // meta.turn is BYTE-IDENTICAL: committing a wake does not advance the
        // world's clock, because a wake is not a turn.
        CHECK(turnOf(db) == turnBefore);
        // The stamp stays at its QUEUE-time value; commit never moves it.
        CHECK(stampOf(db) == stampBefore);

        // …and the wake's writes are all present.
        CHECK(focusOf(db) == "the annex is watching");
        CHECK(journalOf(db) == "what I made of it");
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM catalog WHERE handle = 't0_appended'") == 1);
        CHECK(queryInt(db, "SELECT seeded FROM catalog WHERE handle = 't0_seed_me'") == 1);

        // Taken, not left: a committed wake cannot be committed twice.
        CHECK(bardStateNow() != BardState::Ready);
        bardResetForTest();
    }

    // --- spec test 17: a throw at commit rolls back EVERYTHING --------------
    // A genuine engine fault mid-application — the only kind that reaches past
    // catalogEntryRefusal's pre-flight — must leave the story side exactly as
    // it was, not partly written (REQ-BARD-WAKE-23).
    {
        const TempDbFile worldPath("textworld_bard_commit_throw.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        writeCatalogEntry(db, "character", "t0_present", "present",
                          "an entry that was already here", "pride", 0);
        writeBardFocus(db, "the focus before the wake");
        writeBardJournal(db, "the journal before the wake");

        db.exec(
            "CREATE TRIGGER bard_commit_boom BEFORE INSERT ON catalog "
            "WHEN NEW.handle = 'boom' BEGIN SELECT RAISE(ABORT, 'boom'); END");

        bardRunOneWakeToReady(db, cannedFullWake("a focus that never lands",
                                                 "boom", "t0_present"));

        const std::string before = storySnapshot(db);
        const int64_t stampBefore = stampOf(db);
        const int64_t turnBefore = turnOf(db);

        bardAfterTurn(db);  // must not throw

        // Byte-identical: the focus and journal that applyWakeProposal writes
        // BEFORE the failing entry are rolled back along with it.
        CHECK(storySnapshot(db) == before);
        CHECK(stampOf(db) == stampBefore);  // not advanced on failure
        CHECK(turnOf(db) == turnBefore);

        // And a failed commit poisons nothing: the state is back to Idle and
        // the stamp is unmoved, so the next qualifying event past the ceiling
        // queues a FRESH wake. (The worker keeps the transport it was started
        // with — workerMain copies it at thread start — so what changes here is
        // the WORLD: the fault is removed, and the same wake now lands.)
        CHECK(bardStateNow() == BardState::Idle);
        db.exec("DROP TRIGGER bard_commit_boom");
        bardAdvanceTurns(db, static_cast<int>(kBardMinTurnGap) + 1);
        appendEvent(db, 3, "learned", 0, 0, nullptr);
        bardAfterTurn(db);
        spinUntil([] { return bardStateNow() == BardState::Ready; });
        bardAfterTurn(db);
        CHECK(focusOf(db) == "a focus that never lands");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle = 'boom'") == 1);
        bardResetForTest();
    }

    // --- spec test 18: the snapshot's AGE does not matter -------------------
    // The re-check REQ-BARD-WAKE-24 asks for happens at commit, live: an
    // appended handle taken in the meantime is refused, a mark_seeded handle
    // that no longer resolves is ignored, and the REST of the wake still lands.
    {
        const TempDbFile worldPath("textworld_bard_commit_stale.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        // The wake proposes 'contested' and marks 'vanished' as seeded.
        nlohmann::json entryInput;
        entryInput["entry"] = bardEntry("contested");
        const HttpResponse stale = bardCanned(nlohmann::json::array(
            {bardToolUse("write_focus", {{"text", "the focus still lands"}}),
             bardToolUse("write_journal", {{"text", "the journal still lands"}}),
             bardToolUse("append_catalog", entryInput),
             bardToolUse("mark_seeded", {{"handle", "vanished"}})}));

        bardRunOneWakeToReady(db, stale);

        // …and only NOW, with the wake already snapshotted and waiting, does
        // the world move underneath it: another entry takes the handle, and
        // 'vanished' never existed to begin with.
        writeCatalogEntry(db, "character", "contested", "the other one",
                          "an entry that got there first", "rivalry", 0);
        bardAdvanceTurns(db, 20);  // many turns older than the snapshot

        bardAfterTurn(db);

        // The two impossible parts are dropped…
        CHECK(queryInt(db,
                       "SELECT COUNT(*) FROM catalog WHERE handle = 'contested'") == 1);
        CHECK(queryText(db,
                        "SELECT blurb FROM catalog WHERE handle = 'contested'") ==
              "an entry that got there first");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM catalog WHERE handle = 'vanished'") == 0);

        // …and everything else in the same wake still applies. A stale part
        // costs its own part and nothing more.
        CHECK(focusOf(db) == "the focus still lands");
        CHECK(journalOf(db) == "the journal still lands");
        bardResetForTest();
    }

    // An EMPTY wake — the bard declining to act — commits cleanly and changes
    // nothing. It is a success, and the commit path must treat it as one.
    {
        const TempDbFile worldPath("textworld_bard_commit_empty.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        bardSetWorkerTransportForTest([](const std::string&) {
            return bardCanned(nlohmann::json::array());
        });
        bardStart();
        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, "defeated", 0, 0, nullptr);
        bardAfterTurn(db);
        spinUntil([] { return bardStateNow() == BardState::Ready; });

        const std::string before = storySnapshot(db);
        const int64_t turnBefore = turnOf(db);
        bardAfterTurn(db);
        CHECK(storySnapshot(db) == before);
        CHECK(turnOf(db) == turnBefore);
        CHECK(bardStateNow() == BardState::Idle);
        bardResetForTest();
    }
}

// Step 9, REQ-BARD-WAKE-13/-14/-15: coalescing, composed through the ENGINE
// rather than through the worker alone. Steps 5, 7 and 8 built every piece;
// what is new here is the claim that the trigger path, the state machine and
// the commit path together produce ONE call for a burst of events. A defect
// found here belongs in step 5's or step 7's code, never in a special case.
static void testBardCoalesce() {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();

    const auto turnOf = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    };
    const auto stampOf = [](Db& db) {
        return queryInt(db, "SELECT value FROM meta WHERE key = 'bard_last_wake_turn'");
    };

    // --- spec tests 13 + 15: three events, three ticks, ONE call ------------
    {
        const TempDbFile worldPath("textworld_bard_coalesce_one.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("one call for three kills");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        bardAdvanceTurns(db, 10);

        // Three defeats across three CONSECUTIVE ticks, each followed by the
        // post-turn hook exactly as main() does it.
        for (int i = 0; i < 3; ++i) {
            bardAdvanceTurns(db, 1);
            appendEvent(db, 3, "defeated", 0, 0, nullptr);
            bardAfterTurn(db);
        }

        // The release happens only once a drain is PROVABLY inside the wait, so
        // this cannot pass by accident of timing.
        std::thread drain([] { bardWaitForIdleForTest(); });
        spinUntil([] { return bardWaitingCountForTest() == 1; });
        CHECK(blocking.callCount() == 1);
        CHECK(!blocking.concurrentEntry);

        blocking.release();
        drain.join();
        spinUntil([] { return bardStateNow() == BardState::Ready; });
        CHECK(blocking.callCount() == 1);   // still one, after it finished
        CHECK(!blocking.concurrentEntry);
        bardResetForTest();
    }

    // --- spec test 14: exactly ONE further wake — not zero, not two ---------
    // The ceiling is deliberately taken out of the picture first, so what is
    // being measured is the dirty flag and nothing else.
    {
        const TempDbFile worldPath("textworld_bard_coalesce_further.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("the first wake");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, "defeated", 0, 0, nullptr);
        bardAfterTurn(db);  // wake one, now held mid-call
        spinUntil([&blocking] { return blocking.callCount() == 1; });
        const int64_t queuedAt = stampOf(db);

        // A trigger arrives DURING the running wake. Advance well past the
        // ceiling first, so that the ceiling cannot be what suppresses the
        // second wake and the flag is doing the work.
        bardAdvanceTurns(db, static_cast<int>(kBardMinTurnGap) + 2);
        appendEvent(db, 3, "learned", 0, 0, nullptr);
        bardAfterTurn(db);            // busy: recorded as the flag, not queued
        CHECK(blocking.callCount() == 1);
        CHECK(stampOf(db) == queuedAt);  // nothing was queued, nothing stamped

        // …and two more, to prove the flag is a FLAG and not a counter.
        appendEvent(db, 3, "materialized", 0, 0, nullptr);
        bardAfterTurn(db);
        appendEvent(db, 3, "generated", 0, 0, nullptr);
        bardAfterTurn(db);
        CHECK(blocking.callCount() == 1);

        blocking.release();
        spinUntil([] { return bardStateNow() == BardState::Ready; });

        // The turn after that wake commits: commitReady runs first, then the
        // evaluation — which is exactly why the further wake lands on THIS turn
        // rather than the next one.
        bardAfterTurn(db);
        spinUntil([&blocking] { return blocking.callCount() == 2; });
        CHECK(blocking.callCount() == 2);

        blocking.release();  // already released; the second call passes through
        spinUntil([] { return bardStateNow() == BardState::Ready; });
        bardAfterTurn(db);   // commit wake two; the ceiling now blocks a third

        // EXACTLY two: not one (the triggers were not lost) and not four (one
        // per trigger). The bard owes the world one further look, however many
        // triggers it missed.
        CHECK(blocking.callCount() == 2);
        CHECK(bardStateNow() == BardState::Idle);
        CHECK(!bardTakeDirty());  // and the flag settled clear
        bardResetForTest();
    }

    // --- (c) the REALISTIC window: the ceiling MASKING the flag -------------
    // With the default 5-turn gap against a call that can run for seconds, a
    // trigger arriving 1–3 turns after the queue returns at the CEILING (step
    // 7's ordering, which REQ-BARD-WAKE-9 mandates) and never reaches the dirty
    // branch at all. Without this case, micro-decision 9's "the flag is belt to
    // the query's braces" claim is untested — and this is the common path, the
    // one no other sub-case exercises.
    {
        const TempDbFile worldPath("textworld_bard_coalesce_masked.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        bardResetForTest();
        BlockingTransport blocking;
        blocking.canned = cannedWake("the only wake so far");
        bardSetWorkerTransportForTest(
            [&blocking](const std::string& body) { return blocking(body); });
        bardStart();

        bardAdvanceTurns(db, 10);
        appendEvent(db, 3, "defeated", 0, 0, nullptr);
        bardAfterTurn(db);
        spinUntil([&blocking] { return blocking.callCount() == 1; });
        const int64_t queuedAt = stampOf(db);
        CHECK(queuedAt == turnOf(db));

        // Triggers at +1, +2, +3 — all INSIDE the gap, so the ceiling returns
        // before the flag is ever consulted. The flag stays clear.
        for (int i = 0; i < 3; ++i) {
            bardAdvanceTurns(db, 1);
            appendEvent(db, 3, "defeated", 0, 0, nullptr);
            CHECK(turnOf(db) - stampOf(db) < kBardMinTurnGap);
            bardAfterTurn(db);
        }
        CHECK(blocking.callCount() == 1);

        blocking.release();
        spinUntil([] { return bardStateNow() == BardState::Ready; });
        bardAfterTurn(db);  // commit; the ceiling still blocks an evaluation

        // THE POINT: those three events were not lost. Because the stamp was
        // taken at QUEUE time, they are still inside the trigger query's
        // window, so the first evaluation past the ceiling finds them — with no
        // help from the dirty flag, which the ceiling never let anyone read.
        bardAdvanceTurns(db, static_cast<int>(kBardMinTurnGap) + 1);
        CHECK(turnOf(db) - stampOf(db) >= kBardMinTurnGap);
        bardAfterTurn(db);
        spinUntil([&blocking] { return blocking.callCount() == 2; });
        CHECK(blocking.callCount() == 2);  // exactly one further wake
        CHECK(stampOf(db) > queuedAt);

        spinUntil([] { return bardStateNow() == BardState::Ready; });
        bardResetForTest();
    }
}

// --- Step 10: the degradation claim -----------------------------------------
//
// "The bard failing never makes the game worse than not having a bard" is the
// reason this brick has the shape it has, and spec test 21 is where that stops
// being a sentence in a design. ONE scripted session is run against seven fresh
// worlds from the same fixture, and every failing arm's concatenated
// player-facing bytes are compared against the arm where the bard never
// existed.

struct BardArm {
    const char* name = "";
    bool bardOn = true;
    bool aiOn = true;
    bool startWorker = true;
    HttpTransport overtureTransport;  // never null: the suite reaches no
                                      // production transport, ever
    HttpTransport wakeTransport;
    bool boomTrigger = false;  // admission throws at commit
    bool dropMotives = false;  // the snapshot builders throw at evaluation
};

// What one arm produced. The bytes are the claim; the other two are the
// corroborating evidence that the arms really ran the same session.
struct BardArmResult {
    std::string bytes;   // every turn's player-facing output, concatenated
    int64_t turn = 0;    // meta.turn at the end
    std::string events;  // the whole events log, canonically rendered
};

// One session, scripted identically for every arm. Returns the concatenation of
// every turn's player-facing bytes.
//
// Turns are driven with tickT + render rather than runTurn (plan micro-decision
// 16): the bard requires aiNarrationEnabled(), and runTurn with narration on
// reaches the PRODUCTION resolver and prose transports, which the suite forbids.
// bardAfterTurn is called after each turn's text is captured, mirroring main().
static BardArmResult bardDegradationRun(const BardArm& arm) {
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    const ScopedEnvVar bardGuardEnv("TEXTWORLD_BARD");

    unsetenv("TEXTWORLD_AI");
    if (arm.aiOn) {
        setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    } else {
        unsetenv("ANTHROPIC_API_KEY");
    }
    if (arm.bardOn) {
        unsetenv("TEXTWORLD_BARD");
    } else {
        setenv("TEXTWORLD_BARD", "0", 1);
    }
    bardRefreshEnabledForTest();

    const TempDbFile worldPath("textworld_bard_degrade.db");
    OpenedWorld world = openWorld(worldPath.string(), "tests/combat_fixture.sql");
    Db& db = world.db;

    if (arm.dropMotives) db.exec("DROP TABLE motive_catalog");
    if (arm.boomTrigger) {
        db.exec(
            "CREATE TRIGGER bard_degrade_boom BEFORE INSERT ON catalog "
            "WHEN NEW.handle = 'boom' BEGIN SELECT RAISE(ABORT, 'boom'); END");
    }

    bardResetForTest();
    if (arm.wakeTransport) bardSetWorkerTransportForTest(arm.wakeTransport);

    // main()'s shape: the overture at creation, then the workers.
    if (world.created) bardOverture(db, &arm.overtureTransport);
    if (arm.startWorker) bardStart();

    // The scripted session: movement, a take, and three irreversible events
    // spaced so the ceiling is crossed between them and every arm gets real
    // chances to wake, fail, and commit.
    const HttpTransport fake = [](const std::string&) {
        return cannedCreateRoom("unused", "unused");
    };
    const char* const injectAt[] = {"defeated", "learned", "materialized"};

    BardArmResult result;
    for (int turn = 1; turn <= 24; ++turn) {
        Action a{Verb::Look, 0, ""};
        if (turn % 8 == 3) a = Action{Verb::Go, 0, "north"};
        if (turn % 8 == 5) a = Action{Verb::Wait, 0, ""};

        tickT(db, a, fake);
        const int64_t now = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");

        // One irreversible event every eighth turn — spaced past
        // kBardMinTurnGap, so the ceiling is not what is being tested here.
        if (turn % 8 == 0) appendEvent(db, 3, injectAt[(turn / 8) - 1], 0, 0, nullptr);

        // The player's bytes, captured BEFORE the bard is given the turn —
        // exactly the order main() uses (flush, then hook).
        result.bytes += render(db, now);
        result.bytes += "\x1e";

        bardAfterTurn(db);
    }

    // Drain whatever is in flight, so an arm cannot "pass" by having its
    // failure still pending when the comparison happens.
    if (arm.startWorker) bardWaitForIdleForTest();

    // The corroborating evidence, read while the world file is still open.
    result.turn = queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
    {
        Stmt s = db.prepare(
            "SELECT turn, actor, verb, subject, object, IFNULL(detail,'') "
            "FROM events ORDER BY id");
        while (s.step()) {
            for (int c = 0; c < 6; ++c) result.events += s.colText(c) + "\x1f";
            result.events += "\x1e";
        }
    }

    bardResetForTest();
    return result;
}

// Spec test 21. Seven arms, one comparison each: a failure prints as a diff of
// the whole session rather than as a mystery.
static void testBardDegradation() {
    // Shared canned responses. Every transport here is counted, so the suite's
    // "zero network calls" rule is not merely assumed.
    std::atomic<int> overtureCalls{0};
    std::atomic<int> wakeCalls{0};

    const HttpTransport goodOverture = [&overtureCalls](const std::string&) {
        ++overtureCalls;
        return cannedOverture({"t0_scribe", "t0_bellringer"});
    };
    const HttpTransport failedOverture = [&overtureCalls](const std::string&) {
        ++overtureCalls;
        HttpResponse r;
        r.status = 500;
        r.body = "upstream on fire";
        return r;
    };
    const HttpTransport throwingWake =
        [&wakeCalls](const std::string&) -> HttpResponse {
        ++wakeCalls;
        throw std::runtime_error("wake transport exploded");
    };
    const HttpTransport unparseableWake = [&wakeCalls](const std::string&) {
        ++wakeCalls;
        HttpResponse r;
        r.status = 200;
        r.body = "}{ not json";
        return r;
    };
    const HttpTransport boomWake = [&wakeCalls](const std::string&) {
        ++wakeCalls;
        return cannedFullWake("a focus that never lands", "boom", "t0_scribe");
    };

    // Arm 1, the baseline: the bard never existed. Every other arm is compared
    // against this.
    BardArm baseline;
    baseline.name = "baseline (TEXTWORLD_BARD=0)";
    baseline.bardOn = false;
    baseline.startWorker = false;
    baseline.overtureTransport = goodOverture;
    const BardArmResult expected = bardDegradationRun(baseline);
    CHECK(!expected.bytes.empty());
    CHECK(!expected.events.empty());
    CHECK(expected.turn == 24);

    std::vector<BardArm> arms;
    {
        BardArm a;
        a.name = "overture failed";
        a.overtureTransport = failedOverture;
        a.wakeTransport = unparseableWake;
        arms.push_back(a);
    }
    {
        BardArm a;
        a.name = "worker never started";
        a.startWorker = false;  // AI on, bard on, but no thread
        a.overtureTransport = goodOverture;
        a.wakeTransport = unparseableWake;
        arms.push_back(a);
    }
    {
        BardArm a;
        a.name = "worker threw";
        a.overtureTransport = goodOverture;
        a.wakeTransport = throwingWake;
        arms.push_back(a);
    }
    {
        BardArm a;
        a.name = "commit threw";
        a.overtureTransport = goodOverture;
        a.wakeTransport = boomWake;
        a.boomTrigger = true;
        arms.push_back(a);
    }
    {
        BardArm a;
        a.name = "every wake failed";
        a.overtureTransport = goodOverture;
        a.wakeTransport = unparseableWake;
        arms.push_back(a);
    }
    {
        // Not in the spec's list: micro-decision 15's failure mode, and the one
        // that would otherwise reach main()'s catch and END THE RUN rather than
        // degrade. With motive_catalog gone the snapshot builders throw on
        // every evaluation.
        BardArm a;
        a.name = "evaluation threw";
        a.overtureTransport = goodOverture;
        a.wakeTransport = unparseableWake;
        a.dropMotives = true;
        arms.push_back(a);
    }

    for (const BardArm& arm : arms) {
        const BardArmResult got = bardDegradationRun(arm);

        // ONE string comparison per arm: byte-identical to a session in which
        // the bard was never built. A failure prints as a diff of the whole
        // session rather than as a mystery.
        CHECK(got.bytes == expected.bytes);

        // The world's clock did not move differently…
        CHECK(got.turn == expected.turn);

        // …and the events log is identical too. The bard is allowed to add
        // rows in principle; in every arm here it adds NONE, because no arm
        // commits a wake successfully. An arm that quietly logged something
        // would pass the bytes comparison and fail this one.
        CHECK(got.events == expected.events);

        if (got.bytes != expected.bytes || got.turn != expected.turn ||
            got.events != expected.events) {
            std::printf("  degradation arm '%s' diverged\n", arm.name);
        }
    }

    // THE ANTI-VACUITY CHECK, and the most important assertion in this test
    // after the comparisons themselves. Seven byte-identical sessions is also
    // exactly what a bard that never ran would produce, so the arms have to be
    // shown to have really tried:
    //
    //   * FIVE of the seven arms reach the overture's transport. The baseline
    //     never does — its gate returns before one is touched — and neither
    //     does "evaluation threw", because dropping motive_catalog makes the
    //     builders throw BEFORE the call, which is itself the requirement:
    //     REQ-BARD-WAKE-6 says a builder fault costs the call, not the session.
    //   * every arm with a worker fires wakes off the injected events, so the
    //     wake count is positive — and it is these calls, each failing in its
    //     own way, that the identical bytes are a statement ABOUT.
    //
    // Both counters also account for every invocation made here, which is how
    // the suite's hermetic rule stays true: no production transport is ever
    // constructed on any path above.
    CHECK(overtureCalls.load() == 5);
    CHECK(wakeCalls.load() > 0);
}

// --- REQ-LOG-1/-3 source guard ----------------------------------------------
// The migration is complete only if it STAYS complete. In the same family as
// the band read-only-ness and main()-declaration-order guards: assert as source
// text what no behavioral test can, because a stray write to the error channel
// is invisible from inside the test binary (REQ-LOG-28 keeps the backstop out
// of it) and shows up only as a smear on a player's screen.
static void testLogSourceGuards() {
    // src/log.cpp is the ONE unit allowed to touch the error channel: it owns
    // the default writer and the REQ-LOG-2 terminal duplicate. Every other unit
    // goes through logEmit / logToTerminal. Note world.cpp and main.cpp are on
    // this list too — their REQ-LOG-2 exemptions route through logToTerminal
    // rather than through a direct write of their own.
    for (const char* file :
         {"src/bard.cpp", "src/bardworker.cpp", "src/architect.cpp",
          "src/prose.cpp", "src/nlresolve.cpp", "src/pregen.cpp",
          "src/mutations.cpp", "src/world.cpp", "src/main.cpp",
          "src/profile.cpp", "src/loop.cpp", "src/systems.cpp",
          "src/combat.cpp", "src/render.cpp", "src/band.cpp", "src/db.cpp",
          "src/term.cpp", "src/parser.cpp", "src/aihttp.cpp"}) {
        const std::string src = readFileBytes(file);
        CHECK(!src.empty());
        CHECK(!contains(src, "fprintf(stderr"));
        CHECK(!contains(src, "std::cerr"));
    }

    // The logger is really in the build graph — a source file that compiles
    // nowhere would make every check above vacuously true.
    const std::string cmake = readFileBytes("CMakeLists.txt");
    CHECK(contains(cmake, "src/log.cpp"));

    // REQ-LOG-22/-30: the retired variable survives nowhere in the code. The
    // needle is ASSEMBLED rather than written out, because this file is on the
    // list it checks — spelling it whole here would fail the guard by being it.
    const std::string retired = std::string("TEXTWORLD_") + "PROFILE";
    for (const char* file : {"src/profile.cpp", "src/profile.hpp",
                             "src/log.cpp", "src/log.hpp", "src/main.cpp",
                             "src/loop.cpp", "src/pregen.cpp",
                             "src/bardworker.cpp", "tests/tests.cpp",
                             "README.md"}) {
        CHECK(!contains(readFileBytes(file), retired));
    }
}

// --- REQ-LOG-12/-20/-26: the migration inventory, driven ---------------------
// One row of the REQ-LOG-20 table at a time: drive the condition with a fake
// transport and assert an entry appears at the STATED level with the STATED
// source. A row that silently stopped emitting, or that drifted to a different
// level, is exactly what this catches and what a grep cannot.
//
// The thread labels of REQ-LOG-12 ride along here rather than standing alone,
// because the only honest way to prove a worker's entries say `pregen` or
// `bard` is to drive a real worker thread through a failing transport — which
// is also what two of the ERROR rows need.
static void testLogMigrationCoverage() {
    const ScopedEnvVar levelGuard("TEXTWORLD_LOG_LEVEL");
    const ScopedEnvVar keyGuard("ANTHROPIC_API_KEY");
    const ScopedEnvVar aiGuard("TEXTWORLD_AI");
    setenv("TEXTWORLD_LOG_LEVEL", "debug", 1);
    logRefreshLevel();
    setenv("ANTHROPIC_API_KEY", "test-key-never-used", 1);
    unsetenv("TEXTWORLD_AI");

    std::vector<std::string> entries;
    std::mutex entriesMutex;
    logSetSink([&](const std::string& line) {
        const std::lock_guard<std::mutex> lock(entriesMutex);
        entries.push_back(line);
    });

    // True iff some captured entry is at `level` with source `source`, and
    // (when given) its message contains `needle`.
    auto sawEntry = [&](const char* level, const char* source,
                        const char* needle = nullptr) {
        const std::lock_guard<std::mutex> lock(entriesMutex);
        for (const std::string& line : entries) {
            const ParsedLogLine p = parseLogLine(line);
            if (!p.ok || p.level != level || p.source != source) continue;
            if (needle == nullptr ||
                p.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    auto threadOf = [&](const char* source) {
        const std::lock_guard<std::mutex> lock(entriesMutex);
        for (const std::string& line : entries) {
            const ParsedLogLine p = parseLogLine(line);
            if (p.ok && p.source == source) return p.thread;
        }
        return std::string("<none>");
    };
    auto clear = [&] {
        const std::lock_guard<std::mutex> lock(entriesMutex);
        entries.clear();
    };

    const TempDbFile worldPath("textworld_log_coverage_tests.db");
    Db db = openWorld(worldPath.string(), "tests/fixture.sql").db;

    const HttpResponse notOk{false, 500, "{}"};
    const HttpTransport failing = [&](const std::string&) { return notOk; };
    const HttpTransport throwing = [](const std::string&) -> HttpResponse {
        throw std::runtime_error("transport exploded");
    };

    // --- DEBUG rows: the validation rejections ------------------------------
    clear();
    CHECK(!aiRender(db, 1, failing).has_value());
    CHECK(sawEntry("DEBUG", "prose", "rejected"));  // prose.cpp failClause

    clear();
    CHECK(!aiResolve(db, "take the lantern", failing).has_value());
    CHECK(sawEntry("DEBUG", "nlresolve", "rejected"));

    clear();
    CHECK(!architectProposeRoom(R"({"setting":""})", {}, {}, "east", failing)
               .has_value());
    CHECK(sawEntry("DEBUG", "architect", "rejected"));

    clear();
    CHECK(!validateWakeResponse(HttpResponse{false, 500, "{}"}, {"curiosity"})
               .has_value());
    CHECK(sawEntry("DEBUG", "bard", "rejected"));

    // --- WARN rows: the silent downgrades -----------------------------------
    // A THROWING transport, not a rejecting one: the fallback warning lives in
    // the catch, one layer above the clause rejection asserted just now.
    clear();
    CHECK(!aiRender(db, 1, throwing).has_value());
    CHECK(sawEntry("WARN", "prose", "falling back to templates"));

    clear();
    CHECK(!aiResolve(db, "take the lantern", throwing).has_value());
    CHECK(sawEntry("WARN", "nlresolve", "falling back to parser"));

    // --- ERROR rows, and the REQ-LOG-12 thread labels with them -------------
    clear();
    CHECK(!architectGenerate(db, 1, "east", /*actor=*/3, throwing));
    CHECK(sawEntry("ERROR", "architect", "phase 1 failed"));
    CHECK(threadOf("architect") == "main");  // the default label

    // The pregen worker: a real thread, a throwing transport, and the entry it
    // leaves behind must say thread `pregen` (REQ-LOG-12), not `main`.
    clear();
    unsetenv("TEXTWORLD_PREGEN");
    pregenRefreshEnabledForTest();
    pregenResetForTest();
    pregenSetWorkerTransportForTest(throwing);
    pregenStart();
    CHECK(pregenWorkerRunning());
    {
        PregenJob job;
        job.room = 1;
        job.direction = "east";
        job.contextPayload = R"({"setting":"","origin_name":"stone hall"})";
        job.snapshotTurn = 1;
        pregenSubmit(std::move(job));
    }
    for (int i = 0; i < 400 && !sawEntry("ERROR", "pregen"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pregenResetForTest();
    CHECK(sawEntry("ERROR", "pregen", "job failed"));
    CHECK(threadOf("pregen") == "pregen");

    // The bard worker, the same way.
    clear();
    unsetenv("TEXTWORLD_BARD");
    bardRefreshEnabledForTest();
    bardResetForTest();
    bardSetWorkerTransportForTest(throwing);
    bardStart();
    CHECK(bardWorkerRunning());
    {
        BardJob job;
        job.requestBody = "{}";
        job.motives = {"curiosity"};
        job.snapshotTurn = 1;
        CHECK(bardSubmit(std::move(job)));
    }
    bardWaitForIdleForTest();
    bardResetForTest();
    CHECK(sawEntry("ERROR", "bard", "wake failed"));
    CHECK(threadOf("bard") == "bard");

    // --- privacy (REQ-LOG-26, validation item 18) ---------------------------
    // A transport that echoes a sentinel in the body it RECEIVES and in the
    // body it RETURNS. Neither the sentinel nor the key may appear in the log —
    // at DEBUG, which is the widest setting there is.
    {
        clear();
        const std::string sentinel = "SENTINEL-a7f3c1e9-DO-NOT-LOG";
        const std::string key = "sk-ant-test-KEYSENTINEL-9f2b";
        setenv("ANTHROPIC_API_KEY", key.c_str(), 1);
        std::string sawRequest;
        const HttpTransport echoing = [&](const std::string& body) {
            sawRequest = body;
            return HttpResponse{false, 200,
                                std::string("{\"echo\":\"") + sentinel + "\"}"};
        };
        CHECK(!aiRender(db, 1, echoing).has_value());  // 200 but not a tool call
        CHECK(!sawRequest.empty());  // the fake really ran

        const std::lock_guard<std::mutex> lock(entriesMutex);
        CHECK(!entries.empty());  // and really logged something
        bool leaked = false;
        for (const std::string& line : entries) {
            if (line.find(sentinel) != std::string::npos) leaked = true;
            if (line.find(key) != std::string::npos) leaked = true;
        }
        CHECK(!leaked);
    }

    logSetSink({});
    unsetenv("TEXTWORLD_LOG_LEVEL");
    logRefreshLevel();
}

// Step 7, REQ-BARD-WAKE-8/-12: the two claims that are about ABSENCE, which no
// behavioral test can make.
static void testBardTriggerContract() {
    // REQ-BARD-WAKE-8: main() flushes the player's text BEFORE calling the
    // bard. tickT cannot exercise main()'s own ordering, so it is pinned as
    // source text — the precedent brick 1 set for append-only.
    const std::string main_ = readFileBytes("src/main.cpp");
    const size_t flush = main_.find("std::fflush(stdout);\n\n            // Queue point two");
    const size_t hook = main_.find("bardAfterTurn(db);");
    CHECK(flush != std::string::npos);
    CHECK(hook != std::string::npos);
    CHECK(flush < hook);

    // …and after architectQueuePregen, so the two post-turn schedulers keep a
    // stated order rather than an accidental one.
    const size_t pregen = main_.rfind("architectQueuePregen(db, playerRoom(db));");
    CHECK(pregen != std::string::npos);
    CHECK(pregen < hook);

    // REQ-BARD-WAKE-12: the trigger is DERIVED from the events log, every time.
    // Asserted by absence — there is no new table, no cache, and no state
    // member holding "triggers seen since the last wake".
    const std::string world = readFileBytes("src/world.cpp");
    CHECK(!contains(world, "bard_trigger"));
    CHECK(!contains(world, "pending_wake"));
    const std::string bard = readFileBytes("src/bard.cpp");
    CHECK(contains(bard, "SELECT 1 FROM events"));  // the query is really there
    // The four verbs, and only those four.
    for (const char* verb : {"generated", "defeated", "learned", "materialized"}) {
        CHECK(contains(bard, verb));
    }
    CHECK(!contains(bard, "'downed'"));  // the closest call, deliberately out
}

// Step 6, REQ-BARD-WAKE-3/-5/-18: the two things that are ordering or wording
// rather than behavior, and would otherwise be pinned by review alone.
static void testBardOvertureContract() {
    // Mechanical check 5: the 60 s exception carries its justification, in the
    // header, on the constant. A future reader normalizing it back to the
    // uniform budget has to delete a comment that says not to.
    CHECK(kBardOvertureTimeoutSeconds == 60);
    const std::string header = readFileBytes("src/bard.hpp");
    const size_t at = header.find("kBardOvertureTimeoutSeconds");
    CHECK(at != std::string::npos);
    CHECK(header.find("DELIBERATE EXCEPTION") < at);  // the comment precedes it
    CHECK(contains(header, "Do NOT normalize this back to kAiHttpTimeoutSeconds"));

    // REQ-BARD-WAKE-3 and -18: main()'s declaration order. Getting this wrong
    // is UNDEFINED BEHAVIOR (a live curl handle outliving curl_global_cleanup),
    // and it is invisible at runtime until it is not — so it is pinned as a
    // regression guard rather than left to review. Strictly increasing offsets:
    //
    //   logInit < AiHttpGuard < openWorld < bardOverture < PregenGuard < BardGuard
    //
    // The overture must precede both workers because it runs before any worker
    // thread exists; both guards must follow AiHttpGuard so reverse destruction
    // joins them before curl_global_cleanup. logInit heads the chain for a
    // different reason (REQ-LOG-29): the terminal duplicate must be taken
    // before anything can redirect the error channel, and the backstop must be
    // up before any code that might print — libcurl's own init included.
    const std::string main_ = readFileBytes("src/main.cpp");
    CHECK(!main_.empty());
    const size_t logInit_ = main_.find("logInit(\"logs\")");
    const size_t http = main_.find("const AiHttpGuard httpGuard;");
    CHECK(logInit_ != std::string::npos);
    CHECK(logInit_ < http);
    // The open call is matched WITHOUT its closing paren: this is a source
    // ORDER test, and pinning the full argument list made it fail the moment
    // openWorld gained its fourth parameter (REQ-NPCSTORE-34's major-profile
    // vector). The order is the guarantee; the arguments are not.
    const size_t open = main_.find("openWorld(\"world.db\"");
    const size_t overture = main_.find("bardOverture(db, nullptr)");
    const size_t pregen = main_.find("const PregenGuard pregenGuard;");
    const size_t bard = main_.find("const BardGuard bardGuard;");
    CHECK(http != std::string::npos);
    CHECK(open != std::string::npos);
    CHECK(overture != std::string::npos);
    CHECK(pregen != std::string::npos);
    CHECK(bard != std::string::npos);
    CHECK(http < open);
    CHECK(open < overture);
    CHECK(overture < pregen);
    CHECK(pregen < bard);

    // The overture is guarded by `created` — once per world file, not once per
    // launch — and that condition lives in main(), the only place that knows it.
    CHECK(contains(main_, "if (world.created) bardOverture(db, nullptr);"));
}

// --- The story arc store (specs/story-arc-store.md). Brick 1: schema, seed
// content, mutation helpers, one evaluator, one call site. No AI call, no
// network, no new translation unit — every test below runs offline. ---

// How many story-step advances the world has recorded. One reader for the whole
// story-arc block: three of these tests count 'advanced' rows, and a single
// definition is what stops them drifting if the verb is ever renamed.
static int64_t advancedCount(Db& db) {
    return queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'advanced'");
}

// Step 2: the two new tables and the SCHEMA_VERSION gate (REQ-ARC-STORE-1, -3,
// -4). Modeled on testBardStoreSchema.
static void testStoryStoreSchema() {
    const TempDbFile worldPath("textworld_story_schema_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    // REQ-ARC-STORE-3: story_step, exactly five columns, exactly these names.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type='table' AND name='story_step'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('story_step')") == 5);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('story_step') "
                       "WHERE name IN ('n','condition_kind','condition_arg',"
                       "'prose','reached_turn')") == 5);

    // REQ-ARC-STORE-4: condition_catalog, exactly three columns.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type='table' AND name='condition_catalog'") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('condition_catalog')") == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM pragma_table_info('condition_catalog') "
                       "WHERE name IN ('kind','blurb','arg_kind')") == 3);

    // REQ-ARC-STORE-2 is rows, not a shape: no arc TABLE ships through the bump.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type='table' AND name='story_arc'") == 0);

    // REQ-ARC-STORE-20: the verb vocabulary comment names 'advanced', so the
    // DDL's one machine-checked piece of documentation stays honest.
    CHECK(contains(readFileBytes("src/world.cpp"), "'advanced'"));
}

// REQ-ARC-STORE-1 (validation item 1): a world file written at the PREVIOUS
// version is refused, and the refusal writes nothing. The literal 7 is the real
// predecessor here — this brick is the 7 -> 8 bump — and testBardStoreVersionGate
// keeps the "some other version" case with its own literal.
static void testStoryStoreVersionGate() {
    const TempDbFile worldPath("textworld_story_version_tests.db");
    {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key='schema_version'") ==
              SCHEMA_VERSION);
    }
    {
        Db db(worldPath.string());
        db.exec("UPDATE meta SET value = 7 WHERE key = 'schema_version'");
    }
    const std::string bytesBefore = readFileBytes(worldPath);
    CHECK(!bytesBefore.empty());

    bool refused = false;
    try {
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    } catch (const SchemaMismatch&) {
        refused = true;
    }
    CHECK(refused);
    CHECK(readFileBytes(worldPath) == bytesBefore);  // nothing written on refusal
}

// Step 3: the closed condition vocabulary (REQ-ARC-STORE-5), asserted against
// BOTH seed files. The fixture half is what makes every later admission test
// possible — writeStoryStep validates `kind` against this table, and the bard
// store's testBardStoreShippedSeedMotives exists for the same divergence.
static void testStoryStoreConditionCatalog() {
    const auto checkVocabulary = [](Db& db) {
        CHECK(queryInt(db, "SELECT COUNT(*) FROM condition_catalog") == 4);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM condition_catalog WHERE kind IN "
                           "('enemies_defeated','rooms_built','spell_learned',"
                           "'reached_depth')") == 4);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM condition_catalog "
                           "WHERE blurb IS NULL OR blurb = ''") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM condition_catalog "
                           "WHERE arg_kind NOT IN ('int','spell')") == 0);
        // The two int/spell assignments the evaluator and the admission gate
        // both read: a swap here would be invisible to the count above.
        CHECK(queryText(db, "SELECT arg_kind FROM condition_catalog "
                            "WHERE kind = 'spell_learned'") == "spell");
        CHECK(queryInt(db, "SELECT COUNT(*) FROM condition_catalog "
                           "WHERE arg_kind = 'int'") == 3);
    };

    {
        const TempDbFile worldPath("textworld_story_vocab_seed_tests.db");
        Db db = openWorld(worldPath.string(), "seed/base.sql").db;
        checkVocabulary(db);
    }
    {
        const TempDbFile worldPath("textworld_story_vocab_fixture_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        checkVocabulary(db);
        // The fixture is the EMPTY-step-list world every other test runs in
        // (REQ-ARC-STORE-19a gets exercised for free by the whole suite).
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step") == 0);
    }
}

// Step 4: the three arc meta rows and writeArc (REQ-ARC-STORE-2, -6, -9) —
// validation item 16, plus the arc half of item 2.
static void testStoryStoreArc() {
    // --- the HELPER, against a fixture world whose seed never wrote the rows,
    // which is also what proves the upsert (rather than UPDATE) matters. ---
    {
        const TempDbFile worldPath("textworld_story_arc_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

        const auto arcCount = [&db] {
            return queryInt(db, "SELECT COUNT(*) FROM meta WHERE key LIKE 'arc\\_%' "
                                "ESCAPE '\\'");
        };
        const auto eventCount = [&db] {
            return queryInt(db, "SELECT COUNT(*) FROM events");
        };
        const auto arcValue = [&db](const char* key) {
            Stmt s = db.prepare("SELECT value FROM meta WHERE key = ?");
            s.bind(1, std::string(key));
            CHECK(s.step());
            return s.colText(0);
        };

        CHECK(arcCount() == 0);  // the fixture seeds no arc
        const int64_t eventsBefore = eventCount();

        writeArc(db, "a premise", "a goal", "an ending");
        CHECK(arcCount() == 3);
        CHECK(arcValue("arc_premise") == "a premise");
        CHECK(arcValue("arc_goal") == "a goal");
        CHECK(arcValue("arc_ending") == "an ending");
        CHECK(eventCount() == eventsBefore);  // an arc has not HAPPENED

        // A second call REPLACES; it never appends a fourth row or an event.
        writeArc(db, "another premise", "another goal", "another ending");
        CHECK(arcCount() == 3);
        CHECK(arcValue("arc_premise") == "another premise");
        CHECK(arcValue("arc_goal") == "another goal");
        CHECK(arcValue("arc_ending") == "another ending");
        CHECK(eventCount() == eventsBefore);
    }

    // --- the SHIPPED seed (REQ-ARC-STORE-6): all three rows present, none
    // empty. Distinct from the helper test above, which never opens base.sql. ---
    {
        const TempDbFile worldPath("textworld_story_arc_seed_tests.db");
        Db db = openWorld(worldPath.string(), "seed/base.sql").db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key IN "
                           "('arc_premise','arc_goal','arc_ending')") == 3);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM meta WHERE key IN "
                           "('arc_premise','arc_goal','arc_ending') "
                           "AND (value IS NULL OR value = '')") == 0);
    }
}

// Step 5: writeStoryStep's admission gate (REQ-ARC-STORE-10) — validation
// item 3. A step may not promise a condition the engine cannot check.
static void testStoryStoreWrite() {
    const TempDbFile worldPath("textworld_story_write_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;

    const auto stepCount = [&db] {
        return queryInt(db, "SELECT COUNT(*) FROM story_step");
    };
    const auto eventCount = [&db] {
        return queryInt(db, "SELECT COUNT(*) FROM events");
    };

    CHECK(stepCount() == 0);
    const int64_t eventsBefore = eventCount();

    // Every refusal, and after each one the table is UNCHANGED — the checks
    // all run before any write.
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "phase_of_moon", "3", "the moon turns");
    }));
    CHECK(stepCount() == 0);
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "enemies_defeated", "x", "they fall");
    }));
    CHECK(stepCount() == 0);
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "enemies_defeated", "-1", "they fall");
    }));
    CHECK(stepCount() == 0);
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "enemies_defeated", "2x", "they fall");
    }));
    CHECK(stepCount() == 0);
    // Whole-string, so a leading number followed by prose is refused rather
    // than silently read as 2.
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "enemies_defeated", "2 or 3", "they fall");
    }));
    CHECK(stepCount() == 0);
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "spell_learned", "levitate", "you learn it");
    }));
    CHECK(stepCount() == 0);
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "enemies_defeated", "1", "   ");
    }));
    CHECK(stepCount() == 0);

    // A valid call: exactly one row, reached_turn NULL, and NO event.
    writeStoryStep(db, 1, "enemies_defeated", "1", "  word runs ahead of you  ");
    CHECK(stepCount() == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                       "WHERE reached_turn IS NULL") == 1);
    CHECK(queryText(db, "SELECT condition_kind FROM story_step WHERE n = 1") ==
          "enemies_defeated");
    CHECK(queryText(db, "SELECT condition_arg FROM story_step WHERE n = 1") == "1");
    // The TRIMMED prose is what is stored.
    CHECK(queryText(db, "SELECT prose FROM story_step WHERE n = 1") ==
          "word runs ahead of you");
    CHECK(eventCount() == eventsBefore);  // a step not yet reached has not HAPPENED

    // A duplicate n is refused, and the existing row is untouched.
    CHECK(threwRuntimeError([&db] {
        writeStoryStep(db, 1, "rooms_built", "4", "the lamps go out");
    }));
    CHECK(stepCount() == 1);
    CHECK(queryText(db, "SELECT condition_kind FROM story_step WHERE n = 1") ==
          "enemies_defeated");

    // The spell arm admits a spell that IS in the catalog.
    writeStoryStep(db, 2, "spell_learned", "fire", "the stacks smell of smoke");
    CHECK(stepCount() == 2);
    CHECK(eventCount() == eventsBefore);
}

// Step 6: the five seeded steps (REQ-ARC-STORE-7, -8) — the step half of
// validation item 2, plus the fixture divergence guard.
static void testStoryStoreSeededSteps() {
    {
        const TempDbFile worldPath("textworld_story_steps_tests.db");
        Db db = openWorld(worldPath.string(), "seed/base.sql").db;

        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step") == 5);
        // n is exactly 1..5, not merely five of something.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE n IN (1,2,3,4,5)") == 5);
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT condition_kind) "
                           "FROM story_step") >= 3);
        // Every seeded kind is one the engine can actually check: the seed
        // obeys the same closed vocabulary writeStoryStep enforces.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step s "
                           "LEFT JOIN condition_catalog c ON c.kind = s.condition_kind "
                           "WHERE c.kind IS NULL") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE prose IS NULL OR TRIM(prose) = ''") == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE condition_arg IS NULL OR condition_arg = ''") == 0);
        // REQ-ARC-STORE-8: a fresh world is at step ZERO.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);
        // The two offline-reachable conditions the golden session depends on.
        CHECK(queryText(db, "SELECT condition_kind FROM story_step WHERE n = 1") ==
              "enemies_defeated");
        CHECK(queryText(db, "SELECT condition_kind FROM story_step WHERE n = 2") ==
              "spell_learned");
    }
    {
        // The fixture stays the EMPTY-list world every other test runs in.
        const TempDbFile worldPath("textworld_story_steps_fixture_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step") == 0);
    }
}

// Step 7: stepConditionMet (REQ-ARC-STORE-12, -13, -14) — validation items 4,
// 5, and the first half of 6. The combat fixture has rooms at depths 0 (cell 1),
// 1 (corridor 2, library 11) and 2 (frost study 6, armory 9).
static void testStoryConditions() {
    const TempDbFile worldPath("textworld_story_conditions_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const int64_t player = 3;

    // --- item 4: every kind false, then made true by the SANCTIONED path ---
    db.begin();

    CHECK(!stepConditionMet(db, "enemies_defeated", "1", player));
    defeatEnemy(db, /*enemy=*/7, /*droppedItem=*/0, player);
    CHECK(stepConditionMet(db, "enemies_defeated", "1", player));
    CHECK(!stepConditionMet(db, "enemies_defeated", "2", player));

    CHECK(!stepConditionMet(db, "rooms_built", "1", player));
    {
        // Grown off the outer hall (15), the fixture's far edge, into a
        // direction it has no exit for. Generating off the cell would REPLACE
        // its north exit to the corridor and detach the rest of the graph,
        // which would make every depth assertion below meaningless.
        const RoomProposal proposal{"chapter house",
                                    "A low vaulted room, its shelves bare.",
                                    {"east"}};
        writeGeneratedRoom(db, /*originRoom=*/15, "north", proposal, player);
    }
    CHECK(stepConditionMet(db, "rooms_built", "1", player));
    CHECK(!stepConditionMet(db, "rooms_built", "2", player));

    CHECK(!stepConditionMet(db, "spell_learned", "fire", player));
    learnSpell(db, player, "fire");
    CHECK(stepConditionMet(db, "spell_learned", "fire", player));
    CHECK(!stepConditionMet(db, "spell_learned", "frost", player));

    // The player starts in the cell, which IS the seed room: depth 0.
    CHECK(!stepConditionMet(db, "reached_depth", "2", player));
    moveEntity(db, player, /*toContainer=*/6, player, "moved");  // frost study
    CHECK(stepConditionMet(db, "reached_depth", "2", player));
    CHECK(!stepConditionMet(db, "reached_depth", "3", player));
    // Zero always holds, wherever the player stands.
    CHECK(stepConditionMet(db, "reached_depth", "0", player));

    db.commit();

    // --- item 5 (REQ-ARC-STORE-13): a bogus kind THROWS. Written in by raw
    // SQL, because no sanctioned path can produce one — writeStoryStep refuses
    // it at admission. A silent `false` here would hide an engine bug. ---
    db.exec("INSERT INTO story_step(n, condition_kind, condition_arg, prose) "
            "VALUES (99, 'phase_of_moon', '3', 'the moon turns')");
    CHECK(threwRuntimeError([&db, player] {
        stepConditionMet(db, queryText(db, "SELECT condition_kind FROM story_step "
                                           "WHERE n = 99"),
                         "3", player);
    }));

    // --- item 6, first half (REQ-ARC-STORE-14): no clock, as a FUNCTION-scoped
    // source check. A file-wide grep would fail on correct code, because
    // advanceStoryStep legitimately reads meta.turn to stamp reached_turn. ---
    {
        const std::string src = readFileBytes("src/systems.cpp");
        const size_t begin = src.find("bool stepConditionMet(");
        CHECK(begin != std::string::npos);
        // The function's body ends at the first line that is exactly "}".
        const size_t end = src.find("\n}\n", begin);
        CHECK(end != std::string::npos);
        const std::string body = src.substr(begin, end - begin);
        CHECK(!body.empty());          // the extracted range is not vacuous
        CHECK(body.size() > 200);      // …and is the whole function, not a stub
        CHECK(!contains(body, "meta.turn"));
        CHECK(!contains(body, "key='turn'"));
        CHECK(!contains(body, "key = 'turn'"));
    }
}

// Step 8: advanceStoryStep (REQ-ARC-STORE-11, -11a, -19, -20) — validation
// items 7, 8 and 13.
static void testStoryAdvance() {
    const TempDbFile worldPath("textworld_story_advance_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const int64_t player = 3;

    const auto eventCount = [&db] {
        return queryInt(db, "SELECT COUNT(*) FROM events");
    };

    // --- the EMPTY table: no row to latch, so nothing happens and nothing
    // throws (REQ-ARC-STORE-19a, reached through the helper). ---
    db.begin();
    CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step") == 0);
    {
        const int64_t before = eventCount();
        CHECK(!advanceStoryStep(db, player));
        CHECK(eventCount() == before);
    }

    writeStoryStep(db, 1, "enemies_defeated", "1",
                   "Word of the fight runs ahead of you.");
    db.commit();

    // Move the world off turn 0 so the stamped turn is distinguishable from
    // "never reached" by value as well as by NULL-ness.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    const int64_t turnNow = queryInt(db, "SELECT value FROM meta WHERE key='turn'");
    CHECK(turnNow > 0);

    // --- item 7: the latch fires ONCE and is one-way. ---
    db.begin();
    const int64_t eventsBefore = eventCount();
    CHECK(advanceStoryStep(db, player));
    CHECK(advancedCount(db) == 1);
    CHECK(queryInt(db, "SELECT reached_turn FROM story_step WHERE n = 1") == turnNow);

    // --- item 13: the event's columns. `detail` is byte-identical to the
    // prose read back out of story_step, not to the string passed in. ---
    CHECK(queryInt(db, "SELECT actor FROM events WHERE verb = 'advanced'") == player);
    CHECK(queryInt(db, "SELECT subject FROM events WHERE verb = 'advanced'") == 0);
    CHECK(queryInt(db, "SELECT object FROM events WHERE verb = 'advanced'") == 1);
    CHECK(queryText(db, "SELECT detail FROM events WHERE verb = 'advanced'") ==
          queryText(db, "SELECT prose FROM story_step WHERE n = 1"));

    // A second call latches nothing, appends nothing, and leaves the FIRST
    // call's turn in place.
    const int64_t afterFirst = eventCount();
    CHECK(!advanceStoryStep(db, player));
    CHECK(advancedCount(db) == 1);
    CHECK(eventCount() == afterFirst);
    CHECK(queryInt(db, "SELECT reached_turn FROM story_step WHERE n = 1") == turnNow);
    CHECK(afterFirst == eventsBefore + 1);  // exactly one event for one advance

    // --- item 8 (REQ-ARC-STORE-19): exhaustion is terminal and silent. Ten
    // further calls each return false, append nothing, and throw nothing. ---
    for (int i = 0; i < 10; ++i) {
        CHECK(!advanceStoryStep(db, player));
    }
    CHECK(advancedCount(db) == 1);
    CHECK(eventCount() == afterFirst);
    db.commit();

    // --- REQ-ARC-STORE-11a, the case a prior-read implementation gets wrong:
    // two calls inside ONE transaction latch and describe DIFFERENT steps. ---
    db.begin();
    writeStoryStep(db, 2, "rooms_built", "4", "The Vigil Lamps no longer kindle.");
    writeStoryStep(db, 3, "enemies_defeated", "5", "The breach stands open.");
    CHECK(advanceStoryStep(db, player));
    CHECK(advanceStoryStep(db, player));
    CHECK(advancedCount(db) == 3);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events "
                       "WHERE verb = 'advanced' AND object = 2") == 1);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events "
                       "WHERE verb = 'advanced' AND object = 3") == 1);
    // Each event carries ITS OWN step's prose, not the other's.
    CHECK(queryText(db, "SELECT detail FROM events "
                        "WHERE verb = 'advanced' AND object = 2") ==
          queryText(db, "SELECT prose FROM story_step WHERE n = 2"));
    CHECK(queryText(db, "SELECT detail FROM events "
                        "WHERE verb = 'advanced' AND object = 3") ==
          queryText(db, "SELECT prose FROM story_step WHERE n = 3"));
    db.commit();
}

// Step 9: evaluateStoryAdvance (REQ-ARC-STORE-15a, -16, -16a, -17, -19, -19a).
// Driven DIRECTLY inside a manual transaction — there is no call site yet, so
// runTurn cannot reach it. Validation items 10, 17 and 8, in their direct form.
static void testStoryEvaluate() {
    const int64_t player = 3;

    // --- item 17 (REQ-ARC-STORE-19a): an EMPTY step list is a silent no-op. ---
    {
        const TempDbFile worldPath("textworld_story_eval_empty_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        const int64_t before = queryInt(db, "SELECT COUNT(*) FROM events");
        for (int i = 0; i < 10; ++i) evaluateStoryAdvance(db, player);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == before);
        CHECK(advancedCount(db) == 0);
        db.commit();
    }

    // --- item 10 (REQ-ARC-STORE-16, -16a): it does NOT scan ahead. Step 1's
    // condition is false and step 3's is true; an evaluator that looked past
    // step 1 would fire step 3. ---
    {
        const TempDbFile worldPath("textworld_story_eval_scan_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "enemies_defeated", "1", "they know we are awake");
        writeStoryStep(db, 2, "rooms_built", "4", "the lamps go out");
        writeStoryStep(db, 3, "reached_depth", "0", "the index is read");  // TRUE
        // Step 3's condition holds right now; step 1's does not.
        CHECK(stepConditionMet(db, "reached_depth", "0", player));
        CHECK(!stepConditionMet(db, "enemies_defeated", "1", player));
        for (int i = 0; i < 10; ++i) evaluateStoryAdvance(db, player);
        CHECK(advancedCount(db) == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);
        db.commit();
    }

    // --- the happy path, and item 8 (REQ-ARC-STORE-19) reached through THIS
    // function rather than through advanceStoryStep. ---
    {
        const TempDbFile worldPath("textworld_story_eval_advance_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "reached_depth", "0", "they know we are awake");
        writeStoryStep(db, 2, "enemies_defeated", "9", "the breach stands open");
        evaluateStoryAdvance(db, player);
        CHECK(advancedCount(db) == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events "
                           "WHERE verb = 'advanced' AND object = 1") == 1);
        // Step 2's condition is false, so further calls do nothing…
        for (int i = 0; i < 10; ++i) evaluateStoryAdvance(db, player);
        CHECK(advancedCount(db) == 1);
        // …and once every step IS reached, the rule is a no-op that neither
        // writes nor throws.
        db.exec("UPDATE story_step SET reached_turn = 0 WHERE reached_turn IS NULL");
        const int64_t before = queryInt(db, "SELECT COUNT(*) FROM events");
        for (int i = 0; i < 10; ++i) evaluateStoryAdvance(db, player);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == before);
        db.commit();
    }
}

// Step 10: 'advanced' is renderer-invisible (REQ-ARC-STORE-21) — the mechanism
// behind validation item 14, modeled on the REQ-ARCH-10 test. It is excluded at
// BOTH buildFacts sites, and render() gains no branch for it.
static void testStoryRendererInvisible() {
    const TempDbFile worldPath("textworld_story_invisible_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const int64_t player = 3;

    // A turn carrying BOTH an 'advanced' event and an ordinary one, sharing the
    // same turn number — the load-bearing case. The step is written AFTER the
    // wait turn and latched by hand, so the two events land on the same turn
    // without depending on when the tick's own rule would have fired.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    const int64_t turn = queryInt(db, "SELECT value FROM meta WHERE key='turn'");
    db.begin();
    writeStoryStep(db, 1, "reached_depth", "0", "Word runs ahead of you.");
    CHECK(advanceStoryStep(db, player));
    db.commit();
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(turn) + " AND verb = 'advanced'").c_str()) == 1);
    CHECK(queryInt(db, ("SELECT COUNT(*) FROM events WHERE turn = " +
                        std::to_string(turn) + " AND verb = 'waited'").c_str()) == 1);

    // The template render of that turn is byte-identical to its render with the
    // 'advanced' row deleted: the verb produces no output of its own.
    const std::string withAdvance = render(db, turn);

    // Current-turn payload key `events`: 'advanced' absent, 'waited' present.
    {
        const TurnFacts facts = buildFacts(db, turn);
        const nlohmann::json j = nlohmann::json::parse(facts.payload);
        bool sawAdvanced = false, sawWaited = false;
        for (const auto& e : j["events"]) {
            if (e.value("verb", "") == "advanced") sawAdvanced = true;
            if (e.value("verb", "") == "waited") sawWaited = true;
        }
        CHECK(!sawAdvanced);
        CHECK(sawWaited);
    }

    // recent-events payload key (turn < ?): 'advanced' absent there too. Doing
    // only the current-turn site would leak the prose into the narrator's
    // context for the next six turns.
    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    {
        const TurnFacts facts = buildFacts(db, turn + 1);
        const nlohmann::json j = nlohmann::json::parse(facts.payload);
        bool sawAdvanced = false, sawWaited = false;
        for (const auto& e : j["recent_events"]) {
            if (e.value("verb", "") == "advanced") sawAdvanced = true;
            if (e.value("verb", "") == "waited") sawWaited = true;
        }
        CHECK(!sawAdvanced);
        CHECK(sawWaited);  // the other verb of the same turn DID make it through
    }

    db.exec("DELETE FROM events WHERE verb = 'advanced'");
    CHECK(render(db, turn) == withAdvance);
    CHECK(!withAdvance.empty());

    // render.cpp gains no branch for the verb: unrecognized verbs already
    // render nothing, and how an advance is TOLD is a later brick's decision.
    CHECK(!contains(readFileBytes("src/render.cpp"), "advanced"));
}

// Step 10: the bard's fifth wake verb (REQ-ARC-STORE-22, -23) — validation
// item 15. The predicate is asserted as SOURCE TEXT, which is the precedent
// testNpcStoreInvariants already set and gives its reason for: hasTriggeringEvent
// is file-local, and reaching it for real needs the bard enabled plus a live
// transport. What IS checked behaviourally is the row that predicate selects.
static void testStoryWakeTrigger() {
    // The five-verb list, verbatim. This asserts the fifth verb AND that the
    // other four survived — narrowing to 'advanced' alone is a later brick.
    CHECK(contains(readFileBytes("src/bard.cpp"),
                   "verb IN ('generated','defeated','learned','materialized','advanced')"));

    const TempDbFile worldPath("textworld_story_wake_tests.db");
    Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
    const int64_t player = 3;

    // A world whose bard is idle: bard_last_wake_turn is 0 at init.
    CHECK(queryInt(db, "SELECT value FROM meta WHERE key='bard_last_wake_turn'") == 0);

    CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
    db.begin();
    writeStoryStep(db, 1, "reached_depth", "0", "Word runs ahead of you.");
    CHECK(advanceStoryStep(db, player));
    db.commit();

    // Exactly the row the predicate's query selects: verb 'advanced', at a turn
    // strictly greater than the last queued wake.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'advanced' "
                       "AND turn > (SELECT value FROM meta "
                       "            WHERE key = 'bard_last_wake_turn')") == 1);
}

// The pre-arc golden session (spec AI-Validation item 14, REQ-ARC-STORE-21).
// A fixed script over the SHIPPED seed, run through the TEMPLATE path with AI
// disabled, with every turn's output concatenated and compared to one literal.
// Captured BEFORE the story arc existed, so it is the byte-identity baseline
// for narrated output: this brick adds storage and a rule, and must not change
// one byte of what the player reads. If it stops matching once the rule is
// wired into the tick, something is PRINTING the advance — find that. Do NOT
// re-capture the literal; re-capturing hides the bug the test exists to catch.
//
// It opens seed/base.sql, not tests/combat_fixture.sql: the five story steps
// are seeded in the shipped seed, and a fixture world's story_step table is
// empty, which would make this gate vacuous.
//
// The script has to reach an advance with AI OFF, and that is a constraint
// rather than a preference: with aiNarrationEnabled() false a latent exit is a
// wall (systems.cpp), so no room is ever generated and neither rooms_built nor
// reached_depth can ever become true. Only enemies_defeated and spell_learned
// are reachable offline — which is what fixes the seeded conditions of steps 1
// and 2.
//
// Re-capture (only when a verb's template output changes ON PURPOSE):
//   TW_DUMP_GOLDEN=1 ./build/tests
// and paste the printed block back into kStoryGoldenSession.
static const char* const kStoryGoldenSession = R"GOLDEN(  dormitory cell
-- dormitory cell --------------------------------------------------------------
 Exits    north
 Objects  candle, wand
 You      HP: 12/12 [########]
  You take the wand.
-- dormitory cell --------------------------------------------------------------
 Exits    north
 Objects  candle
 You      HP: 12/12 [########]
  A long panelled corridor, doors shut on either side and the
  ceiling lost in the dark. Somewhere far off a stair creaks to
  itself. A lamp in a wall bracket kindles quietly as you
  approach, and the way south leads back to your cell.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key
 Enemy    goblin grunt  HP: 8/8 [########]
 You      HP: 12/12 [########]  Stun: ready  Ward: ready
  You strike the goblin grunt for 4 damage.
  The goblin grunt winds up a heavy blow — strike it down or
  brace!
  The goblin grunt wounds you for 1 damage.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key
 Enemy    goblin grunt  HP: 4/8 [####....]  [WINDING UP]
 You      HP: 11/12 [#######.]  Stun: ready  Ward: ready
  You strike the goblin grunt for 4 damage.
  The goblin grunt falls. It drops the fire grimoire.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key, fire grimoire
 You      HP: 11/12 [#######.]
  You study the fire grimoire and learn to cast fire.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key, fire grimoire
 You      HP: 11/12 [#######.]
  Time passes.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key, fire grimoire
 You      HP: 11/12 [#######.]
  corridor
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key, fire grimoire
 You      HP: 11/12 [#######.]
  You can't go that way.
-- corridor --------------------------------------------------------------------
 Exits    south
 Objects  key, fire grimoire
 You      HP: 11/12 [#######.]
  dormitory cell
-- dormitory cell --------------------------------------------------------------
 Exits    north
 Objects  candle
 You      HP: 11/12 [#######.]
)GOLDEN";

// The script itself, hoisted so testStoryEmptyStepList can run it VERBATIM
// against the same literal. That the two tests share one array is what makes
// "the same session" structural rather than a pair of lists kept in step by
// hand.
static const char* const kStoryGoldenScript[] = {
    "look",                // looked
    "take wand",           // took
    "go north",            // moved, into the corridor and the goblin
    "attack",              // attacked + the enemy's turn
    "attack",              // defeated -> drops the fire grimoire  [step 1]
    "read fire grimoire",  // learned fire                         [step 2]
    "wait",                // waited: a quiet turn right after an advance
    "look",                // looked
    "go up",               // failed: a latent exit is a wall with AI off
    "go south",            // moved, back through a realized exit
};

static void testStoryGoldenSession() {
    const TempDbFile worldPath("textworld_story_golden_tests.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;

    std::string actual;
    for (const char* line : kStoryGoldenScript) actual += runTurn(db, line).output;

    if (std::getenv("TW_DUMP_GOLDEN") != nullptr) {
        std::printf("--- story golden session ---\n%s--- end ---\n",
                    actual.c_str());
        return;
    }

    CHECK(actual == kStoryGoldenSession);

    // The gate is worthless unless the script really does defeat the goblin and
    // learn fire: those two events are what the seeded steps 1 and 2 hang on,
    // and without them no advance can ever fire on this transcript.
    CHECK(contains(actual, "The goblin grunt falls."));
    CHECK(contains(actual, "and learn to cast fire"));

    // …and, now that the rule is wired into the tick, the run genuinely
    // advances TWO steps while producing those identical bytes. Without this
    // the byte-identity above would be satisfied by a rule that never fired.
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'advanced'") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                       "WHERE reached_turn IS NOT NULL") == 2);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                       "WHERE n IN (1,2) AND reached_turn IS NOT NULL") == 2);
    // One per turn (REQ-ARC-STORE-17): the two advances landed on DIFFERENT
    // turns — the defeat turn and the read turn.
    CHECK(queryInt(db, "SELECT COUNT(DISTINCT turn) FROM events "
                       "WHERE verb = 'advanced'") == 2);
}

// Step 11: the rule as it behaves through WHOLE TURNS (REQ-ARC-STORE-15, -16,
// -17, -18) — validation items 9, 10, 11, 12, and the second half of 6.
static void testStoryAdvanceRule() {

    // --- item 9 (REQ-ARC-STORE-16, -17): steps 1, 2 and 3 all satisfied, and
    // still AT MOST ONE advances per turn, in order. ---
    {
        const TempDbFile worldPath("textworld_story_rule_one_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "reached_depth", "0", "they know we are awake");
        writeStoryStep(db, 2, "reached_depth", "0", "the stacks smell of smoke");
        writeStoryStep(db, 3, "reached_depth", "0", "the lamps go out");
        db.commit();

        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        CHECK(advancedCount(db) == 1);
        CHECK(queryInt(db, "SELECT object FROM events WHERE verb = 'advanced'") == 1);

        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        CHECK(advancedCount(db) == 2);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events "
                           "WHERE verb = 'advanced' AND object = 2") == 1);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events "
                           "WHERE verb = 'advanced' AND object = 3") == 0);
        // Every advance sits on its own turn.
        CHECK(queryInt(db, "SELECT COUNT(DISTINCT turn) FROM events "
                           "WHERE verb = 'advanced'") == 2);
    }

    // --- item 10 end to end (REQ-ARC-STORE-16a): step 1 false, step 3 true,
    // driven through ten REAL turns rather than by direct calls. An evaluator
    // that scanned ahead would fire step 3. ---
    {
        const TempDbFile worldPath("textworld_story_rule_scan_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "enemies_defeated", "1", "they know we are awake");
        writeStoryStep(db, 2, "rooms_built", "4", "the lamps go out");
        writeStoryStep(db, 3, "reached_depth", "0", "the index is read");
        db.commit();
        for (int i = 0; i < 10; ++i) {
            CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        }
        CHECK(advancedCount(db) == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);
    }

    // --- item 11 (REQ-ARC-STORE-18): a turn whose only event is waited, looked
    // or failed never advances a step. Asserted as BEHAVIOUR — it follows from
    // the four conditions, not from a verb filter. The world here is one event
    // short of step 1's condition. ---
    {
        const TempDbFile worldPath("textworld_story_rule_quiet_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "enemies_defeated", "1", "they know we are awake");
        db.commit();
        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        CHECK(advancedCount(db) == 0);
        CHECK(runTurn(db, "look").outcome == TurnOutcome::Ticked);
        CHECK(advancedCount(db) == 0);
        CHECK(runTurn(db, "take key").outcome == TurnOutcome::Ticked);  // fails: not here
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'failed'") >= 1);
        CHECK(advancedCount(db) == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);

        // --- item 6, second half (REQ-ARC-STORE-14): 50 turns of waiting leave
        // reached_turn NULL on every step. Waiting is not a way to advance,
        // because nothing here reads a clock. ---
        for (int i = 0; i < 50; ++i) {
            CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        }
        CHECK(advancedCount(db) == 0);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);
    }

    // --- item 12 (REQ-ARC-STORE-15): ATOMICITY. A throw on the advance path
    // rolls the whole tick back — the turn counter, the event, and the latch. ---
    {
        const TempDbFile worldPath("textworld_story_rule_atomic_tests.db");
        Db db = openWorld(worldPath.string(), "tests/combat_fixture.sql").db;
        db.begin();
        writeStoryStep(db, 1, "reached_depth", "0", "they know we are awake");
        db.commit();
        // Fault the advance itself: Stmt::step turns the SQLite error into a
        // std::runtime_error, which the tick's catch turns into a rollback.
        db.exec("CREATE TRIGGER story_boom AFTER INSERT ON events "
                "WHEN NEW.verb = 'advanced' "
                "BEGIN SELECT RAISE(ABORT, 'boom'); END");

        const int64_t turnBefore =
            queryInt(db, "SELECT value FROM meta WHERE key = 'turn'");
        const int64_t eventsBefore = queryInt(db, "SELECT COUNT(*) FROM events");

        const TurnResult r = runTurn(db, "wait");
        CHECK(r.outcome == TurnOutcome::EngineError);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == turnBefore);
        CHECK(queryInt(db, "SELECT COUNT(*) FROM events") == eventsBefore);
        CHECK(advancedCount(db) == 0);
        // The latch rolled back with everything else — this is the assertion a
        // read-then-write implementation could still fail.
        CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step "
                           "WHERE reached_turn IS NOT NULL") == 0);

        // The connection is still usable: with the trigger gone the same turn
        // ticks and advances, so the rollback left no transaction dangling.
        db.exec("DROP TRIGGER story_boom");
        CHECK(runTurn(db, "wait").outcome == TurnOutcome::Ticked);
        CHECK(advancedCount(db) == 1);
    }
}

// Step 11: validation item 17 (REQ-ARC-STORE-19a), the half testStoryEvaluate
// cannot reach — an EMPTY story_step table produces byte-identical narrated
// output. The literal it is compared against was captured on a tree where the
// table did not exist at all, so the same bytes are now pinned from BOTH
// directions: with the five seeded steps present (two advances) and with the
// table emptied (none).
static void testStoryEmptyStepList() {
    const TempDbFile worldPath("textworld_story_empty_tests.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql").db;
    db.exec("DELETE FROM story_step");
    CHECK(queryInt(db, "SELECT COUNT(*) FROM story_step") == 0);

    std::string actual;
    for (const char* line : kStoryGoldenScript) actual += runTurn(db, line).output;

    CHECK(actual == kStoryGoldenSession);
    CHECK(queryInt(db, "SELECT COUNT(*) FROM events WHERE verb = 'advanced'") == 0);
}

// Step 11: validation item 18 (REQ-ARC-STORE-15a) — the rule has exactly ONE
// call site, in loop.cpp, inside the tick's transaction.
static void testStoryOneCallSite() {
    // Not in any other production unit. systems.{hpp,cpp} is where it is
    // DEFINED, so those two are the exemption, not a second caller.
    for (const char* file : {"src/prose.cpp", "src/bard.cpp", "src/render.cpp",
                             "src/combat.cpp", "src/architect.cpp",
                             "src/npc.cpp", "src/pregen.cpp", "src/main.cpp",
                             "src/mutations.cpp"}) {
        CHECK(!contains(readFileBytes(file), "evaluateStoryAdvance"));
    }

    const std::string loop = readFileBytes("src/loop.cpp");
    const size_t call = loop.find("evaluateStoryAdvance(db, player);");
    CHECK(call != std::string::npos);
    // Exactly one call, not two.
    CHECK(loop.find("evaluateStoryAdvance", call + 1) == std::string::npos);
    // …and it sits between the tick's begin and its commit.
    const size_t begin = loop.find("db.begin();");
    const size_t commit = loop.find("db.commit();", begin);
    CHECK(begin != std::string::npos);
    CHECK(commit != std::string::npos);
    CHECK(begin < call);
    CHECK(call < commit);
    // After the enemy turn, so a step advance and the change that caused it
    // are one atomic fact.
    const size_t combat = loop.find("resolveCombat(db, player, startRoom);");
    CHECK(combat != std::string::npos);
    CHECK(combat < call);
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

    // Same discipline for TEXTWORLD_LOG_LEVEL (REQ-LAT-1, REQ-LOG-19): a
    // developer shell with it set to `debug` would otherwise spray profiling
    // lines through every runTurn test. The threshold is cached at static-init
    // time, so unsetting the var is not enough — the cache must be refreshed
    // too. Note this leaves the suite at the REQ-LOG-18 default of INFO, not
    // at "logging off": what keeps the suite quiet is that nothing here calls
    // logInit() or leaves a sink installed, so the logger is inert
    // (REQ-LOG-28).
    const ScopedEnvVar profileGuard("TEXTWORLD_LOG_LEVEL");
    unsetenv("TEXTWORLD_LOG_LEVEL");
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
    testParseSay();
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
    testExamineGoldenSession();
    testExamineWorldGuards();
    testProseFacts();
    testNlResolveContext();
    testNlResolvePrompt();
    testProseTransport();
    testLogFormatAndLevels();
    testLogFile();
    testLogSourceGuards();
    testLogMigrationCoverage();
    testProfileRecords();
    testProfileBackgroundAndDwell();
    testAiHttpWorkerClient();
    testProfileTurnStages();
    testAiRoleModel();
    testSayIsaShape();
    testAiHttpThreadingContract();
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
    testArchitectStoryContext();
    testArchitectStoryRequestBody();
    testArchitectPrompt();
    testArchitectStoryPrompt();
    testArchitectRequestBody();
    testArchitectGate();
    testArchitectStoryGate();
    testArchitectStoryGenerate();
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
    testArchitectStoryPlacement();
    testResolveGoGenerate();
    testPregenCommit();
    testArchitectStoryPregen();
    testArchitectStoryNoun();
    testPregenOutcomeRecords();
    testProfileGenerateStage();
    testCombatFlee();
    testGeneratedEventInvisible();
    testTermColorGate();
    testTermWidth();
    testTermProseWidth();
    testTermIndent();
    testTermBackgroundColor();
    testTermWrap();
    testBandLayout();
    testBandContent();
    testBandGoldens();
    testBandColor();
    testBandWiring();
    testSpinner();
    testHistoryFile();
    testTitleScreen();
    testWorldStartRoom();
    testRoomSeen();
    testFirstSight();
    testExamineRoom();
    testBandHealthBar();
    testBandBarsInRows();
    testErrorStyling();
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
    testNpcStoreSchema();
    testNpcStoreProfile();
    testNpcStoreMemory();
    testNpcMemoryStampCoversPrevious();
    testNpcStoreLines();
    testNpcStoreMajorKind();
    testNpcStoreGeneratorMenu();
    testNpcStoreProfileParse();
    testNpcStoreMajorFiles();
    testNpcStoreInvariants();
    testSayRefusals();
    testSayRenderBranches();
    testSpeakPrompt();
    testSpeakRequestBody();
    testValidateSpeech();
    testSayConversation();
    testSayResolver();
    testSayInvariants();
    testBardSelExports();
    testBardSelEligible();
    testBardSelEligibleFact();
    testBardWorkerLifecycle();
    testBardWorkerJobs();
    testBardOverture();
    testBardOvertureContract();
    testBardTrigger();
    testBardTriggerContract();
    testBardCommit();
    testBardCoalesce();
    testBardDegradation();
    testBardSelEligibleNewRoom();
    testBardSelHandle();
    testBardSelContext();
    testBardSelWakeContext();
    testBardSelPrompt();
    testBardSelRequestBody();
    testBardSelWakeRequestBody();
    testBardSelGateOverture();
    testBardSelGateWake();
    testBardSelAdmit();
    testBardSelContract();

    testStoryStoreSchema();
    testStoryStoreConditionCatalog();
    testStoryStoreArc();
    testStoryStoreWrite();
    testStoryStoreSeededSteps();
    testStoryConditions();
    testStoryAdvance();
    testStoryEvaluate();
    testStoryRendererInvisible();
    testStoryWakeTrigger();
    testStoryAdvanceRule();
    testStoryEmptyStepList();
    testStoryOneCallSite();
    testStoryStoreVersionGate();
    testStoryGoldenSession();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
