#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRO_DIR="${ROOT_DIR}/pro"
cd "${PRO_DIR}"

# 配置（可修改）
MAC_IP="192.168.1.11"
WSL_IP="192.168.1.101"
MAC_BASE_PORT="9601"
WSL_BASE_PORT="9701"

# 用法：./start_machineA.sh [节点数:2|4|8]
NODE_COUNT=${1:-4}

if [[ ! "$NODE_COUNT" =~ ^(2|4|8)$ ]]; then
  echo "用法: $0 [2|4|8]"
  exit 1
fi

# 清理残留
pkill -9 -f "build/app" 2>/dev/null || true

OUT_DIR="${ROOT_DIR}/output/dual_host/raw"
mkdir -p "${OUT_DIR}" "${PRO_DIR}/logs"

# 创建种子列表（跨机统一：Mac节点 + WSL节点）
SEEDS="${MAC_IP}:${MAC_BASE_PORT}"
for ((i=1; i<NODE_COUNT; i++)); do
  SEEDS+=",${MAC_IP}:$((MAC_BASE_PORT+i))"
done
for ((i=0; i<NODE_COUNT; i++)); do
  SEEDS+=",${WSL_IP}:$((WSL_BASE_PORT+i))"
done
echo "种子节点: ${SEEDS}"

# 创建配置并启动节点
cleanup_pids=()
wait_server_ready() {
  local log_file="$1"
  local waited=0
  while [[ ${waited} -lt 30 ]]; do
    if [[ -f "${log_file}" ]] && grep -q 'tcp server waiting for client' "${log_file}"; then
      return 0
    fi
    sleep 1
    waited=$((waited +1))
  done
  echo "[ERROR] 服务器未就绪: ${log_file}"
  return 1
}

for ((i=0; i<NODE_COUNT; i++)); do
  node_id="A$((i+1))"
  port=$((MAC_BASE_PORT+i))
  cfg_file="${SCRIPT_DIR}/config/nodeA${i}.yaml"
  mkdir -p "$(dirname ${cfg_file})"

  cat > "${cfg_file}" << YAML
node_id: "${node_id}"
listen_port: ${port}
seed_nodes: "${SEEDS}"
self_addr: "${MAC_IP}:${port}"
chunks_dir: "${PRO_DIR}/chunks_A${i}"
chunk_index_file: "${PRO_DIR}/chunk_index_A${i}.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/dual_host_A${i}.log"
YAML

  log_file="${OUT_DIR}/server_A${i}.log"
  "${PRO_DIR}/build/app" --config "${cfg_file}" --mode server > "${log_file}" 2>&1 &
  pid=$!
  cleanup_pids+=("${pid}")
  echo "启动节点 ${node_id} (PID:${pid})"
done

# 等待所有服务器就绪
echo "等待服务器就绪..."
for ((i=0; i<NODE_COUNT; i++)); do
  log_file="${OUT_DIR}/server_A${i}.log"
  wait_server_ready "${log_file}"
  echo "✅ A${i} 就绪"
done

echo ""
echo "=================================="
echo "机器A节点已启动完毕！"
echo "节点数: ${NODE_COUNT}"
echo "端口范围: ${MAC_BASE_PORT} - $((MAC_BASE_PORT+NODE_COUNT-1))"
echo "=================================="
echo ""
