#!/usr/bin/env bash
# 端到端联调预检:实体 STM32 + /dev/ttyUSB0
# 只做检查 + 准备配置,不启动网关本体(网关在前台单独跑,方便看日志)
set -uo pipefail

SERIAL_DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
SERIAL_BAUD="${SERIAL_BAUD:-115200}"
BROKER_HOST="${BROKER_HOST:-localhost}"
DB_PATH="${DB_PATH:-/tmp/gateway.db}"
CONF_OUT="${CONF_OUT:-/tmp/gateway.e2e.conf}"
SRC_CONF="${SRC_CONF:-src/deploy/gateway.conf}"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

fail=0
check() {  # check "描述" 命令...
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
check "网关已编译"          test -x ./build/gateway

info "=== 4. MQTT broker ==="
if mosquitto_sub -h "$BROKER_HOST" -t '$SYS/#' -C 1 -W 2 >/dev/null 2>&1; then
  green "  OK  broker 可连 ($BROKER_HOST:1883)"
else
  red "  FAIL broker 连不上。启动: sudo systemctl start mosquitto"
  fail=1
fi

info "=== 5. 串口是否真有数据(抓 3 秒原始字节) ==="
# STM32 每秒发心跳帧 AA 55 01 03 ...,这里抓一下确认物理链路通
if command -v timeout >/dev/null 2>&1; then
  # 关键:先把线路设成 raw + 正确波特率再抓。否则默认 9600 + canonical(行缓冲)下,
  # STM32 的二进制帧不含换行符,会被行规整缓冲吞掉,物理链路通也读成空 → 假阴性 WARN。
  RAW=$( exec 3<>"$SERIAL_DEV"
         stty -F "$SERIAL_DEV" "$SERIAL_BAUD" raw -echo -crtscts 2>/dev/null
         timeout 3 cat <&3 2>/dev/null | od -An -tx1 | tr -s ' ' | head -c 200 )
  if echo "$RAW" | grep -qi 'aa 55'; then
    green "  OK  抓到含帧头 AA 55 的数据:"
    echo "       $RAW"
  else
    red   "  WARN 3 秒内没抓到 AA55 帧头。可能:STM32 没运行/TX-RX 接反/波特率不符。"
    red   "       抓到的原始字节(可能为空): $RAW"
    # 不直接判 fail:有时心跳周期/采样周期长,先警告
  fi
else
  red "  WARN 没有 timeout 命令,跳过抓包"
fi

info "=== 6. 生成联调配置 $CONF_OUT ==="
if [[ -f "$SRC_CONF" ]]; then
  # 用 | 作 sed 分隔符,避免路径里的 / 冲突(你 memory 里 sed 分隔符那条经验,这里同理)
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
  echo  "    ./build/gateway $CONF_OUT"
  echo  "然后回到本终端跑: ./scripts/e2e_verify.sh"
else
  red "预检有 FAIL 项,先修复再继续。"
  exit 1
fi