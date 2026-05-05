#!/usr/bin/env bash
# 在 pro 目录下可直接：bash 验证_性能.sh
# （实际逻辑在上级目录「老师提出的问题/验证_性能.sh」）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/老师提出的问题/验证_性能.sh" "$@"
