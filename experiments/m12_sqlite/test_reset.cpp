#include "Statement.h"
#include <sqlite3.h>
#include <iostream>
#include <chrono>
#include <cassert>

using namespace std::chrono;
using gateway::Statement;

static void exec_or_die(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "exec failed: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        std::exit(1);
    }
}

// 用一个复用的 prepared stmt 插入 N 条;wrap_txn 决定是否包事务
static long long insert_n(sqlite3* db, int n, bool wrap_txn) {
    exec_or_die(db, "DELETE FROM device_data;");  // 每轮清空,保证可比

    auto t0 = steady_clock::now();

    if (wrap_txn) exec_or_die(db, "BEGIN TRANSACTION;");

    Statement st(db,
        "INSERT INTO device_data(device_id, value, ts) VALUES(?, ?, ?);");

    for (int i = 0; i < n; ++i) {
        st.bind(1, std::string("sensor_01"));
        st.bind(2, 20.0 + i % 10);
        st.bind(3, static_cast<long>(1780000000 + i));
        int rc = st.step();
        assert(rc == SQLITE_DONE && "INSERT step should return SQLITE_DONE");
        st.reset();                  // 关键:复用同一 stmt,换参数再跑
    }

    if (wrap_txn) exec_or_die(db, "COMMIT;");

    auto t1 = steady_clock::now();
    return duration_cast<milliseconds>(t1 - t0).count();
}

static long count_rows(sqlite3* db) {
    Statement st(db, "SELECT COUNT(*) FROM device_data;");
    long c = 0;
    if (st.step() == SQLITE_ROW) c = st.column_int64(0);
    return c;
}

int main() {
    sqlite3* db = nullptr;
    if (sqlite3_open("bench.db", &db) != SQLITE_OK) {
        std::cerr << "open failed\n"; return 1;
    }
    exec_or_die(db,
        "CREATE TABLE IF NOT EXISTS device_data("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " device_id TEXT NOT NULL, value REAL, ts INTEGER NOT NULL);");

    const int N = 10000;

    // --- 测 1:无事务(每条独立 fsync) ---
    long long ms_no = insert_n(db, N, /*wrap_txn=*/false);
    long c1 = count_rows(db);
    std::cout << "[no txn ] " << N << " inserts: " << ms_no << " ms, rows=" << c1 << "\n";
    assert(c1 == N && "row count must match after no-txn insert");

    // --- 测 2:包事务(整批一次 fsync) ---
    long long ms_txn = insert_n(db, N, /*wrap_txn=*/true);
    long c2 = count_rows(db);
    std::cout << "[in txn ] " << N << " inserts: " << ms_txn << " ms, rows=" << c2 << "\n";
    assert(c2 == N && "row count must match after txn insert");

    // --- 验证 reset 复用的数据正确性:抽查首尾两行 ---
    {
        Statement st(db,
            "SELECT value, ts FROM device_data ORDER BY ts ASC LIMIT 1;");
        assert(st.step() == SQLITE_ROW);
        long first_ts = st.column_int64(1);
        assert(first_ts == 1780000000 && "first row ts wrong -> reset/bind broken");
    }
    {
        Statement st(db,
            "SELECT ts FROM device_data ORDER BY ts DESC LIMIT 1;");
        assert(st.step() == SQLITE_ROW);
        long last_ts = st.column_int64(0);
        assert(last_ts == 1780000000 + N - 1 && "last row ts wrong");
    }

    std::cout << "all assertions passed.\n";
    if (ms_no > 0)
        std::cout << "speedup (no-txn / txn) ~ "
                  << (double)ms_no / (ms_txn ? ms_txn : 1) << "x\n";

    sqlite3_close(db);
    return 0;
}