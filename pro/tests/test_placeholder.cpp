// MVP2 测试入口：分片、校验、索引保存与加载
// 说明：轻量测试，不引入第三方测试框架

#include "chunk_store.h"
#include "config.h"
#include "crypto_utils.h"
#include "logger.h"
#include "routing_table.h"
#include "secure_transport.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
// 简单断言宏：失败即返回非 0
#define REQUIRE_TRUE(cond)            \
  do {                                \
    if (!(cond)) {                    \
      LogError("test failed: " #cond);\
      return 1;                       \
    }                                 \
  } while (0)

std::string WriteSampleFile(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  out << "abcdefg\n";
  out << "hijklmn\n";
  out << "opqrstu\n";
  return path.string();
}

int TestSplitAndVerifyMvp1(const std::filesystem::path& base_dir) {
  LogInfo("test: mvp1 split and verify");
  const auto input = WriteSampleFile(base_dir / "input_mvp1.txt");
  const auto chunks_dir = base_dir / "chunks";

  std::vector<ChunkInfo> chunks;
  const std::size_t chunk_size = 8;  // 小分片便于测试
  REQUIRE_TRUE(SplitFileToChunks(input, chunks_dir.string(), chunk_size, &chunks));
  REQUIRE_TRUE(!chunks.empty());
  REQUIRE_TRUE(VerifyChunks(chunks));
  return 0;
}

int TestIndexSaveLoadMvp2(const std::filesystem::path& base_dir) {
  LogInfo("test: mvp2 index save and load");
  const auto input = WriteSampleFile(base_dir / "input_mvp2.txt");
  const auto chunks_dir = base_dir / "chunks";
  const auto index_file = base_dir / "chunk_index.tsv";

  std::vector<ChunkInfo> chunks;
  const std::size_t chunk_size = 8;
  REQUIRE_TRUE(SplitFileToChunks(input, chunks_dir.string(), chunk_size, &chunks));
  REQUIRE_TRUE(SaveChunkIndex(index_file.string(), chunks));

  std::vector<ChunkInfo> loaded;
  REQUIRE_TRUE(LoadChunkIndex(index_file.string(), &loaded));
  REQUIRE_TRUE(!loaded.empty());
  REQUIRE_TRUE(VerifyChunks(loaded));
  return 0;
}

int TestMissingInput() {
  LogInfo("test: missing input file");
  std::vector<ChunkInfo> chunks;
  const bool ok = SplitFileToChunks("no_such_file.txt", "data/nope", 8, &chunks);
  REQUIRE_TRUE(!ok);
  return 0;
}

int TestSeedNodesMvp3(const std::filesystem::path& base_dir) {
  LogInfo("test: mvp3 seed_nodes parse");
  const auto config_path = base_dir / "config_mvp3.yaml";
  std::ofstream out(config_path);
  out << "node_id: \"node-test\"\n";
  out << "seed_nodes: \"127.0.0.1:9001,127.0.0.1:9002\"\n";
  out.close();

  const AppConfig cfg = LoadConfig(config_path.string());
  REQUIRE_TRUE(cfg.node_id == "node-test");
  REQUIRE_TRUE(cfg.seed_nodes.size() == 2);
  REQUIRE_TRUE(cfg.seed_nodes[0] == "127.0.0.1:9001");
  REQUIRE_TRUE(cfg.seed_nodes[1] == "127.0.0.1:9002");
  return 0;
}

int TestRoutingTableMvp3() {
  LogInfo("test: mvp3 routing table add/remove");
  RoutingTable table("node-self", 2);
  REQUIRE_TRUE(table.AddNode("127.0.0.1:9001"));
  REQUIRE_TRUE(table.AddNode("127.0.0.1:9002"));
  table.AddNode("127.0.0.1:9003");
  table.AddNode("127.0.0.1:9004");
  const auto snapshot = table.Snapshot();
  REQUIRE_TRUE(snapshot.size() >= 2);
  REQUIRE_TRUE(snapshot.size() <= 4);
  const auto top_keys = table.TopKKeys(3);
  REQUIRE_TRUE(top_keys.size() <= 3);
  const auto closest = table.TopKClosestKeys("target-key", 2);
  REQUIRE_TRUE(closest.size() <= 2);
  REQUIRE_TRUE(table.RemoveNode("127.0.0.1:9002") || table.RemoveNode("127.0.0.1:9001") ||
              table.RemoveNode("127.0.0.1:9003") || table.RemoveNode("127.0.0.1:9004"));
  return 0;
}

int TestCryptoMvp4() {
  LogInfo("test: mvp4 aes-128-cbc + hmac-sha256");
  const std::vector<uint8_t> key(16, 0x11);
  const std::vector<uint8_t> iv(16, 0x22);
  const std::vector<uint8_t> hmac_key(16, 0x33);
  const std::string plain_text = "hello mvp4 crypto";
  const std::vector<uint8_t> plaintext(plain_text.begin(), plain_text.end());

  std::vector<uint8_t> ciphertext;
  REQUIRE_TRUE(Aes128CbcEncrypt(key, iv, plaintext, &ciphertext));
  REQUIRE_TRUE(!ciphertext.empty());

  std::vector<uint8_t> mac;
  REQUIRE_TRUE(HmacSha256(hmac_key, ciphertext, &mac));
  REQUIRE_TRUE(VerifyHmacSha256(hmac_key, ciphertext, mac));

  std::vector<uint8_t> decrypted;
  REQUIRE_TRUE(Aes128CbcDecrypt(key, iv, ciphertext, &decrypted));
  REQUIRE_TRUE(decrypted == plaintext);

  // 篡改密文后 HMAC 应该校验失败
  std::vector<uint8_t> tampered = ciphertext;
  tampered[0] ^= 0x01;
  REQUIRE_TRUE(!VerifyHmacSha256(hmac_key, tampered, mac));
  return 0;
}

int TestSecureEnvelopeMvp4() {
  LogInfo("test: mvp4 secure envelope encode/decode");
  const std::vector<uint8_t> key(16, 0x10);
  const std::vector<uint8_t> hmac_key(16, 0x20);
  const std::vector<uint8_t> iv(16, 0x30);
  const std::string plain_text = "secure envelope payload";
  const std::vector<uint8_t> plaintext(plain_text.begin(), plain_text.end());

  SecureEnvelope env;
  REQUIRE_TRUE(BuildSecureEnvelopeWithIv(key, hmac_key, iv, plaintext, &env));

  std::string text;
  REQUIRE_TRUE(EncodeSecureEnvelopeToText(env, &text));
  REQUIRE_TRUE(!text.empty());

  SecureEnvelope parsed;
  REQUIRE_TRUE(DecodeSecureEnvelopeFromText(text, &parsed));

  std::vector<uint8_t> decrypted;
  REQUIRE_TRUE(ParseSecureEnvelope(key, hmac_key, parsed, &decrypted));
  REQUIRE_TRUE(decrypted == plaintext);

  // 篡改文本中的密文后解密应失败
  std::string tampered = text;
  if (tampered.size() > 5) {
    tampered[tampered.size() - 2] = (tampered[tampered.size() - 2] == 'a') ? 'b' : 'a';
  }
  REQUIRE_TRUE(!DecryptFromSecureText(key, hmac_key, tampered, &decrypted));
  return 0;
}
}  // namespace

