#!/usr/bin/env bash
# 漂移守卫:校验共享协议核心与 MANIFEST.sha256 记录的指纹一致。
#
# 网关与 STM32 固件两个仓库各带一份本脚本,CI 都会跑。
# vendored 副本改了却没同步到另一端时,这里当场变红 ——
# 「两端协议一致」于是成为构建系统强制的不变量,而不是靠人记得。
#
# 用法:
#   ./scripts/check_proto_sync.sh                    # 校验本仓库副本自洽
#   ./scripts/check_proto_sync.sh <另一仓库的 protocol 目录>   # 跨仓库比对
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_DIR="$REPO_ROOT/protocol"
MANIFEST="$PROTO_DIR/MANIFEST.sha256"

if [[ ! -f "$MANIFEST" ]]; then
    echo "✗ 找不到 $MANIFEST,请先跑 ./scripts/gen_proto_manifest.sh" >&2
    exit 1
fi

echo "== 校验本仓库协议核心自洽 =="
if (cd "$PROTO_DIR" && sha256sum --quiet --check MANIFEST.sha256); then
    echo "✓ protocol/ 与 MANIFEST.sha256 一致"
else
    cat >&2 <<'EOF'

✗ 协议核心已被修改但 MANIFEST 未更新。
  改完协议的正确流程见 protocol/README.md「改协议的正确姿势」:
    1) 补金标准向量  2) 跑 scripts/gen_proto_manifest.sh  3) 同步到固件仓库
EOF
    exit 1
fi

# ---- 可选:与另一个仓库的副本逐文件比对 ----
PEER_DIR="${1:-}"
if [[ -n "$PEER_DIR" ]]; then
    echo
    echo "== 跨仓库比对:$PEER_DIR =="
    if [[ ! -d "$PEER_DIR" ]]; then
        echo "✗ 目录不存在:$PEER_DIR" >&2
        exit 1
    fi

    drift=0
    # 从 MANIFEST 里取文件清单(跳过 # 注释行),逐个 diff
    while read -r _hash file; do
        [[ "$_hash" == \#* ]] && continue
        if [[ ! -f "$PEER_DIR/$file" ]]; then
            echo "✗ 对端缺失:$file"
            drift=1
        elif ! diff -q "$PROTO_DIR/$file" "$PEER_DIR/$file" >/dev/null; then
            echo "✗ 已漂移:$file"
            diff -u "$PROTO_DIR/$file" "$PEER_DIR/$file" | head -20
            drift=1
        fi
    done < <(grep -v '^#' "$MANIFEST")

    if [[ $drift -ne 0 ]]; then
        echo >&2
        echo "✗ 两端协议核心不一致 —— 上下位机会以不同方式解释同一串字节。" >&2
        exit 1
    fi
    echo "✓ 两端协议核心逐字节一致"
fi
