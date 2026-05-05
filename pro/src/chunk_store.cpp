#include "chunk_store.h"

#include "logger.h"
#include "sha256.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
/**
 * @brief 从输入流读取固定大小的数据块（用于顺序切分）
 *
 * @param in     已打开的二进制输入流
 * @param buffer 输出缓冲区（会被覆盖并 resize 到实际读取字节数）
 * @param size   期望读取字节数
 *
 * @return true 本次读取获得了 >0 字节；false 表示 EOF 或读取失败。
 *
 * @note 这里用 gcount() 判断实际读取量，以适配最后一个 chunk 小于 size 的情况。
 */
bool ReadFileChunk(std::ifstream* in, std::vector<uint8_t>* buffer, std::size_t size) {
  buffer->assign(size, 0);
  in->read(reinterpret_cast<char*>(buffer->data()), static_cast<std::streamsize>(size));
  const std::streamsize got = in->gcount();
  if (got <= 0) {
    buffer->clear();
    return false;
  }
  buffer->resize(static_cast<std::size_t>(got));
  return true;
}

/**
 * @brief 将 owners_csv 解析为 owners 列表
 *
 * owners_csv 格式：`host:port,host:port,...`
 * - "-" 代表空列表（占位）
 * - 解析时会过滤空项与 "-" 项
 */
std::vector<std::string> SplitOwners(const std::string& value) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream iss(value);
  while (std::getline(iss, cur, ',')) {
    if (!cur.empty() && cur != "-") {
      out.push_back(cur);
    }
  }
  return out;
}

/**
 * @brief 将 owners 列表拼接为 owners_csv（用于索引序列化）
 *
 * - owners 为空时返回 "-"（占位，便于反序列化区分空/缺失）
 */
std::string JoinOwners(const std::vector<std::string>& owners) {
  if (owners.empty()) {
    return "-";
  }
  std::string out;
  for (std::size_t i = 0; i < owners.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += owners[i];
  }
  return out;
}

/**
 * @brief 归一化 source 字段（用于索引序列化）
 *
 * - source 为空时写入 "-"（占位）
 */
std::string NormalizeSource(const std::string& source) {
  return source.empty() ? "-" : source;
}

/**
 * @brief 将 src owners 合并进 dst（去重）
 *
 * @note owners 去重采用 O(n^2) 的简单策略（规模一般较小：副本数/候选 owner 数有限）。
 */
void MergeOwners(const std::vector<std::string>& src,
                 std::vector<std::string>* dst) {
  if (!dst) {
    return;
  }
  for (const auto& s : src) {
    bool exists = false;
    for (const auto& d : *dst) {
      if (d == s) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      dst->push_back(s);
    }
  }
}
}  // namespace

bool SplitFileToChunks(const std::string& input_path,
                       const std::string& output_dir,
                       std::size_t chunk_size_bytes,
                       std::vector<ChunkInfo>* out_chunks) {
  if (!out_chunks) {
    return false;
  }
  out_chunks->clear();

  // 记录关键参数，便于实验复现（论文中也可直接引用日志对齐实验设置）。
  LogInfo("split start: input=" + input_path +
          " chunk_size_bytes=" + std::to_string(chunk_size_bytes));

  std::ifstream in(input_path, std::ios::binary);
  if (!in.is_open()) {
    LogError("split failed: cannot open input file");
    return false;
  }

  // 输出目录通常按“节点维度”隔离（node_x/chunks），避免多节点测试互相污染。
  std::filesystem::create_directories(output_dir);

  std::size_t index = 0;
  std::vector<uint8_t> buffer;
  while (ReadFileChunk(&in, &buffer, chunk_size_bytes)) {
    // chunk_id = SHA-256(plaintext bytes)，作为内容寻址键（content-addressed）。
    const std::string hash = Sha256Hex(buffer);
    const std::string filename = hash + ".chunk";
    const std::filesystem::path out_path =
        std::filesystem::path(output_dir) / filename;

    std::ofstream out(out_path, std::ios::binary);
    if (!out.is_open()) {
      LogError("split failed: cannot write chunk " + out_path.string());
      return false;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(buffer.size()));

    ChunkInfo info;
    info.chunk_id = hash;
    info.path = out_path.string();
    info.size = buffer.size();
    out_chunks->push_back(info);

    // 记录每个 chunk 的信息（用于定位 chunk 数量、大小以及落盘路径）。
    LogInfo("chunk[" + std::to_string(index) + "] id=" + hash +
            " size=" + std::to_string(buffer.size()) +
            " path=" + out_path.string());
    ++index;
  }

  LogInfo("split done: chunks=" + std::to_string(out_chunks->size()));
  return true;
}

bool VerifyChunks(const std::vector<ChunkInfo>& chunks) {
  LogInfo("verify start: chunks=" + std::to_string(chunks.size()));
  std::size_t ok = 0;
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    const ChunkInfo& info = chunks[i];
    std::ifstream in(info.path, std::ios::binary);
    if (!in.is_open()) {
      LogError("verify failed: cannot open " + info.path);
      return false;
    }

    // 读取整个分片文件并重新计算 SHA-256，验证 chunk_id 是否与内容一致。
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const std::string hash = Sha256Hex(data);
    if (hash != info.chunk_id) {
      LogError("verify mismatch: path=" + info.path +
               " expect=" + info.chunk_id + " actual=" + hash);
      return false;
    }
    LogInfo("verify ok: " + info.chunk_id);
    ++ok;
  }
  LogInfo("verify done: ok=" + std::to_string(ok));
  return true;
}

