#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 将二进制数据编码为十六进制字符串（小写）
 *
 * 用途：
 * - 通过文本协议传输二进制内容（如 PUT_CHUNK_HEX / PUT_CHUNK_HEX2）
 * - 便于日志与调试观察
 *
 * @return 长度恒为 `data.size() * 2` 的小写 hex 字符串
 */
std::string EncodeHex(const std::vector<uint8_t>& data);

/**
 * @brief 将十六进制字符串解码为二进制数据
 *
 * 容错与约束：
 * - 输入长度必须为偶数
 * - 支持大小写十六进制字符（内部统一 tolower）
 * - 任意非法字符返回 false
 */
bool DecodeHex(const std::string& hex, std::vector<uint8_t>* out);
