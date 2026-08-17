#!/usr/bin/env bash
# 使用 socat、node-sim 和网关完成可重复的无硬件端到端测试。
# 除正常上下行外，还覆盖应答丢失、重试、线路噪声和热加载。
#
# 调用方式：
#   ./scripts/e2e_vserial.sh              # 默认验证 build/dev 产物
#   BUILD_DIR=build/asan ./scripts/e2e_vserial.sh    # 改用指定构建树
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"  # 当前仓库的绝对路径。
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/dev}"                 # gateway 与 node-sim 所在构建树。
BROKER_HOST="${BROKER_HOST:-localhost}"                        # e2e 进程共同使用的 MQTT Broker。

GATEWAY_BIN="$BUILD_DIR/gateway"                    # 被测网关可执行文件。
SIM_BIN="$BUILD_DIR/tools/node-sim/node-sim"        # 模拟 STM32 节点的可执行文件。

DB_PATH=/tmp/e2e_vserial.db                          # 本轮测试独占的 SQLite 数据库。
DB_RELOAD_PATH=/tmp/e2e_vserial_reload.db            # SIGHUP 热切换后的目标数据库。
CONF=/tmp/e2e_vserial.conf                           # 从部署模板派生的临时网关配置。
# 启动输出和异步运行日志均重定向到同一文件。
GW_OUT=/tmp/e2e_vserial_gateway.out                  # 网关 stdout/stderr 合并输出文件。
GW_LOG="$GW_OUT"                                    # 业务日志断言使用的语义别名。
SIM_LOG=/tmp/e2e_vserial_sim.log                     # 当前 node-sim 实例的输出文件。
ACK_LOG=/tmp/e2e_vserial_ack.log                     # MQTT 响应主题的持续订阅结果。
TTY_GW=/tmp/ttyV0                                    # PTY 对的网关端稳定链接。
TTY_NODE=/tmp/ttyV1                                  # PTY 对的模拟节点端稳定链接。

# 输出函数统一终端颜色；check 单独维护统计。
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

pass=0; total=0  # 已通过检查数和已执行检查总数。
check() {  # 第一个参数是检查名称，第二个参数按 shell 状态码解释。
  total=$((total+1))
  if [[ "$2" == "0" ]]; then green "  PASS  $1"; pass=$((pass+1));
  else red "  FAIL  $1"; fi
}

