#pragma once
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace gateway {

class Statement {
public:
    // 构造即 prepare。失败抛异常,绝不让半成品对象存在
    Statement(sqlite3* db, const char* sql) : db_(db) {
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare failed: ")
                                     + sqlite3_errmsg(db_));
        }
    }

    // 析构即 finalize。noexcept——析构不抛
    ~Statement() noexcept {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    // 禁止拷贝:句柄独占,拷了会被 double finalize
    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;

    // 允许移动:转移所有权后把源指针置空,防止源析构时重复释放
    Statement(Statement&& o) noexcept : db_(o.db_), stmt_(o.stmt_) {
        o.stmt_ = nullptr;
    }
    Statement& operator=(Statement&& o) noexcept {
        if (this != &o) {                       // 自赋值保护
            if (stmt_) sqlite3_finalize(stmt_);  // 先释放自己原持有的
            db_   = o.db_;
            stmt_ = o.stmt_;
            o.stmt_ = nullptr;
        }
        return *this;
    }

    // 薄封装:bind 索引从 1,column 从 0,这层不改变那个事实,只是少写 stmt_
    void bind(int i, const std::string& s) {
        sqlite3_bind_text(stmt_, i, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    void bind(int i, long   v) { sqlite3_bind_int64 (stmt_, i, v); }

    // step 透传:调用方自己判断 SQLITE_ROW / SQLITE_DONE
    int step() { return sqlite3_step(stmt_); }

    // 新增:倒回初始可执行态,以便换参数再跑。≠ finalize。
    // sqlite3_reset 只重置执行游标,不清除已 bind 的值;
    // 这里顺带 clear_bindings 把绑定也清掉,保证每轮干净重 bind。
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    long        column_int64 (int i) { return sqlite3_column_int64(stmt_, i); }
    double      column_double(int i) { return sqlite3_column_double(stmt_, i); }
    std::string column_text  (int i) {
        // 当场拷成 string,避开下一次 step 后裸指针悬垂的坑
        const unsigned char* p = sqlite3_column_text(stmt_, i);
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }

private:
    sqlite3*      db_   = nullptr;   // 不拥有,只借用(连接由更外层管)
    sqlite3_stmt* stmt_ = nullptr;   // 拥有,负责 finalize
};

} // namespace gateway