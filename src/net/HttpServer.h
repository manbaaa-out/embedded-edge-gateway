#pragma once
#include "Database.h"
 
namespace gateway {
 
// 在【调用线程】里阻塞运行 HTTP 监控服务:
//   epoll Reactor(M7) + timerfd 空闲超时踢出(M9) + HTTP 解析(M11)。
// 用传入的【只读】连接 roDb 查库 —— 与主链写连接配合,靠 WAL 实现读写并发。
// 本函数内部是 loop.loop() 永久阻塞,通常放在独立 std::thread 里运行。
void runHttpServer(Database& roDb);
 
}