# 轮询时间覆盖异步日志的刷新周期，避免业务已完成但日志尚未落盘的竞争。
wait_for_log() {  # 用法：wait_for_log <正则> [最多轮询次数，每次 0.1 秒]
  local pattern="$1"        # grep -E 使用的完成标志正则。
  local attempts="${2:-40}" # 最大轮询次数，默认覆盖四秒。
  local i                    # 当前轮询编号。
  for ((i=0; i<attempts; ++i)); do
    grep -qE "$pattern" "$GW_LOG" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

# PIDS 收集本脚本启动的 socat、网关、订阅者和所有模拟器，退出时统一回收。
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

# 验证工具、产物和 Broker 后再创建后台进程。
for tool in socat mosquitto_pub mosquitto_sub sqlite3 curl; do
  command -v "$tool" >/dev/null || {
    red "缺少 $tool,请先 apt install socat mosquitto-clients sqlite3 curl"
    exit 1
  }
done
[[ -x "$GATEWAY_BIN" ]] || { red "找不到 $GATEWAY_BIN,请先 cmake --build --preset dev"; exit 1; }
[[ -x "$SIM_BIN"     ]] || { red "找不到 $SIM_BIN(需 -DGATEWAY_BUILD_TOOLS=ON)"; exit 1; }
mosquitto_pub -h "$BROKER_HOST" -t e2e/ping -m x 2>/dev/null || {
  red "连不上 MQTT broker $BROKER_HOST,请先 systemctl start mosquitto"; exit 1; }

info "起虚拟串口对: $TTY_GW(网关侧) ↔ $TTY_NODE(节点侧)"
socat pty,raw,echo=0,link=$TTY_GW pty,raw,echo=0,link=$TTY_NODE >/dev/null 2>&1 &
PIDS+=($!)
for _ in $(seq 20); do [[ -e "$TTY_GW" && -e "$TTY_NODE" ]] && break; sleep 0.1; done
[[ -e "$TTY_GW" ]] || { red "虚拟串口没建起来"; exit 1; }

# 从部署模板派生一次性测试配置。
rm -f "$DB_PATH" "$DB_PATH-wal" "$DB_PATH-shm" \
      "$DB_RELOAD_PATH" "$DB_RELOAD_PATH-wal" "$DB_RELOAD_PATH-shm" \
      "$GW_OUT" "$ACK_LOG"
sed -e "s#^serial_path.*=.*#serial_path = $TTY_GW#" \
    -e "s#^db_path.*=.*#db_path = $DB_PATH#" \
    -e "s#^mqtt_host.*=.*#mqtt_host = $BROKER_HOST#" \
    "$REPO_ROOT/deploy/gateway.conf" > "$CONF"

info "起网关"
"$GATEWAY_BIN" "$CONF" >"$GW_OUT" 2>&1 &
PIDS+=($!)
sleep 1.5

mosquitto_sub -h "$BROKER_HOST" -t 'gateway/ack/#' -t 'gateway/resp/#' -v >"$ACK_LOG" 2>&1 &
PIDS+=($!)

info "=== A. 上行:采集 → 双写(SQLite + MQTT)==="
start_sim() {   # 所有位置参数原样追加到 node-sim 命令行。
  : > "$SIM_LOG"
  "$SIM_BIN" "$TTY_NODE" "$@" >"$SIM_LOG" 2>&1 &
  SIM_PID=$!; PIDS+=($SIM_PID)  # SIM_PID 始终指向当前活动模拟器。
  # 等模拟器打开 PTY 后再发送命令，避免启动窗口内的字节丢失。
  for _ in $(seq 30); do grep -q "node-sim 已启动" "$SIM_LOG" && return 0; sleep 0.1; done
  red "  node-sim 没起来"; return 1
}
# 停止当前模拟器并等待 PTY 写端释放，随后才能安全启动下一种故障模式。
stop_sim() { kill "$SIM_PID" 2>/dev/null; wait "$SIM_PID" 2>/dev/null; sleep 0.3; }

start_sim --period 1
sleep 4

ROWS=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;" 2>/dev/null || echo 0)  # 全部遥测行数。
check "遥测已落库(实际 $ROWS 条)" "$([[ "$ROWS" -gt 0 ]] && echo 0 || echo 1)"

for dev in temperature humidity illuminance status_dht11; do
  n=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data WHERE device_id='$dev';" 2>/dev/null || echo 0)  # 当前设备行数。
  check "设备 $dev 有数据" "$([[ "$n" -gt 0 ]] && echo 0 || echo 1)"
done

# 心跳只表示节点在线，不产生遥测记录。
HB=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data WHERE device_id LIKE '%heartbeat%';" 2>/dev/null || echo 0)  # 误落库心跳数。
check "心跳帧未落库" "$([[ "$HB" -eq 0 ]] && echo 0 || echo 1)"

UP=$(timeout 3 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/up/#' -v 2>/dev/null)  # 三秒窗口内捕获的 MQTT 遥测。
check "上行 MQTT 有数据" "$([[ -n "$UP" ]] && echo 0 || echo 1)"

info "=== B. 下行:命令 → 串口 → 应答 → 销账(重构前无硬件做不到)==="
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_light -m ''; sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th    -m ''; sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/set_period  -m 2 ; sleep 1.5

check "查光照收到应答"   "$(grep -qE 'gateway/resp/[0-9]+ ok,[0-9]+$'          "$ACK_LOG" && echo 0 || echo 1)"
check "查温湿度收到应答" "$(grep -qE 'gateway/resp/[0-9]+ ok,[0-9.]+,[0-9.]+'  "$ACK_LOG" && echo 0 || echo 1)"
check "设周期收到 ACK"   "$(grep -qE 'gateway/ack/[0-9]+ ok'                   "$ACK_LOG" && echo 0 || echo 1)"
check "网关侧完成配对销账" "$(grep -q 'ACK matched' "$GW_LOG" && echo 0 || echo 1)"

# 翻译失败应通过拒绝回执返回原因。
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/set_period -m 0; sleep 1
check "非法参数被拒且带原因" "$(grep -q 'gateway/ack/rejected' "$ACK_LOG" && echo 0 || echo 1)"
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/no_such_cmd -m ''; sleep 1
check "未知命令被拒"         "$(grep -qc 'gateway/ack/rejected' "$ACK_LOG" >/dev/null && echo 0 || echo 1)"

info "=== C. 故障路径:丢应答 → 同 seq 重发 → 节点幂等补发 ==="
stop_sim
start_sim --period 9 --no-ack
sleep 1

mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th -m ''
sleep 4

RETRIES=$(grep -c 'downlink RETRY' "$GW_LOG")  # 网关对当前无应答命令的累计重发日志数。
check "超时后重发了 3 次(实际 $RETRIES)" "$([[ "$RETRIES" -eq 3 ]] && echo 0 || echo 1)"

# 所有重试必须复用初始序号，节点才能将其识别为同一命令。
SEQS=$(grep 'downlink RETRY' "$GW_LOG" | grep -oE 'seq=[0-9]+' | sort -u | wc -l)  # 重发日志中的唯一 seq 数量。
check "三次重发复用同一个 seq(§6.2)" "$([[ "$SEQS" -eq 1 ]] && echo 0 || echo 1)"

# 重复序号只触发缓存应答，不重新执行命令。
IDEM=$(grep -c '重发 → 补发上次应答' "$SIM_LOG")  # 模拟节点命中重复 seq 缓存的次数。
check "节点按 seq 判重发未重复执行(实际 $IDEM 次)" "$([[ "$IDEM" -eq 3 ]] && echo 0 || echo 1)"

wait_for_log 'downlink FAILED'
check "重试耗尽后判失败" "$?"
check "失败结局回到 MQTT" "$(grep -q 'timeout' "$ACK_LOG" && echo 0 || echo 1)"

info "=== D. 抗干扰:线路噪声下 FSM 仍能 resync ==="
stop_sim
BEFORE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")  # 启用噪声前的落库基线。
start_sim --period 1 --garbage 0.8
sleep 4
AFTER=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")   # 噪声运行窗口后的落库总数。

check "80% 噪声注入下仍持续落库(新增 $((AFTER-BEFORE)) 条)" \
      "$([[ "$((AFTER-BEFORE))" -gt 0 ]] && echo 0 || echo 1)"

info "=== E. 热加载:数据库读写连接在同一切换边界迁移 ==="
# 关闭噪声实例并恢复稳定采集，使切库后的落库和 HTTP 查询断言不依赖随机帧幸存率。
stop_sim
start_sim --period 1
GW_PID=$(pgrep -f "$GATEWAY_BIN $CONF" | head -1)  # 以完整命令定位本脚本启动的网关。
if [[ -n "$GW_PID" ]]; then
  # 只修改测试配置中的 db_path；新库在 reload 前不存在，确保结果不可能来自旧数据。
  sed -i -e "s#^db_path.*=.*#db_path = $DB_RELOAD_PATH#" "$CONF"
  kill -HUP "$GW_PID"
  wait_for_log 'http reader switched to new database'
  READ_SWITCHED=$?  # 保留等待状态，避免后续 SQLite/curl 命令覆盖 `$?`。
  check "SIGHUP 后进程仍存活" "$(kill -0 "$GW_PID" 2>/dev/null && echo 0 || echo 1)"
  check "写连接完成切库" "$(grep -q 'telemetry writer switched to new database' "$GW_LOG" && echo 0 || echo 1)"
  check "HTTP 只读连接随后切库" "$READ_SWITCHED"

  # 回调发生时旧队列边界已经刷完；此后的采集只能进入新库。
  OLD_AT_SWITCH=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")
  sleep 3
  OLD_AFTER=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")
  NEW_ROWS=$(sqlite3 "$DB_RELOAD_PATH" "SELECT COUNT(*) FROM device_data;" 2>/dev/null || echo 0)
  check "切换后旧库停止增长" "$([[ "$OLD_AFTER" -eq "$OLD_AT_SWITCH" ]] && echo 0 || echo 1)"
  check "切换后遥测写入新库(实际 $NEW_ROWS 条)" "$([[ "$NEW_ROWS" -gt 0 ]] && echo 0 || echo 1)"

  HTTP_DATA=$(curl -fsS 'http://127.0.0.1:8888/api/data?dev=temperature&n=10' 2>/dev/null || true)
  check "HTTP 查询读取新库数据" "$(grep -q '\"device_id\":\"temperature\"' <<<"$HTTP_DATA" && echo 0 || echo 1)"
fi

echo
if [[ "$pass" == "$total" ]]; then
  green "=== 全部通过:$pass/$total ==="
  exit 0
else
  red "=== 失败:$pass/$total 通过 ==="
  echo "网关日志: $GW_LOG  (启动输出: $GW_OUT)"
  echo "节点日志: $SIM_LOG"
  echo "MQTT 回执: $ACK_LOG"
  exit 1
fi
