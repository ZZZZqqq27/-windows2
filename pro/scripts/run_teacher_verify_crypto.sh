#!/usr/bin/env bash
# 从 pro 目录调用「老师提出的问题/验证_加密传输.sh」
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "${ROOT}/老师提出的问题/验证_加密传输.sh" "$@"
