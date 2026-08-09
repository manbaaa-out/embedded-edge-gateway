#pragma once
#include <sqlite3.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include "gateway/core/log/Logger.h"
#include "gateway/storage/Statement.h"
#include <vector>

namespace gateway {

struct DataRow {
    std::string device_id;
    double      value;
    long        ts;
};

class Database {
public:
    explicit Database(const std::string& path, bool readonly = false) {
        if (readonly) {
            // 只读打开。前置条件:库已由主链写连接创建。以最小权限在连接层禁写。
            int rc = sqlite3_open_v2(path.c_str(), &db_,
                                     SQLITE_OPEN_READONLY, nullptr);
            if (rc != SQLITE_OK) {
                std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
                if (db_) sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error("sqlite3_open(readonly) failed: " + msg);
            }
            return;   // 只读连接不建表、不开 WAL、不缓存 insert 语句
        }
 
        // ---- 读写连接 ----
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("sqlite3_open failed: " + msg);
        }
        execNoThrow("PRAGMA journal_mode=WAL;");
        execNoThrow("PRAGMA synchronous=NORMAL;");
 
        const char* ddl =
            "CREATE TABLE IF NOT EXISTS device_data("
            " id        INTEGER PRIMARY KEY AUTOINCREMENT,"
            " device_id TEXT    NOT NULL,"
            " value     REAL,"
            " ts        INTEGER NOT NULL);";
        char* err = nullptr;
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "?";
            sqlite3_free(err);
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("create table failed: " + msg);
        }
 
        // 复合索引,对应查询的 WHERE device_id = ? ORDER BY ts DESC
        execNoThrow("CREATE INDEX IF NOT EXISTS idx_dev_ts "
                    "ON device_data(device_id, ts);");
 
        insertStmt_ = new Statement(db_,
            "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);");
    }

    ~Database() noexcept {
        delete insertStmt_;          // 必须先 finalize 语句再 close 连接,顺序不可颠倒
        if (db_) sqlite3_close(db_);
    }

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    void insert(const std::string& device_id, double value, long ts) {
        std::lock_guard<std::mutex> lock(mtx_);
        insertStmt_->bind(1, device_id);
        insertStmt_->bind(2, value);
        insertStmt_->bind(3, ts);
        if (insertStmt_->step() != SQLITE_DONE) {
            LOG_ERROR("db insert step failed: %s", sqlite3_errmsg(db_));
        }
        insertStmt_->reset();
    }

    std::vector<DataRow> query(const std::string& device_id, int limit) {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<DataRow> rows;
        Statement st(db_,
            "SELECT device_id, value, ts FROM device_data "
            "WHERE device_id = ? ORDER BY ts DESC LIMIT ?;");
        st.bind(1, device_id);
        st.bind(2, static_cast<long>(limit));
        while (st.step() == SQLITE_ROW) {
            rows.push_back(DataRow{
                st.column_text(0),
                st.column_double(1),
                static_cast<long>(st.column_int64(2))
            });
        }
        return rows;
    }

    long count() {
        std::lock_guard<std::mutex> lock(mtx_);
        Statement st(db_, "SELECT COUNT(*) FROM device_data;");
        return (st.step() == SQLITE_ROW) ? st.column_int64(0) : -1;
    }

private:
    void execNoThrow(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            LOG_WARN("db pragma failed: %s", err ? err : "?");
            sqlite3_free(err);
        }
    }

    sqlite3*   db_         = nullptr;   // 独占所有权,析构时 close
    Statement* insertStmt_ = nullptr;   // 独占所有权,缓存的 insert 语句
    std::mutex mtx_;                    // 保护 db_ 与 insertStmt_
};

} // namespace gateway
