#include "config.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
/**
 * @brief 去除首尾空白字符
 *
 * 用于配置行解析的标准化：避免因为空格/制表符导致 key/value 识别失败。
 */
std::string Trim(const std::string& s) {
  const char* whitespace = " \t\r\n";
  auto start = s.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return "";
  }
  auto end = s.find_last_not_of(whitespace);
  return s.substr(start, end - start + 1);
}

/**
 * @brief 解析单行 `key: value` 配置
 *
 * - 仅取第一个 ':' 作为分隔符
 * - value 支持双引号包裹（便于包含逗号、空格等）
 *
 * @return true 表示解析成功；false 表示该行不是可识别的键值对
 */
bool ParseLine(const std::string& line, std::string* key, std::string* value) {
  auto pos = line.find(':');
  if (pos == std::string::npos) {
    return false;
  }
  *key = Trim(line.substr(0, pos));
  *value = Trim(line.substr(pos + 1));
  if (key->empty() || value->empty()) {
    return false;
  }
  if (value->size() >= 2 && value->front() == '"' && value->back() == '"') {
    *value = value->substr(1, value->size() - 2);
  }
  return true;
}
}  // namespace

std::vector<std::string> SplitCsv(const std::string& value) {
  // seed_nodes 采用 CSV：`host:port,host:port,...`
  std::vector<std::string> out;
  std::string cur;
  std::istringstream iss(value);
  while (std::getline(iss, cur, ',')) {
    const std::string trimmed = Trim(cur);
    if (!trimmed.empty()) {
      out.push_back(trimmed);
    }
  }
  return out;
}

AppConfig LoadConfig(const std::string& path) {
  AppConfig cfg;
  std::ifstream in(path);
  if (!in.is_open()) {
    // 设计取舍：配置缺失时返回默认 cfg，避免 demo/测试脚本因缺文件直接崩溃。
    return cfg;
  }
  std::string line;
  while (std::getline(in, line)) {
    auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    std::string key;
    std::string value;
    if (!ParseLine(trimmed, &key, &value)) {
      continue;
    }
    // 显式 key 分派：字段少、依赖轻；便于论文中列举“配置项—功能点”映射关系。
    if (key == "node_id") {
      cfg.node_id = value;
    } else if (key == "listen_port") {
      cfg.listen_port = std::stoi(value);
    } else if (key == "dht_k") {
      cfg.dht_k = std::stoi(value);
    } else if (key == "max_nodes") {
      cfg.max_nodes = std::stoi(value);
    } else if (key == "seed_nodes") {
      cfg.seed_nodes = SplitCsv(value);
    } else if (key == "self_addr") {
      cfg.self_addr = value;
    } else if (key == "routing_capacity") {
      cfg.routing_capacity = static_cast<std::size_t>(std::stoul(value));
    } else if (key == "chunk_size_mb") {
      cfg.chunk_size_mb = std::stoi(value);
    } else if (key == "chunks_dir") {
      cfg.chunks_dir = value;
    } else if (key == "chunk_index_file") {
      cfg.chunk_index_file = value;
    } else if (key == "aes_key_hex") {
      cfg.aes_key_hex = value;
    } else if (key == "hmac_key_hex") {
      cfg.hmac_key_hex = value;
    } else if (key == "download_strategy") {
      cfg.download_strategy = value;
    } else if (key == "download_stats_file") {
      cfg.download_stats_file = value;
    } else if (key == "upload_meta_file") {
      cfg.upload_meta_file = value;
    } else if (key == "upload_replica_file") {
      cfg.upload_replica_file = value;
    } else if (key == "http_port") {
      try {
        cfg.http_port = std::stoi(value);
      } catch (...) {
        // 端口解析失败时回落到默认端口，避免因为配置脏数据导致无法启动 HTTP。
        cfg.http_port = 8080;
      }
    } else if (key == "log_level") {
      cfg.log_level = value;
    } else if (key == "log_file") {
      cfg.log_file = value;
    }
  }
  return cfg;
}
