#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=================================="
echo "清理所有测试数据 (Mac + WSL)"
echo "=================================="

read -p "确认清理两边所有测试数据？此操作不可恢复 (y/N): " confirm
if [[ "${confirm}" != "y" && "${confirm}" != "Y" ]]; then
  echo "取消清理"
  exit 0
fi

echo ""
echo "[1/2] 清理 Mac 侧数据..."
"${SCRIPT_DIR}/cleanup_mac.sh"

echo ""
echo "[2/2] 清理 WSL 侧数据..."
"${SCRIPT_DIR}/cleanup_wsl.sh"

echo ""
echo "=================================="
echo "✅ 所有测试数据已清理"
echo "=================================="
