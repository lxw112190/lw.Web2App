#pragma once

#include <memory>
#include <string>

namespace lwweb {

// 为生成后的应用提供与 HTTP 端口无关的真正单实例锁。
// Windows 使用命名 Mutex，Linux 使用进程退出时自动释放的 flock。
class SingleInstanceGuard {
 public:
  explicit SingleInstanceGuard(const std::string& app_id);
  ~SingleInstanceGuard();
  SingleInstanceGuard(const SingleInstanceGuard&) = delete;
  SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lwweb
