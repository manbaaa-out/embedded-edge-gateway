#pragma once

// SQLite 连接。一个类两种角色:读写连接(WAL + 建表 + 缓存 insert 语句)与只读连接。
//
// 为什么非要分成两个连接 —— WAL 让读写不互斥,但【并发的单位是连接,不是线程】。
// 同一个连接上的语句共享事务上下文,还被 SQLite 内部互斥串起来;读写共用一个连接时
// WAL 一点作用都发挥不出来。所以 HTTP 那条路径必须单开一个只读连接,不是多此一举。
//
// 本类内部没有锁,因为没有共享:写连接归遥测写线程独占,只读连接归 HTTP 线程独占。
// 这条契约由调用方保证 —— 与 CommandTracker 用的是同一套约定。
//
// 顺带两个独立的点别混:「单独一个连接」是为了并发,SQLITE_OPEN_READONLY 是为了
// 最小权限(HTTP 那条路将来写出 bug 也在连接层被挡住),后者与性能无关。

#include <sqlite3.h>
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

// SQLite 连接的 RAII 封装。
//
// 线程模型:一个实例只允许被一条线程访问,故内部无锁。该前提由调用方保证,
// 本类不做强制 —— 与 CommandTracker 用的是同一套约定。
// 本项目里两个实例各自归属一条线程:
//   写连接   由 TelemetryPipeline 的写线程独占(app 层只持有指针用于热加载替换,
//            从不调用其任何方法);
//   只读连接 由 HTTP 监控线程独占。
// 两条连接经 WAL 实现读写并发,并发性由 SQLite 在文件层保证,不需要进程内的锁。
//
// 此处曾有一把 mtx_ 保护 db_ 与 insertStmt_,那是线程池时代的遗留:当时 N 个
// worker 同时 insert,必须串行化。但那把锁把「并行落库」又变回了串行,只是多付了
// 锁竞争的开销 —— 真正的修法是消除共享(单写线程),而不是保护共享。
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

    // 一整批记录写在同一个事务里。
    //
    // 批量的收益不在于少几次函数调用,而在于事务边界:每条 INSERT 单独提交时,
    // SQLite 要为每条各走一遍 WAL 帧写入与提交记录;包进一个事务后这份开销
    // 按批摊薄。批越大越划算,而批的大小由写线程「一次能从队列里捞到多少」
    // 自然决定 —— 空闲时一批就是一帧,忙起来自动变大,不需要额外的攒批延迟。
    //
    // 部分失败的处理:单条 step 失败只记日志并继续,最后仍提交。理由是遥测
    // 数据条目之间彼此独立,为一条坏记录回滚整批会放大故障;事务在这里的作用
    // 是摊薄提交开销,不是维护跨记录的一致性。
    void insertBatch(const std::vector<DataRow>& rows) {
        if (rows.empty()) return;

        execNoThrow("BEGIN;");
        for (const auto& r : rows) {
            insertStmt_->bind(1, r.device_id);
            insertStmt_->bind(2, r.value);
            insertStmt_->bind(3, r.ts);
            if (insertStmt_->step() != SQLITE_DONE) {
                LOG_ERROR("db insert step failed: %s", sqlite3_errmsg(db_));
            }
            insertStmt_->reset();
        }
        execNoThrow("COMMIT;");
    }

    std::vector<DataRow> query(const std::string& device_id, int limit) {
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
};

} // namespace gateway
