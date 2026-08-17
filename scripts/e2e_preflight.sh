#!/usr/bin/env bash
# 实体 STM32 联调预检。依次验证串口设备与权限、命令行依赖、Broker 连通性和原始帧头，
# 再从部署模板生成一次性配置；不启动网关，以便调用者在前台观察完整日志。
set -uo pipefail

# 所有联调路径均可由同名环境变量覆盖，默认值适合本机 USB 转串口测试。
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"       # 实体节点连接到主机的串口设备。
SERIAL_BAUD="${SERIAL_BAUD:-115200}"           # 抓取原始字节时使用的串口波特率。
BROKER_HOST="${BROKER_HOST:-localhost}"         # 网关与验证客户端共同连接的 Broker。
DB_PATH="${DB_PATH:-/tmp/gateway.db}"           # 联调网关写入的临时 SQLite 文件。
CONF_OUT="${CONF_OUT:-/tmp/gateway.e2e.conf}"   # 根据部署模板生成的联调配置。
SRC_CONF="${SRC_CONF:-deploy/gateway.conf}"     # 不直接修改的源配置模板。

# 三个输出函数接收任意文本参数，分别标记失败、成功和阶段信息。
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

fail=0  # 任一硬性预检失败后置 1，末尾统一决定退出状态。
check() {  # 第一个参数是描述，其余参数作为不经 shell 重解析的命令执行。
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then green "  OK  $desc"; else red "  FAIL $desc"; fail=1; fi
}

info "=== 1. 串口设备 ==="
if [[ -e "$SERIAL_DEV" ]]; then
  green "  OK  设备存在: $SERIAL_DEV"
  ls -l "$SERIAL_DEV"
else
  red "  FAIL 找不到 $SERIAL_DEV —— USB-TTL 没插好,或设备名不对(试 ls /dev/ttyUSB* /dev/ttyACM*)"
  fail=1
fi

info "=== 2. 读写权限 ==="
if [[ -r "$SERIAL_DEV" && -w "$SERIAL_DEV" ]]; then
  green "  OK  可读可写"
else
  red "  FAIL 无权限。执行: sudo usermod -aG dialout \$USER 后重新登录,或临时 sudo chmod 666 $SERIAL_DEV"
  fail=1
fi

info "=== 3. 必备工具 ==="
check "mosquitto_sub 存在" command -v mosquitto_sub
check "mosquitto_pub 存在" command -v mosquitto_pub
check "sqlite3 存在"       command -v sqlite3
check "网关已编译"          test -x ./build/dev/gateway

info "=== 4. MQTT broker ==="
if mosquitto_sub -h "$BROKER_HOST" -t '$SYS/#' -C 1 -W 2 >/dev/null 2>&1; then
  green "  OK  broker 可连 ($BROKER_HOST:1883)"
else
  red "  FAIL broker 连不上。启动: sudo systemctl start mosquitto"
  fail=1
fi

info "=== 5. 串口是否真有数据(抓 3 秒原始字节) ==="
# 按目标波特率切到 raw 模式后抓取帧头，避免 canonical 模式等待换行而误报无数据。
if command -v timeout >/dev/null 2>&1; then
  # RAW 保存最多 200 个字符的十六进制采样，既用于帧头判定也用于失败诊断。
  RAW=$( exec 3<>"$SERIAL_DEV"
         stty -F "$SERIAL_DEV" "$SERIAL_BAUD" raw -echo -crtscts 2>/dev/null
         timeout 3 cat <&3 2>/dev/null | od -An -tx1 | tr -s ' ' | head -c 200 )
  if echo "$RAW" | grep -qi 'aa 55'; then
    green "  OK  抓到含帧头 AA 55 的数据:"
    echo "       $RAW"
  else
    red   "  WARN 3 秒内没抓到 AA55 帧头。可能:STM32 没运行/TX-RX 接反/波特率不符。"
    red   "       抓到的原始字节(可能为空): $RAW"
    # 无帧头可能只是采样间隔较长，因此保留为警告。
  fi
else
  red "  WARN 没有 timeout 命令,跳过抓包"
fi

info "=== 6. 生成联调配置 $CONF_OUT ==="
if [[ -f "$SRC_CONF" ]]; then
  # 使用非斜杠分隔符，以便设备和数据库路径直接参与替换。
  sed -e "s|^serial_path =.*|serial_path = $SERIAL_DEV|" \
      -e "s|^serial_baud =.*|serial_baud = $SERIAL_BAUD|" \
      -e "s|^db_path     =.*|db_path     = $DB_PATH|" \
      -e "s|^mqtt_host   =.*|mqtt_host   = $BROKER_HOST|" \
      "$SRC_CONF" > "$CONF_OUT"
  green "  OK  已生成。关键项:"
  grep -E '^(serial_path|serial_baud|db_path|mqtt_host)' "$CONF_OUT" | sed 's/^/       /'
else
  red "  FAIL 找不到模板 $SRC_CONF"; fail=1
fi

echo
if [[ $fail -eq 0 ]]; then
  green "预检通过。下一步在【单独终端】前台启动网关:"
  echo  "    ./build/dev/gateway $CONF_OUT"
  echo  "然后回到本终端跑: ./scripts/e2e_verify.sh"
else
  red "预检有 FAIL 项,先修复再继续。"
  exit 1
fi
