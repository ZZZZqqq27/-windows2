#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=================================="
echo "停止所有节点 (Mac + WSL)"
echo "=================================="

echo ""
echo "[1/2] 停止 Mac 侧节点..."
"${SCRIPT_DIR}/stop_mac.sh"

echo ""
echo "[2/2] 停止 WSL 侧节点..."
"${SCRIPT_DIR}/stop_wsl.sh"

echo ""
echo "=================================="
echo "✅ 所有节点已停止"
echo "=================================="
