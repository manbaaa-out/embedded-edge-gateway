#!/usr/bin/env bash
# 无硬件端到端入口。每组测试自行创建并回收 PTY、独立 MQTT Broker 和临时数据库，
# 不要求系统 Broker 服务，不复用固定 /tmp/ttyV* 或用户正在使用的 MQTT 主题。
# 调用：./scripts/e2e_vserial.sh
#       BUILD_DIR=build/asan ./scripts/e2e_vserial.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/dev}"
GATEWAY_BIN="$BUILD_DIR/gateway"
SIM_BIN="$BUILD_DIR/tools/node-sim/node-sim"

for tool in python3 mosquitto mosquitto_pub mosquitto_sub; do
    command -v "$tool" >/dev/null || {
        echo "缺少 $tool；请安装 python3 mosquitto mosquitto-clients" >&2
        exit 1
    }
done
for binary in "$GATEWAY_BIN" "$SIM_BIN"; do
    [[ -x "$binary" ]] || {
        echo "找不到 $binary；请先 cmake --build --preset dev（启用 GATEWAY_BUILD_TOOLS）" >&2
        exit 1
    }
done

python3 -B "$REPO_ROOT/tests/e2e/serial_output.py" "$GATEWAY_BIN"
python3 -B "$REPO_ROOT/tests/e2e/sr_transport.py" "$GATEWAY_BIN" "$SIM_BIN"
python3 -B "$REPO_ROOT/tests/e2e/architecture_e2e.py" "$GATEWAY_BIN"
echo '=== 全部通过：3/3 组（串口输出、SR 传输、多循环架构）==='
