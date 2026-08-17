#!/usr/bin/env bash
# 将当前工作区同步到树莓派，并在目标机上完成构建。
#
# 默认执行 release 构建，可按需追加测试或系统安装。
#
#   ./scripts/sync_build.sh                 # 同步后执行 release 构建
#   ./scripts/sync_build.sh --test          # 使用目标机的系统 GTest 跑测试
#   ./scripts/sync_build.sh --install       # 安装或更新 systemd 服务
#
# 可通过环境变量覆盖目标机、目录、preset 和串口：
#   指定主机和串口：GW_REMOTE=pi@192.168.1.10 GW_SERIAL_DEV=/dev/serial0 ./scripts/sync_build.sh --install
#
# rsync 直接同步未提交的工作区，并保留目标机的构建缓存。
# 构建、测试和安装均在目标机执行。
set -uo pipefail

REMOTE="${GW_REMOTE:-pi@raspberrypi.local}"                  # ssh/rsync 使用的目标主机。
REMOTE_DIR="${GW_REMOTE_DIR:-embedded-edge-gateway}"         # 目标用户目录下的源码镜像路径。
PRESET="${GW_PRESET:-release}"                               # 远端配置和构建采用的 CMake preset。
SERIAL_DEV="${GW_SERIAL_DEV:-/dev/serial0}"                  # --install 时写入目标配置的串口。

[[ "$PRESET" =~ ^[A-Za-z0-9._-]+$ ]] || { printf '非法 preset: %s\n' "$PRESET" >&2; exit 2; }
[[ "$REMOTE_DIR" =~ ^[A-Za-z0-9._/-]+$ ]] || { printf '非法远端目录: %s\n' "$REMOTE_DIR" >&2; exit 2; }
[[ "$SERIAL_DEV" =~ ^/dev/[A-Za-z0-9._/+:-]+$ ]] || { printf '非法串口路径: %s\n' "$SERIAL_DEV" >&2; exit 2; }

DO_TEST=0     # 出现 --test 时置 1，额外构建并运行目标机测试。
DO_INSTALL=0  # 出现 --install 时置 1，远程调用生产安装脚本。
for arg in "$@"; do
  case "$arg" in
    --test)    DO_TEST=1 ;;
    --install) DO_INSTALL=1 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) printf '未知参数: %s(-h 看用法)\n' "$arg" >&2; exit 2 ;;
  esac
done

# 三个输出函数仅统一颜色和阶段格式。
red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

cd "$(dirname "$0")/.." || exit 1

# BatchMode 预检可在未配置免密登录时快速失败。
info "=== 1. 连接 $REMOTE ==="
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE" true 2>/dev/null; then
    red "连不上或未配免密。先执行: ssh-copy-id $REMOTE"
    exit 1
fi
green "  OK"

# --delete 保持远端源码镜像一致；排除构建目录以保留增量缓存。
info "=== 2. 同步源码 → $REMOTE:$REMOTE_DIR ==="
rsync -az --delete --info=stats1 \
    --exclude='build*/' --exclude='.git/' --exclude='.cache/' \
    ./ "$REMOTE:$REMOTE_DIR/" 2>&1 | grep -E "^(sent|total size)" | sed 's/^/  /'
sync_rc=${PIPESTATUS[0]}  # 管道首段 rsync 的状态，不能以末尾 sed 状态代替。
if [[ $sync_rc -ne 0 ]]; then red "源码同步失败"; exit 1; fi

info "=== 3. 远端构建(preset: $PRESET)==="
ssh "$REMOTE" "cd '$REMOTE_DIR' && cmake --preset $PRESET >/dev/null && \
               time cmake --build --preset $PRESET" 2>&1 | tail -8 | sed 's/^/  /'
build_rc=${PIPESTATUS[0]}  # 管道首段 ssh 的状态，保留远端构建结果。
if [[ $build_rc -ne 0 ]]; then red "构建失败"; exit 1; fi
green "  构建完成"

# 目标机测试使用系统 GTest，避免运行时再从网络获取源码。
if [[ $DO_TEST -eq 1 ]]; then
    info "=== 4. 远端单测 ==="
    ssh "$REMOTE" "cd '$REMOTE_DIR' && \
        cmake -B build/pitest -G Ninja -DCMAKE_BUILD_TYPE=Debug \
              -DGATEWAY_BUILD_TESTS=ON -DGATEWAY_USE_SYSTEM_GTEST=ON >/dev/null && \
        cmake --build build/pitest >/dev/null && \
        ctest --test-dir build/pitest --output-on-failure -LE e2e" 2>&1 | tail -12 | sed 's/^/  /'
    [[ ${PIPESTATUS[0]} -ne 0 ]] && { red "单测未通过"; exit 1; }
fi

# 安装脚本负责保留配置、准备 UART，并启动受限的 systemd 服务。
if [[ $DO_INSTALL -eq 1 ]]; then
    info "=== 5. 安装 systemd 服务(串口: $SERIAL_DEV) ==="
    if ! ssh -t "$REMOTE" "cd '$REMOTE_DIR' && \
        sudo ./scripts/install_rpi.sh --build-dir 'build/$PRESET' --serial '$SERIAL_DEV'"; then
        red "安装失败"
        exit 1
    fi
fi

echo
green "完成。目标机上的产物: $REMOTE_DIR/build/$PRESET/gateway"
if [[ $DO_INSTALL -eq 0 ]]; then echo "  装到系统里: $0 --install"; fi
if [[ $DO_TEST    -eq 0 ]]; then echo "  顺带跑单测: $0 --test"; fi
exit 0
