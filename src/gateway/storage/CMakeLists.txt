# storage 层:SQLite 封装,含读写连接、只读连接与 WAL 配置。
# Database 与 Statement 均为 header-only 的 RAII 封装,故声明为 INTERFACE 库。
add_library(gw_storage INTERFACE)
find_package(SQLite3 REQUIRED)
target_link_libraries(gw_storage INTERFACE gateway_includes SQLite::SQLite3)
