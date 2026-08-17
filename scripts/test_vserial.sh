#!/bin/bash
# 虚拟串口冒烟测试：先在 ttyV0 建立限时读取，再从 ttyV1 写入帧头字节，避免发送端
# 在接收 PTY 尚未打开时丢数据。输出保留为十六进制，供人直接确认 aa 55。
set -e

PORT_RX=/tmp/ttyV0     # 本次验证的接收端。
PORT_TX=/tmp/ttyV1     # 本次验证的发送端。

# 符号链接只在 start_vserial.sh 运行期间存在。
if [[ ! -e "$PORT_RX" || ! -e "$PORT_TX" ]]; then
    echo "错误:找不到 $PORT_RX 或 $PORT_TX"
    echo "请先在另一个终端运行 ./scripts/start_vserial.sh 并保持它挂着。"
    exit 1
fi

# 先启动限时接收，再发送，避免 PTY 尚未打开时丢失测试字节。
TMP=$(mktemp)  # 接收端原始字节的临时文件。
timeout 2 cat "$PORT_RX" > "$TMP" &
READER_PID=$!  # 后台 cat 的 PID，用于等待两秒超时完成。

# WSL 中进程和 PTY 就绪可能稍慢。
sleep 1

printf '\xAA\x55' > "$PORT_TX"

wait "$READER_PID" 2>/dev/null || true

# 十六进制输出避免终端解释不可打印字节。
echo "收到字节(十六进制):"
od -An -tx1 "$TMP"
rm -f "$TMP"
