#pragma once
#include "gateway/storage/Database.h"
 
namespace gateway {
 
// 在调用线程中阻塞运行 HTTP 监控服务:epoll Reactor + timerfd 空闲连接回收 + HTTP 解析。
// roDb 须为只读连接,与主链路的写连接配合,依靠 SQLite 的 WAL 模式实现读写并发。
// 本函数内部永久阻塞,通常放在独立 std::thread 中运行。
void runHttpServer(Database& roDb);
 
}