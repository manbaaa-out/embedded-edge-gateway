#include <sqlite3.h>
#include <iostream>

int main() {
    sqlite3* db = nullptr;
    if (sqlite3_open("test.db", &db) != SQLITE_OK) {
        std::cerr << "open: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    // 查某个设备的所有记录,按时间倒序。? 同样走 bind
    const char* sql =
        "SELECT id, device_id, value, ts FROM device_data "
        "WHERE device_id = ? ORDER BY ts DESC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "prepare: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return 1;
    }

    sqlite3_bind_text(stmt, 1, "sensor_01", -1, SQLITE_TRANSIENT);

    int rc;
    // 循环 step:每次 SQLITE_ROW 代表拿到一行
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        // column 索引从 0 开始(注意!和 bind 的从 1 开始相反)
        long        id   = sqlite3_column_int64(stmt, 0);
        const char* dev  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        double      val  = sqlite3_column_double(stmt, 2);
        long        ts   = sqlite3_column_int64(stmt, 3);

        std::cout << id << " | " << dev << " | " << val << " | " << ts << "\n";
    }

    // 循环正常结束时 rc 应为 SQLITE_DONE;否则是出错了
    if (rc != SQLITE_DONE) {
        std::cerr << "step error: " << sqlite3_errmsg(db) << "\n";
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}