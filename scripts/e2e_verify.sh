#!/usr/bin/env bash
# 实体节点端到端验证。按数据路径依次检查 MQTT 上行、SQLite 持久化、HTTP 查询以及
# query_th/set_period 下行闭环；运行前需用 e2e_preflight.sh 生成配置并另行启动网关。
set -uo pipefail

BROKER_HOST="${BROKER_HOST:-localhost}"  # MQTT 验证客户端连接的 Broker。
DB_PATH="${DB_PATH:-/tmp/gateway.db}"    # e2e_preflight 配置给网关的数据库。
HTTP_PORT="${HTTP_PORT:-8888}"           # 待查询的网关 HTTP 监听端口。

# 输出函数只负责颜色和统一前缀，不改变测试计数。
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

pass=0; total=0  # 已通过断言数和已执行断言总数。
assert() {  # 第一个参数是断言名称；第二个参数沿用 shell 的 0=成功约定。
  total=$((total+1))
  if [[ "$2" == "0" ]]; then green "  PASS [$1]"; pass=$((pass+1));
  else red "  FAIL [$1]"; fi
}

info "=== A. 上行:MQTT 收 gateway/up/# (抓 8 秒) ==="
# 在有限窗口内等待任意遥测主题。
UP=$(timeout 8 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/up/#' -v 2>/dev/null)  # 采样窗口内的全部上行文本。
if [[ -n "$UP" ]]; then
  green "  收到上行:"; echo "$UP" | sed 's/^/       /'
  assert "上行 MQTT 有数据" 0
else
  assert "上行 MQTT 有数据" 1
fi

# 主题检查仅补充诊断，不计入通过总数。
if echo "$UP" | grep -q 'gateway/up/temperature'; then
  green "  OK  含 temperature 主题"
fi
if echo "$UP" | grep -q 'gateway/up/illuminance'; then
  green "  OK  含 illuminance 主题"
fi

info "=== B. 上行:SQLite 落库(等 5 秒让 worker 写入) ==="
sleep 5
if [[ -f "$DB_PATH" ]]; then
  CNT=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM device_data;" 2>/dev/null)  # 当前持久化总行数。
  green "  当前总行数: ${CNT:-?}"
  green "  最近 5 条:"
  sqlite3 -header -column "$DB_PATH" \
    "SELECT device_id,value,ts FROM device_data ORDER BY ts DESC LIMIT 5;" 2>/dev/null | sed 's/^/       /'
  # 异步写线程获得刷新时间后，表中应至少存在一条记录。
  [[ "${CNT:-0}" -gt 0 ]] && assert "SQLite 已落库" 0 || assert "SQLite 已落库" 1
else
  red "  数据库文件不存在: $DB_PATH"; assert "SQLite 已落库" 1
fi

info "=== C. HTTP 接口查询 ==="
if command -v curl >/dev/null 2>&1; then
  RESP=$(curl -s "http://localhost:${HTTP_PORT}/api/data?dev=temperature&n=3")  # HTTP JSON 响应正文。
  green "  GET /api/data?dev=temperature&n=3 ->"
  echo "       $RESP"
  echo "$RESP" | grep -q '"device_id"' && assert "HTTP API 返回 JSON 数据" 0 || assert "HTTP API 返回 JSON 数据" 1
else
  red "  WARN 无 curl,跳过 HTTP 检查"
fi

info "=== D. 下行命令闭环:query_th(0x21) -> 期望 gateway/resp/<seq> ==="
# 先订阅回执再发送命令，避免快速应答早于订阅建立。
# 临时文件跨越后台订阅进程保存 query_th 的响应和拒绝回执。
RESP_FILE=$(mktemp)
timeout 6 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/resp/#' -t 'gateway/ack/#' -v > "$RESP_FILE" 2>/dev/null &
SUB_PID=$!  # 当前后台订阅者 PID，用于等待完整六秒观察窗口。
sleep 1
mosquitto_pub -h "$BROKER_HOST" -t gateway/cmd/query_th -m ''
info "  已发 query_th,等待应答..."
wait $SUB_PID 2>/dev/null
if grep -q 'gateway/resp/' "$RESP_FILE"; then
  green "  收到查询应答:"; sed 's/^/       /' "$RESP_FILE"
  # 查询成功响应应包含状态、温度和湿度。
  grep -q 'ok,' "$RESP_FILE" && assert "下行 query_th 闭环(ACK 成功)" 0 || assert "下行 query_th 闭环(ACK 成功)" 1
else
  red "  6 秒内无应答。检查:STM32 RX(PA10)接线 / 固件是否在解析下行 / inflight 是否超时重发3次后判失败"
  assert "下行 query_th 闭环(ACK 成功)" 1
fi
rm -f "$RESP_FILE"

info "=== E. 下行命令:set_period(0x22,单位=秒)-> 期望 gateway/ack/<seq> ok ==="
# set_period 单独使用新文件，避免上一命令内容造成误匹配。
ACK_FILE=$(mktemp)
timeout 6 mosquitto_sub -h "$BROKER_HOST" -t 'gateway/ack/#' -v > "$ACK_FILE" 2>/dev/null &
SUB_PID=$!  # 复用变量跟踪当前阶段订阅者。
sleep 1
# set_period 的协议单位为秒。
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

echo
info "===== 总结 ====="
if [[ $pass -eq $total ]]; then
  green "  全部通过 ($pass/$total):上行落库 + 上行 MQTT + 下行 ACK 闭环 OK"
else
  red   "  $pass/$total 通过,有未过项,见上面 FAIL。"
  exit 1
fi
