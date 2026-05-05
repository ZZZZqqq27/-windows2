#!/usr/bin/env bash
# 从 pro 目录调用「老师提出的问题/验证_性能.sh」
# （单源 replica=0 vs 多副本 MULTI_REPLICA；可传环境变量与参数，见该脚本头注释）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "${ROOT}/老师提出的问题/验证_性能.sh" "$@"
