#!/usr/bin/env bash
set -euo pipefail

# 进入项目根目录（脚本所在目录的上一级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[test] build"
cmake -S . -B build
cmake --build build

echo "[test] prepare input file (mvp4 3-node)"
mkdir -p data/mvp4_3n
printf "mvp4 3-node demo file\nline2\nline3\n" > data/mvp4_3n/input.txt

echo "[test] prepare configs"
mkdir -p data/mvp4_3n/configs
cat > data/mvp4_3n/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9201
seed_nodes: "127.0.0.1:9202,127.0.0.1:9203"
self_addr: "127.0.0.1:9201"
chunks_dir: "data/mvp4_3n/node_a/chunks"
chunk_index_file: "data/mvp4_3n/node_a/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_node_a.log"
EOF

cat > data/mvp4_3n/configs/node_b.yaml <<'EOF'
node_id: "node-b"
listen_port: 9202
seed_nodes: "127.0.0.1:9201,127.0.0.1:9203"
self_addr: "127.0.0.1:9202"
chunks_dir: "data/mvp4_3n/node_b/chunks"
chunk_index_file: "data/mvp4_3n/node_b/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_node_b.log"
EOF

cat > data/mvp4_3n/configs/node_c.yaml <<'EOF'
node_id: "node-c"
listen_port: 9203
seed_nodes: "127.0.0.1:9201,127.0.0.1:9202"
self_addr: "127.0.0.1:9203"
chunks_dir: "data/mvp4_3n/node_c/chunks"
chunk_index_file: "data/mvp4_3n/node_c/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_node_c.log"
EOF

echo "[test] split and index at node-b/node-c"
./build/app --config data/mvp4_3n/configs/node_b.yaml --input data/mvp4_3n/input.txt
./build/app --config data/mvp4_3n/configs/node_c.yaml --input data/mvp4_3n/input.txt

CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp4_3n/node_b/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[test] failed to read chunk_id from index"
  exit 1
fi
echo "[test] chunk_id=${CHUNK_ID}"

echo "[test] start node-b/node-c servers"
./build/app --config data/mvp4_3n/configs/node_b.yaml --mode server > logs/mvp4_3n_node_b_server.log 2>&1 &
PID_B=$!
./build/app --config data/mvp4_3n/configs/node_c.yaml --mode server > logs/mvp4_3n_node_c_server.log 2>&1 &
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

echo "[test] resource check (rss < 200MB per node)"
MAX_RSS_KB=200000
RSS_B_KB="$(ps -o rss= -p "${PID_B}" 2>/dev/null | tr -d ' ' || true)"
RSS_C_KB="$(ps -o rss= -p "${PID_C}" 2>/dev/null | tr -d ' ' || true)"
if [[ -z "${RSS_B_KB}" || -z "${RSS_C_KB}" ]]; then
  echo "[test] rss check skipped (ps not permitted)"
fi
if [[ -n "${RSS_B_KB}" && "${RSS_B_KB}" -gt "${MAX_RSS_KB}" ]]; then
  echo "[test] node-b rss too high: ${RSS_B_KB} KB"
  exit 1
fi
if [[ -n "${RSS_C_KB}" && "${RSS_C_KB}" -gt "${MAX_RSS_KB}" ]]; then
  echo "[test] node-c rss too high: ${RSS_C_KB} KB"
  exit 1
fi

echo "[test] node-a plain download (find + get + verify)"
./build/app --config data/mvp4_3n/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9202 --chunk_id "${CHUNK_ID}" \
  > logs/mvp4_3n_client_plain.log 2>&1

echo "[test] node-a secure download (find + get_sec + decrypt + verify)"
./build/app --config data/mvp4_3n/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9202 --chunk_id "${CHUNK_ID}" --secure \
  > logs/mvp4_3n_client_secure.log 2>&1

echo "[test] verify logs"
awk '/mvp3: download ok/ {found=1} END {exit !found}' logs/mvp4_3n_client_plain.log
awk '/mvp4: secure download ok/ {found=1} END {exit !found}' logs/mvp4_3n_client_secure.log

echo "[test] done"
