#pragma once

#include "lwweb/ipc/ipc_message.h"
#include "lwweb/ipc/ipc_permissions.h"
#include "lwweb/packer/manifest.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace lwweb {

enum class IpcExecution { Immediate, Worker, UiThread };

struct IpcRuntimeServices {
  std::string platform;
  std::string arch = "x64";
  std::string runtime_version;
  std::function<std::optional<std::filesystem::path>()> select_directory;
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
  std::shared_ptr<IpcFilesystemPermissions> filesystem_;
  mutable std::mutex pending_mutex_;
  std::set<std::string> pending_ids_;
};

// 注入页面的稳定 JavaScript API。transport 为 "windows" 或 "linux"。
std::string BuildIpcBridgeScript(const std::string& transport,
                                 const std::string& platform,
                                 const std::string& transport_token = {});

}  // namespace lwweb
