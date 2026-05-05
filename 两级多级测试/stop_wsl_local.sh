#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=================================="
echo "停止 WSL 侧节点"
echo "=================================="

pkill -9 -f "build/app" 2>/dev/null || true

rm -f "${SCRIPT_DIR}/config/"*.yaml 2>/dev/null || true

echo "✅ WSL 侧节点已停止"
