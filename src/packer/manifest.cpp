#include "lwweb/packer/manifest.h"

#include "lwweb/common/error.h"
#include "lwweb/common/path_utils.h"

#include <nlohmann/json.hpp>

namespace lwweb {

void ValidateManifest(const Manifest& manifest) {
  if (manifest.format != "lw-web-app" || manifest.version != 1)
    throw Error("Unsupported manifest format or version");
  if (manifest.title.empty() || manifest.title.size() > 512)
    throw Error("Application title must contain 1 to 512 UTF-8 bytes");
  if (manifest.width < 320 || manifest.width > 16384 || manifest.height < 240 ||
      manifest.height > 16384)
    throw Error("Window dimensions are outside the supported range");
  if (manifest.mode == AppMode::Local) {
    if (!NormalizeArchivePath(manifest.entry)) throw Error("Unsafe manifest entry path");
  } else if (!IsSupportedHttpUrl(manifest.url)) {
    throw Error("URL mode requires an http:// or https:// URL");
  }
  if (!manifest.payload_sha256.empty() && manifest.payload_sha256.size() != 64)
    throw Error("Manifest SHA-256 is invalid");
}

std::string SerializeManifest(const Manifest& manifest, bool pretty) {
  ValidateManifest(manifest);
  nlohmann::json value = {
      {"format", manifest.format},
      {"version", manifest.version},
      {"mode", manifest.mode == AppMode::Local ? "local" : "url"},
      {"entry", manifest.entry},
      {"title", manifest.title},
      {"width", manifest.width},
      {"height", manifest.height},
      {"resizable", manifest.resizable},
      {"devtools", manifest.devtools},
      {"spa_fallback", manifest.spa_fallback},
      {"payload_sha256", manifest.payload_sha256}};
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
    manifest.entry = value.value("entry", "index.html");
    manifest.url = value.value("url", "");
    manifest.title = value.value("title", "lw.Web2App App");
    manifest.width = value.value("width", 1280u);
    manifest.height = value.value("height", 800u);
    manifest.resizable = value.value("resizable", true);
    manifest.devtools = value.value("devtools", false);
    manifest.spa_fallback = value.value("spa_fallback", true);
    manifest.payload_sha256 = value.value("payload_sha256", "");
    ValidateManifest(manifest);
    return manifest;
  } catch (const Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Error(std::string("Invalid manifest: ") + error.what());
  }
}

}  // namespace lwweb
