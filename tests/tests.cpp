// Micro test harness: CHECK(cond) records failures; main() reports a summary.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "action.hpp"
#include "db.hpp"
#include "loop.hpp"
#include "mutations.hpp"
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
    const std::string seedPath = "seed/base.sql";

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

static void testParser() {
    const TempDbFile worldPath("textworld_parser_tests.db");

    Db db = openWorld(worldPath.string(), "seed/base.sql");

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

    Db db = openWorld(worldPath.string(), "seed/base.sql");

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

    Db db = openWorld(worldPath.string(), "seed/base.sql");

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

// Substring helper for renderer output checks.
static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static void testRender() {
    const TempDbFile worldPath("textworld_render_tests.db");

    Db db = openWorld(worldPath.string(), "seed/base.sql");
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
        Db db = openWorld(worldPath.string(), "seed/base.sql");
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
        Db db = openWorld(worldPath.string(), "seed/base.sql");

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
        Db db = openWorld(worldPath.string(), "seed/base.sql");

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
        Db db = openWorld(worldPath.string(), "seed/base.sql");
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

static void testProseFacts() {
    using nlohmann::json;

    const TempDbFile worldPath("textworld_prose_tests.db");
    Db db = openWorld(worldPath.string(), "seed/base.sql");

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

    // --- hygiene sweep over every payload built this session ---
    for (int64_t t = 1; t <= 8; ++t) {
        checkPayloadHygiene(buildFacts(db, t).payload);
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

// --- persistence after play (REQ-PROTO-12 item 8, REQ-PROTO-10): a played
// world survives a full close/reopen with turn counter, entity positions, and
// the complete event transcript intact — and is still playable afterward. ---
static void testPersistence() {
    const TempDbFile worldPath("textworld_persist_tests.db");

    // Play three turns through the production path, then close (Db destructor
    // at scope exit releases the connection; DELETE journal mode means the
    // file on disk is the full committed state).
    {
        Db db = openWorld(worldPath.string(), "seed/base.sql");
        CHECK(runTurn(db, "take lantern").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "go north").outcome == TurnOutcome::Ticked);
        CHECK(runTurn(db, "drop lantern").outcome == TurnOutcome::Ticked);
        CHECK(queryInt(db, "SELECT value FROM meta WHERE key = 'turn'") == 3);
    }

    // Reopen the same file: everything preserved.
    {
        Db db = openWorld(worldPath.string(), "seed/base.sql");

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
        Db db = openWorld(origPath.string(), "seed/base.sql");
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
        Db db = openWorld(copyPath.string(), "seed/base.sql");
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
        Db orig = openWorld(origPath.string(), "seed/base.sql");
        CHECK(queryInt(orig, "SELECT value FROM meta WHERE key = 'turn'") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 3") == 2);
        CHECK(queryInt(orig, "SELECT container FROM location WHERE entity = 4") == 3);

        Db copy = openWorld(copyPath.string(), "seed/base.sql");
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

int main() {
    CHECK(1 + 1 == 2);

    testDb();
    testWorld();
    testParser();
    testMutations();
    testSystems();
    testRender();
    testLoop();
    testProseFacts();
    testPersistence();
    testPortability();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
