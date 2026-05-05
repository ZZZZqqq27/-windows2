#!/usr/bin/env bash
# A/B 对比实验：
# A 组：单源基线（replica=0）
# B 组：多副本 + DHT（replica=3）
# 输出：成功率、平均时延、P95 时延（基于 download_stats.tsv）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PRO_DIR}"

ROUNDS="${ROUNDS:-30}"                # 每组下载轮次
PORT_BASE="${PORT_BASE:-9910}"        # 实际端口：9911~9914
INJECT_PRIMARY_DOWN="${INJECT_PRIMARY_DOWN:-0}"  # 1=中途关闭 node-1 做故障注入

DATA_ROOT="data/ab_replica_compare"
LOG_DIR="logs"
mkdir -p "${DATA_ROOT}" "${LOG_DIR}"

echo "[对比实验] 正在编译工程…"
cmake -S . -B build 2>/dev/null || true
cmake --build build

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
  echo "[对比实验] 错误：服务未就绪，日志：${log_file}"
  return 1
}

create_configs() {
  local case_name="$1"
  local case_dir="${DATA_ROOT}/${case_name}"
  local cfg_dir="${case_dir}/configs"
  mkdir -p "${cfg_dir}"

  for i in 1 2 3 4; do
    local port=$((PORT_BASE + i))
    local others=""
    for j in 1 2 3 4; do
      [[ "${j}" == "${i}" ]] && continue
      others="${others}127.0.0.1:$((PORT_BASE + j)),"
    done
    others="${others%,}"

    cat > "${cfg_dir}/node_${i}.yaml" <<EOF
node_id: "ab-node-${i}"
listen_port: ${port}
seed_nodes: "${others}"
self_addr: "127.0.0.1:${port}"
routing_capacity: 8
chunk_size_mb: 1
chunks_dir: "${case_dir}/node_${i}/chunks"
chunk_index_file: "${case_dir}/node_${i}/chunk_index.tsv"
download_stats_file: "${case_dir}/node_${i}/download_stats.tsv"
upload_meta_file: "${case_dir}/node_${i}/upload_meta.tsv"
upload_replica_file: "${case_dir}/node_${i}/upload_replica.tsv"
aes_key_hex: "00112233445566778899aabbccddeeff"
hmac_key_hex: "0102030405060708090a0b0c0d0e0f10"
download_strategy: "round_robin"
log_file: "${LOG_DIR}/ab_${case_name}_node_${i}.log"
EOF
    mkdir -p "${case_dir}/node_${i}/chunks"
  done

  printf "A/B 对比测试用例：%s\n第2行\n第3行\n下载测试负载内容\n" "${case_name}" \
    > "${case_dir}/input.txt"
}

calc_metrics() {
  local tsv_file="$1"
  if [[ ! -s "${tsv_file}" ]]; then
    echo "0 0 0 0.00 无 无"
    return 0
  fi

  # total ok fail success_rate avg_latency_ms(success-only)
  local basic
  basic="$(awk -F'\t' '
    {
      total++;
      if ($4 == 1) {
        ok++;
        sum += $5;
      }
    }
    END {
      fail = total - ok;
      rate = (total > 0) ? (ok * 100.0 / total) : 0.0;
      avg = (ok > 0) ? (sum / ok) : -1;
      if (avg < 0) {
        printf "%d %d %d %.2f 无", total, ok, fail, rate;
      } else {
        printf "%d %d %d %.2f %.2f", total, ok, fail, rate, avg;
      }
    }' "${tsv_file}")"

  local p95
  p95="$(awk -F'\t' '$4 == 1 {print $5}' "${tsv_file}" | sort -n | awk '
    {
      arr[++n] = $1;
    }
    END {
      if (n == 0) {
        print "无";
        exit;
      }
      idx = int((n * 95 + 99) / 100);
      if (idx < 1) idx = 1;
      if (idx > n) idx = n;
      print arr[idx];
    }')"

  echo "${basic} ${p95}"
}

