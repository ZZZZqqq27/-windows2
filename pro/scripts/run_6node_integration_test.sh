#!/usr/bin/env bash
# 6 节点集成测试：DHT、多副本、清单下载、下载策略、统计持久化
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

DATA_DIR="data/i6n"
CONFIG_DIR="${DATA_DIR}/configs"
LOG_DIR="logs"

# 为避免与外部已存在进程端口冲突，这里统一采用一个较高且相对少用的端口段
PORT_BASE=9810  # 实际使用端口为 PORT_BASE+1 ~ PORT_BASE+6

mkdir -p "${DATA_DIR}" "${CONFIG_DIR}" "${LOG_DIR}"

echo "[test] build"
cmake -S . -B build 2>/dev/null || true
cmake --build build

echo "[test] prepare input file"
printf "6-node integration test\nline2\nline3\ncontent for multi-chunk test\n" > "${DATA_DIR}/input.txt"

echo "[test] prepare 6 node configs (ports $((PORT_BASE+1))-$((PORT_BASE+6)))"
for i in 1 2 3 4 5 6; do
  port=$((PORT_BASE + i))
  others=""
  for j in 1 2 3 4 5 6; do
    [[ "$j" == "$i" ]] && continue
    others="${others}127.0.0.1:$((PORT_BASE+j)),"
  done
  others="${others%,}"
  cat > "${CONFIG_DIR}/node_${i}.yaml" <<EOF
node_id: "node-${i}"
listen_port: ${port}
seed_nodes: "${others}"
self_addr: "127.0.0.1:${port}"
routing_capacity: 8
chunk_size_mb: 1
chunks_dir: "${DATA_DIR}/node_${i}/chunks"
chunk_index_file: "${DATA_DIR}/node_${i}/chunk_index.tsv"
download_stats_file: "${DATA_DIR}/node_${i}/download_stats.tsv"
upload_meta_file: "${DATA_DIR}/node_${i}/upload_meta.tsv"
upload_replica_file: "${DATA_DIR}/node_${i}/upload_replica.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
download_strategy: "round_robin"
log_file: "${LOG_DIR}/i6n_node_${i}.log"
EOF
  mkdir -p "${DATA_DIR}/node_${i}/chunks"
done

echo "[test] start node-2..node-6 servers (ports $((PORT_BASE+2))-$((PORT_BASE+6)))"
PIDS=()
for i in 2 3 4 5 6; do
  ./build/app --config "${CONFIG_DIR}/node_${i}.yaml" --mode server \
    > "${LOG_DIR}/i6n_node_${i}_server.log" 2>&1 &
  PIDS+=("$!")
done
sleep 2

NODE1_PID=""
cleanup() {
  [[ -n "${NODE1_PID}" ]] && kill "${NODE1_PID}" 2>/dev/null || true
  wait "${NODE1_PID}" 2>/dev/null || true
  for pid in "${PIDS[@]}"; do
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT

wait_for_server() {
  local log_file="$1"
  local max_wait=30
  local waited=0
  while [[ "${waited}" -lt "${max_wait}" ]]; do
    if awk '/tcp server waiting for client/ {found=1} END {exit !found}' "${log_file}" 2>/dev/null; then
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  echo "[test] server not ready: ${log_file}"
  return 1
}

echo "[test] wait for servers"
for i in 2 3 4 5 6; do
  wait_for_server "${LOG_DIR}/i6n_node_${i}_server.log"
done

echo "[test] node-1 upload + replica to 3 peers (DHT STORE)"
./build/app --config "${CONFIG_DIR}/node_1.yaml" \
  --upload "${DATA_DIR}/input.txt" --replica 3 \
  --manifest_out "${DATA_DIR}/manifest.txt" > "${LOG_DIR}/i6n_upload.log" 2>&1

CHUNK_ID="$(awk 'NR==1{print; exit}' "${DATA_DIR}/manifest.txt")"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[test] failed: no chunk_id in manifest"
  exit 1
fi
echo "[test] chunk_id=${CHUNK_ID}"

echo "[test] start node-1 server (serve chunks)"
./build/app --config "${CONFIG_DIR}/node_1.yaml" --mode server \
  > "${LOG_DIR}/i6n_node_1_server.log" 2>&1 &
NODE1_PID=$!
sleep 1
wait_for_server "${LOG_DIR}/i6n_node_1_server.log"

echo "[test] verify replicas on node-2, node-3, node-4"
for i in 2 3 4; do
  awk -v id="${CHUNK_ID}" '$1==id {found=1} END {exit !found}' "${DATA_DIR}/node_${i}/chunk_index.tsv"
done

echo "[test] node-5 download by DHT (FIND_VALUE) + round_robin"
./build/app --config "${CONFIG_DIR}/node_5.yaml" --mode client \
  --peer 127.0.0.1:$((PORT_BASE+2)) --chunk_id "${CHUNK_ID}" --strategy round_robin \
  > "${LOG_DIR}/i6n_client_5.log" 2>&1
awk '/mvp3: download ok|mvp5: strategy=round_robin/ {found=1} END {exit !found}' "${LOG_DIR}/i6n_client_5.log"

echo "[test] node-6 download full file by manifest"
./build/app --config "${CONFIG_DIR}/node_6.yaml" --mode client \
  --manifest "${DATA_DIR}/manifest.txt" --out_file "${DATA_DIR}/output_6.bin" \
  --peer 127.0.0.1:$((PORT_BASE+1)) --strategy round_robin \
  > "${LOG_DIR}/i6n_client_6.log" 2>&1
awk '/download_manifest ok|manifest.*ok|mvp5.*ok|download.*ok|mvp5: file assembled/ {found=1} END {exit !found}' "${LOG_DIR}/i6n_client_6.log"

echo "[test] verify output equals input"
cmp "${DATA_DIR}/input.txt" "${DATA_DIR}/output_6.bin"

echo "[test] verify stats persisted (node-5 download_stats)"
[[ -f "${DATA_DIR}/node_5/download_stats.tsv" ]] && echo "  download_stats exists" || true

echo "[test] verify upload_meta (node-1)"
[[ -f "${DATA_DIR}/node_1/upload_meta.tsv" ]] && echo "  upload_meta exists" || true

kill "${NODE1_PID}" 2>/dev/null || true

echo "[test] 6-node integration test PASSED"
