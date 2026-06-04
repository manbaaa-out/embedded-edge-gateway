#include "Statement.h"
#include <iostream>

int main() {
    sqlite3* db = nullptr;
    sqlite3_open("test.db", &db);

    try {
        gateway::Statement st(db,
            "SELECT id, device_id, value, ts FROM device_data "
            "WHERE device_id = ? ORDER BY ts DESC;");
        st.bind(1, std::string("sensor_01"));

        while (st.step() == SQLITE_ROW) {
            std::cout << st.column_int64(0) << " | "
                      << st.column_text(1)  << " | "
                      << st.column_double(2)<< " | "
                      << st.column_int64(3) << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "db error: " << e.what() << "\n";
    }   // st 离开作用域 → 自动 finalize,哪怕中途抛异常也释放

    sqlite3_close(db);
    return 0;
}