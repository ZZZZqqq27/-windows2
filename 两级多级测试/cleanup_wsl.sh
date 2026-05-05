#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRO_DIR="${ROOT_DIR}/pro"

echo "=================================="
echo "清理 WSL 侧测试数据"
echo "=================================="

read -p "确认清理？此操作会删除所有测试数据和chunk文件 (y/N): " confirm
if [[ "${confirm}" != "y" && "${confirm}" != "Y" ]]; then
  echo "取消清理"
  exit 0
fi

rm -rf "${ROOT_DIR}/output/dual_host" 2>/dev/null || true
rm -rf "${ROOT_DIR}/data/dual_host" 2>/dev/null || true
rm -rf "${PRO_DIR}/chunks_A"* 2>/dev/null || true
rm -rf "${PRO_DIR}/chunks_B"* 2>/dev/null || true
rm -f "${PRO_DIR}/chunk_index_A"*.tsv 2>/dev/null || true
rm -f "${PRO_DIR}/chunk_index_B"*.tsv 2>/dev/null || true
rm -rf "${PRO_DIR}/logs" 2>/dev/null || true
rm -f "${SCRIPT_DIR}/config/"*.yaml 2>/dev/null || true

echo "✅ WSL 侧测试数据已清理"
