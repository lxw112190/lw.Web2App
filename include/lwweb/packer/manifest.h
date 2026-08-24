#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lwweb {

enum class AppMode { Local, Url };

// 控制生成应用的滚动文件日志；默认最多占用约 10 MiB 磁盘空间。
struct LoggingConfig {
  bool enabled = true;
  std::string level = "info";
  std::uint64_t max_file_size = 2ull * 1024 * 1024;
  std::uint32_t max_files = 5;
};

// 将页面的同源请求受控转发到一个固定的传统 HTTP 后台。
// prefix 不暴露任意目标地址，避免把桌面应用变成通用 SSRF 代理。
struct BackendProxyConfig {
  bool enabled = false;
  std::string origin;
  std::string prefix = "/__lw_proxy__";
  std::uint32_t connect_timeout_ms = 5000;
  std::uint32_t read_timeout_ms = 30000;
  std::uint64_t max_request_size = 16ull * 1024 * 1024;
  std::uint64_t max_response_size = 64ull * 1024 * 1024;
};

// 为本地页面开放的受控 Native IPC 能力。能力和文件系统根目录均由打包时固定，
// Runtime 不接受网页临时扩大权限；系统目录选择器产生的授权只在本次会话有效。
struct IpcConfig {
  bool enabled = false;
  std::vector<std::string> capabilities;
  std::vector<std::string> filesystem_roots;
};

// 描述生成后桌面应用的运行方式和窗口行为。
// 该结构与 Payload 中的 manifest.json 一一对应；新增字段必须保持向后兼容。
struct Manifest {
  std::string format = "lw-web-app";
  std::uint32_t version = 1;
  AppMode mode = AppMode::Local;
  std::string app_id;
  // entry 是 ZIP 中真实存在的 HTML；start_path 是 WebView 首次打开的本地 URL 路径。
  std::string entry = "index.html";
  std::string start_path = "/";
  std::string url;
  std::string title = "lw.Web2App App";
  std::uint32_t width = 1280;
  std::uint32_t height = 800;
  bool resizable = true;
  bool fullscreen = true;
  bool devtools = false;
  BackendProxyConfig backend_proxy;
  IpcConfig ipc;
  bool spa_fallback = true;
  LoggingConfig logging;
  // 仅用于读取 LWWEB001；LWWEB002 的内容摘要存放在 Footer，避免循环依赖。
  std::string legacy_payload_sha256;
};

bool IsValidAppId(const std::string& app_id);
void ValidateManifest(const Manifest& manifest);
std::string SerializeManifest(const Manifest& manifest, bool pretty = false);
Manifest ParseManifest(const std::string& json);

}  // namespace lwweb
