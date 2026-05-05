#include "routing_table.h"

#include <algorithm>
#include <functional>

namespace {
std::uint64_t Hash64(const std::string& s) {
  return static_cast<std::uint64_t>(std::hash<std::string>{}(s));
}
}  // namespace

RoutingTable::RoutingTable(const std::string& self_id, std::size_t capacity)
    : self_id_(self_id),
      capacity_(capacity),
      buckets_(64) {}

std::uint64_t RoutingTable::SelfHash() const {
  return Hash64(self_id_);
}

std::uint64_t RoutingTable::Distance(const std::string& key) const {
  return SelfHash() ^ Hash64(key);
}

std::size_t RoutingTable::BucketIndex(std::uint64_t dist) const {
  if (dist == 0) {
    return 0;
  }
  for (int i = 63; i >= 0; --i) {
    if ((dist >> i) & 1ULL) {
      return static_cast<std::size_t>(i);
    }
  }
  return 0;
}

bool RoutingTable::AddNode(const std::string& key) {
  if (key == self_id_) {
    return false;
  }
  const std::uint64_t dist = Distance(key);
  const std::size_t idx = BucketIndex(dist);
  auto& bucket = buckets_[idx];
  for (const auto& n : bucket) {
    if (n.key == key) {
      return false;
    }
  }
  if (bucket.size() >= capacity_) {
    return false;
  }
  NodeInfo info;
  info.key = key;
  info.dist = dist;
  bucket.push_back(info);
  return true;
}

bool RoutingTable::RemoveNode(const std::string& key) {
  bool removed = false;
  for (auto& bucket : buckets_) {
    const auto it = std::remove_if(bucket.begin(), bucket.end(),
                                   [&key](const NodeInfo& n) { return n.key == key; });
    if (it != bucket.end()) {
      bucket.erase(it, bucket.end());
      removed = true;
    }
  }
  return removed;
}

std::vector<NodeInfo> RoutingTable::Snapshot() const {
  std::vector<NodeInfo> out;
  for (const auto& bucket : buckets_) {
    for (const auto& n : bucket) {
      out.push_back(n);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const NodeInfo& a, const NodeInfo& b) {
              return a.dist < b.dist;
            });
  return out;
}

std::vector<std::string> RoutingTable::TopKKeys(std::size_t k) const {
  const auto snap = Snapshot();
  std::vector<std::string> out;
  const std::size_t n = std::min(k, snap.size());
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(snap[i].key);
  }
  return out;
}
//------已完成 从本地路由表的所有节点中，找出离目标 target 最近的 k 个节点
std::vector<std::string> RoutingTable::TopKClosestKeys(const std::string& target,
                                                       std::size_t k) const {
  struct Item {
    std::string key;
    std::uint64_t dist;
  };
  const std::uint64_t target_hash = Hash64(target);
  std::vector<Item> items;
  for (const auto& bucket : buckets_) {
    for (const auto& n : bucket) {
      Item it;
      it.key = n.key;
      it.dist = target_hash ^ Hash64(n.key);
      items.push_back(it);
    }
  }
  std::sort(items.begin(), items.end(),
            [](const Item& a, const Item& b) { return a.dist < b.dist; });
  std::vector<std::string> out;
  const std::size_t n = std::min(k, items.size());
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(items[i].key);
  }
  return out;
}
