#!/usr/bin/env bash
# 重新渲染本目录下所有 *.mmd 为同名 *.png（改了图就跑这个）。
# 依赖：node/npm（首次会用 npx 自动拉取 mermaid-cli + Chromium，需联网）。
set -euo pipefail
cd "$(dirname "$0")"

# puppeteer 在无沙箱/WSL 环境下的参数
cat > .pptr.json <<'JSON'
{ "args": ["--no-sandbox", "--disable-setuid-sandbox", "--disable-gpu", "--disable-dev-shm-usage"] }
JSON

for m in fig*.mmd; do
  out="${m%.mmd}.png"
  echo "渲染 $m -> $out"
  npx -y @mermaid-js/mermaid-cli -i "$m" -o "$out" -p .pptr.json -b white -s 2
done
echo "完成：所有 .mmd 已渲染为 .png"
