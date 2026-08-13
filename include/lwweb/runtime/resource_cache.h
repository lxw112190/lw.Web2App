#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lwweb {

// 线程安全、按字节容量淘汰的 LRU 资源缓存。
// 仅缓存小型热点资源，大文件由 ZipResourceStore 每次按需解压。
class ResourceCache {
 public:
  explicit ResourceCache(std::size_t capacity_bytes = 32 * 1024 * 1024);
  std::optional<std::vector<std::uint8_t>> Get(const std::string& key);
  void Put(std::string key, std::vector<std::uint8_t> value);
  void Clear();

 private:
  // 缓存条目及其在最近使用顺序链表中的位置。
  struct Item {
    std::vector<std::uint8_t> value;
    std::list<std::string>::iterator position;
  };
  void Evict();

  std::size_t capacity_;
  std::size_t size_ = 0;
  std::list<std::string> order_;
  std::unordered_map<std::string, Item> items_;
  std::mutex mutex_;
};

}  // namespace lwweb
