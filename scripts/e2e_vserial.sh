#!/usr/bin/env bash
# ============================================================
# 无硬件端到端验证:socat 虚拟串口 + node-sim 节点模拟器 + 网关
#
# 覆盖真机联调脚本(e2e_preflight + e2e_verify)覆盖不到的东西 ——
# 【故障路径】。丢应答、丢帧、线路噪声,在真硬件上要靠拔线和运气,
# 这里是命令行参数,而且可复现(固定随机种子)。
#
# 全程自起自停,不需要人盯着开四个终端。
#
# 用法:
#   ./scripts/e2e_vserial.sh              # 用 build/dev 下的产物
#   BUILD_DIR=build/asan ./scripts/e2e_vserial.sh
# ============================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/dev}"
BROKER_HOST="${BROKER_HOST:-localhost}"

GATEWAY_BIN="$BUILD_DIR/gateway"
SIM_BIN="$BUILD_DIR/tools/node-sim/node-sim"

DB_PATH=/tmp/e2e_vserial.db
CONF=/tmp/e2e_vserial.conf
# 注意:网关的结构化日志由 AsyncLogger 写到固定路径 /tmp/gateway.log,
# 不走 stdout —— 这里两个都留着:GW_OUT 抓启动期的致命错,GW_LOG 抓运行日志。
GW_OUT=/tmp/e2e_vserial_gateway.out
GW_LOG=/tmp/gateway.log
SIM_LOG=/tmp/e2e_vserial_sim.log
ACK_LOG=/tmp/e2e_vserial_ack.log
TTY_GW=/tmp/ttyV0
TTY_NODE=/tmp/ttyV1

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

pass=0; total=0
check() {  # check "描述" <条件是否成立:0/1>
  total=$((total+1))
  if [[ "$2" == "0" ]]; then green "  PASS  $1"; pass=$((pass+1));
  else red "  FAIL  $1"; fi
}

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null; done
  wait 2>/dev/null
}
trap cleanup EXIT

# ---------- 前置检查 ----------
for tool in socat mosquitto_pub mosquitto_sub sqlite3; do
  command -v "$tool" >/dev/null || { red "缺少 $tool,请先 apt install socat mosquitto-clients sqlite3"; exit 1; }
done
[[ -x "$GATEWAY_BIN" ]] || { red "找不到 $GATEWAY_BIN,请先 cmake --build --preset dev"; exit 1; }
[[ -x "$SIM_BIN"     ]] || { red "找不到 $SIM_BIN(需 -DGATEWAY_BUILD_TOOLS=ON)"; exit 1; }
mosquitto_pub -h "$BROKER_HOST" -t e2e/ping -m x 2>/dev/null || {
  red "连不上 MQTT broker $BROKER_HOST,请先 systemctl start mosquitto"; exit 1; }

# ---------- 起虚拟串口 ----------
info "起虚拟串口对: $TTY_GW(网关侧) ↔ $TTY_NODE(节点侧)"
socat pty,raw,echo=0,link=$TTY_GW pty,raw,echo=0,link=$TTY_NODE >/dev/null 2>&1 &
PIDS+=($!)
for _ in $(seq 20); do [[ -e "$TTY_GW" && -e "$TTY_NODE" ]] && break; sleep 0.1; done
[[ -e "$TTY_GW" ]] || { red "虚拟串口没建起来"; exit 1; }

# ---------- 配置 + 起网关 ----------
rm -f "$DB_PATH" "$GW_OUT" "$ACK_LOG"
: > "$GW_LOG"      # 清空而非删除:AsyncLogger 已按路径持有 fd
sed -e "s#^serial_path.*=.*#serial_path = $TTY_GW#" \
    -e "s#^db_path.*=.*#db_path = $DB_PATH#" \
    "$REPO_ROOT/deploy/gateway.conf" > "$CONF"

info "起网关"
"$GATEWAY_BIN" "$CONF" >"$GW_OUT" 2>&1 &
PIDS+=($!)
sleep 1.5

mosquitto_sub -h "$BROKER_HOST" -t 'gateway/ack/#' -t 'gateway/resp/#' -v >"$ACK_LOG" 2>&1 &
PIDS+=($!)

# ============================================================
info "=== A. 上行:采集 → 双写(SQLite + MQTT)==="
# ============================================================
start_sim() {   # start_sim <额外参数...>
  : > "$SIM_LOG"
  "$SIM_BIN" "$TTY_NODE" "$@" >"$SIM_LOG" 2>&1 &
  SIM_PID=$!; PIDS+=($SIM_PID)
  # 等它真的起来再往下走 —— PTY 换手时若立刻发命令,对端还没 open,
  # 内核会把写入丢掉,表现为"命令莫名其妙没到"。
  for _ in $(seq 30); do grep -q "node-sim 已启动" "$SIM_LOG" && return 0; sleep 0.1; done
  red "  node-sim 没起来"; return 1
}
stop_sim() { kill "$SIM_PID" 2>/dev/null; wait "$SIM_PID" 2>/dev/null; sleep 0.3; }

