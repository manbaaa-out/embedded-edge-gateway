#pragma once
#include <sqlite3.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <cstdio>
#include "Statement.h"

namespace gateway{
class Database {
public:
    explicit Database(const std::string& path) {
        // 1) 打开连接。失败抛异常,不留半成品对象(RAII 原则)。
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            // open 失败时 db_ 仍可能非空,errmsg 能取到原因,取完要 close。
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("sqlite3_open failed: " + msg);
        }
 
        // 2) 开 WAL:读写并发更好(网络线程写 + 将来查询线程读)。
        //    PRAGMA 无参,用 exec 即可。失败不致命,打印告警继续。
        execNoThrow("PRAGMA journal_mode=WAL;");
        execNoThrow("PRAGMA synchronous=NORMAL;");
 
        // 3) 建表。每次启动都跑,靠 IF NOT EXISTS 保住已有数据。
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
    }

    ~Database() noexcept { if (db_) sqlite3_close(db_); }

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    void insert(const std::string& device_id, double value, long ts) {
        std::lock_guard<std::mutex> lock(mtx_);
        try {
            Statement st(db_,
                "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);");
            st.bind(1, device_id);
            st.bind(2, value);
            st.bind(3, ts);
            if (st.step() != SQLITE_DONE) {
                fprintf(stderr, "[db] insert step failed: %s\n", sqlite3_errmsg(db_));
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "[db] insert error: %s\n", e.what());
        }
        // st 离开作用域 → finalize(异常路径也释放)
    }
 
    // 仅供验证/调试:数一数现在有多少行。
    long count() {
        std::lock_guard<std::mutex> lock(mtx_);
        Statement st(db_, "SELECT COUNT(*) FROM device_data;");
        return (st.step() == SQLITE_ROW) ? st.column_int64(0) : -1;
    }

private:
    // 无参 PRAGMA 用,失败只告警不抛(WAL 开不起来不致命)
    void execNoThrow(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            fprintf(stderr, "[db] pragma warn: %s\n", err ? err : "?");
            sqlite3_free(err);
        }
    }
 
    sqlite3*   db_ = nullptr;   // 拥有,负责 close
    std::mutex mtx_;            // 保护 db_,串行化所有访问

};

}