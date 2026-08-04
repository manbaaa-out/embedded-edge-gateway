#pragma once
#include <sqlite3.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <cstdio>
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
            // 只读打开:库必须已存在(主链连接已建好)。最小权限:连接层禁写。
            int rc = sqlite3_open_v2(path.c_str(), &db_,
                                     SQLITE_OPEN_READONLY, nullptr);
            if (rc != SQLITE_OK) {
                std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
                if (db_) sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error("sqlite3_open(readonly) failed: " + msg);
            }
            return;   // 只读连接到此为止:不建表、不开WAL、不缓存 insert 语句
        }
 
        // ----- 读写连接(主链):原逻辑不变 -----
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
 
        // [S5改] 建复合索引,加速 query 的 WHERE device_id=? ORDER BY ts DESC
        execNoThrow("CREATE INDEX IF NOT EXISTS idx_dev_ts "
                    "ON device_data(device_id, ts);");
 
        insertStmt_ = new Statement(db_,
            "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);");
    }

    ~Database() noexcept {
        delete insertStmt_;          // 先 finalize 语句,再 close 连接(顺序要紧)
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
            fprintf(stderr, "[db] insert step failed: %s\n", sqlite3_errmsg(db_));
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
            fprintf(stderr, "[db] pragma warn: %s\n", err ? err : "?");
            sqlite3_free(err);
        }
    }

    sqlite3*   db_         = nullptr;   // 拥有,负责 close
    Statement* insertStmt_ = nullptr;   // 拥有,缓存的 insert 语句
    std::mutex mtx_;                    // 保护 db_ 和 insertStmt_
};

} // namespace gateway
