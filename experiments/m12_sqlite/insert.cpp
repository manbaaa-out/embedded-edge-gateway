#include <sqlite3.h>
#include <iostream>
#include <ctime>

int main() {
    sqlite3* db = nullptr;
    if (sqlite3_open("test.db", &db) != SQLITE_OK) {
        std::cerr << "open: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    // ? 是占位符,值稍后 bind。注意 id 不写——AUTOINCREMENT 自动给
    const char* sql =
        "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;          // 预编译语句句柄,又一个不透明指针
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "prepare: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    // bind:占位符索引从 1 开始(不是 0!)
    const char* device_id = "sensor_01";
    double v = 25.3;
    long ts = static_cast<long>(std::time(nullptr));   // 当前 Unix 秒

    sqlite3_bind_text(stmt, 1, device_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, v);
    sqlite3_bind_int64(stmt, 3, ts);

    // step:INSERT 期望返回 SQLITE_DONE(完成),不是 SQLITE_ROW
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "step: " << sqlite3_errmsg(db) << "\n";
    } else {
        std::cout << "inserted, rowid = " << sqlite3_last_insert_rowid(db) << "\n";
    }

    sqlite3_finalize(stmt);   // 释放语句句柄
    sqlite3_close(db);
    return 0;
}