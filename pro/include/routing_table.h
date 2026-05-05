#pragma once
#include <cstdint>
#include <string>
#include <vector>
// 路由表中的节点信息（简化版）
struct NodeInfo {
  std::string key;      // 节点唯一标识（这里用地址字符串）
  std::uint64_t dist;   // 与本节点的距离（简化：hash xor）
};
// 简化路由表（k-bucket 结构）
// 规则：
// - 节点去重
// - 每个 bucket 容量限制
// - 距离：使用 std::hash 简化为 64 位距离
class RoutingTable {
 public:
  RoutingTable(const std::string& self_id, std::size_t capacity);
  // 添加节点（去重 + 容量限制）
  bool AddNode(const std::string& key);
  // 删除节点
  bool RemoveNode(const std::string& key);
  // 获取当前路由表快照（按距离升序）
  std::vector<NodeInfo> Snapshot() const;
  // 获取距离最近的若干节点 key（用于候选列表）
  std::vector<std::string> TopKKeys(std::size_t k) const;
  // 获取距离 target 最近的若干节点 key（用于 DHT 查找）
  std::vector<std::string> TopKClosestKeys(const std::string& target,
                                           std::size_t k) const;
 private:
  std::uint64_t SelfHash() const;
  std::uint64_t Distance(const std::string& key) const;
  std::size_t BucketIndex(std::uint64_t dist) const;
  std::string self_id_;
  std::size_t capacity_;
  std::vector<std::vector<NodeInfo>> buckets_;
};
