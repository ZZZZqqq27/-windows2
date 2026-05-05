#!/usr/bin/env bash
set -euo pipefail

# 进入项目根目录（脚本所在目录的上一级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[demo] build"
cmake -S . -B build
cmake --build build

echo "[demo] prepare input file for MVP4 secure"
mkdir -p data/mvp4
printf "mvp4 secure demo file\nline2\nline3\n" > data/mvp4/input.txt

echo "[demo] prepare configs"
mkdir -p data/mvp4/configs
cat > data/mvp4/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9101
seed_nodes: "127.0.0.1:9102"
self_addr: "127.0.0.1:9101"
chunks_dir: "data/mvp4/node_a/chunks"
chunk_index_file: "data/mvp4/node_a/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_node_a.log"
EOF

cat > data/mvp4/configs/node_b.yaml <<'EOF'
node_id: "node-b"
listen_port: 9102
seed_nodes: "127.0.0.1:9101"
self_addr: "127.0.0.1:9102"
chunks_dir: "data/mvp4/node_b/chunks"
chunk_index_file: "data/mvp4/node_b/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_node_b.log"
EOF

echo "[demo] split and index at node-b"
./build/app --config data/mvp4/configs/node_b.yaml --input data/mvp4/input.txt

CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp4/node_b/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[demo] failed to read chunk_id from index"
  exit 1
fi
echo "[demo] chunk_id=${CHUNK_ID}"

echo "[demo] start node-b server"
./build/app --config data/mvp4/configs/node_b.yaml --mode server > logs/mvp4_node_b_server.log 2>&1 &
PID_B=$!
sleep 1

echo "[demo] node-a secure download"
./build/app --config data/mvp4/configs/node_a.yaml --mode client --peer 127.0.0.1:9102 --chunk_id "${CHUNK_ID}" --secure

echo "[demo] stop node-b server"
if kill -0 "${PID_B}" 2>/dev/null; then
  kill "${PID_B}" || true
  wait "${PID_B}" 2>/dev/null || true
fi

echo "[demo] done"
