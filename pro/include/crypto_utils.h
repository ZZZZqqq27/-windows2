#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// 生成指定长度的随机字节（常用于 IV/nonce）
// 返回 true 表示生成成功
bool GenerateRandomBytes(std::size_t len, std::vector<uint8_t>* out);

// AES-128-CBC 加密
// - key 必须为 16 字节
// - iv 必须为 16 字节
// - plaintext 为任意长度明文
// - ciphertext 为输出密文（包含 PKCS#7 padding 后的结果）
bool Aes128CbcEncrypt(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& iv,
                      const std::vector<uint8_t>& plaintext,
                      std::vector<uint8_t>* ciphertext);

// AES-128-CBC 解密
// - key/iv 同加密要求
// - ciphertext 为密文
// - plaintext 为解密后的明文（已去除 PKCS#7 padding）
bool Aes128CbcDecrypt(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& iv,
                      const std::vector<uint8_t>& ciphertext,
                      std::vector<uint8_t>* plaintext);

// 计算 HMAC-SHA256
// - key 为 HMAC 密钥
// - data 为输入数据（可为密文/明文）
// - out 为输出摘要（32 字节）
bool HmacSha256(const std::vector<uint8_t>& key,
                const std::vector<uint8_t>& data,
                std::vector<uint8_t>* out);

// 校验 HMAC-SHA256 是否匹配
// - expected 为已有的摘要
bool VerifyHmacSha256(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& data,
                      const std::vector<uint8_t>& expected);
