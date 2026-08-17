#!/usr/bin/env bash
# 共享协议漂移守卫。第一阶段用 MANIFEST.sha256 检查本仓库内容是否未经登记修改；
# 可选第二阶段按同一清单逐文件比较另一仓库的 protocol/，确保两端解释相同字节。
#
# 调用方式：
#   ./scripts/check_proto_sync.sh                    # 只核验本仓库副本与清单
#   ./scripts/check_proto_sync.sh <对端 protocol 目录>        # 再执行逐文件跨仓库核验
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"  # 当前网关仓库根目录。
PROTO_DIR="$REPO_ROOT/protocol"                                # 本地共享协议副本。
MANIFEST="$PROTO_DIR/MANIFEST.sha256"                          # 本地副本的预期指纹清单。

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

# 提供对端目录时，以清单中的相对路径逐文件比较。
PEER_DIR="${1:-}"  # 可选的另一仓库 protocol/ 路径；为空时只检查本地自洽性。
if [[ -n "$PEER_DIR" ]]; then
    echo
    echo "== 跨仓库比对:$PEER_DIR =="
    if [[ ! -d "$PEER_DIR" ]]; then
        echo "✗ 目录不存在:$PEER_DIR" >&2
        exit 1
    fi

    drift=0  # 任一文件缺失或内容不同后置 1，完成全部比较再统一失败。
    # 清单的前导说明行不参与文件比较。
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
