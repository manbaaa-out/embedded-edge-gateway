#!/usr/bin/env bash
# ============================================================
# 在开发机上改代码,推到树莓派上编译 —— 一条命令走完。
#
#   ./scripts/sync_build.sh                 # 同步 + release 构建
#   ./scripts/sync_build.sh --test          # 顺带跑单测(需目标机装 libgtest-dev)
#   ./scripts/sync_build.sh --install       # 顺带装到 /usr/local/bin 并重启服务
#
# 目标机地址等用环境变量覆盖,与本目录其它脚本一致:
#   GW_REMOTE=pi@192.168.1.10  GW_PRESET=release  ./scripts/sync_build.sh
#
# 为什么用 rsync 而不是 git push/pull:开发中途的改动往往还没提交,
# 而恰恰是这些改动需要立刻拿到真机上编。rsync 同步工作区,不关心提交状态。
# ============================================================
set -uo pipefail

REMOTE="${GW_REMOTE:-pi@raspberrypi.local}"
REMOTE_DIR="${GW_REMOTE_DIR:-embedded-edge-gateway}"
PRESET="${GW_PRESET:-release}"

DO_TEST=0
DO_INSTALL=0
for arg in "$@"; do
  case "$arg" in
    --test)    DO_TEST=1 ;;
    --install) DO_INSTALL=1 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) printf '未知参数: %s(-h 看用法)\n' "$arg" >&2; exit 2 ;;
  esac
done

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m[*]\033[0m %s\n' "$*"; }

cd "$(dirname "$0")/.." || exit 1

# ---- 1. 连通性 ----
# 先探一下再传,免得 rsync 卡在密码提示上(免密没配好时最常见的症状)。
info "=== 1. 连接 $REMOTE ==="
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE" true 2>/dev/null; then
    red "连不上或未配免密。先执行: ssh-copy-id $REMOTE"
    exit 1
fi
green "  OK"

# ---- 2. 同步源码 ----
# --delete 让目标机跟着删掉本地已删的文件,否则改名/删文件后远端会留下幽灵源文件,
# 仍被 glob 到、编进产物,查起来极其费解。
# 但必须排除 build/ —— 远端的构建目录要留着,增量编译全靠它(首次约 25s,之后只编改动的)。
info "=== 2. 同步源码 → $REMOTE:$REMOTE_DIR ==="
rsync -az --delete --info=stats1 \
    --exclude='build*/' --exclude='.git/' --exclude='.cache/' \
    ./ "$REMOTE:$REMOTE_DIR/" 2>&1 | grep -E "^(sent|total size)" | sed 's/^/  /'

# ---- 3. 远端构建 ----
info "=== 3. 远端构建(preset: $PRESET)==="
ssh "$REMOTE" "cd '$REMOTE_DIR' && cmake --preset $PRESET >/dev/null && \
               time cmake --build --preset $PRESET" 2>&1 | tail -8 | sed 's/^/  /'
build_rc=${PIPESTATUS[0]}
if [[ $build_rc -ne 0 ]]; then red "构建失败"; exit 1; fi
green "  构建完成"

# ---- 4. 单测(可选)----
# 目标机上编不出 gtest:GitHub 直连不通,FetchContent 拉不下来。
# 所以走 GATEWAY_USE_SYSTEM_GTEST=ON,用 apt 装的那份 —— tests/CMakeLists.txt 里
# 留这个开关就是为了这个场景。
if [[ $DO_TEST -eq 1 ]]; then
    info "=== 4. 远端单测 ==="
    ssh "$REMOTE" "cd '$REMOTE_DIR' && \
        cmake -B build/pitest -G Ninja -DCMAKE_BUILD_TYPE=Debug \
              -DGATEWAY_BUILD_TESTS=ON -DGATEWAY_USE_SYSTEM_GTEST=ON >/dev/null && \
        cmake --build build/pitest >/dev/null && \
        ctest --test-dir build/pitest --output-on-failure -LE e2e" 2>&1 | tail -12 | sed 's/^/  /'
    [[ ${PIPESTATUS[0]} -ne 0 ]] && { red "单测未通过"; exit 1; }
fi

# ---- 5. 安装(可选)----
# 不用 cmake --install:它按 CMAKE_INSTALL_SYSCONFDIR 把配置装到 <prefix>/etc,
# 而 gateway.service 里写的是 /etc/gateway.conf,两者对不上。这里只换二进制,
# 配置与 unit 由首次部署时手工放好(见 README 部署一节)。
if [[ $DO_INSTALL -eq 1 ]]; then
    info "=== 5. 安装并重启服务 ==="
    ssh -t "$REMOTE" "cd '$REMOTE_DIR' && \
        sudo install -m755 build/$PRESET/gateway /usr/local/bin/gateway && \
        sudo install -m755 build/$PRESET/tools/node-sim/node-sim /usr/local/bin/node-sim 2>/dev/null; \
        systemctl is-enabled gateway >/dev/null 2>&1 && sudo systemctl restart gateway && \
            echo '  服务已重启' || echo '  服务未 enable,跳过重启'"
fi

echo
green "完成。目标机上的产物: $REMOTE_DIR/build/$PRESET/gateway"
[[ $DO_INSTALL -eq 0 ]] && echo "  装到系统里: $0 --install"
[[ $DO_TEST    -eq 0 ]] && echo "  顺带跑单测: $0 --test"