start_sim --period 1
sleep 4

ROWS=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;" 2>/dev/null || echo 0)
check "遥测已落库(实际 $ROWS 条)" "$([[ "$ROWS" -gt 0 ]] && echo 0 || echo 1)"

for dev in temperature humidity illuminance status_dht11; do
  n=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data WHERE device_id='$dev';" 2>/dev/null || echo 0)
  check "设备 $dev 有数据" "$([[ "$n" -gt 0 ]] && echo 0 || echo 1)"
done

# 心跳帧不该落库 —— 它只证明节点活着,没有数值
HB=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data WHERE device_id LIKE '%heartbeat%';" 2>/dev/null || echo 0)
check "心跳帧未落库" "$([[ "$HB" -eq 0 ]] && echo 0 || echo 1)"

UP=$(timeout 3 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/up/#' -v 2>/dev/null)
check "上行 MQTT 有数据" "$([[ -n "$UP" ]] && echo 0 || echo 1)"

# ============================================================
info "=== B. 下行:命令 → 串口 → 应答 → 销账(重构前无硬件做不到)==="
# ============================================================
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_light -m ''; sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th    -m ''; sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/set_period  -m 2 ; sleep 1.5

check "查光照收到应答"   "$(grep -qE 'gateway/resp/[0-9]+ ok,[0-9]+$'          "$ACK_LOG" && echo 0 || echo 1)"
check "查温湿度收到应答" "$(grep -qE 'gateway/resp/[0-9]+ ok,[0-9.]+,[0-9.]+'  "$ACK_LOG" && echo 0 || echo 1)"
check "设周期收到 ACK"   "$(grep -qE 'gateway/ack/[0-9]+ ok'                   "$ACK_LOG" && echo 0 || echo 1)"
check "网关侧完成配对销账" "$(grep -q 'ACK matched' "$GW_LOG" && echo 0 || echo 1)"

# 非法参数应被网关当场挡下并说明原因,而不是静默丢弃
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/set_period -m 0; sleep 1
check "非法参数被拒且带原因" "$(grep -q 'gateway/ack/rejected' "$ACK_LOG" && echo 0 || echo 1)"
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/no_such_cmd -m ''; sleep 1
check "未知命令被拒"         "$(grep -qc 'gateway/ack/rejected' "$ACK_LOG" >/dev/null && echo 0 || echo 1)"

# ============================================================
info "=== C. 故障路径:丢应答 → 同 seq 重发 → 节点幂等补发 ==="
# ============================================================
stop_sim
start_sim --period 9 --no-ack
sleep 1

mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th -m ''
sleep 4

RETRIES=$(grep -c 'downlink RETRY' "$GW_LOG")
check "超时后重发了 3 次(实际 $RETRIES)" "$([[ "$RETRIES" -eq 3 ]] && echo 0 || echo 1)"

# §6.2 命脉:三次重发必须用【同一个 seq】,否则节点侧幂等判断失效
SEQS=$(grep 'downlink RETRY' "$GW_LOG" | grep -oE 'seq=[0-9]+' | sort -u | wc -l)
check "三次重发复用同一个 seq(§6.2)" "$([[ "$SEQS" -eq 1 ]] && echo 0 || echo 1)"

# 节点侧的另一半:识别出重发,只补发应答,不重复执行命令本体
IDEM=$(grep -c '重发 → 补发上次应答' "$SIM_LOG")
check "节点按 seq 判重发未重复执行(实际 $IDEM 次)" "$([[ "$IDEM" -eq 3 ]] && echo 0 || echo 1)"

check "重试耗尽后判失败" "$(grep -q 'downlink FAILED' "$GW_LOG" && echo 0 || echo 1)"
check "失败结局回到 MQTT" "$(grep -q 'timeout' "$ACK_LOG" && echo 0 || echo 1)"

# ============================================================
info "=== D. 抗干扰:线路噪声下 FSM 仍能 resync ==="
# ============================================================
stop_sim
BEFORE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")
start_sim --period 1 --garbage 0.8
sleep 4
AFTER=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")

check "80% 噪声注入下仍持续落库(新增 $((AFTER-BEFORE)) 条)" \
      "$([[ "$((AFTER-BEFORE))" -gt 0 ]] && echo 0 || echo 1)"

# ============================================================
info "=== E. 热加载:SIGHUP 后进程存活且继续工作 ==="
# ============================================================
GW_PID=$(pgrep -f "$GATEWAY_BIN $CONF" | head -1)
if [[ -n "$GW_PID" ]]; then
  kill -HUP "$GW_PID"
  sleep 1
  check "SIGHUP 后进程仍存活" "$(kill -0 "$GW_PID" 2>/dev/null && echo 0 || echo 1)"
  check "热加载走到 reload done" "$(grep -q 'reload done' "$GW_LOG" && echo 0 || echo 1)"

  BEFORE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")
  sleep 3
  AFTER=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;")
  check "热加载后采集未中断" "$([[ "$AFTER" -gt "$BEFORE" ]] && echo 0 || echo 1)"
fi

# ============================================================
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
