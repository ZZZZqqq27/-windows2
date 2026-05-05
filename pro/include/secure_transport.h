#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 简化版加密信封：
// - iv: 16 字节
// - hmac: HMAC-SHA256 摘要（32 字节）
// - ciphertext: AES-128-CBC 密文
struct SecureEnvelope {
  std::vector<uint8_t> iv;
  std::vector<uint8_t> hmac;
  std::vector<uint8_t> ciphertext;
};

// 构建加密信封（随机 iv）
bool BuildSecureEnvelope(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const std::vector<uint8_t>& plaintext,
                         SecureEnvelope* out);

// 构建加密信封（指定 iv，便于测试）
bool BuildSecureEnvelopeWithIv(const std::vector<uint8_t>& key,
                               const std::vector<uint8_t>& hmac_key,
                               const std::vector<uint8_t>& iv,
                               const std::vector<uint8_t>& plaintext,
                               SecureEnvelope* out);

// 校验 HMAC 并解密
bool ParseSecureEnvelope(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const SecureEnvelope& env,
                         std::vector<uint8_t>* plaintext);

// 信封编码为可打印文本：
// "ENC1 <iv_hex> <hmac_hex> <cipher_hex>"
bool EncodeSecureEnvelopeToText(const SecureEnvelope& env, std::string* out);

// 从可打印文本解析信封
bool DecodeSecureEnvelopeFromText(const std::string& text, SecureEnvelope* out);

// 便捷接口：明文 -> 文本信封（随机 iv）
bool EncryptToSecureText(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const std::vector<uint8_t>& plaintext,
                         std::string* out_text);

// 便捷接口：文本信封 -> 明文
bool DecryptFromSecureText(const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& hmac_key,
                           const std::string& text,
                           std::vector<uint8_t>* plaintext);
