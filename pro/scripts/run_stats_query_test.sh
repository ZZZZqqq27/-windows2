#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_APP="${ROOT_DIR}/build/app"
TEST_DIR="${ROOT_DIR}/data/test_stats_query"

mkdir -p "${TEST_DIR}"

DOWNLOAD_FILE="${TEST_DIR}/download_stats.tsv"
UPLOAD_META_FILE="${TEST_DIR}/upload_meta.tsv"
UPLOAD_REPLICA_FILE="${TEST_DIR}/upload_replica.tsv"
TEMP_CFG="${TEST_DIR}/app.yaml"

cat > "${DOWNLOAD_FILE}" <<'EOF'
2026-03-10 02:00:00	abcd1234	127.0.0.1:9001	1	12	random
2026-03-10 02:01:00	abcd1234	127.0.0.1:9002	0	18	random
EOF

cat > "${UPLOAD_META_FILE}" <<'EOF'
2026-03-10 02:00:00	input.txt	42	127.0.0.1:9000	data/manifest.txt
EOF

cat > "${UPLOAD_REPLICA_FILE}" <<'EOF'
2026-03-10 02:00:00	abcd1234	127.0.0.1:9001	1
2026-03-10 02:00:00	abcd1234	127.0.0.1:9002	0
EOF

cp "${ROOT_DIR}/configs/app.yaml" "${TEMP_CFG}"
cat >> "${TEMP_CFG}" <<EOF
download_stats_file: "${DOWNLOAD_FILE}"
upload_meta_file: "${UPLOAD_META_FILE}"
upload_replica_file: "${UPLOAD_REPLICA_FILE}"
EOF

echo "[test] stats: download json"
"${BUILD_APP}" --config "${TEMP_CFG}" --mode stats \
  --stats_type download --json \
  --limit 2 2>/dev/null | \
  awk 'BEGIN{ok=0} /"type":"download"/{ok=1} END{exit ok?0:1}'

echo "[test] stats: upload_meta json"
"${BUILD_APP}" --config "${TEMP_CFG}" --mode stats \
  --stats_type upload_meta --json \
  --limit 1 2>/dev/null | \
  awk 'BEGIN{ok=0} /"type":"upload_meta"/{ok=1} END{exit ok?0:1}'

echo "[test] stats: upload_replica json"
"${BUILD_APP}" --config "${TEMP_CFG}" --mode stats \
  --stats_type upload_replica --json \
  --limit 2 2>/dev/null | \
  awk 'BEGIN{ok=0} /"type":"upload_replica"/{ok=1} END{exit ok?0:1}'

echo "[test] stats query ok"
