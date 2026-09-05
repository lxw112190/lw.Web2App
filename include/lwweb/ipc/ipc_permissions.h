#pragma once

#include "lwweb/packer/manifest.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace lwweb {

bool HasIpcCapability(const IpcConfig& config, const std::string& capability);
bool IsAllowedIpcSource(const std::string& source_url,
                        const std::string& local_origin);

// Manifest 中声明的授权根。pending 根目录可以在 Runtime 启动时尚不存在，
// 但仍保留其词法边界与解析后的策略边界，目录后续创建时无需重启即可使用。
struct ConfiguredRootGrant {
  std::filesystem::path declared_root;
  std::filesystem::path policy_root;
  bool pending_at_start = false;
};

// 解析 Manifest 固定根目录并维护仅本次进程有效的目录选择授权。
// 每次文件操作都会重新规范化路径，以阻止 ..、符号链接和重解析点逃逸。
class IpcFilesystemPermissions {
 public:
  IpcFilesystemPermissions(std::vector<std::string> configured_roots,
                           std::string app_id);

  void AddSessionGrant(const std::filesystem::path& directory);
  std::filesystem::path RequireExisting(const std::string& utf8_path) const;
  std::filesystem::path RequireDestination(const std::string& utf8_path) const;
 std::vector<std::filesystem::path> Roots() const;

 private:
  bool IsAuthorized(const std::filesystem::path& requested,
                    const std::filesystem::path& resolved) const;
  std::vector<ConfiguredRootGrant> configured_roots_;
  mutable std::mutex mutex_;
  std::vector<std::filesystem::path> session_grants_;
};

}  // namespace lwweb
