#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 计算输入数据的 SHA-256（小写 hex 字符串）
 *
 * 核心用途：
 * - 生成 chunk_id（内容寻址键）
 * - 在 VerifyChunks 中做完整性校验
 *
 * @return 长度固定 64 的小写 hex 字符串
 */
std::string Sha256Hex(const std::vector<uint8_t>& data);

/**
 * @brief 计算输入缓冲区的 SHA-256（小写 hex 字符串）
 *
 * @param data 指向原始字节缓冲区
 * @param size 缓冲区长度
 */
std::string Sha256Hex(const uint8_t* data, std::size_t size);
