# 多事件循环架构验收记录

验证日期：2026-10-08。代码基线：`f15cd1a`（独立管理 Reactor）。

串口 epoll、libmosquitto 网络循环、Boost.Beast/Asio HTTP 循环、管理信号 epoll 独立运行。
GatewayCore 统一处理应用业务，SQLite 读、写和管理工作各有专用执行器，另有异步日志线程，
正常稳态共 9 个线程。管理 Reactor（gateway-admin）只接收信号、交接任务与停机通知；
管理工作线程（gateway-manage）执行配置/资源重载与普通 MQTT 发布/替换。源码职责图见
[architecture.svg](architecture.svg)，完整实现问答见[总面经](master-interview/index.html)。

| 检查 | 结果 |
|---|---|
| Debug，严格告警视为错误 | 构建成功；CTest 153/153 通过（150 个单测、3 组端到端） |
| ASan + UBSan + LeakSanitizer | 全量 CTest 153/153 通过，无 sanitizer 报告 |
| Release，严格告警视为错误 | 构建成功 |
| 隔离发布目录全新 Debug 构建 | 153/153 通过，源码与工作区逐文件一致 |
| 端到端测试入口 | CTest 执行全部 3 组脚本；每组使用独立 Broker 与 PTY |
| 共享协议 | SHA256 清单通过，与本机 STM32 工程逐字节一致 |
| 面经 | 120 道理论、54 道项目题，249 处回指，62 份源码快照；链接及源码哈希检查通过 |
| 面经导出 | PDF 177/62/238 页；逐段完整性、桌面/手机布局和交互检查通过，抽样视觉检查通过；ZIP CRC 通过 |

本机使用 GCC 13.3、Boost 1.83、SQLite 3.45.1；项目要求 Boost 至少 1.74。
测试使用 Linux 主机、真实 TCP socket、Mosquitto、SQLite 和 PTY，没有连接开发板。

## 重点回归

- 新增 6 个隔离子进程单测：空闲 stop 唤醒、事件/工作线程分离、慢工作不阻塞 TERM、停机通知仅一次、回调异常通知、信号屏蔽前提及部分初始化 fd 回收（其中线程分离与慢工作为同一用例）。
- 进程级验证 9 条线程及信号屏蔽继承；signalfd 只属于管理 epoll，与串口 epoll 分开。`/proc/task/*/syscall` 可读时进一步确认两个线程等待各自 epoll，否则仅该辅助检查降级。
- 把配置文件替换为 FIFO，阻塞真实重载读取；确认串口遥测与 HTTP 继续，重复 SIGHUP 合并，SIGTERM/SIGINT 均能在放行 FIFO 前让串口主循环退出；放行后正常完成 join。

- HTTP 分片、流水线、keep-alive、HEAD、解析大小限制、超时、跨线程回复、重复/迟到回复、停机后的回复。
- 业务线程归属、512 个命令额度直到终态发布调用才释放、完成事件预留容量、查询回业务线程、队列排空。
- 串口短写与输出暂停、300 条命令排队、SR 窗口与换会话、丢包重传和迟到帧隔离。
- 慢 HTTP、串口暂停期间其他入口继续推进；并发命令、查询和遥测；活动连接下停机。
- 数据库切换、失败回退、MQTT/串口热加载、旧会话命令终止；同一 tty 的候选 fd 不提前修改 termios。
- 子进程 fd 上限压到 64，触发 HTTP accept 的 EMFILE 退避，验证已有连接和命令继续工作。

## 复现

```sh
sudo apt install build-essential cmake ninja-build libsqlite3-dev libmosquitto-dev \
    libboost-dev libboost-system-dev mosquitto mosquitto-clients
cmake --preset dev
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure --no-tests=error
cmake --preset asan
cmake --build --preset asan
ctest --test-dir build/asan --output-on-failure --no-tests=error
cmake --preset release -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build --preset release
./scripts/e2e_vserial.sh
./scripts/check_proto_sync.sh ../STM32Project/Protocol/edge_proto
```

`ctest --preset dev` 和 `ctest --preset asan` 按项目配置只运行单测；上面的 `--test-dir`
命令包含端到端测试。Broker 工具缺失时 CMake 不注册端到端测试，请先安装依赖。

这些结果证明所列主机功能和并发边界，不代表实机 UART 时序、吞吐量或延迟测量，也不是
ThreadSanitizer 的数据竞争验证。MQTT 尚无持久化 outbox；发布调用不等于 broker/云端确认。
管理 Reactor 能及时接收退出事件，不代表后台任务可被取消；同步 DNS/connect/loop_stop
仍可能延迟管理工作线程及最终停机。初始 MQTT 创建先入工作队列，管理 Reactor 启动后，
主线程仍需等待该初始化完成才进入串口循环。数据库切换以写队列入队顺序为准；
配置快照先于资源应用发布，失败后同配置的再次重载可能无差异可应用。
