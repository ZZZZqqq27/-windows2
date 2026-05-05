#!/usr/bin/env bash
set -euo pipefail

# 进入项目根目录（脚本所在目录的上一级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[demo] build"
cmake -S . -B build
cmake --build build

echo "[demo] prepare input file"
mkdir -p data/mvp5_replica
printf "mvp5 replica demo file\nline2\nline3\n" > data/mvp5_replica/input.txt

echo "[demo] prepare configs (3 nodes)"
mkdir -p data/mvp5_replica/configs
cat > data/mvp5_replica/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9501
seed_nodes: "127.0.0.1:9502,127.0.0.1:9503"
self_addr: "127.0.0.1:9501"
chunks_dir: "data/mvp5_replica/node_a/chunks"
chunk_index_file: "data/mvp5_replica/node_a/chunk_index.tsv"
log_file: "logs/mvp5_node_a.log"
EOF

cat > data/mvp5_replica/configs/node_b.yaml <<'EOF'
node_id: "node-b"
listen_port: 9502
seed_nodes: "127.0.0.1:9501,127.0.0.1:9503"
self_addr: "127.0.0.1:9502"
chunks_dir: "data/mvp5_replica/node_b/chunks"
chunk_index_file: "data/mvp5_replica/node_b/chunk_index.tsv"
log_file: "logs/mvp5_node_b.log"
EOF

cat > data/mvp5_replica/configs/node_c.yaml <<'EOF'
node_id: "node-c"
listen_port: 9503
seed_nodes: "127.0.0.1:9501,127.0.0.1:9502"
self_addr: "127.0.0.1:9503"
chunks_dir: "data/mvp5_replica/node_c/chunks"
chunk_index_file: "data/mvp5_replica/node_c/chunk_index.tsv"
log_file: "logs/mvp5_node_c.log"
EOF

echo "[demo] start node-b/node-c servers"
./build/app --config data/mvp5_replica/configs/node_b.yaml --mode server > logs/mvp5_node_b_server.log 2>&1 &
PID_B=$!
./build/app --config data/mvp5_replica/configs/node_c.yaml --mode server > logs/mvp5_node_c_server.log 2>&1 &
PID_C=$!
sleep 1

cleanup() {
  if kill -0 "${PID_B}" 2>/dev/null; then
    kill "${PID_B}" || true
    wait "${PID_B}" 2>/dev/null || true
  fi
  if kill -0 "${PID_C}" 2>/dev/null; then
    kill "${PID_C}" || true
    wait "${PID_C}" 2>/dev/null || true
  fi
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
  echo "[demo] server not ready: ${log_file}"
  return 1
}

wait_for_server logs/mvp5_node_b_server.log
wait_for_server logs/mvp5_node_c_server.log

echo "[demo] node-a upload + replica to 2 peers"
./build/app --config data/mvp5_replica/configs/node_a.yaml --upload data/mvp5_replica/input.txt --replica 2

CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp5_replica/node_a/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[demo] failed to read chunk_id from node-a index"
  exit 1
fi
echo "[demo] chunk_id=${CHUNK_ID}"

echo "[demo] verify replicas stored on node-b/node-c"
awk -v id="${CHUNK_ID}" '$1==id {found=1} END {exit !found}' data/mvp5_replica/node_b/chunk_index.tsv
awk -v id="${CHUNK_ID}" '$1==id {found=1} END {exit !found}' data/mvp5_replica/node_c/chunk_index.tsv

echo "[demo] verify source field"
awk -v id="${CHUNK_ID}" 'BEGIN{FS="\t"} $1==id && $5=="upload" {found=1} END {exit !found}' \
  data/mvp5_replica/node_a/chunk_index.tsv
awk -v id="${CHUNK_ID}" 'BEGIN{FS="\t"} $1==id && $5=="replica" {found=1} END {exit !found}' \
  data/mvp5_replica/node_b/chunk_index.tsv

echo "[demo] verify FIND_VALUE returns owners"
./build/app --config data/mvp5_replica/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9502 --chunk_id "${CHUNK_ID}" --strategy round_robin \
  > logs/mvp5_find_client.log 2>&1
awk '/mvp5: strategy=round_robin/ {found=1} END {exit !found}' logs/mvp5_find_client.log
awk -v a="127.0.0.1:9501" -v b="127.0.0.1:9502" '
  /tcp server replied: (FIND_VALUE_RESP|FIND_RESP)/ && $0 ~ a && $0 ~ b {found=1} END {exit !found}' \
  logs/mvp5_node_b_server.log logs/mvp5_node_c_server.log

echo "[demo] done"
