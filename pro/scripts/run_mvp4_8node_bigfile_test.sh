#!/usr/bin/env bash
# ============================================================
# 8 节点大文件测试脚本（MVP4：分片 + 明文/加密下载）
#
# 演示要点：
# - 生成 120MB 大文件，验证分片与索引能力
# - 8 个节点（node-a 为客户端，node-2~8 为服务端）
# - 在 node-2、node-3 上分别对同一文件做分片与索引（--input 模式）
# - node-a 通过 DHT 以明文和加密两种方式下载同一分片
# - 验证 GET_CHUNK 与 GET_CHUNK_SEC 均能正确拉取并校验
#
# 端口：9401（node-a）、9402~9408（node-2~8）
# 数据目录：data/mvp4_8n_big/
# ============================================================
set -euo pipefail

# --- 1) 进入 pro 目录 ---
# 脚本在 pro/scripts/ 下，PRO_DIR 为 pro/
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

echo "[test] build"
cmake -S . -B build
cmake --build build

# --- 2) 准备 120MB 大文件 ---
# dd：从 /dev/urandom 读取随机数据，写入目标文件，块大小 1MB，共 120 块
# 使用随机数据使每个分片内容不同，chunk_id 各异，便于区分验证
echo "[test] prepare big input file (>=100MB)"
mkdir -p data/mvp4_8n_big
BIG_FILE="data/mvp4_8n_big/input_120mb.bin"
dd if=/dev/urandom of="${BIG_FILE}" bs=1m count=120 status=none

# --- 3) 生成 8 节点配置文件 ---
# node-a：客户端角色，seed_nodes 指向 node-2~8，用于 DHT 查找
# node-2~8：服务端角色，seed_nodes 为空（本脚本中由 node-2 作为入口）
# 端口：9401（node-a）、9402~9408（node-2~8）
# aes_key_hex / hmac_key_hex：加密下载时使用，两端需一致
echo "[test] prepare configs (8 nodes)"
mkdir -p data/mvp4_8n_big/configs

# node-a 配置（客户端）
cat > data/mvp4_8n_big/configs/node_a.yaml <<'EOF'
node_id: "node-a"
listen_port: 9401
seed_nodes: "127.0.0.1:9402,127.0.0.1:9403,127.0.0.1:9404,127.0.0.1:9405,127.0.0.1:9406,127.0.0.1:9407,127.0.0.1:9408"
self_addr: "127.0.0.1:9401"
chunks_dir: "data/mvp4_8n_big/node_a/chunks"
chunk_index_file: "data/mvp4_8n_big/node_a/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_8n_big_node_a.log"
EOF

# node-2~8 配置（服务端）
for i in {2..8}; do
  cat > "data/mvp4_8n_big/configs/node_${i}.yaml" <<EOF
node_id: "node-${i}"
listen_port: 940${i}
seed_nodes: ""
self_addr: "127.0.0.1:940${i}"
chunks_dir: "data/mvp4_8n_big/node_${i}/chunks"
chunk_index_file: "data/mvp4_8n_big/node_${i}/chunk_index.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
log_file: "logs/mvp4_8n_big_node_${i}.log"
EOF
done

# --- 4) 分片与索引（--input 模式）---
# 在 node-2、node-3 上分别对同一大文件执行分片：
# - 按 chunk_size 切分文件
# - 计算每个分片的 SHA256 作为 chunk_id
# - 写入 chunk_index.tsv，落盘到 chunks 目录
# 两个节点做同样操作，便于后续从不同 owner 下载（本脚本主要用 node-2）
echo "[test] split and index at node-2/node-3 (big file)"
./build/app --config data/mvp4_8n_big/configs/node_2.yaml --input "${BIG_FILE}"
./build/app --config data/mvp4_8n_big/configs/node_3.yaml --input "${BIG_FILE}"

# 取第一个分片的 chunk_id，后续明文/加密下载会用到
CHUNK_ID="$(awk 'NR==1 {print $1}' data/mvp4_8n_big/node_2/chunk_index.tsv)"
if [[ -z "${CHUNK_ID}" ]]; then
  echo "[test] failed to read chunk_id from index"
  exit 1
fi
echo "[test] chunk_id=${CHUNK_ID}"

# --- 5) 启动 node-2~8 服务端 ---
# 后台运行，日志重定向到 logs/mvp4_8n_big_node_X_server.log
# PIDS 数组记录进程号，脚本退出时 cleanup 统一杀进程
echo "[test] start node-2..node-8 servers"
PIDS=()
for i in {2..8}; do
  ./build/app --config "data/mvp4_8n_big/configs/node_${i}.yaml" --mode server \
    > "logs/mvp4_8n_big_node_${i}_server.log" 2>&1 &
  PIDS+=("$!")
done
sleep 1

# 脚本退出时（正常/异常）清理所有 server 进程
cleanup() {
  for pid in "${PIDS[@]}"; do
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

# 轮询日志，直到出现 "tcp server waiting for client" 表示服务端就绪
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
for i in {2..8}; do
  wait_for_server "logs/mvp4_8n_big_node_${i}_server.log"
done

# --- 6) node-a 明文下载 ---
# --peer 127.0.0.1:9402：入口节点为 node-2
# 流程：连 node-2 → FIND_VALUE 查 chunk_id 的 owners → GET_CHUNK 拉取明文
# 输出重定向到 logs，便于排查
echo "[test] node-a plain download (find + get + verify)"
./build/app --config data/mvp4_8n_big/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9402 --chunk_id "${CHUNK_ID}" \
  > logs/mvp4_8n_big_client_plain.log 2>&1

# --- 7) node-a 加密下载 ---
# 与上一步相同，但加 --secure：
# 流程：FIND_VALUE → GET_CHUNK_SEC 请求 → 服务端返回 CHUNK_SEC（IV+密文+HMAC）
# 客户端解密并校验 HMAC 后得到明文
echo "[test] node-a secure download (find + get_sec + decrypt + verify)"
./build/app --config data/mvp4_8n_big/configs/node_a.yaml --mode client \
  --peer 127.0.0.1:9402 --chunk_id "${CHUNK_ID}" --secure \
  > logs/mvp4_8n_big_client_secure.log 2>&1

# --- 8) 验证日志 ---
# 明文：需出现 "mvp5: download ok bytes=..."
# 加密：需出现 "mvp4: secure mode enabled" 且 "mvp5: download ok bytes=..."
# 使用 grep -q 而非 awk，避免日志中的二进制分片内容被误解析导致乱码输出
echo "[test] verify logs"
grep -q 'mvp5: download ok bytes=' logs/mvp4_8n_big_client_plain.log || { echo "[test] failed: plain download"; exit 1; }
grep -q 'mvp4: secure mode enabled' logs/mvp4_8n_big_client_secure.log || { echo "[test] failed: secure mode"; exit 1; }
grep -q 'mvp5: download ok bytes=' logs/mvp4_8n_big_client_secure.log || { echo "[test] failed: secure download"; exit 1; }

echo "[test] done"
