# 多事件循环架构验收记录

验证日期：2026-10-04。代码基线：`8776875f54b162db4be253b32a1eb722cbc35dd3`。

串口 epoll、libmosquitto 网络循环、Boost.Beast/Asio HTTP 循环独立运行；GatewayCore
统一处理应用业务，SQLite 读、写和资源控制各有专用执行器。源码职责图见
[architecture.svg](architecture.svg)，完整实现问答见[总面经](master-interview/index.html)。

| 检查 | 结果 |
|---|---|
| Debug，严格告警视为错误 | 构建成功；CTest 147/147 通过（144 个单测、3 组端到端） |
| ASan + UBSan + LeakSanitizer | 全量 CTest 147/147 通过，无 sanitizer 报告 |
| Release，严格告警视为错误 | 构建成功；自定义前缀安装后的程序、配置、systemd 单元完整 |
| 隔离发布目录全新 Debug 构建 | 147/147 通过；与工作区源文件和测试逐文件一致 |
| `scripts/e2e_vserial.sh` | 3/3 组通过，各组启动独立 Broker 和 PTY |
| 共享协议 | SHA256 清单通过，与本机 STM32 工程逐字节一致 |
| Cortex-M3 freestanding C99 | 严格告警编译通过；无 malloc/printf/free/memcpy/memset 依赖 |
| protocol 格式 | 7 个 C/头文件通过 clang-format 检查 |
| 面经 | 120 道理论、54 道项目题，249 处回指，60 份源码快照；链接及源码哈希检查通过 |
| 面经导出 | PDF 177/62/238 页；逐段完整性、桌面/手机布局和交互检查通过，抽样视觉检查通过；ZIP CRC 通过 |

本机使用 GCC 13.3、Boost 1.83、SQLite 3.45.1；项目要求 Boost 至少 1.74。
测试使用 Linux 主机、真实 TCP socket、Mosquitto、SQLite 和 PTY，没有连接开发板。

## 重点回归

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
同步 DNS/connect/loop_stop 仍可能延迟控制线程及停机。数据库切换以写队列入队顺序为准；
配置快照先于资源应用发布，失败后同配置的再次重载可能无差异可应用。
