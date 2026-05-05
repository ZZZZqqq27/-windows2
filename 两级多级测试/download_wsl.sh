#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRO_DIR="${ROOT_DIR}/pro"
cd "${PRO_DIR}"

# 加载环境变量
ENV_FILE=${1:-}
if [[ -z "${ENV_FILE}" || ! -f "${ENV_FILE}" ]]; then
  echo "[ERROR] 请先在Mac侧运行 prepare_dataset.sh，然后复制 env.sh 和 chunk_ids.txt 到WSL！"
  exit 1
fi

source "${ENV_FILE}"

# 测试参数
NODE_A_COUNT=${2:-4}
NODE_B_COUNT=${3:-4}
TOTAL_NODES=$((NODE_A_COUNT + NODE_B_COUNT))

# 输出目录
OUT_DIR="${ROOT_DIR}/output/dual_host"
RAW_DIR="${OUT_DIR}/raw_wsl"
SUMMARY_DIR="${OUT_DIR}/summary"
mkdir -p "${RAW_DIR}" "${SUMMARY_DIR}"

# 文件输出
RUN_TSV="${SUMMARY_DIR}/runs_wsl_${RUN_TAG}.tsv"
GROUP_TSV="${SUMMARY_DIR}/group_summary_wsl_${RUN_TAG}.tsv"

# 使用后24个chunk
START_IDX=24
END_IDX=47

get_time_ms() {
  python3 -c 'import time; print(int(time.time() * 1000))'
}

# 读取chunk_id
chunk_ids=()
while IFS= read -r line; do
  chunk_ids+=("${line}")
done < "${SCRIPT_DIR}/${CHUNK_IDS_FILE}"

# 把CONCURRENCIES字符串转成数组
read -r -a CONC_ARR <<< "${CONCURRENCIES}"

echo "=================================="
echo "WSL侧下载测试"
echo "=================================="
echo "节点分布: ${NODE_A_COUNT}+${NODE_B_COUNT}"
echo "测试并发: ${CONCURRENCIES}"
echo "下载范围: ${START_IDX} ~ ${END_IDX} (共24个文件)"
echo "=================================="

# 准备TSV表头
printf "timestamp\tgroup_id\tround\tmode\tfile_count\tfile_size_mb\tconcurrency\tnodeA_count\tnodeB_count\ttotal_nodes\tsuccess\tfail\telapsed_ms\tthroughput_mb_s\n" > "${RUN_TSV}"
printf "group_id\tmode\tfile_count\tfile_size_mb\tconcurrency\tnodeA_count\tnodeB_count\ttotal_nodes\trounds\tavg_elapsed_ms\tavg_throughput_mb_s\tsuccess_rate\n" > "${GROUP_TSV}"

# B0节点配置文件
CFG_FILE="${SCRIPT_DIR}/config/nodeB0.yaml"
if [[ ! -f "${CFG_FILE}" ]]; then
  echo "[ERROR] 请先运行 start_machineB.sh 启动节点！"
  exit 1
fi

# WSL连接自己的B节点（作为客户端）
PEER_ADDR="${WSL_IP}:${WSL_BASE_PORT}"

