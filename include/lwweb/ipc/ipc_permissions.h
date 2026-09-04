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
  bool IsWithinRoots(const std::filesystem::path& canonical) const;
  std::vector<std::filesystem::path> configured_roots_;
  mutable std::mutex mutex_;
  std::vector<std::filesystem::path> session_grants_;
};

}  // namespace lwweb
