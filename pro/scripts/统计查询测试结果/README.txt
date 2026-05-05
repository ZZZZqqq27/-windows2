统计查询测试：脚本副本 + 一次执行结果归档

目录位置（与 run_stats_query_test.sh 同级）：
  pro/scripts/统计查询测试结果/

内容说明
  run_stats_query_test.sh
      与 pro/scripts/run_stats_query_test.sh 内容一致的副本（便于单独打包本文件夹）。

  console_full.log
      在 pro/ 下执行「原脚本」时的完整终端输出。

  stats_download.json / stats_upload_meta.json / stats_upload_replica.json
      使用同一次测试生成的 data/test_stats_query/app.yaml 与 TSV，
      分别调用 --mode stats --json 得到的原始 JSON（写论文或给 GPT 可直接引用）。

  _fixture/
      本次运行后从 pro/data/test_stats_query/ 复制的输入样例：
      download_stats.tsv、upload_meta.tsv、upload_replica.tsv、app.yaml
      （路径仍指向原 data 目录；若需离线复现，可手工改 app.yaml 内路径为相对本机 _fixture）。

复现命令（在 pro 目录）
  bash scripts/run_stats_query_test.sh

本次归档记录
  exit_code=0
  生成时间：见各文件时间戳
