#!/usr/bin/env bash
# 独立全量功能端到端测试（不调用其他测试脚本）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

DATA_DIR="data/full_e2e"
CFG_DIR="${DATA_DIR}/configs"
LOG_DIR="logs"
PORT_BASE=9860   # 实际端口: 9861~9866
HTTP_PORT=18888

mkdir -p "${DATA_DIR}" "${CFG_DIR}" "${LOG_DIR}"

PIDS=()
HTTP_PID=""

cleanup() {
  [[ -n "${HTTP_PID}" ]] && kill "${HTTP_PID}" 2>/dev/null || true
  wait "${HTTP_PID}" 2>/dev/null || true
  for pid in "${PIDS[@]}"; do
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT

wait_for_log() {
  local file="$1"
  local pattern="$2"
  local max_wait="${3:-30}"
  local waited=0
  while [[ "${waited}" -lt "${max_wait}" ]]; do
    if awk -v p="${pattern}" '$0 ~ p {found=1} END {exit !found}' "${file}" 2>/dev/null; then
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  echo "[full] timeout waiting log pattern '${pattern}' in ${file}"
  return 1
}

echo "[full] build"
cmake -S . -B build >/dev/null
cmake --build build >/dev/null

echo "[full] run unit test binary"
./build/mvp2_tests >/dev/null

echo "[full] prepare input"
printf "full e2e test file\nline2\nline3\nround robin + secure + manifest\n" > "${DATA_DIR}/input.txt"

echo "[full] prepare 6 node configs"
for i in 1 2 3 4 5 6; do
  port=$((PORT_BASE + i))
  others=""
  for j in 1 2 3 4 5 6; do
    [[ "${j}" == "${i}" ]] && continue
    others="${others}127.0.0.1:$((PORT_BASE + j)),"
  done
  others="${others%,}"

  cat > "${CFG_DIR}/node_${i}.yaml" <<EOF
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
http_port: ${HTTP_PORT}
log_file: "${LOG_DIR}/full_node_${i}.log"
EOF
  mkdir -p "${DATA_DIR}/node_${i}/chunks"
done

echo "[full] start node-2..node-6 server"
for i in 2 3 4 5 6; do
  ./build/app --config "${CFG_DIR}/node_${i}.yaml" --mode server \
    > "${LOG_DIR}/full_node_${i}_server.log" 2>&1 &
  PIDS+=("$!")
done

for i in 2 3 4 5 6; do
  wait_for_log "${LOG_DIR}/full_node_${i}_server.log" "tcp server waiting for client" 30
done

echo "[full] node-1 upload with replica=3 + manifest"
./build/app --config "${CFG_DIR}/node_1.yaml" \
  --upload "${DATA_DIR}/input.txt" --replica 3 \
  --manifest_out "${DATA_DIR}/manifest.txt" \
  > "${LOG_DIR}/full_upload.log" 2>&1

CHUNK_ID="$(awk 'NR==1 {print; exit}' "${DATA_DIR}/manifest.txt")"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[full] failed: empty manifest chunk id"
  exit 1
fi
echo "[full] chunk_id=${CHUNK_ID}"

echo "[full] start node-1 server"
./build/app --config "${CFG_DIR}/node_1.yaml" --mode server \
  > "${LOG_DIR}/full_node_1_server.log" 2>&1 &
PIDS+=("$!")
wait_for_log "${LOG_DIR}/full_node_1_server.log" "tcp server waiting for client" 30

echo "[full] verify replica index on node-2/3/4"
for i in 2 3 4; do
  awk -v id="${CHUNK_ID}" '$1==id {found=1} END {exit !found}' "${DATA_DIR}/node_${i}/chunk_index.tsv"
done

echo "[full] node-5 plain download by DHT"
./build/app --config "${CFG_DIR}/node_5.yaml" --mode client \
  --peer "127.0.0.1:$((PORT_BASE+2))" --chunk_id "${CHUNK_ID}" --strategy round_robin \
  > "${LOG_DIR}/full_client_5_plain.log" 2>&1
awk '/mvp5: strategy=round_robin/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_client_5_plain.log"
awk '/mvp5: download ok bytes=/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_client_5_plain.log"

echo "[full] node-5 secure download by DHT"
./build/app --config "${CFG_DIR}/node_5.yaml" --mode client \
  --peer "127.0.0.1:$((PORT_BASE+2))" --chunk_id "${CHUNK_ID}" --strategy round_robin --secure \
  > "${LOG_DIR}/full_client_5_secure.log" 2>&1
awk '/mvp4: secure mode enabled/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_client_5_secure.log"
awk '/mvp5: download ok bytes=/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_client_5_secure.log"

echo "[full] node-6 manifest full file download"
./build/app --config "${CFG_DIR}/node_6.yaml" --mode client \
  --peer "127.0.0.1:$((PORT_BASE+1))" \
  --manifest "${DATA_DIR}/manifest.txt" --out_file "${DATA_DIR}/output_6.bin" \
  --strategy round_robin \
  > "${LOG_DIR}/full_client_6_manifest.log" 2>&1
awk '/mvp5: file assembled:/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_client_6_manifest.log"
cmp "${DATA_DIR}/input.txt" "${DATA_DIR}/output_6.bin"

echo "[full] verify stats modes output"
./build/app --config "${CFG_DIR}/node_5.yaml" --mode stats --stats_type download --json --limit 10 \
  > "${LOG_DIR}/full_stats_download.json"
./build/app --config "${CFG_DIR}/node_1.yaml" --mode stats --stats_type upload_meta --json --limit 10 \
  > "${LOG_DIR}/full_stats_upload_meta.json"
./build/app --config "${CFG_DIR}/node_1.yaml" --mode stats --stats_type upload_replica --json --limit 20 \
  > "${LOG_DIR}/full_stats_upload_replica.json"
awk '/"type":"download"/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_stats_download.json"
awk '/"type":"upload_meta"/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_stats_upload_meta.json"
awk '/"type":"upload_replica"/ {ok=1} END{exit !ok}' "${LOG_DIR}/full_stats_upload_replica.json"

echo "[full] verify http mode can start"
./build/app --config "${CFG_DIR}/node_1.yaml" --mode http --http_port "${HTTP_PORT}" \
  > "${LOG_DIR}/full_http.log" 2>&1 &
HTTP_PID="$!"
wait_for_log "${LOG_DIR}/full_http.log" "http: server listening on 0.0.0.0:${HTTP_PORT}" 20
kill "${HTTP_PID}" 2>/dev/null || true
wait "${HTTP_PID}" 2>/dev/null || true
HTTP_PID=""

echo "[full] ALL FEATURES PASSED"
