# edge_proto —— 上下位机共享协议核心

> 网关(C++17 / 树莓派)与 [STM32 FreeRTOS 传感节点](https://github.com/manbaaa-out/stm32-learning) 之间串口协议的**唯一权威实现**。两个仓库编译**同一份源码**、跑**同一份金标准向量**。

## 这个目录为什么存在

重构前,协议的 TYPE 与结果码是这样分布的:

| 位置 | 形态 |
|---|---|
| 网关 `src/app/GatewayApp.cpp` | 匿名 namespace 里的 `constexpr uint8_t kDHT11 = 0x01;` 与一堆裸 `0x05`/`0x20` |
| 节点 `App/cmd_service.c` | `switch (type) { case 0x20: ... }` 的裸数字 |
| 模拟器 `fake_stm32.cpp` | 自己抄的第三份 `buildFrame` + 第三份 CRC |

没有任何机制保证三份一致。结果是可预料的:**`0x22` 采样周期的单位,协议文档写 ms、网关注释写秒、节点注释写秒、README 写秒 —— 四处三种说法,谁都没发现。**

现在,协议的每一个数值只存在于 [`include/edge_proto/edge_proto.h`](include/edge_proto/edge_proto.h) 一处。两端 `#include` 同一个文件,想不一致都难。

## 内容

```
include/edge_proto/
├── edge_proto.h     契约单一真相:版本 · TYPE 字典 · 结果码 · payload 布局 · 量纲 · 时序契约
├── edge_crc16.h     CRC16-MODBUS
├── edge_frame.h     组帧 + 逐字节收帧 FSM(8 状态)
└── edge_sr.h        SR 接收窗口 8 / 顺序执行 / 会话隔离 / 结果缓存
src/
├── edge_crc16.c
├── edge_frame.c
└── edge_sr.c
vectors/
├── crc16.csv        CRC 金标准向量(docs/protocol.md §4.3)
└── frames.csv       帧编解码金标准向量(§7 全部示例 + 边界 + 错误路径)
```

## 可移植性契约

这份代码要能被 STM32 固件原样编译,所以:

- **纯 C99**,不用 GNU 扩展(`C_EXTENSIONS OFF`)
- **无 malloc、无 stdio、无平台头**,所有缓冲区由调用方提供
- **不做 I/O**:解析错误只累加到 `edge_parser_stats_t`,怎么呈现由调用方决定 —— 网关走 `LOG_WARN`,节点可以塞进 `0x04` 状态帧上报
- **不依赖父项目的构建环境**:本目录的 `CMakeLists.txt` 不引用 `gateway_options`、不 `find_package`

这些不是靠自觉。CI 里有一个 job 用 `arm-none-eabi-gcc -mcpu=cortex-m3 -ffreestanding` 编译本目录,任何一个 `#include <stdio.h>` 或 `malloc` 调用都会在那里当场暴露。实测占用:

```
   text    data     bss   filename
    560       0       0   edge_crc16.o      (含 512 字节 CRC 查表)
    412       0       0   edge_frame.o
```

上述大小是原 CRC/编解码模块的历史测量。SR 模块同样无堆分配、无 I/O，不依赖 libc；
调用方持有固定状态，通过回调注入时钟、同步执行器和帧输出。SR 接收窗口为 8，存储池为 16 槽，
结果保留最多 5 秒并在确认后提前释放。完整流程见 [SR 设计](../docs/command-dedup.md)。

协议主版本已升为 2：旧裸命令和旧 ACK 不再用于命令处理，两端必须一起更新。

## 两端如何接入

### 网关侧(本仓库)

```cmake
add_subdirectory(protocol)
target_link_libraries(<your_target> PRIVATE edge_proto)
```

### STM32 侧

固件仓库把本目录 vendored 到 `Protocol/edge_proto/`,在 `Protocol/CMakeLists.txt` 中加入目标:

```cmake
add_subdirectory(edge_proto)
target_link_libraries(protocol PUBLIC edge_proto)
```

**为什么是 vendored 副本而不是 git submodule**:submodule 会把整个网关仓库(含 web 资源、SQLite 封装、HTTP 服务)拖进固件工程,为少量协议源码付这个代价不值。副本的代价是可能漂移 —— 而漂移正好是可以机器化检测的:

```bash
./scripts/check_proto_sync.sh            # 两仓共享文件 SHA256 比对,不一致即失败
```

`MANIFEST.sha256` 记录本目录每个共享文件的指纹,两个仓库的 CI 都会跑这个脚本。副本改了没同步,构建就红。

> 若日后想更彻底,可把本目录拆成独立仓库 `edge-proto`,两端各自 submodule 引入 —— 现在的结构对此是平滑的。

## 改协议的正确姿势

1. 改 `edge_proto.h` / `edge_frame.c`(**单一真相在这里**)
2. 往 `vectors/*.csv` 补金标准向量,先让两端测试变红
3. 跑 `./scripts/gen_proto_manifest.sh` 刷新 `MANIFEST.sha256`
4. 把本目录同步到固件仓库的 `Protocol/edge_proto/`
5. 两端 CI 都绿了,才算改完
6. 不向后兼容的改动必须升主版本号(`EDGE_PROTO_VERSION_MAJOR`),并在 `docs/protocol.md` §9 记一笔

散文版协议说明见 [`docs/protocol.md`](../docs/protocol.md)。**文档与本目录的头文件有出入时,以头文件为准** —— 只有头文件会被编译器检查。
