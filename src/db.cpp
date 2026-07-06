#include "db.hpp"

#include <stdexcept>

#include "sqlite3.h"

namespace {

[[noreturn]] void throwError(sqlite3* db) {
    throw std::runtime_error(db ? sqlite3_errmsg(db) : "sqlite3: unknown error");
}

void checkOk(sqlite3* db, int rc) {
    if (rc != SQLITE_OK) throwError(db);
}

}  // namespace

// --- Stmt ---

Stmt::~Stmt() {
    sqlite3_finalize(stmt_);  // safe on nullptr
}

Stmt::Stmt(Stmt&& other) noexcept
    : db_(other.db_), stmt_(other.stmt_) {
    other.db_ = nullptr;
    other.stmt_ = nullptr;
}

Stmt& Stmt::operator=(Stmt&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = other.stmt_;
        other.db_ = nullptr;
        other.stmt_ = nullptr;
    }
    return *this;
}

void Stmt::bind(int index, int64_t value) {
    checkOk(db_, sqlite3_bind_int64(stmt_, index, value));
}

void Stmt::bind(int index, const std::string& value) {
    checkOk(db_, sqlite3_bind_text(stmt_, index, value.data(),
                                   static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT));
}

bool Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throwError(db_);
}

int64_t Stmt::colInt(int col) {
    return sqlite3_column_int64(stmt_, col);
}

std::string Stmt::colText(int col) {
    const unsigned char* text = sqlite3_column_text(stmt_, col);
    if (!text) {
        // NULL means either SQL NULL (fine) or allocation failure (must throw).
        if (sqlite3_errcode(db_) == SQLITE_NOMEM) throwError(db_);
        return {};
    }
    int size = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(size));
}

// --- Db ---

Db::Db(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3: out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
}

Db::~Db() {
    sqlite3_close(db_);
}

Db::Db(Db&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        sqlite3_close(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Db::exec(const char* sql) {
    checkOk(db_, sqlite3_exec(db_, sql, nullptr, nullptr, nullptr));
}

Stmt Db::prepare(const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    checkOk(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr));
    return Stmt(db_, stmt);
}

int64_t Db::changes() {
    return sqlite3_changes64(db_);
}

void Db::begin() { exec("BEGIN"); }
void Db::commit() { exec("COMMIT"); }

void Db::rollback() {
    // Safe to call unconditionally. Certain errors (SQLITE_FULL/IOERR/NOMEM,
    // some failed COMMITs) make SQLite roll back automatically, leaving the
    // connection in autocommit mode; issuing ROLLBACK then would itself fail
    // ("cannot rollback - no transaction is active") and throw out of a catch
    // block. No-op when no transaction is active.
    if (sqlite3_get_autocommit(db_)) return;
    exec("ROLLBACK");
}
