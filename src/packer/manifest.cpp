#include "lwweb/packer/manifest.h"

#include "lwweb/common/error.h"
#include "lwweb/common/http_origin.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace lwweb {

bool IsValidAppId(const std::string& app_id) {
  if (app_id.size() < 3 || app_id.size() > 128 || app_id.front() == '.' ||
      app_id.back() == '.')
    return false;
  return std::all_of(app_id.begin(), app_id.end(), [](unsigned char value) {
    return std::isalnum(value) || value == '.' || value == '_' || value == '-';
  });
}

void ValidateManifest(const Manifest& manifest) {
  if (manifest.format != "lw-web-app" || manifest.version != 1)
    throw Error("Unsupported manifest format or version");
  if (manifest.title.empty() || manifest.title.size() > 512)
    throw Error("Application title must contain 1 to 512 UTF-8 bytes");
  if (!manifest.app_id.empty() && !IsValidAppId(manifest.app_id))
    throw Error("Application ID must contain 3 to 128 letters, digits, dots, underscores, or hyphens");
  if (manifest.width < 320 || manifest.width > 16384 || manifest.height < 240 ||
      manifest.height > 16384)
    throw Error("Window dimensions are outside the supported range");
  if (manifest.logging.level != "debug" && manifest.logging.level != "info" &&
      manifest.logging.level != "warn" && manifest.logging.level != "error")
    throw Error("Logging level must be debug, info, warn, or error");
  if (manifest.logging.max_file_size < 64 * 1024 ||
      manifest.logging.max_file_size > 128ull * 1024 * 1024 ||
      manifest.logging.max_files == 0 || manifest.logging.max_files > 20)
    throw Error("Logging rotation configuration is outside the supported range");
  if (manifest.mode == AppMode::Local) {
    if (!NormalizeArchivePath(manifest.entry)) throw Error("Unsafe manifest entry path");
    if (!IsSafeStartPath(manifest.start_path)) throw Error("Unsafe manifest start path");
    if (manifest.entry == "__lw_file__" ||
        manifest.entry.rfind("__lw_file__/", 0) == 0 ||
        manifest.start_path == "/__lw_file__" ||
        manifest.start_path.rfind("/__lw_file__/", 0) == 0)
      throw Error("Manifest path conflicts with the local file bridge prefix");
  } else if (!IsSupportedHttpUrl(manifest.url)) {
    throw Error("URL mode requires an http:// or https:// URL");
  }
  if (manifest.ipc.enabled && manifest.mode != AppMode::Local)
    throw Error("Native IPC is available only for local packaged sites");
  if (!manifest.ipc.enabled &&
      (!manifest.ipc.capabilities.empty() || !manifest.ipc.filesystem_roots.empty()))
    throw Error("Native IPC capabilities require ipc.enabled=true");
  static const std::set<std::string> supported_capabilities = {
      "app.info", "dialog.directory", "dialog.file", "fs.exists", "fs.list", "fs.read",
      "fs.mkdir", "fs.copy", "fs.move", "fs.trash", "fs.delete"};
  std::set<std::string> unique_capabilities;
  for (const auto& capability : manifest.ipc.capabilities) {
    if (!supported_capabilities.count(capability))
      throw Error("Unsupported Native IPC capability: " + capability +
                  ". Supported names: app.info, dialog.directory, dialog.file, "
                  "fs.exists, fs.list, fs.read, fs.mkdir, fs.copy, fs.move, "
                  "fs.trash, fs.delete");
    if (!unique_capabilities.insert(capability).second)
      throw Error("Duplicate Native IPC capability: " + capability);
  }
  if (manifest.ipc.filesystem_roots.size() > 32)
    throw Error("Native IPC filesystem root count exceeds the safety limit");
  for (const auto& root : manifest.ipc.filesystem_roots) {
    if (root.empty() || root.size() > 4096 || root.find('\0') != std::string::npos)
      throw Error("Native IPC filesystem root is invalid");
  }
  const auto& proxy = manifest.backend_proxy;
  if (proxy.enabled) {
    if (manifest.mode != AppMode::Local)
      throw Error("Backend proxy is available only for local packaged sites");
    const auto origin = ParseHttpOrigin(proxy.origin);
    if (!origin || origin->scheme != "http")
      throw Error("Backend proxy currently requires a valid http:// origin without a path");
    if (proxy.prefix.size() < 3 || proxy.prefix.size() > 128 ||
        proxy.prefix.front() != '/' || proxy.prefix.back() == '/' ||
        !IsCanonicalArchivePath(proxy.prefix.substr(1)))
      throw Error("Backend proxy prefix is unsafe");
    if (proxy.prefix == "/__lw_file__" ||
        proxy.prefix.rfind("/__lw_file__/", 0) == 0)
      throw Error("Backend proxy conflicts with the local file bridge prefix");
    if (manifest.start_path == proxy.prefix ||
        manifest.start_path.rfind(proxy.prefix + "/", 0) == 0)
      throw Error("Start path conflicts with the backend proxy prefix");
    if (proxy.connect_timeout_ms < 100 || proxy.connect_timeout_ms > 300000 ||
        proxy.read_timeout_ms < 100 || proxy.read_timeout_ms > 300000)
      throw Error("Backend proxy timeout is outside the supported range");
    if (proxy.max_request_size < 1024 ||
        proxy.max_request_size > 256ull * 1024 * 1024 ||
        proxy.max_response_size < 1024 ||
        proxy.max_response_size > 512ull * 1024 * 1024)
      throw Error("Backend proxy size limit is outside the supported range");
  }
  if (!manifest.legacy_payload_sha256.empty()) {
    try {
      (void)ParseHexDigest(manifest.legacy_payload_sha256);
    } catch (...) {
      throw Error("Manifest SHA-256 is invalid");
    }
  }
}

