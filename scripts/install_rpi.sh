#!/usr/bin/env bash
# 在树莓派目标机上完成生产安装。需要 root；可重复执行。
#
#   sudo ./scripts/install_rpi.sh
#   sudo ./scripts/install_rpi.sh --serial /dev/ttyUSB0
#   sudo ./scripts/install_rpi.sh --build-dir build/release
set -euo pipefail

BUILD_DIR="build/release"
SERIAL_DEV="/dev/serial0"
DB_PATH="/var/lib/gateway/gateway.db"

usage() {
  sed -n '2,7p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || { printf '%s\n' '--build-dir 缺少参数' >&2; exit 2; }
      BUILD_DIR="$2"; shift 2 ;;
    --serial)
      [[ $# -ge 2 ]] || { printf '%s\n' '--serial 缺少参数' >&2; exit 2; }
      SERIAL_DEV="$2"; shift 2 ;;
    --db)
      [[ $# -ge 2 ]] || { printf '%s\n' '--db 缺少参数' >&2; exit 2; }
      DB_PATH="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf '未知参数: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  printf '%s\n' '请用 sudo 运行此脚本。' >&2
  exit 1
fi
if [[ ! "$SERIAL_DEV" =~ ^/dev/[A-Za-z0-9._/+:-]+$ ]]; then
  printf '非法串口路径: %s\n' "$SERIAL_DEV" >&2
  exit 2
fi
if [[ ! "$DB_PATH" =~ ^/var/lib/gateway(/.*)?$ ]]; then
  printf '数据库必须位于 systemd 允许写入的 /var/lib/gateway 下: %s\n' "$DB_PATH" >&2
  exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GATEWAY_BIN="$BUILD_DIR/gateway"
UNIT_TEMPLATE="deploy/gateway.service.in"
CONFIG_TEMPLATE="deploy/gateway.conf"
JOURNALD_TEMPLATE="deploy/journald-persistent.conf"
for path in "$GATEWAY_BIN" "$UNIT_TEMPLATE" "$CONFIG_TEMPLATE" "$JOURNALD_TEMPLATE"; do
  [[ -e "$path" ]] || { printf '缺少 %s，请先完成 release 构建。\n' "$path" >&2; exit 1; }
done
[[ -x "$GATEWAY_BIN" ]] || { printf '%s 不可执行。\n' "$GATEWAY_BIN" >&2; exit 1; }

TMP_FILES=()
cleanup() {
  if [[ ${#TMP_FILES[@]} -gt 0 ]]; then rm -f -- "${TMP_FILES[@]}"; fi
}
trap cleanup EXIT

UNIT_TMP="$(mktemp)"
CONFIG_TMP="$(mktemp)"
TMP_FILES+=("$UNIT_TMP" "$CONFIG_TMP")

# CMake 和本脚本共用同一份 unit 模板。本脚本的生产路径固定为 /usr/local + /etc。
sed -e 's|@CMAKE_INSTALL_FULL_BINDIR@|/usr/local/bin|g' \
    -e 's|@GATEWAY_CONFIG_FILE@|/etc/gateway.conf|g' \
    "$UNIT_TEMPLATE" > "$UNIT_TMP"

# 首次安装采用仓库样例；升级时保留用户的其余配置，只规范串口与持久化数据库路径。
CONFIG_SOURCE="$CONFIG_TEMPLATE"
[[ -f /etc/gateway.conf ]] && CONFIG_SOURCE=/etc/gateway.conf
awk -v serial="$SERIAL_DEV" -v db="$DB_PATH" '
  BEGIN { serial_seen = 0; db_seen = 0 }
  /^[[:space:]]*serial_path[[:space:]]*=/ {
    print "serial_path = " serial; serial_seen = 1; next
  }
  /^[[:space:]]*db_path[[:space:]]*=/ {
    print "db_path     = " db; db_seen = 1; next
  }
  { print }
  END {
    if (!serial_seen) print "serial_path = " serial
    if (!db_seen) print "db_path     = " db
  }
' "$CONFIG_SOURCE" > "$CONFIG_TMP"

install -m 0755 "$GATEWAY_BIN" /usr/local/bin/gateway
install -m 0644 "$CONFIG_TMP" /etc/gateway.conf
install -m 0644 "$UNIT_TMP" /etc/systemd/system/gateway.service
install -d -m 0750 /var/lib/gateway

# Debian/Raspberry Pi OS 可能通过 vendor drop-in 强制 Storage=volatile。
# 更晚加载的本项目 drop-in 覆盖它，并为持久 journal 创建标准目录。
install -d -m 0755 /etc/systemd/journald.conf.d
install -m 0644 "$JOURNALD_TEMPLATE" \
  /etc/systemd/journald.conf.d/90-gateway-persistent.conf
if getent group systemd-journal >/dev/null; then
  install -d -o root -g systemd-journal -m 2755 /var/log/journal
else
  install -d -o root -g root -m 0755 /var/log/journal
fi
systemctl restart systemd-journald
journalctl --flush

NEEDS_REBOOT=0
if [[ "$SERIAL_DEV" == /dev/serial0 ]]; then
  command -v raspi-config >/dev/null 2>&1 || {
    printf '%s\n' '找不到 raspi-config，无法安全配置板载 UART。' >&2
    exit 1
  }

  # 1 表示关闭串口登录控制台，0 表示启用 UART 硬件（raspi-config 的约定）。
  [[ "$(raspi-config nonint get_serial_cons)" -ne 0 ]] || NEEDS_REBOOT=1
  [[ "$(raspi-config nonint get_serial_hw)" -eq 0 ]] || NEEDS_REBOOT=1
  raspi-config nonint do_serial_cons 1
  raspi-config nonint do_serial_hw 0

  # 当前启动周期里先停止 agetty；boot 参数的 console=serial0 要到重启后才真正消失。
  systemctl stop serial-getty@serial0.service serial-getty@ttyS0.service 2>/dev/null || true
fi

systemctl daemon-reload
systemctl enable gateway.service >/dev/null

if [[ $NEEDS_REBOOT -eq 1 ]]; then
  systemctl stop gateway.service 2>/dev/null || true
  printf '%s\n' '安装完成；已关闭串口登录控制台并启用 UART。'
  printf '%s\n' '请重启树莓派：sudo reboot。gateway.service 会在下次开机自动启动。'
  exit 0
fi

if [[ ! -e "$SERIAL_DEV" ]]; then
  printf '安装完成，但串口设备不存在，服务暂未启动: %s\n' "$SERIAL_DEV" >&2
  exit 1
fi

systemctl restart gateway.service
if ! systemctl is-active --quiet gateway.service; then
  systemctl status gateway.service --no-pager -l >&2 || true
  exit 1
fi

printf '安装完成，gateway.service 已运行（串口 %s，数据库 %s，journal 已持久化）。\n' \
       "$SERIAL_DEV" "$DB_PATH"
