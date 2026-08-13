#include "lwweb/packer/manifest.h"

#include "lwweb/common/error.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

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
  } else if (!IsSupportedHttpUrl(manifest.url)) {
    throw Error("URL mode requires an http:// or https:// URL");
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
                    {"max_files", manifest.logging.max_files}}}};
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
    manifest.url = value.value("url", "");
    manifest.title = value.value("title", "lw.Web2App App");
    manifest.width = value.value("width", 1280u);
    manifest.height = value.value("height", 800u);
    manifest.resizable = value.value("resizable", true);
    // Legacy packages did not have this field and should retain their windowed behavior.
    manifest.fullscreen = value.value("fullscreen", false);
    manifest.devtools = value.value("devtools", false);
    manifest.spa_fallback = value.value("spa_fallback", true);
    if (const auto logging = value.find("logging"); logging != value.end()) {
      manifest.logging.enabled = logging->value("enabled", true);
      manifest.logging.level = logging->value("level", "info");
      manifest.logging.max_file_size = logging->value("max_file_size", 2ull * 1024 * 1024);
      manifest.logging.max_files = logging->value("max_files", 5u);
    } else {
      manifest.logging.enabled = false;
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