std::string SerializeManifest(const Manifest& manifest, bool pretty) {
  ValidateManifest(manifest);
  nlohmann::json value = {
      {"format", manifest.format},
      {"version", manifest.version},
      {"mode", manifest.mode == AppMode::Local ? "local" : "url"},
      {"app_id", manifest.app_id},
      {"entry", manifest.entry},
      {"start_path", manifest.start_path},
      {"title", manifest.title},
      {"width", manifest.width},
      {"height", manifest.height},
      {"resizable", manifest.resizable},
      {"fullscreen", manifest.fullscreen},
      {"devtools", manifest.devtools},
      {"spa_fallback", manifest.spa_fallback},
      {"logging", {{"enabled", manifest.logging.enabled},
                    {"level", manifest.logging.level},
                    {"max_file_size", manifest.logging.max_file_size},
                    {"max_files", manifest.logging.max_files}}},
      {"backend_proxy",
       {{"enabled", manifest.backend_proxy.enabled},
        {"origin", manifest.backend_proxy.origin},
        {"prefix", manifest.backend_proxy.prefix},
        {"connect_timeout_ms", manifest.backend_proxy.connect_timeout_ms},
        {"read_timeout_ms", manifest.backend_proxy.read_timeout_ms},
        {"max_request_size", manifest.backend_proxy.max_request_size},
        {"max_response_size", manifest.backend_proxy.max_response_size}}},
      {"ipc", {{"enabled", manifest.ipc.enabled},
               {"capabilities", manifest.ipc.capabilities},
               {"filesystem_roots", manifest.ipc.filesystem_roots}}}};
  if (!manifest.legacy_payload_sha256.empty())
    value["payload_sha256"] = manifest.legacy_payload_sha256;
  if (manifest.mode == AppMode::Url) value["url"] = manifest.url;
  return value.dump(pretty ? 2 : -1);
}

Manifest ParseManifest(const std::string& json) {
  try {
    const auto value = nlohmann::json::parse(json);
    Manifest manifest;
    manifest.format = value.at("format").get<std::string>();
    manifest.version = value.at("version").get<std::uint32_t>();
    const auto mode = value.value("mode", "local");
    manifest.mode = mode == "url" ? AppMode::Url : AppMode::Local;
    manifest.app_id = value.value("app_id", "");
    manifest.entry = value.value("entry", "index.html");
    manifest.start_path = value.value("start_path", "/");
    manifest.url = value.value("url", "");
    manifest.title = value.value("title", "lw.Web2App App");
    manifest.width = value.value("width", 1280u);
    manifest.height = value.value("height", 800u);
    manifest.resizable = value.value("resizable", true);
    // Legacy packages did not have this field and should retain their windowed behavior.
    manifest.fullscreen = value.value("fullscreen", false);
    manifest.devtools = value.value("devtools", false);
    manifest.spa_fallback = value.value("spa_fallback", true);
    if (const auto proxy = value.find("backend_proxy"); proxy != value.end()) {
      manifest.backend_proxy.enabled = proxy->value("enabled", false);
      manifest.backend_proxy.origin = proxy->value("origin", "");
      manifest.backend_proxy.prefix = proxy->value("prefix", "/__lw_proxy__");
      manifest.backend_proxy.connect_timeout_ms =
          proxy->value("connect_timeout_ms", 5000u);
      manifest.backend_proxy.read_timeout_ms = proxy->value("read_timeout_ms", 30000u);
      manifest.backend_proxy.max_request_size =
          proxy->value("max_request_size", 16ull * 1024 * 1024);
      manifest.backend_proxy.max_response_size =
          proxy->value("max_response_size", 64ull * 1024 * 1024);
    }
    if (const auto logging = value.find("logging"); logging != value.end()) {
      manifest.logging.enabled = logging->value("enabled", true);
      manifest.logging.level = logging->value("level", "info");
      manifest.logging.max_file_size = logging->value("max_file_size", 2ull * 1024 * 1024);
      manifest.logging.max_files = logging->value("max_files", 5u);
    } else {
      manifest.logging.enabled = false;
    }
    if (const auto ipc = value.find("ipc"); ipc != value.end()) {
      manifest.ipc.enabled = ipc->value("enabled", false);
      manifest.ipc.capabilities =
          ipc->value("capabilities", std::vector<std::string>{});
      manifest.ipc.filesystem_roots =
          ipc->value("filesystem_roots", std::vector<std::string>{});
    }
    manifest.legacy_payload_sha256 = value.value("payload_sha256", "");
    ValidateManifest(manifest);
    return manifest;
  } catch (const Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Error(std::string("Invalid manifest: ") + error.what());
  }
}

}  // namespace lwweb