int main() {
  LoggerConfig cfg;
  cfg.level = LogLevel::Info;
  InitLogger(cfg);

  const std::filesystem::path base_dir_mvp1 = "data/test_mvp1";
  const std::filesystem::path base_dir_mvp2 = "data/test_mvp2";
  const std::filesystem::path base_dir_mvp3 = "data/test_mvp3";
  std::filesystem::remove_all(base_dir_mvp1);
  std::filesystem::remove_all(base_dir_mvp2);
  std::filesystem::remove_all(base_dir_mvp3);
  std::filesystem::create_directories(base_dir_mvp1);
  std::filesystem::create_directories(base_dir_mvp2);
  std::filesystem::create_directories(base_dir_mvp3);

  if (TestSplitAndVerifyMvp1(base_dir_mvp1) != 0) {
    return 1;
  }
  if (TestIndexSaveLoadMvp2(base_dir_mvp2) != 0) {
    return 1;
  }
  if (TestMissingInput() != 0) {
    return 1;
  }
  if (TestSeedNodesMvp3(base_dir_mvp3) != 0) {
    return 1;
  }
  if (TestRoutingTableMvp3() != 0) {
    return 1;
  }
  if (TestCryptoMvp4() != 0) {
    return 1;
  }
  if (TestSecureEnvelopeMvp4() != 0) {
    return 1;
  }

  LogInfo("all tests passed");
  return 0;
}
