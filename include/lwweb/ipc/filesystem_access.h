#pragma once

#include "lwweb/ipc/ipc_permissions.h"

#include <memory>

#include <nlohmann/json.hpp>

namespace lwweb {

namespace ipc_detail {

// fs.move 在系统重命名报告跨文件系统错误后使用的保底实现。它先在目标
// 文件系统写入临时文件并发布到最终路径，发布成功后才删除源文件。
// 该函数独立暴露仅用于对这条低频分支做确定性的单元测试。
void MoveRegularFileByCopyAndDelete(const std::filesystem::path& source,
                                    const std::filesystem::path& destination,
                                    bool overwrite);

}  // namespace ipc_detail

// 跨平台的受控文件系统服务。所有公开方法都先通过
// IpcFilesystemPermissions 重新校验路径，再执行实际磁盘操作。
class IpcFilesystemAccess {
 public:
  explicit IpcFilesystemAccess(
      std::shared_ptr<IpcFilesystemPermissions> permissions);

  void GrantDirectory(const std::filesystem::path& directory);
  std::filesystem::path OpenReadPath(const nlohmann::json& params) const;
  std::filesystem::path TrashPath(const nlohmann::json& params) const;
  nlohmann::json Exists(const nlohmann::json& params) const;
  nlohmann::json List(const nlohmann::json& params) const;
  nlohmann::json MakeDirectory(const nlohmann::json& params) const;
  nlohmann::json Copy(const nlohmann::json& params) const;
  nlohmann::json Move(const nlohmann::json& params) const;
  nlohmann::json Delete(const nlohmann::json& params) const;

 private:
  std::shared_ptr<IpcFilesystemPermissions> permissions_;
};

}  // namespace lwweb
