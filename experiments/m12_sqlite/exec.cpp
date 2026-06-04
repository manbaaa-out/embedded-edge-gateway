#include <sqlite3.h>
#include <iostream>

int main() {
    sqlite3* db = nullptr;
    if (sqlite3_open("test.db", &db) != SQLITE_OK) {
        std::cerr << "open failed: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    const char* sql =
        "CREATE TABLE IF NOT EXISTS device_data ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  device_id TEXT    NOT NULL,"
        "  value     REAL,"
        "  ts        INTEGER NOT NULL"   // Unix 时间戳,存整数
        ");";

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "exec failed: " << errmsg << "\n";
        sqlite3_free(errmsg);   // 注意:errmsg 由 sqlite 分配,必须用 sqlite3_free 释放
    } else {
        std::cout << "table created\n";
    }

    sqlite3_close(db);
    return 0;
}