// Thin RAII wrapper over sqlite3. All errors throw std::runtime_error
// carrying the sqlite3_errmsg text.
#pragma once

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

class Stmt {
public:
    explicit Stmt(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    ~Stmt();

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept;
    Stmt& operator=(Stmt&& other) noexcept;

    void bind(int index, int64_t value);
    void bind(int index, const std::string& value);
    bool step();  // true = row available, false = done
    int64_t colInt(int col);
    std::string colText(int col);

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;

    void exec(const char* sql);
    Stmt prepare(const char* sql);
    // Rows changed by the most recent INSERT/UPDATE/DELETE (sqlite3_changes64).
    int64_t changes();
    void begin();
    void commit();
    // Safe to call unconditionally: a no-op when no transaction is active
    // (e.g. after an error that made SQLite auto-rollback), so error paths
    // may always call it without risking a throw from the cleanup itself.
    void rollback();

private:
    sqlite3* db_ = nullptr;
};
