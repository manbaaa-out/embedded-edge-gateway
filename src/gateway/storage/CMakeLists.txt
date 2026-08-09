# storage 层:SQLite 封装,含读写连接、只读连接与 WAL 配置。
# Database 与 Statement 均为 header-only 的 RAII 封装,故声明为 INTERFACE 库。
add_library(gw_storage INTERFACE)
find_package(SQLite3 REQUIRED)
# gw_log 走 INTERFACE:Database.h 里直接用 LOG_*,依赖随头文件传给每个使用者。
target_link_libraries(gw_storage INTERFACE gateway_includes SQLite::SQLite3 gw_log)
