#include "lwweb/runtime/resource_cache.h"

namespace lwweb {

ResourceCache::ResourceCache(std::size_t capacity_bytes) : capacity_(capacity_bytes) {}

std::optional<std::vector<std::uint8_t>> ResourceCache::Get(const std::string& key) {
  std::lock_guard lock(mutex_);
  const auto it = items_.find(key);
  if (it == items_.end()) return std::nullopt;
  order_.splice(order_.begin(), order_, it->second.position);
  return it->second.value;
}

void ResourceCache::Put(std::string key, std::vector<std::uint8_t> value) {
  if (value.size() > capacity_ / 4) return;
  std::lock_guard lock(mutex_);
  if (const auto existing = items_.find(key); existing != items_.end()) {
    size_ -= existing->second.value.size();
    order_.erase(existing->second.position);
    items_.erase(existing);
  }
  order_.push_front(key);
  size_ += value.size();
  items_.emplace(std::move(key), Item{std::move(value), order_.begin()});
  Evict();
}

void ResourceCache::Clear() {
  std::lock_guard lock(mutex_);
  items_.clear();
  order_.clear();
  size_ = 0;
}

void ResourceCache::Evict() {
  while (size_ > capacity_ && !order_.empty()) {
    auto it = items_.find(order_.back());
    if (it != items_.end()) {
      size_ -= it->second.value.size();
      items_.erase(it);
    }
    order_.pop_back();
  }
}

}  // namespace lwweb

