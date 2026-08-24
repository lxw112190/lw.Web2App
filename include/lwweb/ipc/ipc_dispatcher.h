#pragma once

#include "lwweb/ipc/filesystem_access.h"
#include "lwweb/ipc/ipc_message.h"
#include "lwweb/packer/manifest.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lwweb {

enum class IpcExecution { Immediate, Worker, UiThread };

// 系统文件选择窗口使用的安全过滤器；extensions 不包含通配符或路径字符。
struct OpenFileFilter {
  std::string name;
  std::vector<std::string> extensions;
};

// dialog.openFile 的平台无关参数。无论单选或多选，IPC 统一返回 files[]。
struct OpenFileDialogOptions {
  bool multiple = false;
  std::vector<OpenFileFilter> filters;
};

class LocalFileGrantManager;

struct IpcRuntimeServices {
  std::string platform;
  std::string arch = "x64";
  std::string runtime_version;
  std::function<std::optional<std::filesystem::path>()> select_directory;
  std::function<std::optional<std::vector<std::filesystem::path>>(
      const OpenFileDialogOptions&)> open_files;
  std::function<void(const std::filesystem::path&)> trash_file;
  std::shared_ptr<LocalFileGrantManager> file_grants;
};

// 共享的请求校验、Capability 判定和方法分发器。WebView2/WebKitGTK 仅负责
// 传输 JSON；平台 UI 对话框通过 IpcRuntimeServices 注入。
class IpcDispatcher {
 public:
  IpcDispatcher(Manifest manifest, IpcRuntimeServices services);

  IpcResponse Dispatch(const IpcRequest& request);
  IpcExecution ExecutionFor(const std::string& method) const;
  bool TryBegin(const std::string& id);
  void End(const std::string& id);
  bool Enabled() const { return manifest_.ipc.enabled; }

 private:
  void RequireCapability(const std::string& capability) const;
  nlohmann::json DispatchImpl(const IpcRequest& request);

  Manifest manifest_;
  IpcRuntimeServices services_;
  std::shared_ptr<IpcFilesystemAccess> filesystem_;
  mutable std::mutex pending_mutex_;
  std::set<std::string> pending_ids_;
};

// 注入页面的稳定 JavaScript API。transport 为 "windows" 或 "linux"。
std::string BuildIpcBridgeScript(const std::string& transport,
                                 const std::string& platform,
                                 const std::string& transport_token = {});

}  // namespace lwweb
