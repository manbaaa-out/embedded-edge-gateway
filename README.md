# embedded-edge-gateway

C++17 多协议嵌入式 Linux 边缘网关,目标运行环境:Raspberry Pi 4B。

## 模块状态

| 模块 | 路径 | 状态 |
|---|---|---|
| M3 日志 | `src/log/` | ✅ Logger 同步外壳 + AsyncLogger 双缓冲后端 / Meyers 单例 / 后台 flush 线程(3 秒间隔)/ 落盘 `/tmp/gateway.log` |
| M4 串口 | `src/serial/` | ✅ termios RAII 类 / Rule of Five / 8N1 / VMIN=1 VTIME=0 |
| M5 协议 | `src/protocol/` | ✅ 8 状态 FSM + CRC16-MODBUS,详见 [docs/m5_frame_protocol.md](docs/m5_frame_protocol.md) |
| M6 队列 | `src/concurrent/ThreadSafeQueue.h` | ✅ 模板类 / mutex + 双 cv / 有界容量 / shutdown 排空语义 |
| M8 线程池 | `src/concurrent/ThreadPool.h` | ✅ 基于 M6 队列 / `submit()` 返回 `std::future` / RAII join 工作线程 |
| M14 构建 | `CMakeLists.txt` | ✅ CMake 3.15+ / C++17 / Modern CMake(INTERFACE 库收纳选项) |

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Phase 1 Demo(M4 串口 + M5 协议解析端到端)

需要 3 个终端,演示 STM32 节点(模拟)→ 树莓派网关的完整数据链路。

**终端 1 — 启动虚拟串口对**:
```bash
./scripts/start_vserial.sh
```
保持运行不要关。会创建 `/tmp/ttyV0`(网关侧)和 `/tmp/ttyV1`(STM32 侧)。

**终端 2 — 启动网关**:
```bash
./build/gateway /tmp/ttyV0
```
启动后阻塞等待帧到来。

**终端 3 — 启动假 STM32**:

先编译 experiments 下的 fake_stm32:
```bash
cd experiments/m5_parser
g++ -std=c++17 -Wall CRC16.cpp fake_stm32.cpp -o fake_stm32
./fake_stm32 /tmp/ttyV1
```

**预期网关输出**(每秒一帧,循环 4 类业务帧):
```
[FRAME] type=0x03 payload[0]=             (心跳)
[FRAME] type=0x02 payload[2]=01 90        (光照 = 400 lux)
[FRAME] type=0x01 payload[5]=00 FD 02 5D 00  (温湿度 = 25.3°C 60.5%)
[FRAME] type=0x04 payload[1]=01           (设备状态)
...
```

详细协议格式参见 [docs/m5_frame_protocol.md](docs/m5_frame_protocol.md)。

## License

(待定)
