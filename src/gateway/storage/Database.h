#pragma once

// SQLite 遥测存储封装。
//
// 读写实例负责模式初始化、schema 和复用 INSERT 语句；只读实例只提供查询。类内部
// 不加锁，一个实例必须由一条线程串行使用。项目用独立的写连接与 HTTP 只读连接，并
// 尽力启用 WAL 获得文件级读写并发，避免在同一连接上共享事务状态。

#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include "gateway/core/log/Logger.h"
#include "gateway/storage/Statement.h"
#include <vector>

namespace gateway {

/** 数据库 device_data 表的一条业务记录。 */
struct DataRow {
    std::string device_id; /**< 逻辑指标名，例如 temperature 或 illuminance。 */
    double value;          /**< 已完成协议定标的业务数值。 */
    long ts;               /**< 收帧时刻的 Unix 时间戳，单位秒。 */
};

/**
 * SQLite 连接的独占 RAII 对象。读写模式负责建表、尽力启用 WAL/NORMAL，并缓存
 * INSERT；只读模式以 SQLITE_OPEN_READONLY 打开现有文件，不执行任何初始化 SQL。
 */
class Database {
public:
    /**
     * @brief 打开数据库连接并按模式初始化。
     * @param path SQLite 文件路径；只读模式下文件必须已经存在。
     * @param readonly true 创建查询专用连接，false 创建遥测写连接。
     * @throws std::runtime_error 打开、建表或预编译 INSERT 失败。
     * @throws std::bad_alloc 标准字符串、Statement 或日志缓冲分配失败。
     * @throws std::system_error 构造期首次记录告警且日志线程无法创建。
     *
     * WAL、synchronous 和索引创建的 SQLite 错误不会直接使构造失败，而是记录告警。
     */
    explicit Database(const std::string& path, bool readonly = false) {
        if (readonly) {
            // 只读模式要求数据库文件已存在。
            int rc = sqlite3_open_v2(path.c_str(), &db_,
                                     SQLITE_OPEN_READONLY, nullptr); // 打开结果码。
            if (rc != SQLITE_OK) {
                std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory"; // 关闭前复制错误。
                if (db_) sqlite3_close(db_);
                db_ = nullptr;
                throw std::runtime_error("sqlite3_open(readonly) failed: " + msg);
            }
            return;
        }
 
        int rc = sqlite3_open(path.c_str(), &db_); // 读写连接的打开结果码。
        if (rc != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory"; // 关闭前复制错误。
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("sqlite3_open failed: " + msg);
        }
        execNoThrow("PRAGMA journal_mode=WAL;");
        execNoThrow("PRAGMA synchronous=NORMAL;");
 
        const char* ddl = // 持久化 schema；id 只作稳定主键，查询按 device_id/ts。
            "CREATE TABLE IF NOT EXISTS device_data("
            " id        INTEGER PRIMARY KEY AUTOINCREMENT,"
            " device_id TEXT    NOT NULL,"
            " value     REAL,"
            " ts        INTEGER NOT NULL);";
        char* err = nullptr; // sqlite3_exec 在失败时分配的错误文本，由 sqlite3_free 释放。
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "?"; // 释放 SQLite 缓冲区前取得所有权副本。
            sqlite3_free(err);
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("create table failed: " + msg);
        }
 
        // 支持按设备筛选并按时间倒序读取。
        execNoThrow("CREATE INDEX IF NOT EXISTS idx_dev_ts "
                    "ON device_data(device_id, ts);");
 
        insertStmt_ = new Statement(db_, // 连接生命周期内复用的三参数 INSERT。
            "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);");
    }

    /** 先 finalize 缓存语句，再关闭本对象独占的 SQLite 连接。 */
    ~Database() noexcept {
        // SQLite 要求先结束关联语句，再关闭连接。
        delete insertStmt_;
        if (db_) sqlite3_close(db_);
    }

    /** 复制源对象不会被读取；连接和缓存语句均为独占资源。 */
    Database(const Database&)            = delete;
    /** 复制源对象不会被读取；禁止多个对象关闭同一连接。 */
    Database& operator=(const Database&) = delete;