run_download() {
  local conc=$1
  local gid="N${TOTAL_NODES}_C${conc}"

  echo ""
  echo "----------------------------------"
  echo "测试组: ${gid}"
  echo "----------------------------------"

  local total_elapsed=0
  local total_success=0
  local total_fail=0

  for ((r=1; r<=ROUNDS; r++)); do
    local start_ms end_ms elapsed_ms
    start_ms=$(get_time_ms)
    local success=0
    local fail=0

    # 下载后24个，全部走并发
    local pids=()
    local active=0
    local idx=0

    for ((i=START_IDX; i<=END_IDX; i++)); do
      cid="${chunk_ids[$i]}"
      idx=$((idx + 1))

      (
        if "${PRO_DIR}/build/app" --config "${CFG_FILE}" --mode client --peer "${PEER_ADDR}" --chunk_id "${cid}" --strategy round_robin >> "${RAW_DIR}/${gid}_client_${RUN_TAG}.log" 2>&1; then
          echo ok
        else
          echo fail
        fi
      ) > "${PRO_DIR}/tmp_wsl_job_${r}_${idx}.status" &

      pids+=("$!")
      active=$((active + 1))

      if [[ ${active} -ge ${conc} ]]; then
        wait "${pids[0]}" || true
        pids=("${pids[@]:1}")
        active=$((active - 1))
      fi
    done

    for pid in "${pids[@]}"; do wait "${pid}" || true; done

    # 统计
    for ((i=START_IDX; i<=END_IDX; i++)); do
      local j=$((i - START_IDX + 1))
      local status_file="${PRO_DIR}/tmp_wsl_job_${r}_${j}.status"
      if [[ -f "${status_file}" ]]; then
        local status=$(cat "${status_file}" 2>/dev/null || echo fail)
        if [[ "${status}" == "ok" ]]; then
          success=$((success + 1))
        else
          fail=$((fail + 1))
        fi
        rm -f "${status_file}" 2>/dev/null
      else
        fail=$((fail + 1))
      fi
    done

    end_ms=$(get_time_ms)
    elapsed_ms=$((end_ms - start_ms))
    [[ ${elapsed_ms} -le 0 ]] && elapsed_ms=1

    local throughput
    local count=$((END_IDX - START_IDX + 1))
    throughput=$(awk -v fc="${count}" -v fsmb="${FILE_SIZE_MB}" -v ms="${elapsed_ms}" 'BEGIN{printf "%.3f", (fc*fsmb*1000)/ms}')

    printf "%s\t%s\t%d\tparallel\t%d\t%.1f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\n" \
      "$(date '+%F %T')" "${gid}" $r $count "${FILE_SIZE_MB}" $conc ${NODE_A_COUNT} ${NODE_B_COUNT} ${TOTAL_NODES} $success $fail $elapsed_ms $throughput >> "${RUN_TSV}"

    total_elapsed=$((total_elapsed + elapsed_ms))
    total_success=$((total_success + success))
    total_fail=$((total_fail + fail))

    echo "  轮次 ${r}/${ROUNDS}: 成功${success}/${count} 耗时${elapsed_ms}ms"
  done

  local avg_elapsed avg_tp success_rate total_req
  avg_elapsed=$(awk -v x="$total_elapsed" -v r="$ROUNDS" 'BEGIN{printf "%.3f", x/r}')
  local count=$((END_IDX - START_IDX + 1))
  avg_tp=$(awk -v x="$total_elapsed" -v fc="${count}" -v fsmb="${FILE_SIZE_MB}" -v r="$ROUNDS" 'BEGIN{printf "%.3f", (r*fc*fsmb*1000)/x}')
  total_req=$((total_success + total_fail))
  success_rate=$(awk -v ok="$total_success" -v all="$total_req" 'BEGIN{printf "%.3f", ok/all}')

  printf "%s\tparallel\t%d\t%.1f\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\n" \
    "${gid}" $count "${FILE_SIZE_MB}" $conc ${NODE_A_COUNT} ${NODE_B_COUNT} ${TOTAL_NODES} ${ROUNDS} ${avg_elapsed} ${avg_tp} ${success_rate} >> "${GROUP_TSV}"

  echo "✅ ${gid} 完成: 平均${avg_elapsed}ms 吞吐${avg_tp}MB/s"
}

# 开始跑
for c in "${CONC_ARR[@]}"; do
  run_download $c
done

echo ""
echo "=================================="
echo "✅ WSL侧测试完成！"
echo "=================================="
echo "逐轮明细: ${RUN_TSV}"
echo "分组汇总: ${GROUP_TSV}"
echo "请将这两个TSV文件复制到Mac侧汇总！"
echo "=================================="
