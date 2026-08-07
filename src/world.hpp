// World file lifecycle: schema DDL, open-or-create, schema_version gate,
// seed execution. See .lore/work/design/engine-foundation.md §3 and §8.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "db.hpp"

// Bump whenever the DDL in world.cpp changes shape. On mismatch openWorld()
// refuses the file (no migrations until a world worth keeping exists).
inline constexpr int64_t SCHEMA_VERSION = 7;

// Thrown by openWorld() when an existing world file carries a different
// schema_version. Nothing has been written to the file when this is thrown;
// callers should catch it (or std::exception) and exit nonzero. The refusal
// message telling the user to delete the world file has already been printed
// to stderr.
class SchemaMismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Open (or create) the world database at `path`.
//
// - File absent, or present but uninitialized (no `meta` table — e.g. a
//   zero-byte file): applies the schema DDL, executes the seed SQL script,
//   and records meta.schema_version = SCHEMA_VERSION and meta.turn = 0,
//   all in one transaction.
// - File present and initialized: reads meta.schema_version. On mismatch,
//   prints a refusal message to stderr and throws SchemaMismatch WITHOUT
//   writing to the database.
//
// Interface contract: `seedPath` is resolved relative to the current working
// directory; the binary is always launched from the repo root, where the
// default "seed/base.sql" resolves. Tests may pass an explicit path.
//
// `settingPath` (default "seed/setting.txt") is a freeform setting document
// loaded once into meta.setting at init (REQ-ARCH-1). Unlike the mandatory
// seed, it is read TOLERANTLY: an absent or empty file leaves meta.setting
// empty/absent and init still succeeds — an empty setting simply yields a
// thinner architect prompt. Zero DDL: this is a new meta *row*, not a shape.
//
// The return carries `created` alongside the handle (REQ-BARD-WAKE-1): true
// IFF initialize() ran on THIS call. It is the once-ever hook the bard's
// overture hangs on, and it is FALSE for every resumed session — a world is
// authored once and then only played. The SchemaMismatch path returns nothing
// at all, so `created` is never observed on a refused file.
struct OpenedWorld {
    Db db;
    bool created = false;
};

// --- Hand-authored major characters (specs/npc-memory-store.md) --------------

// One hand-authored major-character file, as main.cpp read it off disk. `name`
// is carried only so a malformed file can be NAMED in the failure
// (REQ-NPCSTORE-31); nothing in the world file records it.
//
// The files arrive as DATA, not as a path: main.cpp does the reading and
// world.cpp performs no file I/O of its own for these (REQ-NPCSTORE-34). That
// also suits the test suite, which writes no files at all — a loader test
// builds this vector in C++.
struct MajorProfileFile {
    std::string name;  // the file's name, for the failure message
    std::string text;  // its bytes, verbatim
};

// The parsed form: the header supplies the catalog columns, the body supplies
// catalog_profile.profile and is MODEL-FACING ONLY (REQ-NPCSTORE-28). The
// engine-owned rules — never explain a mechanic, never volunteer background,
// never name what does not exist — are deliberately NOT here. They live in the
// prompt the engine controls, because a rule a player will actively attack
// cannot live in a file an author can edit or forget.
//
// There is no `goal` field (REQ-NPCSTORE-30). Movement is a later brick and
// nothing here reads a goal; shipping the field inert would be the thing the
// bard fact store already declined to do with catalog_binding.
struct MajorProfile {
    std::string handle, name, motive, profile;
    int64_t tier = 0;
};

// Parse one profile file: `key: value` header lines, one blank line, then the
// body VERBATIM. The header ends at that first blank line and NEVER resumes
// (REQ-NPCSTORE-27), so a `key: value` line inside the body is body text.
//
// Returns "" on success, else the reason — WITHOUT the file name, which the
// caller prefixes. Pure: no database, no I/O, no logging.
std::string parseMajorProfile(const std::string& text, MajorProfile& out);

// Open (or create) the world, optionally seeding it with hand-authored major
// characters. `majors` is EMPTY in the normal case and that is not a fault, not
// a diagnostic, and not a degraded world (REQ-NPCSTORE-32) — a world with no
// cast is a valid world. The default keeps every existing call site unchanged.
//
// The files are written inside initialize()'s existing transaction, so major
// rows exist the instant openWorld returns and before any overture call is
// possible (REQ-NPCSTORE-33) — and a malformed file rolls the whole world back
// rather than leaving it half-seeded.
OpenedWorld openWorld(const std::string& path,
                      const std::string& seedPath = "seed/base.sql",
                      const std::string& settingPath = "seed/setting.txt",
                      const std::vector<MajorProfileFile>& majors = {});
