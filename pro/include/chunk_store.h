#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 分片元数据（用于索引、校验与传输）
 *
 * 该结构是“分片生命周期”的核心载体：
 * - 本地切分后：记录 chunk_id / path / size
 * - 多副本/下载后：owners 记录可提供该分片的节点地址列表（host:port）
 * - source 用于区分分片来源（上传产生 upload；副本落盘 replica；缺省用 "-"）
 */
struct ChunkInfo {
  /// 分片内容的 SHA-256（16 进制字符串）；也是分片文件名的一部分（<chunk_id>.chunk）。
  std::string chunk_id;

  /// 分片落盘的完整路径（绝对/相对取决于上层传入的 output_dir / configs）。
  std::string path;

  /// 分片字节数（落盘文件大小；用于统计/校验；不是 chunk_size 的上限配置）。
  std::size_t size = 0;

  /// 拥有该分片的节点列表（元素格式通常为 "127.0.0.1:PORT"）。
  std::vector<std::string> owners;

  /// 分片来源标签（典型值：upload / replica；空值在序列化时会归一化为 "-"）。
  std::string source;
};

/**
 * @brief 将输入文件按固定大小切分为若干 chunk 并落盘
 *
 * @param input_path       输入文件路径
 * @param output_dir       输出目录（函数会创建目录）
 * @param chunk_size_bytes 每个分片的目标大小（最后一个分片可能小于该值）
 * @param out_chunks       输出：按顺序返回每个分片的元数据（chunk_id/path/size）
 *
 * @return true 成功；false 失败（如文件无法打开/写入失败等）
 *
 * @note 分片文件名固定为：`<SHA-256(hex)>.chunk`，chunk_id 由分片内容计算得到。
 */
bool SplitFileToChunks(const std::string& input_path,
                       const std::string& output_dir,
                       std::size_t chunk_size_bytes,
                       std::vector<ChunkInfo>* out_chunks);

/**
 * @brief 校验分片：重新计算每个分片文件的 SHA-256，并与 ChunkInfo::chunk_id 比较
 *
 * @return true 全部匹配；false 任意不匹配/文件无法读取。
 */
bool VerifyChunks(const std::vector<ChunkInfo>& chunks);

/**
 * @brief 保存分片索引（TSV 文本）
 *
 * 行格式（TSV）：
 * - v1 最小字段：chunk_id<TAB>path<TAB>size
 * - 当前实现额外字段：owners_csv<TAB>source
 *
 * owners_csv：
 * - 多个 owner 使用逗号分隔
 * - 无 owner 时写 "-"（占位，便于反序列化）
 *
 * source：
 * - 空值写 "-"（占位）
 */
bool SaveChunkIndex(const std::string& index_path,
                    const std::vector<ChunkInfo>& chunks);

/**
 * @brief 加载分片索引（TSV 文本）
 *
 * 兼容：若行字段不足 3 个，则跳过该行。
 * 若文件不存在，则返回 false（并记录 warn），并清空 out_chunks。
 */
bool LoadChunkIndex(const std::string& index_path,
                    std::vector<ChunkInfo>* out_chunks);

/**
 * @brief 保存 manifest（顺序列表）
 *
 * manifest 只存 chunk_id（每行一个），用于“整文件组装下载”时维持 chunk 顺序。
 */
bool SaveChunkManifest(const std::string& manifest_path,
                       const std::vector<ChunkInfo>& chunks);

/**
 * @brief 读取 manifest（顺序列表）
 *
 * @return true 成功；false 文件不存在或不可读
 */
bool LoadChunkManifest(const std::string& manifest_path,
                       std::vector<std::string>* out_chunk_ids);

/**
 * @brief 将单个 chunk 数据落盘为 `<chunk_id>.chunk`
 *
 * @param chunk_id   已知 chunk_id（一般由上层计算/协议携带）
 * @param data       chunk 原始字节
 * @param output_dir 输出目录（函数会创建目录）
 * @param out_info   输出：chunk_id/path/size
 */
bool SaveChunkFile(const std::string& chunk_id,
                   const std::vector<uint8_t>& data,
                   const std::string& output_dir,
                   ChunkInfo* out_info);

/**
 * @brief Upsert 方式写入索引：存在则更新，不存在则追加
 *
 * 合并策略：
 * - path：若传入非空，则覆盖旧值
 * - size：若传入非 0，则覆盖旧值
 * - owners：做去重合并（保留旧 owners + 新 owners）
 * - source：若旧 source 为空且新 source 非空，则写入
 */
bool UpsertChunkIndex(const std::string& index_path,
                      const ChunkInfo& info);

/**
 * @brief 在已加载的 chunk 列表中，查找 chunk_id 对应的本地文件路径
 *
 * 这是 server 处理 GET_CHUNK/GET_CHUNK_SEC 时最常用的查找函数。
 */
bool FindChunkPath(const std::vector<ChunkInfo>& chunks,
                   const std::string& chunk_id,
                   std::string* out_path);
