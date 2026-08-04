#!/usr/bin/env bash
# 生成 protocol/MANIFEST.sha256 —— 共享协议核心的文件指纹。
#
# 改完协议后跑一次,再把 protocol/ 同步到 STM32 固件仓库。
# 两端的 check_proto_sync.sh 靠这份指纹判断「副本是否已漂移」。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT/protocol"

# 共享清单:这些文件必须两端逐字节相同。
# README/CMakeLists 不入清单 —— 它们是各自仓库的接入说明与构建描述,本就该不同。
FILES=(
    include/edge_proto/edge_proto.h
    include/edge_proto/edge_crc16.h
    include/edge_proto/edge_frame.h
    src/edge_crc16.c
    src/edge_frame.c
    vectors/crc16.csv
    vectors/frames.csv
)

{
    echo "# edge_proto 共享文件指纹 —— 由 scripts/gen_proto_manifest.sh 生成,勿手改"
    echo "# 校验方式:./scripts/check_proto_sync.sh"
    echo "# 协议版本:$(grep -oP 'EDGE_PROTO_VERSION_MAJOR \K\d+' include/edge_proto/edge_proto.h).$(grep -oP 'EDGE_PROTO_VERSION_MINOR \K\d+' include/edge_proto/edge_proto.h)"
    sha256sum "${FILES[@]}"
} > MANIFEST.sha256

echo "已更新 protocol/MANIFEST.sha256:"
sed -n '4,$p' MANIFEST.sha256
