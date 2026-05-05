#include "hex_utils.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace {
/**
 * @brief 将单个 hex 字符转换为数值 [0,15]
 *
 * @return -1 表示非法字符
 *
 * @note DecodeHex 允许大小写，内部通过 tolower 归一化。
 */
int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  return -1;
}
}  // namespace

std::string EncodeHex(const std::vector<uint8_t>& data) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const auto b : data) {
    // 高 4bit + 低 4bit 映射到 hex 字符
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

//已经完成  // 输入：待转换的十六进制字符串 // 输出：存放转换后二进制字节的指针
bool DecodeHex(const std::string& hex, std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  // hex 必须成对出现（每 2 个字符代表 1 字节）
  if (hex.size() % 2 != 0) {
    return false;
  }
  std::vector<uint8_t> data;
  data.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = HexValue(hex[i]);
    const int lo = HexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    // 还原 1 字节：hi 作为高 4bit，lo 作为低 4bit
    data.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  *out = std::move(data);
  return true;
}
