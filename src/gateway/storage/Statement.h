#pragma once
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace gateway {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare failed: ")
                                     + sqlite3_errmsg(db_));
        }
    }
    ~Statement() noexcept { if (stmt_) sqlite3_finalize(stmt_); }

    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& o) noexcept : db_(o.db_), stmt_(o.stmt_) { o.stmt_ = nullptr; }
    Statement& operator=(Statement&& o) noexcept {
        if (this != &o) {
            if (stmt_) sqlite3_finalize(stmt_);
            db_ = o.db_; stmt_ = o.stmt_; o.stmt_ = nullptr;
        }
        return *this;
    }

    void bind(int i, const std::string& s) {
        sqlite3_bind_text(stmt_, i, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    void bind(int i, long   v) { sqlite3_bind_int64 (stmt_, i, v); }

    int step() { return sqlite3_step(stmt_); }
    void reset() { sqlite3_reset(stmt_); sqlite3_clear_bindings(stmt_); }

    long        column_int64 (int i) { return sqlite3_column_int64(stmt_, i); }
    double      column_double(int i) { return sqlite3_column_double(stmt_, i); }
    std::string column_text  (int i) {
        const unsigned char* p = sqlite3_column_text(stmt_, i);
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }

private:
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace gateway
