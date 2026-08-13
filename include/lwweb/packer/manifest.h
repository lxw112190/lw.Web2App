#pragma once

#include <cstdint>
#include <string>

namespace lwweb {

enum class AppMode { Local, Url };

// 描述生成后桌面应用的运行方式和窗口行为。
// 该结构与 Payload 中的 manifest.json 一一对应；新增字段必须保持向后兼容。
struct Manifest {
  std::string format = "lw-web-app";
  std::uint32_t version = 1;
  AppMode mode = AppMode::Local;
  std::string entry = "index.html";
  std::string url;
  std::string title = "lw.Web2App App";
  std::uint32_t width = 1280;
  std::uint32_t height = 800;
  bool resizable = true;
  bool devtools = false;
  bool spa_fallback = true;
  std::string payload_sha256;
};

void ValidateManifest(const Manifest& manifest);
std::string SerializeManifest(const Manifest& manifest, bool pretty = false);
Manifest ParseManifest(const std::string& json);

}  // namespace lwweb
