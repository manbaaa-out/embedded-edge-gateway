#!/usr/bin/env bash
# 端到端验证:上行落库 + 上行 MQTT + 下行命令 ACK 闭环
# 前提:网关已在另一终端用 e2e_preflight.sh 生成的配置前台运行
set -uo pipefail

BROKER_HOST="${BROKER_HOST:-localhost}"
DB_PATH="${DB_PATH:-/tmp/gateway.db}"
HTTP_PORT="${HTTP_PORT:-8888}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

pass=0; total=0
assert() {  # assert "描述" 实际是否为真(0/非0)
  total=$((total+1))
  if [[ "$2" == "0" ]]; then green "  PASS [$1]"; pass=$((pass+1));
  else red "  FAIL [$1]"; fi
}

# ----------------------------------------------------------------------
info "=== A. 上行:MQTT 收 gateway/up/# (抓 8 秒) ==="
# STM32 周期上报温湿度/光照/状态。订阅 8 秒,看是否收到任意一条上行。
UP=$(timeout 8 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/up/#' -v 2>/dev/null)
if [[ -n "$UP" ]]; then
  green "  收到上行:"; echo "$UP" | sed 's/^/       /'
  assert "上行 MQTT 有数据" 0
else
  assert "上行 MQTT 有数据" 1
fi

# 具体设备校验:温度应落在合理范围(可选,软判断)
if echo "$UP" | grep -q 'gateway/up/temperature'; then
  green "  OK  含 temperature 主题"
fi
if echo "$UP" | grep -q 'gateway/up/illuminance'; then
  green "  OK  含 illuminance 主题"
fi

# ----------------------------------------------------------------------
info "=== B. 上行:SQLite 落库(等 5 秒让 worker 写入) ==="
sleep 5
if [[ -f "$DB_PATH" ]]; then
  CNT=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;" 2>/dev/null)
  green "  当前总行数: ${CNT:-?}"
  green "  最近 5 条:"
  sqlite3 -header -column "$DB_PATH" \
    "SELECT device_id,value,ts FROM device_data ORDER BY ts DESC LIMIT 5;" 2>/dev/null | sed 's/^/       /'
  # 断言:库里至少有数据
  [[ "${CNT:-0}" -gt 0 ]] && assert "SQLite 已落库" 0 || assert "SQLite 已落库" 1
else
  red "  数据库文件不存在: $DB_PATH"; assert "SQLite 已落库" 1
fi

# ----------------------------------------------------------------------
info "=== C. HTTP 接口查询 ==="
if command -v curl >/dev/null 2>&1; then
  RESP=$(curl -s "http://localhost:${HTTP_PORT}/api/data?dev=temperature&n=3")
  green "  GET /api/data?dev=temperature&n=3 ->"
  echo "       $RESP"
  echo "$RESP" | grep -q '"device_id"' && assert "HTTP API 返回 JSON 数据" 0 || assert "HTTP API 返回 JSON 数据" 1
else
  red "  WARN 无 curl,跳过 HTTP 检查"
fi

# ----------------------------------------------------------------------
info "=== D. 下行命令闭环:query_th(0x21) -> 期望 gateway/resp/<seq> ==="
# 后台先订阅回执,再发命令,等应答。STM32 的 on_frame 会现采温湿度回 0x05。
RESP_FILE=$(mktemp)
timeout 6 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/resp/#' -t 'gateway/ack/#' -v > "$RESP_FILE" 2>/dev/null &
SUB_PID=$!
sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th -m ''
info "  已发 query_th,等待应答..."
wait $SUB_PID 2>/dev/null
if grep -q 'gateway/resp/' "$RESP_FILE"; then
  green "  收到查询应答:"; sed 's/^/       /' "$RESP_FILE"
  # 应答格式: "ok,<温>,<湿>"
  grep -q 'ok,' "$RESP_FILE" && assert "下行 query_th 闭环(ACK 成功)" 0 || assert "下行 query_th 闭环(ACK 成功)" 1
else
  red "  6 秒内无应答。检查:STM32 RX(PA10)接线 / 固件是否在解析下行 / inflight 是否超时重发3次后判失败"
  assert "下行 query_th 闭环(ACK 成功)" 1
fi
rm -f "$RESP_FILE"

# ----------------------------------------------------------------------
info "=== E. 下行命令:set_period(0x22,单位=秒)-> 期望 gateway/ack/<seq> ok ==="
ACK_FILE=$(mktemp)
timeout 6 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/ack/#' -v > "$ACK_FILE" 2>/dev/null &
SUB_PID=$!
sleep 1
# 注意:这里是秒!发 10 表示 10 秒采样周期(和 makeDownlinkHandler + 固件 on_frame 0x22 一致)
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/set_period -m '10'
info "  已发 set_period=10(秒),等待 ACK..."
wait $SUB_PID 2>/dev/null
if grep -q 'gateway/ack/.*ok' "$ACK_FILE"; then
  green "  收到 ACK:"; sed 's/^/       /' "$ACK_FILE"
  assert "下行 set_period 闭环(ACK ok)" 0
else
  red "  无 ACK ok。内容:"; sed 's/^/       /' "$ACK_FILE"
  assert "下行 set_period 闭环(ACK ok)" 1
fi
rm -f "$ACK_FILE"

# ----------------------------------------------------------------------
echo
info "===== 总结 ====="
if [[ $pass -eq $total ]]; then
  green "  全部通过 ($pass/$total):上行落库 + 上行 MQTT + 下行 ACK 闭环 OK"
else
  red   "  $pass/$total 通过,有未过项,见上面 FAIL。"
  exit 1
fi