// World file lifecycle: schema DDL, open-or-create, schema_version gate,
// seed execution. See .lore/work/design/engine-foundation.md §3 and §8.
#pragma once

#include <stdexcept>
#include <string>

#include "db.hpp"

// Bump whenever the DDL in world.cpp changes shape. On mismatch openWorld()
// refuses the file (no migrations until a world worth keeping exists).
inline constexpr int64_t SCHEMA_VERSION = 5;

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
Db openWorld(const std::string& path,
             const std::string& seedPath = "seed/base.sql",
             const std::string& settingPath = "seed/setting.txt");
