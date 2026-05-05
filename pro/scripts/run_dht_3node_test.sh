#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${ROOT_DIR}/logs"
DATA_DIR="${ROOT_DIR}/data/console_3n"

mkdir -p "${LOG_DIR}"

echo "[test] build"
cmake --build "${ROOT_DIR}/build" >/dev/null

echo "[test] start node-b server"
"${ROOT_DIR}/build/app" --config "${DATA_DIR}/configs/node_b.yaml" --mode server > "${LOG_DIR}/dht_node_b.log" 2>&1 &
NODE_B_PID=$!

echo "[test] start node-c server"
"${ROOT_DIR}/build/app" --config "${DATA_DIR}/configs/node_c.yaml" --mode server > "${LOG_DIR}/dht_node_c.log" 2>&1 &
NODE_C_PID=$!

sleep 1

echo "[test] upload on node-a (store DHT index)"
"${ROOT_DIR}/build/app" --config "${DATA_DIR}/configs/node_a.yaml" \
  --upload "${DATA_DIR}/input_a.txt" \
  --manifest_out "${DATA_DIR}/dht_manifest.txt" > "${LOG_DIR}/dht_upload.log" 2>&1

CHUNK_ID="$(awk 'NR==1{print; exit}' "${DATA_DIR}/dht_manifest.txt")"
echo "[test] chunk_id=${CHUNK_ID}"

echo "[test] start node-a server (serve chunks)"
"${ROOT_DIR}/build/app" --config "${DATA_DIR}/configs/node_a.yaml" --mode server > "${LOG_DIR}/dht_node_a.log" 2>&1 &
NODE_A_PID=$!

sleep 1

echo "[test] node-b download by DHT"
"${ROOT_DIR}/build/app" --config "${DATA_DIR}/configs/node_b.yaml" --mode client \
  --peer "127.0.0.1:12000" --chunk_id "${CHUNK_ID}" > "${LOG_DIR}/dht_client_b.log" 2>&1

echo "[test] done"

kill "${NODE_A_PID}" "${NODE_B_PID}" "${NODE_C_PID}" 2>/dev/null || true
