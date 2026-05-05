#!/usr/bin/env bash
set -euo pipefail

# 进入项目根目录（脚本所在目录的上一级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[demo] build"
cmake -S . -B build
cmake --build build

echo "[demo] prepare input file for MVP2 network"
mkdir -p data
printf "mvp2 network demo file\nline2\nline3\n" > data/mvp2_net_input.txt

echo "[demo] split and index (MVP2 local)"
./build/app --config configs/app.yaml --input data/mvp2_net_input.txt

CHUNK_ID="$(awk 'NR==1 {print $1}' data/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[demo] failed to read chunk_id from index"
  exit 1
fi
echo "[demo] first chunk_id=${CHUNK_ID}"

echo "[demo] start server"
./build/app --config configs/app.yaml --mode server > logs/mvp2_server.log 2>&1 &
SERVER_PID=$!
sleep 1

echo "[demo] run client request (hello + chunk)"
./build/app --config configs/app.yaml --mode client --peer 127.0.0.1:9000 --chunk_id "${CHUNK_ID}"

echo "[demo] stop server"
if kill -0 "${SERVER_PID}" 2>/dev/null; then
  kill "${SERVER_PID}" || true
  wait "${SERVER_PID}" 2>/dev/null || true
fi

echo "[demo] done"
