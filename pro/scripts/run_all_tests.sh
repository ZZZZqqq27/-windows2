#!/usr/bin/env bash
# 全功能集成测试：依次执行所有子测试，覆盖 MVP2~MVP6 全部能力
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

FAILED=()
PASSED=()

run_test() {
  local name="$1"
  local cmd="$2"
  echo ""
  echo "========== [${name}] =========="
  if eval "${cmd}"; then
    PASSED+=("${name}")
    echo "[OK] ${name} PASSED"
    return 0
  else
    FAILED+=("${name}")
    echo "[FAIL] ${name} failed"
    return 1
  fi
}

# 测试间短暂等待，确保端口释放
wait_ports() {
  sleep 2
}

echo "[all] build"
cmake -S . -B build 2>/dev/null || true
cmake --build build

echo "[all] run unit tests (mvp2_tests)"
if ./build/mvp2_tests 2>/dev/null; then
  PASSED+=("mvp2_tests")
  echo "[OK] mvp2_tests PASSED"
else
  FAILED+=("mvp2_tests")
  echo "[FAIL] mvp2_tests failed (may be placeholder)"
fi
wait_ports

run_test "stats_query" "bash scripts/run_stats_query_test.sh"
wait_ports

run_test "mvp2_network" "bash scripts/run_mvp2_network_demo.sh"
wait_ports

run_test "mvp4_secure" "bash scripts/run_mvp4_secure_demo.sh"
wait_ports

run_test "mvp5_replica" "bash scripts/run_mvp5_replica_demo.sh"
wait_ports

run_test "mvp4_3node" "bash scripts/run_mvp4_3node_test.sh"
wait_ports

run_test "dht_3node" "bash scripts/run_dht_3node_test.sh"
wait_ports

run_test "6node_integration" "bash scripts/run_6node_integration_test.sh"

# 汇总
echo ""
echo "=========================================="
echo "[all] SUMMARY"
echo "=========================================="
echo "PASSED (${#PASSED[@]}): ${PASSED[*]:-none}"
echo "FAILED (${#FAILED[@]}): ${FAILED[*]:-none}"
echo ""

if [[ ${#FAILED[@]} -gt 0 ]]; then
  echo "[all] Some tests failed. Exit 1."
  exit 1
fi

echo "[all] All tests PASSED."
exit 0
