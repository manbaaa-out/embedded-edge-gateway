# Database 与 Statement 均为 header-only，INTERFACE target 只聚合其编译和链接依赖。
add_library(gw_storage INTERFACE)
find_package(SQLite3 REQUIRED)
# 消费者编译 Database.h 需要 SQLite 头和日志宏，最终链接也需要两者实现，故全部传播。
target_link_libraries(gw_storage INTERFACE gateway_includes SQLite::SQLite3 gw_log)
