#include <sqlite3.h>
#include <iostream>

int main() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open("test.db", &db);
    if (rc != SQLITE_OK) {
        std::cerr << "open failed" << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    std::cout << "opened, sqlite version: " << sqlite3_libversion() << "\n";
    sqlite3_close(db);  // 必须配对关闭,否则句柄泄漏
    return 0;
}