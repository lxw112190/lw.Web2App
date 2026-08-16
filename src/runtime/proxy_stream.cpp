#include "lwweb/runtime/proxy_stream.h"

#include <algorithm>
#include <stdexcept>

namespace lwweb {

ProxyStream::ProxyStream(std::size_t capacity) : capacity_(capacity) {
  if (!capacity_) throw std::invalid_argument("Proxy stream capacity must be positive");
}

bool ProxyStream::Push(const char* data, std::size_t length) {
  std::size_t offset = 0;
  while (offset < length) {
    std::unique_lock lock(mutex_);
    writable_.wait(lock, [this] {
      return canceled_ || failed_ || finished_ || buffered_ < capacity_;
    });
    if (canceled_ || failed_ || finished_) return false;
    const auto part = (std::min)(length - offset, capacity_ - buffered_);
    chunks_.emplace_back(data + offset, part);
    buffered_ += part;
    offset += part;
    lock.unlock();
    readable_.notify_one();
  }
  return true;
}

ProxyStreamReadResult ProxyStream::Read(std::string& chunk) {
  std::unique_lock lock(mutex_);
  readable_.wait(lock, [this] {
    return canceled_ || failed_ || finished_ || !chunks_.empty();
  });
  if (failed_ || canceled_) return ProxyStreamReadResult::Failed;
  if (chunks_.empty()) return ProxyStreamReadResult::Finished;
  chunk = std::move(chunks_.front());
  chunks_.pop_front();
  buffered_ -= chunk.size();
  lock.unlock();
  writable_.notify_one();
  return ProxyStreamReadResult::Data;
}

void ProxyStream::Finish() {
  {
    std::lock_guard lock(mutex_);
    if (!canceled_ && !failed_) finished_ = true;
  }
  readable_.notify_all();
  writable_.notify_all();
}

void ProxyStream::Fail() {
  {
    std::lock_guard lock(mutex_);
    failed_ = true;
    finished_ = true;
    chunks_.clear();
    buffered_ = 0;
  }
  readable_.notify_all();
  writable_.notify_all();
}

void ProxyStream::Cancel() {
  {
    std::lock_guard lock(mutex_);
    canceled_ = true;
    finished_ = true;
    chunks_.clear();
    buffered_ = 0;
  }
  readable_.notify_all();
  writable_.notify_all();
}

}  // namespace lwweb
