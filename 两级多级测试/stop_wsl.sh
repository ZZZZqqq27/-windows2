#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRO_DIR="${ROOT_DIR}/pro"

echo "=================================="
echo "停止 WSL 侧节点"
echo "=================================="

ssh "${WSL_IP:-192.168.1.3}" "pkill -9 -f 'build/app' 2>/dev/null || true; rm -f '${SCRIPT_DIR}/config/'*.yaml 2>/dev/null || true" 2>/dev/null || {
  echo "[WARNING] 无法连接到 WSL 机器，请手动在 WSL 侧执行: pkill -9 -f 'build/app'; rm -f '${SCRIPT_DIR}/config/'*.yaml"
}

echo "✅ WSL 侧节点已停止"