    /**
     * @brief 尽量在一个事务中插入一批遥测记录。
     * @param rows 待写行；空集合无操作。调用期间不得从其他线程访问本实例。
     * @pre 本实例必须以 readonly=false 构造；只读实例没有 insertStmt_。
     *
     * BEGIN 失败后仍在连接当时的事务状态下逐条尝试写入；bind/reset 返回码
     * 当前被忽略，单条 step 失败只记日志并继续。COMMIT 失败后尝试
     * ROLLBACK；若回滚也失败，事务仍可能保持打开。本接口不返回部分成功数量，
     * 调用方不能据此做可靠重放。
     */
    void insertBatch(const std::vector<DataRow>& rows) {
        if (rows.empty()) return;

        // BEGIN 失败后仍在连接当前事务状态下逐条尝试 INSERT，以保留可写的数据。
        execNoThrow("BEGIN;");
        for (const auto& r : rows) { // r 是本轮绑定到缓存 INSERT 的只读业务行。
            insertStmt_->bind(1, r.device_id);
            insertStmt_->bind(2, r.value);
            insertStmt_->bind(3, r.ts);
            if (insertStmt_->step() != SQLITE_DONE) {
                LOG_ERROR("db insert step failed: %s", sqlite3_errmsg(db_));
            }
            insertStmt_->reset();
        }
        // COMMIT 失败后尝试回滚，使下一批有机会重新开启事务。
        if (!exec("COMMIT;")) {
            LOG_ERROR("db COMMIT failed: %s — rolling back to clear the open transaction",
                      sqlite3_errmsg(db_));
            execNoThrow("ROLLBACK;");
        }
    }

    /**
     * @brief 查询指定指标最近的记录。
     * @param device_id 与 device_data.device_id 精确匹配的逻辑指标名。
     * @param limit 传给 SQLite LIMIT 的行数；调用方应传正数。
     * @return 按 ts 从新到旧排列的值对象；SQL 执行错误会表现为空或截断结果。
     */
    std::vector<DataRow> query(const std::string& device_id, int limit) {
        std::vector<DataRow> rows; // 拥有查询结果，返回后不依赖 SQLite 行缓冲区。
        Statement st(db_,          // 本函数作用域内独占的查询语句。
            "SELECT device_id, value, ts FROM device_data "
            "WHERE device_id = ? ORDER BY ts DESC LIMIT ?;");
        st.bind(1, device_id);
        st.bind(2, static_cast<long>(limit));
        while (st.step() == SQLITE_ROW) { // 每轮复制当前 SQLite 行的三列。
            rows.push_back(DataRow{
                st.column_text(0),
                st.column_double(1),
                static_cast<long>(st.column_int64(2))
            });
        }
        return rows;
    }

    /**
     * @brief 统计 device_data 的总行数。
     * @return 查询成功时为行数；未得到 SQLITE_ROW 时返回 -1。
     */
    long count() {
        Statement st(db_, "SELECT COUNT(*) FROM device_data;"); // 单行聚合查询。
        return (st.step() == SQLITE_ROW) ? st.column_int64(0) : -1;
    }

private:
    /**
     * @brief 执行不返回结果集的 SQL。
     * @param sql 以 NUL 结尾的 SQL 文本。
     * @return sqlite3_exec 成功返回 true；失败时释放错误文本并返回 false。
     */
    bool exec(const char* sql) {
        char* err = nullptr; // SQLite 可选分配的错误文本，仅用于按 API 要求释放。
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            sqlite3_free(err);
            return false;
        }
        return true;
    }

    /**
     * @brief 执行辅助 SQL，SQLite 返回失败时记录 SQL 和错误而不主动转成异常。
     * @param sql 以 NUL 结尾的 SQL 文本。
     *
     * 日志后端自身的分配或初始化异常仍可从本函数传出。
     */
    void execNoThrow(const char* sql) {
        char* err = nullptr; // 失败时由 SQLite 分配，日志完成后释放。
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            LOG_WARN("db exec failed [%s]: %s", sql, err ? err : "?");
            sqlite3_free(err);
        }
    }

    sqlite3* db_ = nullptr; /**< 本对象独占的连接句柄，析构时 close。 */
    Statement* insertStmt_ = nullptr; /**< 仅读写模式创建的缓存 INSERT，析构时 delete。 */
};

} // namespace gateway
