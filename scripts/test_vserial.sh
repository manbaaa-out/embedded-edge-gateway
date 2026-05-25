#!/bin/bash
# 测试虚拟串口:先让接收端就绪,再从另一端发送,避免发送早于接收的竞争
set -e

PORT_RX=/tmp/ttyV0     # 接收端
PORT_TX=/tmp/ttyV1     # 发送端

# 先确认串口存在,不存在直接提示(说明 start_vserial.sh 没在跑)
if [[ ! -e "$PORT_RX" || ! -e "$PORT_TX" ]]; then
    echo "错误:找不到 $PORT_RX 或 $PORT_TX"
    echo "请先在另一个终端运行 ./scripts/start_vserial.sh 并保持它挂着。"
    exit 1
fi

# 后台启动接收端:安静地把收到的字节落到临时文件(最多等 2 秒)
TMP=$(mktemp)
timeout 2 cat "$PORT_RX" > "$TMP" &
READER_PID=$!

# 给接收端足够时间真正打开设备进入读取状态(WSL 下 cat/od 启动偏慢)
sleep 1

# 发送测试字节
printf '\xAA\x55' > "$PORT_TX"

# 等接收端结束(收满或 2 秒超时)
wait "$READER_PID" 2>/dev/null || true

# 统一以十六进制显示收到的内容
echo "收到字节(十六进制):"
od -An -tx1 "$TMP"
rm -f "$TMP"