bool SaveChunkIndex(const std::string& index_path,
                    const std::vector<ChunkInfo>& chunks) {
  LogInfo("index save start: path=" + index_path +
          " chunks=" + std::to_string(chunks.size()));

  std::filesystem::path p(index_path);
  if (p.has_parent_path()) {
    // 索引文件通常位于 data/<scenario>/node_x/ 下，确保父目录存在。
    std::filesystem::create_directories(p.parent_path());
  }

  std::ofstream out(index_path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    LogError("index save failed: cannot open " + index_path);
    return false;
  }

  for (const auto& info : chunks) {
    // TSV 序列化：为兼容早期版本，前三列固定；后续列为 owners/source 扩展。
    out << info.chunk_id << "\t" << info.path << "\t" << info.size
        << "\t" << JoinOwners(info.owners)
        << "\t" << NormalizeSource(info.source) << "\n";
  }

  LogInfo("index save done");
  return true;
}
/*
*LoadChunkIndex 方法功能说明
这个方法的作用是：从 TSV（制表符分隔）文件中加载分块索引信息，恢复所有已上传/下载的文件分块元数据。
 *
 *
 *
 *
 *
 */
bool LoadChunkIndex(const std::string& index_path,
                    std::vector<ChunkInfo>* out_chunks) {
  if (!out_chunks) {
    return false;
  }
  out_chunks->clear();

  LogInfo("index load start: path=" + index_path);
  std::ifstream in(index_path);
  if (!in.is_open()) {
    LogWarn("index load skipped: file not found " + index_path);
    return false;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    // 逐行按 TAB 切分；字段不足则跳过，避免因脏数据导致整体失败。
    std::vector<std::string> parts;
    std::string cur;
    std::istringstream iss(line);
    while (std::getline(iss, cur, '\t')) {
      parts.push_back(cur);
    }
    if (parts.size() < 3) {
      LogWarn("index load: skip invalid line");
      continue;
    }
    ChunkInfo info;
    info.chunk_id = parts[0];
    info.path = parts[1];
    try {
      info.size = static_cast<std::size_t>(std::stoull(parts[2]));
    } catch (...) {
      LogWarn("index load: skip invalid size");
      continue;
    }
    if (parts.size() >= 4) {
      // owners_csv：逗号分隔；"-" 表示空 owners。
      info.owners = SplitOwners(parts[3]);
    }
    if (parts.size() >= 5 && parts[4] != "-") {
      // source 列使用 "-" 作为占位（空值）。
      info.source = parts[4];
    }
    out_chunks->push_back(info);
  }

  LogInfo("index load done: chunks=" + std::to_string(out_chunks->size()));
  return true;
}

bool SaveChunkManifest(const std::string& manifest_path,
                       const std::vector<ChunkInfo>& chunks) {
  std::filesystem::path p(manifest_path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path());
  }
  std::ofstream out(manifest_path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    LogError("manifest save failed: cannot open " + manifest_path);
    return false;
  }
  for (const auto& info : chunks) {
    // manifest 只保存顺序（chunk_id 列表）；整文件下载时按该顺序拼装。
    out << info.chunk_id << "\n";
  }
  return true;
}

bool LoadChunkManifest(const std::string& manifest_path,
                       std::vector<std::string>* out_chunk_ids) {
  if (!out_chunk_ids) {
    return false;
  }
  out_chunk_ids->clear();
  std::ifstream in(manifest_path);
  if (!in.is_open()) {
    LogWarn("manifest load skipped: file not found " + manifest_path);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    out_chunk_ids->push_back(line);
  }
  return true;
}

bool SaveChunkFile(const std::string& chunk_id,
                   const std::vector<uint8_t>& data,
                   const std::string& output_dir,
                   ChunkInfo* out_info) {
  if (!out_info) {
    return false;
  }
  if (chunk_id.empty()) {
    LogError("chunk save failed: empty chunk_id");
    return false;
  }
  std::filesystem::create_directories(output_dir);
  const std::filesystem::path out_path =
      std::filesystem::path(output_dir) / (chunk_id + ".chunk");
  std::ofstream out(out_path, std::ios::binary);
  if (!out.is_open()) {
    LogError("chunk save failed: cannot write " + out_path.string());
    return false;
  }
  if (!data.empty()) {
    // 注意：二进制写入，不做额外编码；上层协议可能负责 hex/secure 封装。
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  ChunkInfo info;
  info.chunk_id = chunk_id;
  info.path = out_path.string();
  info.size = data.size();
  *out_info = info;
  return true;
}

bool UpsertChunkIndex(const std::string& index_path,
                      const ChunkInfo& info) {
  // 读取现有索引 -> in-memory 合并 -> 重新落盘（简单可靠；索引规模通常较小）。
  std::vector<ChunkInfo> existing;
  LoadChunkIndex(index_path, &existing);
  bool updated = false;
  for (auto& item : existing) {
    if (item.chunk_id == info.chunk_id) {
      // 更新路径/大小（仅在新值有效时覆盖），避免覆盖已有的正确字段。
      if (!info.path.empty()) {
        item.path = info.path;
      }
      if (info.size != 0) {
        item.size = info.size;
      }
      // owners 采用“集合并集”语义：副本分发/下载时不断补充 owners。
      MergeOwners(info.owners, &item.owners);
      if (item.source.empty() && !info.source.empty()) {
        item.source = info.source;
      }
      updated = true;
      break;
    }
  }
  if (!updated) {
    // 新 chunk：直接追加。
    existing.push_back(info);
  }
  return SaveChunkIndex(index_path, existing);
}

bool FindChunkPath(const std::vector<ChunkInfo>& chunks,
                   const std::string& chunk_id,
                   std::string* out_path) {
  if (!out_path) {
    return false;
  }
  for (const auto& info : chunks) {
    if (info.chunk_id == chunk_id) {
      *out_path = info.path;
      return true;
    }
  }
  return false;
}
