#include "crypto_utils.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdint>
#include <vector>

namespace {
bool BytesEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  bool ok = true;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ok = ok && (a[i] == b[i]);
  }
  return ok;
}
}  // namespace

bool GenerateRandomBytes(std::size_t len, std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  out->assign(len, 0);
  if (len == 0) {
    return true;
  }
  const int rc = RAND_bytes(reinterpret_cast<unsigned char*>(out->data()),
                            static_cast<int>(len));
  return rc == 1;
}

bool Aes128CbcEncrypt(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& iv,
                      const std::vector<uint8_t>& plaintext,
                      std::vector<uint8_t>* ciphertext) {
  if (!ciphertext || key.size() != 16 || iv.size() != 16) {
    return false;
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }

  bool ok = true;
  int out_len1 = 0;
  int out_len2 = 0;
  std::vector<uint8_t> out(plaintext.size() + 16);  // 预留 padding

  if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                         key.data(), iv.data()) != 1) {
    ok = false;
  }

  if (ok && EVP_EncryptUpdate(ctx,
                              reinterpret_cast<unsigned char*>(out.data()),
                              &out_len1,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              static_cast<int>(plaintext.size())) != 1) {
    ok = false;
  }

  if (ok && EVP_EncryptFinal_ex(ctx,
                                reinterpret_cast<unsigned char*>(out.data()) + out_len1,
                                &out_len2) != 1) {
    ok = false;
  }

  EVP_CIPHER_CTX_free(ctx);

  if (!ok) {
    return false;
  }

  out.resize(static_cast<std::size_t>(out_len1 + out_len2));
  *ciphertext = std::move(out);
  return true;
}

bool Aes128CbcDecrypt(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& iv,
                      const std::vector<uint8_t>& ciphertext,
                      std::vector<uint8_t>* plaintext) {
  if (!plaintext || key.size() != 16 || iv.size() != 16) {
    return false;
  }

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    return false;
  }

  bool ok = true;
  int out_len1 = 0;
  int out_len2 = 0;
  std::vector<uint8_t> out(ciphertext.size());

  if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                         key.data(), iv.data()) != 1) {
    ok = false;
  }

  if (ok && EVP_DecryptUpdate(ctx,
                              reinterpret_cast<unsigned char*>(out.data()),
                              &out_len1,
                              reinterpret_cast<const unsigned char*>(ciphertext.data()),
                              static_cast<int>(ciphertext.size())) != 1) {
    ok = false;
  }

  if (ok && EVP_DecryptFinal_ex(ctx,
                                reinterpret_cast<unsigned char*>(out.data()) + out_len1,
                                &out_len2) != 1) {
    ok = false;
  }

  EVP_CIPHER_CTX_free(ctx);

  if (!ok) {
    return false;
  }

  out.resize(static_cast<std::size_t>(out_len1 + out_len2));
  *plaintext = std::move(out);
  return true;
}

bool HmacSha256(const std::vector<uint8_t>& key,
                const std::vector<uint8_t>& data,
                std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  unsigned int len = 0;
  std::vector<uint8_t> digest(EVP_MAX_MD_SIZE, 0);
  unsigned char* rc = HMAC(EVP_sha256(),
                           key.data(),
                           static_cast<int>(key.size()),
                           data.data(),
                           data.size(),
                           digest.data(),
                           &len);
  if (!rc || len == 0) {
    return false;
  }
  digest.resize(len);
  *out = std::move(digest);
  return true;
}

bool VerifyHmacSha256(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& data,
                      const std::vector<uint8_t>& expected) {
  std::vector<uint8_t> actual;
  if (!HmacSha256(key, data, &actual)) {
    return false;
  }
  return BytesEqual(actual, expected);
}
