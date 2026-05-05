#!/usr/bin/env bash
set -euo pipefail

# 三节点 FIND_CHUNK 演示脚本（A 查询 -> 通过 B/C 下载）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[mvp3] build"
cmake -S . -B build
cmake --build build

echo "[mvp3] prepare input file"
mkdir -p data/mvp3
printf "mvp3 find_chunk demo file\nline2\nline3\n" > data/mvp3/input.txt

echo "[mvp3] prepare configs"
mkdir -p data/mvp3/configs

cat > data/mvp3/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9001
seed_nodes: "127.0.0.1:9002,127.0.0.1:9003"
self_addr: "127.0.0.1:9001"
chunks_dir: "data/mvp3/node_a/chunks"
chunk_index_file: "data/mvp3/node_a/chunk_index.tsv"
log_file: "logs/mvp3_node_a.log"
EOF

cat > data/mvp3/configs/node_b.yaml <<'EOF'
node_id: "node-b"
listen_port: 9002
seed_nodes: "127.0.0.1:9001,127.0.0.1:9003"
self_addr: "127.0.0.1:9002"
chunks_dir: "data/mvp3/node_b/chunks"
chunk_index_file: "data/mvp3/node_b/chunk_index.tsv"
log_file: "logs/mvp3_node_b.log"
EOF

cat > data/mvp3/configs/node_c.yaml <<'EOF'
node_id: "node-c"
listen_port: 9003
seed_nodes: "127.0.0.1:9001,127.0.0.1:9002"
self_addr: "127.0.0.1:9003"
chunks_dir: "data/mvp3/node_c/chunks"
chunk_index_file: "data/mvp3/node_c/chunk_index.tsv"
log_file: "logs/mvp3_node_c.log"
EOF

echo "[mvp3] generate chunks on B and C"
./build/app --config data/mvp3/configs/node_b.yaml --input data/mvp3/input.txt
./build/app --config data/mvp3/configs/node_c.yaml --input data/mvp3/input.txt

CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp3/node_b/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[mvp3] failed to read chunk_id"
  exit 1
fi
echo "[mvp3] chunk_id=${CHUNK_ID}"

echo "[mvp3] start servers B and C"
./build/app --config data/mvp3/configs/node_b.yaml --mode server > logs/mvp3_node_b_server.log 2>&1 &
PID_B=$!
./build/app --config data/mvp3/configs/node_c.yaml --mode server > logs/mvp3_node_c_server.log 2>&1 &
PID_C=$!
sleep 1

echo "[mvp3] node A query and download"
./build/app --config data/mvp3/configs/node_a.yaml --mode client --peer 127.0.0.1:9002 --chunk_id "${CHUNK_ID}"

echo "[mvp3] stop servers"
if kill -0 "${PID_B}" 2>/dev/null; then
  kill "${PID_B}" || true
  wait "${PID_B}" 2>/dev/null || true
fi
if kill -0 "${PID_C}" 2>/dev/null; then
  kill "${PID_C}" || true
  wait "${PID_C}" 2>/dev/null || true
fi

echo "[mvp3] done"
