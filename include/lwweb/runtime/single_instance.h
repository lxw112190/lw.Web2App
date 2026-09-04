#pragma once

#include <memory>
#include <string>

namespace lwweb {

// 为生成后的应用提供与 HTTP 端口无关的真正单实例锁。
// Windows 使用命名 Mutex，并在重复启动时通知已有窗口；Linux 使用进程退出时自动释放的 flock。
class SingleInstanceGuard {
 public:
  explicit SingleInstanceGuard(const std::string& app_id);
  ~SingleInstanceGuard();
  // 返回当前进程是否持有主实例锁；重复启动时会通知已有实例后返回 false。
  bool IsPrimary() const noexcept;
  SingleInstanceGuard(const SingleInstanceGuard&) = delete;
  SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lwweb
