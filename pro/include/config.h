#pragma once

#include <string>
#include <vector>

/**
 * @brief 应用启动配置（贯穿各 MVP 的统一配置入口）
 *
 * 论文写作建议：
 * - 将 AppConfig 视作“节点运行时参数集合”，用于控制：网络端口、数据落盘路径、
 *   加密密钥、下载策略、统计文件位置、HTTP 端口等。
 * - 脚本（`pro/scripts/*.sh`）通常会为每个节点生成独立的 YAML，
 *   从而实现多节点同机运行与数据隔离（data/<scenario>/node_x/...）。
 */
struct AppConfig {
  std::string node_id = "node-001";  // 节点唯一标识
  int listen_port = 9000;           // 监听端口
  int dht_k = 8;                    // DHT k-bucket 参数（预留）
  int max_nodes = 50;               // 最大节点数（预留）
  std::vector<std::string> seed_nodes;  // 静态种子节点列表
  std::string self_addr = "127.0.0.1:9000";  // 节点对外地址
  std::size_t routing_capacity = 8;     // 路由表容量上限
  int chunk_size_mb = 1;            // 分片大小（MB）
  std::string chunks_dir = "data/chunks";  // 分片输出目录
  std::string chunk_index_file = "data/chunk_index.tsv";  // 分片索引文件
  std::string aes_key_hex = "00112233445566778899aabbccddeeff";  // AES-128 密钥（16字节十六进制）
  std::string hmac_key_hex = "0102030405060708090a0b0c0d0e0f10";  // HMAC 密钥（16字节十六进制）
  std::string download_strategy = "random";  // 下载策略（random/round_robin）
  std::string download_stats_file = "data/download_stats.tsv";  // 下载统计持久化文件
  std::string upload_meta_file = "data/upload_meta.tsv";  // 上传文件元数据持久化
  std::string upload_replica_file = "data/upload_replica.tsv";  // 上传副本结果持久化
  int http_port = 8080;  // HTTP API 端口（最小可用后端接口）
  std::string log_level = "info";   // 日志级别
  std::string log_file = "logs/app.log";  // 日志文件路径
};

/**
 * @brief 读取简单 YAML 风格的 `key: value` 配置（支持 `#` 注释）
 *
 * 解析策略（刻意保持“最小可用”以降低依赖）：
 * - 仅支持单行 `key: value`
 * - value 可用双引号包裹
 * - seed_nodes 支持 CSV（逗号分隔）
 *
 * @param path 配置文件路径
 * @return 解析得到的 AppConfig；若文件不存在/无法打开，则返回默认值（不抛异常）
 */
AppConfig LoadConfig(const std::string& path);
