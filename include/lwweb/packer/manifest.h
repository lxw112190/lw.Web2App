#pragma once

#include <cstdint>
#include <string>

namespace lwweb {

enum class AppMode { Local, Url };

// 控制生成应用的滚动文件日志；默认最多占用约 10 MiB 磁盘空间。
struct LoggingConfig {
  bool enabled = true;
  std::string level = "info";
  std::uint64_t max_file_size = 2ull * 1024 * 1024;
  std::uint32_t max_files = 5;
};

// 描述生成后桌面应用的运行方式和窗口行为。
// 该结构与 Payload 中的 manifest.json 一一对应；新增字段必须保持向后兼容。
struct Manifest {
  std::string format = "lw-web-app";
  std::uint32_t version = 1;
  AppMode mode = AppMode::Local;
  std::string app_id;
  std::string entry = "index.html";
  std::string url;
  std::string title = "lw.Web2App App";
  std::uint32_t width = 1280;
  std::uint32_t height = 800;
  bool resizable = true;
  bool fullscreen = true;
  bool devtools = false;
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
