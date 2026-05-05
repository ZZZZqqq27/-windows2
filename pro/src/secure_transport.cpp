#include "secure_transport.h"

#include "crypto_utils.h"
#include "hex_utils.h"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kIvSize = 16;
constexpr std::size_t kHmacSize = 32;

bool BuildMacInput(const std::vector<uint8_t>& iv,
                   const std::vector<uint8_t>& ciphertext,
                   std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  out->clear();
  out->reserve(iv.size() + ciphertext.size());
  out->insert(out->end(), iv.begin(), iv.end());
  out->insert(out->end(), ciphertext.begin(), ciphertext.end());
  return true;
}

bool SplitBySpace(const std::string& text, std::vector<std::string>* out) {
  if (!out) {
    return false;
  }
  out->clear();
  std::istringstream iss(text);
  std::string item;
  while (iss >> item) {
    out->push_back(item);
  }
  return true;
}
}  // namespace
//------对外的api 生成随机数IV ,构建加密信封
bool BuildSecureEnvelope(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const std::vector<uint8_t>& plaintext,
                         SecureEnvelope* out) {
  if (!out) {
    return false;
  }
  std::vector<uint8_t> iv;
  if (!GenerateRandomBytes(kIvSize, &iv)) {
    return false;
  }
  return BuildSecureEnvelopeWithIv(key, hmac_key, iv, plaintext, out);
}

//------对外的api 手动指定IV(一般不用)
bool BuildSecureEnvelopeWithIv(const std::vector<uint8_t>& key,
                               const std::vector<uint8_t>& hmac_key,
                               const std::vector<uint8_t>& iv,
                               const std::vector<uint8_t>& plaintext,
                               SecureEnvelope* out) {
  if (!out || iv.size() != kIvSize) {
    return false;
  }
  std::vector<uint8_t> ciphertext;
  if (!Aes128CbcEncrypt(key, iv, plaintext, &ciphertext)) {
    return false;
  }
  std::vector<uint8_t> mac_input;
  if (!BuildMacInput(iv, ciphertext, &mac_input)) {
    return false;
  }
  std::vector<uint8_t> mac;
  if (!HmacSha256(hmac_key, mac_input, &mac)) {
    return false;
  }
  SecureEnvelope env;
  env.iv = iv;
  env.ciphertext = std::move(ciphertext);
  env.hmac = std::move(mac);
  *out = std::move(env);
  return true;
}

//------对外的api 验签+解密
bool ParseSecureEnvelope(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const SecureEnvelope& env,
                         std::vector<uint8_t>* plaintext) {
  if (!plaintext || env.iv.size() != kIvSize || env.hmac.size() != kHmacSize) {
    return false;
  }
  std::vector<uint8_t> mac_input;
  if (!BuildMacInput(env.iv, env.ciphertext, &mac_input)) {
    return false;
  }
  if (!VerifyHmacSha256(hmac_key, mac_input, env.hmac)) {
    return false;
  }
  return Aes128CbcDecrypt(key, env.iv, env.ciphertext, plaintext);
}
//------对外的api 
bool EncodeSecureEnvelopeToText(const SecureEnvelope& env, std::string* out) {
  if (!out) {
    return false;
  }
  if (env.iv.size() != kIvSize || env.hmac.size() != kHmacSize ||
      env.ciphertext.empty()) {
    return false;
  }
  const std::string iv_hex = EncodeHex(env.iv);
  const std::string hmac_hex = EncodeHex(env.hmac);
  const std::string cipher_hex = EncodeHex(env.ciphertext);
  *out = "ENC1 " + iv_hex + " " + hmac_hex + " " + cipher_hex;
  return true;
}

//------对外的api 从文本解析出信封结构
bool DecodeSecureEnvelopeFromText(const std::string& text, SecureEnvelope* out) {
  if (!out) {
    return false;
  }
  std::vector<std::string> parts;
  if (!SplitBySpace(text, &parts)) {
    return false;
  }
  if (parts.size() != 4 || parts[0] != "ENC1") {
    return false;
  }
  SecureEnvelope env;
  if (!DecodeHex(parts[1], &env.iv)) {
    return false;
  }
  if (!DecodeHex(parts[2], &env.hmac)) {
    return false;
  }
  if (!DecodeHex(parts[3], &env.ciphertext)) {
    return false;
  }
  *out = std::move(env);
  return true;
}

//------对外的api 高级借口：直接加密-》输出安全字符串
bool EncryptToSecureText(const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& hmac_key,
                         const std::vector<uint8_t>& plaintext,
                         std::string* out_text) {
  if (!out_text) {
    return false;
  }
  SecureEnvelope env;
  if (!BuildSecureEnvelope(key, hmac_key, plaintext, &env)) {
    return false;
  }
  return EncodeSecureEnvelopeToText(env, out_text);
}
 
//------对外的api 高级借口：直接输入安全字符串->输出明文
bool DecryptFromSecureText(const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& hmac_key,
                           const std::string& text,
                           std::vector<uint8_t>* plaintext) {
  if (!plaintext) {
    return false;
  }
  SecureEnvelope env;
  if (!DecodeSecureEnvelopeFromText(text, &env)) {
    return false;
  }
  return ParseSecureEnvelope(key, hmac_key, env, plaintext);
}
