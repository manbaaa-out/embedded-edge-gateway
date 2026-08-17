#pragma once

// sqlite3_stmt 的独占 RAII 封装。
//
// 预编译语句把 SQL 结构与运行期数据分离，重复执行时只需重新绑定参数。Statement 拥有
// sqlite3_stmt*，但只借用 sqlite3*；因此它必须先于对应 Database/连接析构，且与连接
// 一样由单线程串行使用。绑定和 reset 的 SQLite 返回码当前不向外暴露，执行结果通过
// step() 的原始 SQLite 状态码判断。

#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace gateway {

/** 独占一个已 prepare 的 SQLite 语句，可移动、不可复制。 */
class Statement {
public:
    /**
     * @brief 在现有连接上预编译 SQL。
     * @param db 借用的有效 SQLite 连接，必须比本对象存活更久。
     * @param sql 以 NUL 结尾的 SQL 文本；本类只编译第一条语句。
     * @throws std::runtime_error sqlite3_prepare_v2 失败。
     */
    Statement(sqlite3* db, const char* sql) : db_(db) {
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr); // SQLite prepare 结果码。
        if (rc != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare failed: ")
                                     + sqlite3_errmsg(db_));
        }
    }
    /** finalize 当前拥有的语句；移动后空对象不执行任何操作。 */
    ~Statement() noexcept { if (stmt_) sqlite3_finalize(stmt_); }

    /** 复制源对象不会被读取；禁止两个对象共同 finalize 一个 stmt。 */
    Statement(const Statement&)            = delete;
    /** 复制源对象不会被读取；禁止共享 stmt 所有权。 */
    Statement& operator=(const Statement&) = delete;
    /** @param o 提供 stmt 的源对象；调用后仅可析构或再次赋值。 */
    Statement(Statement&& o) noexcept : db_(o.db_), stmt_(o.stmt_) { o.stmt_ = nullptr; }
    /**
     * @param o 提供 stmt 的源对象；调用后被置为无 stmt 状态。
     * @return *this；先 finalize 当前 stmt，再从 o 接管。
     */
    Statement& operator=(Statement&& o) noexcept {
        if (this != &o) {
            if (stmt_) sqlite3_finalize(stmt_);
            db_ = o.db_; stmt_ = o.stmt_; o.stmt_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief 绑定文本参数。
     * @param i SQLite 的 1-based 参数索引。
     * @param s 文本值；SQLITE_TRANSIENT 使 SQLite 在返回前复制内容。
     */
    void bind(int i, const std::string& s) {
        sqlite3_bind_text(stmt_, i, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    /**
     * @brief 绑定浮点参数。
     * @param i SQLite 的 1-based 参数索引。
     * @param v 写入 SQLite REAL 的值。
     */
    void bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); }
    /**
     * @brief 绑定整数参数。
     * @param i SQLite 的 1-based 参数索引。
     * @param v 转换为 SQLite 64 位整数的值。
     */
    void bind(int i, long   v) { sqlite3_bind_int64 (stmt_, i, v); }

    /** @return sqlite3_step() 的原始状态码，例如 SQLITE_ROW 或 SQLITE_DONE。 */
    int step() { return sqlite3_step(stmt_); }

    /** 结束本次执行并清空全部绑定，使下一轮不会沿用旧参数。 */
    void reset() { sqlite3_reset(stmt_); sqlite3_clear_bindings(stmt_); }

    /** @param i 0-based 列索引。@return 当前行该列的 64 位整数转换为 long。 */
    long        column_int64 (int i) { return sqlite3_column_int64(stmt_, i); }
    /** @param i 0-based 列索引。@return 当前行该列的 double 值。 */
    double      column_double(int i) { return sqlite3_column_double(stmt_, i); }
    /**
     * @param i 0-based 列索引。
     * @return 当前行该列的文本副本；SQL NULL 转为空字符串。
     */
    std::string column_text  (int i) {
        const unsigned char* p = sqlite3_column_text(stmt_, i); // SQLite 所有的临时 UTF-8 视图。
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }

private:
    sqlite3* db_ = nullptr;      /**< 借用连接，仅用于构造错误消息，不负责 close。 */
    sqlite3_stmt* stmt_ = nullptr; /**< 独占预编译语句，析构时 finalize。 */
};

} // namespace gateway
