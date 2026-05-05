#!/usr/bin/env bash
set -euo pipefail

# 进入项目根目录（脚本所在目录的上一级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[test] build"
cmake -S . -B build
cmake --build build

echo "[test] prepare big input file (>=100MB)"
mkdir -p data/mvp4_3n_big
BIG_FILE="data/mvp4_3n_big/input_120mb.bin"
dd if=/dev/zero of="${BIG_FILE}" bs=1m count=120 status=none

echo "[test] prepare configs"
mkdir -p data/mvp4_3n_big/configs
cat > data/mvp4_3n_big/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9301
seed_nodes: "127.0.0.1:9302,127.0.0.1:9303"
self_addr: "127.0.0.1:9301"
chunks_dir: "data/mvp4_3n_big/node_a/chunks"
chunk_index_file: "data/mvp4_3n_big/node_a/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_big_node_a.log"
EOF

cat > data/mvp4_3n_big/configs/node_b.yaml <<'EOF'
node_id: "node-b"
listen_port: 9302
seed_nodes: "127.0.0.1:9301,127.0.0.1:9303"
self_addr: "127.0.0.1:9302"
chunks_dir: "data/mvp4_3n_big/node_b/chunks"
chunk_index_file: "data/mvp4_3n_big/node_b/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_big_node_b.log"
EOF

cat > data/mvp4_3n_big/configs/node_c.yaml <<'EOF'
node_id: "node-c"
listen_port: 9303
seed_nodes: "127.0.0.1:9301,127.0.0.1:9302"
self_addr: "127.0.0.1:9303"
chunks_dir: "data/mvp4_3n_big/node_c/chunks"
chunk_index_file: "data/mvp4_3n_big/node_c/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_3n_big_node_c.log"
EOF

echo "[test] split and index at node-b/node-c (big file)"
./build/app --config data/mvp4_3n_big/configs/node_b.yaml --input "${BIG_FILE}"
./build/app --config data/mvp4_3n_big/configs/node_c.yaml --input "${BIG_FILE}"

CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp4_3n_big/node_b/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[test] failed to read chunk_id from index"
  exit 1
fi
echo "[test] chunk_id=${CHUNK_ID}"

echo "[test] start node-b/node-c servers"
./build/app --config data/mvp4_3n_big/configs/node_b.yaml --mode server > logs/mvp4_3n_big_node_b_server.log 2>&1 &
PID_B=$!
./build/app --config data/mvp4_3n_big/configs/node_c.yaml --mode server > logs/mvp4_3n_big_node_c_server.log 2>&1 &
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
  local max_wait=60
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

echo "[test] wait for servers ready"
wait_for_server logs/mvp4_3n_big_node_b_server.log
wait_for_server logs/mvp4_3n_big_node_c_server.log

echo "[test] node-a plain download (find + get + verify)"
./build/app --config data/mvp4_3n_big/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9302 --chunk_id "${CHUNK_ID}" \
  > logs/mvp4_3n_big_client_plain.log 2>&1

echo "[test] node-a secure download (find + get_sec + decrypt + verify)"
./build/app --config data/mvp4_3n_big/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9302 --chunk_id "${CHUNK_ID}" --secure \
  > logs/mvp4_3n_big_client_secure.log 2>&1

echo "[test] verify logs"
awk '/mvp3: download ok/ {found=1} END {exit !found}' logs/mvp4_3n_big_client_plain.log
awk '/mvp4: secure download ok/ {found=1} END {exit !found}' logs/mvp4_3n_big_client_secure.log

echo "[test] done"