run_case() {
  local case_name="$1"
  local replica="$2"
  local case_label="${3:-${case_name}}"
  local case_dir="${DATA_ROOT}/${case_name}"
  local cfg_dir="${case_dir}/configs"
  local case_log="${LOG_DIR}/ab_${case_name}.log"
  : > "${case_log}"

  echo "[对比实验][${case_label}] 准备数据目录…"
  rm -rf "${case_dir}"
  mkdir -p "${case_dir}"
  create_configs "${case_name}"

  local pids=()
  local node1_pid=""
  cleanup_case() {
    if [[ -n "${node1_pid}" ]]; then
      kill "${node1_pid}" 2>/dev/null || true
      wait "${node1_pid}" 2>/dev/null || true
    fi
    for pid in "${pids[@]}"; do
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    done
  }
  trap cleanup_case RETURN

  echo "[对比实验][${case_label}] 启动节点 2～4 的服务端…"
  for i in 2 3 4; do
    ./build/app --config "${cfg_dir}/node_${i}.yaml" --mode server \
      > "${LOG_DIR}/ab_${case_name}_node_${i}_server.log" 2>&1 &
    pids+=("$!")
  done
  sleep 1
  for i in 2 3 4; do
    wait_for_server "${LOG_DIR}/ab_${case_name}_node_${i}_server.log"
  done

  echo "[对比实验][${case_label}] 上传文件，副本数 replica=${replica}…"
  ./build/app --config "${cfg_dir}/node_1.yaml" \
    --upload "${case_dir}/input.txt" \
    --replica "${replica}" \
    --manifest_out "${case_dir}/manifest.txt" \
    > "${LOG_DIR}/ab_${case_name}_upload.log" 2>&1

  local chunk_id
  chunk_id="$(awk 'NR==1{print; exit}' "${case_dir}/manifest.txt")"
  if [[ -z "${chunk_id}" ]]; then
    echo "[对比实验][${case_label}] 失败：清单中 chunk_id 为空"
    exit 1
  fi
  echo "[对比实验][${case_label}] chunk_id=${chunk_id}"

  echo "[对比实验][${case_label}] 启动节点 1 的服务端…"
  ./build/app --config "${cfg_dir}/node_1.yaml" --mode server \
    > "${LOG_DIR}/ab_${case_name}_node_1_server.log" 2>&1 &
  node1_pid="$!"
  sleep 1
  wait_for_server "${LOG_DIR}/ab_${case_name}_node_1_server.log"

  echo "[对比实验][${case_label}] 开始下载轮次：共 ${ROUNDS} 轮…"
  local ok_count=0
  local fail_count=0
  local cutoff=$((ROUNDS / 2))
  for ((i=1; i<=ROUNDS; i++)); do
    if [[ "${INJECT_PRIMARY_DOWN}" == "1" && "${i}" -eq "${cutoff}" ]]; then
      echo "[对比实验][${case_label}] 故障注入：第 ${i} 轮起关闭节点 1（主源）" | tee -a "${case_log}"
      kill "${node1_pid}" 2>/dev/null || true
      wait "${node1_pid}" 2>/dev/null || true
      node1_pid=""
      sleep 1
    fi

    if ./build/app --config "${cfg_dir}/node_4.yaml" --mode client \
      --peer "127.0.0.1:$((PORT_BASE+2))" \
      --chunk_id "${chunk_id}" \
      --strategy round_robin \
      >> "${case_log}" 2>&1; then
      ok_count=$((ok_count + 1))
    else
      fail_count=$((fail_count + 1))
    fi
  done
  echo "[对比实验][${case_label}] 进程退出码统计：成功 ${ok_count} 次，失败 ${fail_count} 次（注意：可能与 TSV 中逐次尝试记录不一致）"

  local stats_file="${case_dir}/node_4/download_stats.tsv"
  local total ok fail rate avg p95
  read -r total ok fail rate avg p95 < <(calc_metrics "${stats_file}")

  printf "%s\t%d\t%d\t%d\t%s\t%s\t%s\n" \
    "${case_label}" "${total}" "${ok}" "${fail}" "${rate}" "${avg}" "${p95}" \
    >> "${LOG_DIR}/ab_compare_summary.tsv"
}

echo -e "方案\t统计条数\t成功次数\t失败次数\t成功率(%)\t平均时延(ms)\tP95时延(ms)" \
  > "${LOG_DIR}/ab_compare_summary.tsv"

run_case "A_single_source" 0 "A·单源基线（replica=0）"
run_case "B_replica3_dht" 3 "B·多副本+DHT（replica=3）"

echo ""
echo "[对比实验] 汇总结果："
cat "${LOG_DIR}/ab_compare_summary.tsv"
echo ""
echo "[对比实验] 已完成。输出文件："
echo "  - ${LOG_DIR}/ab_compare_summary.tsv（汇总表）"
echo "  - ${LOG_DIR}/ab_A_single_source.log"
echo "  - ${LOG_DIR}/ab_B_replica3_dht.log"
echo ""
echo "[对比实验] 可选环境变量示例："
echo "  INJECT_PRIMARY_DOWN=1 ROUNDS=40 bash scripts/run_ab_replica_compare.sh"
