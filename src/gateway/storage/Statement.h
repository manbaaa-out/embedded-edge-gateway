#pragma once

// sqlite3_stmt 的 RAII 封装:构造即 prepare,析构即 finalize。
//
// 预编译省掉的是每次插入的解析与查询计划生成;更要紧的是参数绑定天然免疫 SQL 注入,
// 绑定的值永远被当作数据,不会被解析成语法。当前 device_id 来自解码器的固定字符串,
// 注入风险不大 —— 但哪天它变成来自网络的字符串,拼接 SQL 就是个洞。
//
// 两处易错见下方注释:bind 的 SQLITE_TRANSIENT,以及移动构造里的置空。

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

    // stmt 是独占资源。移动时必须把源置空,否则两个对象析构时都会 finalize ——
    // 与 SerialPort 移动后置 fd_ = -1 是同一件事,所有 RAII 移动语义的固定动作。
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

    // 末参告诉 SQLite 这块内存能活多久:STATIC = 「到 step 之前都有效,你直接引用」,
    // TRANSIENT = 「马上就没了,你自己拷一份」。这里绑的是调用方传进来的引用,
    // 生命周期由调用方决定,用 STATIC 就是在赌 —— 赌输了读到已释放内存,
    // 而 SQLite 不会报错,只会写进去一段垃圾。多一次拷贝换掉一整类生命周期问题。
    void bind(int i, const std::string& s) {
        sqlite3_bind_text(stmt_, i, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    void bind(int i, long   v) { sqlite3_bind_int64 (stmt_, i, v); }

    int step() { return sqlite3_step(stmt_); }

    // sqlite3_reset 只把语句倒回可重新执行的状态,绑定的值仍留着。下一次若只绑了
    // 部分参数,没绑的会沿用上一次的值 —— 表现为「偶尔有一条数据的某个字段是上一条
    // 的」,极难查。当前每次都绑全,严格说 clear_bindings 不是必需的;加上它是为了
    // 将来有人加一个可选字段时不会踩坑。
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
