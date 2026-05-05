#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRO_DIR="${ROOT_DIR}/pro"
cd "${PRO_DIR}"

# 配置（可修改）
MAC_IP="192.168.1.11"
WSL_IP="192.168.1.3"
MAC_BASE_PORT="9601"
WSL_BASE_PORT="9701"

# 测试参数
FILE_SIZE_MB=0.1
FILE_COUNT=48
DATASET_DIR="${ROOT_DIR}/data/dual_host/dataset_${FILE_SIZE_MB}MB_${FILE_COUNT}files"
OUT_DIR="${ROOT_DIR}/output/dual_host"
mkdir -p "${OUT_DIR}" "${DATASET_DIR}"

RUN_TAG="$(date +%Y%m%d_%H%M%S)"
CHUNK_IDS_FILE="${SCRIPT_DIR}/chunk_ids_${RUN_TAG}.txt"

get_time_ms() {
  python3 -c 'import time; print(int(time.time() * 1000))'
}

echo "=================================="
echo "准备数据集"
echo "=================================="
echo "文件大小: ${FILE_SIZE_MB}MB"
echo "文件总数: ${FILE_COUNT}"
echo "=================================="

# 1. 生成随机文件
echo ""
echo "1. 生成随机文件..."
for ((i=0; i<FILE_COUNT; i++)); do
  f="${DATASET_DIR}/f_$(printf "%03d" $i).bin"
  if [[ ! -f "${f}" ]]; then
    size_kb=$(awk -v mb="$FILE_SIZE_MB" 'BEGIN{printf "%d", mb*1024}')
    dd if=/dev/urandom of="${f}" bs=1k count="${size_kb}" status=none
  fi
done
echo "✅ 文件已就绪"

# 2. 上传到 A0 节点（第一个节点，索引从0开始）
echo ""
echo "2. 上传文件到 A0 节点..."
cfg_file="${SCRIPT_DIR}/config/nodeA0.yaml"
if [[ ! -f "${cfg_file}" ]]; then
  echo "[ERROR] 请先运行 start_machineA.sh 启动节点！"
  exit 1
fi

for f in "${DATASET_DIR}"/*.bin; do
  "${PRO_DIR}/build/app" --config "${cfg_file}" --input "${f}" > /dev/null 2>&1 || true
done

# 3. 收集 chunk_id
echo ""
echo "3. 收集 chunk_id..."
chunk_ids=()
index_file="${PRO_DIR}/chunk_index_A0.tsv"
while IFS= read -r line; do
  chunk_ids+=("${line}")
done < <(awk '{print $1}' "${index_file}" | head -n "${FILE_COUNT}")

if [[ "${#chunk_ids[@]}" -eq 0 ]]; then
  echo "[ERROR] 未找到chunk_id！"
  exit 1
fi

# 保存到文件
> "${CHUNK_IDS_FILE}"
for cid in "${chunk_ids[@]}"; do
  echo "${cid}" >> "${CHUNK_IDS_FILE}"
done

echo "✅ chunk_id 已保存到: ${CHUNK_IDS_FILE}"

# 4. 生成环境变量文件（数组改为字符串格式，便于跨平台source）
ENV_FILE="${OUT_DIR}/env_${RUN_TAG}.sh"
cat > "${ENV_FILE}" << EOF
#!/usr/bin/env bash

# 环境变量，复制到两台机器上使用

export RUN_TAG="${RUN_TAG}"
export MAC_IP="${MAC_IP}"
export WSL_IP="${WSL_IP}"
export MAC_BASE_PORT="${MAC_BASE_PORT}"
export WSL_BASE_PORT="${WSL_BASE_PORT}"
export FILE_SIZE_MB="${FILE_SIZE_MB}"
export FILE_COUNT="${FILE_COUNT}"
export ROUNDS=2

# 并发度用空格分隔的字符串，下载脚本里会转成数组
export CONCURRENCIES="4 8 16"

# chunk_id 文件路径（相对于 SCRIPT_DIR）
export CHUNK_IDS_FILE="chunk_ids_${RUN_TAG}.txt"
EOF

chmod +x "${ENV_FILE}"

echo ""
echo "=================================="
echo "✅ 数据集准备完成！"
echo "=================================="
echo "请将以下文件复制到 WSL 侧："
echo "  1. ${CHUNK_IDS_FILE}"
echo "  2. ${ENV_FILE}"
echo "  3. ${DATASET_DIR}/ (可选)"
echo "=================================="
