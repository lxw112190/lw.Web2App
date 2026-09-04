#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace lwweb {

// 轻量跨平台目录监听器。当前实现使用受限轮询，保证 Windows/Linux 行为一致；
// 事件只表示“需要重新扫描”的提示，不承诺数据库级别的变更记录。
class FileWatchService {
 public:
  using EventCallback = std::function<void(const std::string&, const nlohmann::json&)>;

  explicit FileWatchService(EventCallback callback);
  ~FileWatchService();
  FileWatchService(const FileWatchService&) = delete;
  FileWatchService& operator=(const FileWatchService&) = delete;

  std::string Watch(const std::filesystem::path& directory, bool recursive,
                    std::uint32_t debounce_ms = 150);
  bool Unwatch(const std::string& watcher_id);
  void StopAll();

 private:
  struct WatchState;
  void Run(const std::shared_ptr<WatchState>& state);

  EventCallback callback_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<WatchState>> watches_;
  std::atomic<std::uint64_t> sequence_{0};
};

}  // namespace lwweb
