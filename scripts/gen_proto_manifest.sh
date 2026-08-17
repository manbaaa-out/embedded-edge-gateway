#!/usr/bin/env bash
# 共享协议清单生成器。以固定顺序计算接口、实现和金标准向量的 SHA-256，并在文件头
# 写入协议主次版本；协议变更后应将整个集合和新清单同步到固件仓库。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"  # 与调用目录无关的仓库绝对路径。
cd "$REPO_ROOT/protocol"

# FILES 是相对 protocol/ 的权威同步集合；构建文件和说明文档允许两仓库不同。
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
