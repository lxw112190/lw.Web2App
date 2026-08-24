#pragma once

#include "lwweb/ipc/ipc_permissions.h"

#include <memory>

#include <nlohmann/json.hpp>

namespace lwweb {

// 跨平台的受控文件系统服务。所有公开方法都先通过
// IpcFilesystemPermissions 重新校验路径，再执行实际磁盘操作。
class IpcFilesystemAccess {
 public:
  explicit IpcFilesystemAccess(
      std::shared_ptr<IpcFilesystemPermissions> permissions);

  void GrantDirectory(const std::filesystem::path& directory);
  nlohmann::json Exists(const nlohmann::json& params) const;
  nlohmann::json List(const nlohmann::json& params) const;
  nlohmann::json Copy(const nlohmann::json& params) const;
  nlohmann::json Move(const nlohmann::json& params) const;
  nlohmann::json Delete(const nlohmann::json& params) const;

 private:
  std::shared_ptr<IpcFilesystemPermissions> permissions_;
};

}  // namespace lwweb
