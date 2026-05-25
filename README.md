# embedded-edge-gateway

C++17 多协议嵌入式 Linux 边缘网关,目标运行环境:Raspberry Pi 4B。

## 模块状态

| 模块 | 路径 | 状态 |
|---|---|---|
| M3 日志 | `src/log/` | 雏形(简易宏,Phase 2 升级) |
| M4 串口 | `src/serial/` | 完成(RAII + termios 8N1 + 移动语义) |

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

需要先启动虚拟串口对(开发测试用):

```bash
socat -d -d pty,raw,echo=0,link=/tmp/ttyV0 pty,raw,echo=0,link=/tmp/ttyV1
```

然后另开终端:

```bash
./build/gateway
```

## License

(待定)
