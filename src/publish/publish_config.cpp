#include "lwweb/publish/publish_config.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/pe_version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>

namespace lwweb {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMaxProjectConfigSize = 1024 * 1024;

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool IsSecretField(const std::string& key) {
  const auto lower = LowerAscii(key);
  const auto ends_with = [&lower](const std::string& suffix) {
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  return lower.find("password") != std::string::npos ||
         lower.find("secret") != std::string::npos || lower == "token" ||
         ends_with("_token") || lower == "api_key" || ends_with("_api_key") ||
         lower == "pfx" || lower == "pfx_file" || lower == "pfx_path";
}

void RejectSecrets(const Json& value, const std::string& location) {
  if (value.is_object()) {
    for (auto item = value.begin(); item != value.end(); ++item) {
      if (IsSecretField(item.key()))
        throw Error("Secret field is forbidden in lwweb.json: " + location +
                    item.key());
      RejectSecrets(item.value(), location + item.key() + ".");
    }
  } else if (value.is_array()) {
    for (const auto& item : value) RejectSecrets(item, location);
  }
}

void RequireObject(const Json& value, const std::string& location) {
  if (!value.is_object())
    throw Error("lwweb.json field must be an object: " + location);
}

void RejectUnknownKeys(const Json& value,
                       std::initializer_list<const char*> allowed,
                       const std::string& location) {
  RequireObject(value, location);
  for (auto item = value.begin(); item != value.end(); ++item) {
    const auto known = std::find_if(
        allowed.begin(), allowed.end(), [&item](const char* key) {
          return item.key() == key;
        });
    if (known == allowed.end())
      throw Error("Unknown lwweb.json field: " + location + item.key());
  }
}

const Json& RequiredObject(const Json& parent, const char* key,
                           const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end())
    throw Error("Missing lwweb.json object: " + location + key);
  RequireObject(*found, location + key);
  return *found;
}

const Json* OptionalObject(const Json& parent, const char* key,
                           const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end()) return nullptr;
  RequireObject(*found, location + key);
  return &*found;
}

std::string StringValue(const Json& parent, const char* key,
                        const std::string& fallback,
                        const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end()) return fallback;
  if (!found->is_string())
    throw Error("lwweb.json field must be a string: " + location + key);
  const auto value = found->get<std::string>();
  if (value.find('\0') != std::string::npos)
    throw Error("lwweb.json string contains NUL: " + location + key);
  return value;
}

std::string RequiredString(const Json& parent, const char* key,
                           const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end())
    throw Error("Missing lwweb.json field: " + location + key);
  return StringValue(parent, key, {}, location);
}

bool BoolValue(const Json& parent, const char* key, bool fallback,
               const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end()) return fallback;
  if (!found->is_boolean())
    throw Error("lwweb.json field must be a boolean: " + location + key);
  return found->get<bool>();
}

std::uint64_t UnsignedValue(const Json& parent, const char* key,
                            std::uint64_t fallback,
                            const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end()) return fallback;
  if (!found->is_number_unsigned())
    throw Error("lwweb.json field must be an unsigned integer: " +
                location + key);
  return found->get<std::uint64_t>();
}

std::uint32_t Uint32Value(const Json& parent, const char* key,
                          std::uint32_t fallback,
                          const std::string& location) {
  const auto value = UnsignedValue(parent, key, fallback, location);
  if (value > (std::numeric_limits<std::uint32_t>::max)())
    throw Error("lwweb.json integer is too large: " + location + key);
  return static_cast<std::uint32_t>(value);
}

std::vector<std::string> StringArrayValue(
    const Json& parent, const char* key, const std::string& location) {
  const auto found = parent.find(key);
  if (found == parent.end()) return {};
  if (!found->is_array())
    throw Error("lwweb.json field must be an array: " + location + key);
  std::vector<std::string> values;
  values.reserve(found->size());
  for (const auto& item : *found) {
    if (!item.is_string())
      throw Error("lwweb.json array must contain strings: " + location + key);
    const auto value = item.get<std::string>();
    if (value.find('\0') != std::string::npos)
      throw Error("lwweb.json array contains NUL: " + location + key);
    values.push_back(value);
  }
  return values;
}

std::filesystem::path ResolveConfigPath(
    const std::filesystem::path& directory, const std::string& text,
    const std::string& field, bool allow_empty = false) {
  if (text.empty()) {
    if (allow_empty) return {};
    throw Error("lwweb.json path must not be empty: " + field);
  }
  auto path = std::filesystem::u8path(text);
  if (!path.is_absolute()) path = directory / path;
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error)
    throw Error("Cannot resolve lwweb.json path " + field + ": " +
                error.message());
  return absolute.lexically_normal();
}

void ValidateProjectIdentity(const ProjectAppConfig& app) {
  if (!IsValidAppId(app.id))
    throw Error("lwweb.json app.id is invalid");
  if (app.name.empty() || app.name.size() > 128)
    throw Error("lwweb.json app.name must contain 1 to 128 UTF-8 bytes");
  if (app.name == "." || app.name == ".." ||
      app.name.find('/') != std::string::npos ||
      app.name.find('\\') != std::string::npos ||
      std::any_of(app.name.begin(), app.name.end(), [](unsigned char value) {
        return value < 0x20;
      }))
    throw Error("lwweb.json app.name is unsafe for an artifact name");
  if (app.version.empty() || app.version.size() > 64)
    throw Error("lwweb.json app.version is invalid");
  (void)NormalizePeVersion(Utf8ToWide(app.version));
}

void ValidateSigningConfig(const SigningConfig& signing) {
  std::size_t hex_digits = 0;
  for (const auto character : signing.certificate_thumbprint) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isspace(byte)) continue;
    if (!std::isxdigit(byte))
      throw Error("lwweb.json certificate thumbprint must be hexadecimal");
    ++hex_digits;
  }
  if (signing.enabled && hex_digits != 40)
    throw Error("lwweb.json signing requires a 40-digit SHA-1 certificate thumbprint");
  if (!signing.enabled &&
      (!signing.certificate_thumbprint.empty() ||
       !signing.timestamp_url.empty() || !signing.signtool.empty()))
    throw Error("lwweb.json signing values require publish.windows.signing.enabled=true");
  if (!signing.timestamp_url.empty() &&
      !IsSupportedHttpUrl(signing.timestamp_url))
    throw Error("lwweb.json timestamp_url must use http:// or https://");
}

}  // namespace

ProjectConfig LoadProjectConfig(
    const std::filesystem::path& config_file) {
  try {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(config_file, error);
    if (error || !std::filesystem::is_regular_file(absolute, error))
      throw Error("lwweb.json does not exist: " + config_file.u8string());
    if (FileSize(absolute) > kMaxProjectConfigSize)
      throw Error("lwweb.json exceeds the 1 MiB limit");
    auto text = ReadFileText(absolute);
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
      text.erase(0, 3);
    const auto root = Json::parse(text);
    RequireObject(root, "root");
    RejectSecrets(root, "");
    RejectUnknownKeys(root, {"schema", "app", "web", "window", "runtime",
                             "publish"}, "");

    ProjectConfig config;
    config.config_file = absolute.lexically_normal();
    const auto directory = config.config_file.parent_path();
    config.schema = Uint32Value(root, "schema", 0, "");
    if (config.schema != 1)
      throw Error("Unsupported lwweb.json schema; expected schema 1");

    const auto& app = RequiredObject(root, "app", "");
    RejectUnknownKeys(app, {"id", "name", "version", "company",
                            "description", "copyright", "icon"}, "app.");
    config.app.id = RequiredString(app, "id", "app.");
    config.app.name = RequiredString(app, "name", "app.");
    config.app.version = RequiredString(app, "version", "app.");
    config.app.company = StringValue(app, "company", {}, "app.");
    config.app.description = StringValue(app, "description", {}, "app.");
    config.app.copyright = StringValue(app, "copyright", {}, "app.");
    const auto icon = StringValue(app, "icon", {}, "app.");
    config.app.icon = ResolveConfigPath(directory, icon, "app.icon", true);
    ValidateProjectIdentity(config.app);

    auto& pack = config.pack;
    pack.manifest.mode = AppMode::Local;
    pack.manifest.app_id = config.app.id;
    pack.manifest.title = config.app.name;
    pack.metadata.product_name = Utf8ToWide(config.app.name);
    pack.metadata.company_name = Utf8ToWide(config.app.company);
    pack.metadata.file_description = Utf8ToWide(
        config.app.description.empty() ? config.app.name
                                       : config.app.description);
    pack.metadata.version = NormalizePeVersion(Utf8ToWide(config.app.version));
    pack.metadata.copyright = Utf8ToWide(config.app.copyright);
    pack.metadata.icon = config.app.icon;

    const auto& web = RequiredObject(root, "web", "");
    RejectUnknownKeys(web, {"source", "entry", "start_path"}, "web.");
    pack.source_directory = ResolveConfigPath(
        directory, RequiredString(web, "source", "web."), "web.source");
    pack.manifest.entry = StringValue(web, "entry", "index.html", "web.");
    pack.manifest.start_path = StringValue(
        web, "start_path", SuggestedStartPath(pack.manifest.entry), "web.");

    if (const auto* window = OptionalObject(root, "window", "")) {
      RejectUnknownKeys(*window, {"width", "height", "fullscreen",
                                  "resizable"}, "window.");
      pack.manifest.width = Uint32Value(*window, "width", 1280, "window.");
      pack.manifest.height = Uint32Value(*window, "height", 800, "window.");
      pack.manifest.fullscreen =
          BoolValue(*window, "fullscreen", true, "window.");
      pack.manifest.resizable =
          BoolValue(*window, "resizable", true, "window.");
    }

    if (const auto* runtime = OptionalObject(root, "runtime", "")) {
      RejectUnknownKeys(*runtime, {"spa_fallback", "devtools", "logging",
                                   "backend_proxy", "ipc"}, "runtime.");
      pack.manifest.spa_fallback =
          BoolValue(*runtime, "spa_fallback", true, "runtime.");
      pack.manifest.devtools =
          BoolValue(*runtime, "devtools", false, "runtime.");
      if (const auto* logging = OptionalObject(*runtime, "logging", "runtime.")) {
        RejectUnknownKeys(*logging, {"enabled", "level", "max_file_size",
                                     "max_files"}, "runtime.logging.");
        pack.manifest.logging.enabled =
            BoolValue(*logging, "enabled", true, "runtime.logging.");
        pack.manifest.logging.level =
            StringValue(*logging, "level", "info", "runtime.logging.");
        pack.manifest.logging.max_file_size = UnsignedValue(
            *logging, "max_file_size", 2ull * 1024 * 1024,
            "runtime.logging.");
        pack.manifest.logging.max_files = Uint32Value(
            *logging, "max_files", 5, "runtime.logging.");
      }
      if (const auto* proxy =
              OptionalObject(*runtime, "backend_proxy", "runtime.")) {
        RejectUnknownKeys(*proxy, {"enabled", "origin", "prefix",
                                   "connect_timeout_ms", "read_timeout_ms",
                                   "max_request_size", "max_response_size"},
                          "runtime.backend_proxy.");
        pack.manifest.backend_proxy.enabled = BoolValue(
            *proxy, "enabled", false, "runtime.backend_proxy.");
        pack.manifest.backend_proxy.origin = StringValue(
            *proxy, "origin", {}, "runtime.backend_proxy.");
        pack.manifest.backend_proxy.prefix = StringValue(
            *proxy, "prefix", "/__lw_proxy__", "runtime.backend_proxy.");
        pack.manifest.backend_proxy.connect_timeout_ms = Uint32Value(
            *proxy, "connect_timeout_ms", 5000, "runtime.backend_proxy.");
        pack.manifest.backend_proxy.read_timeout_ms = Uint32Value(
            *proxy, "read_timeout_ms", 30000, "runtime.backend_proxy.");
        pack.manifest.backend_proxy.max_request_size = UnsignedValue(
            *proxy, "max_request_size", 16ull * 1024 * 1024,
            "runtime.backend_proxy.");
        pack.manifest.backend_proxy.max_response_size = UnsignedValue(
            *proxy, "max_response_size", 64ull * 1024 * 1024,
            "runtime.backend_proxy.");
      }
      if (const auto* ipc = OptionalObject(*runtime, "ipc", "runtime.")) {
        RejectUnknownKeys(*ipc, {"enabled", "capabilities",
                                 "filesystem_roots"}, "runtime.ipc.");
        pack.manifest.ipc.enabled =
            BoolValue(*ipc, "enabled", false, "runtime.ipc.");
        pack.manifest.ipc.capabilities =
            StringArrayValue(*ipc, "capabilities", "runtime.ipc.");
        pack.manifest.ipc.filesystem_roots =
            StringArrayValue(*ipc, "filesystem_roots", "runtime.ipc.");
      }
    }
    ValidateManifest(pack.manifest);

    const auto* publish = OptionalObject(root, "publish", "");
    if (publish) {
      RejectUnknownKeys(*publish, {"output", "windows", "linux"},
                        "publish.");
    }
    const Json empty_object = Json::object();
    const auto& publish_value = publish ? *publish : empty_object;
    config.publish.output = ResolveConfigPath(
        directory, StringValue(publish_value, "output", "./release",
                               "publish."),
        "publish.output");

    if (const auto* windows =
            OptionalObject(publish_value, "windows", "publish.")) {
      RejectUnknownKeys(*windows, {"portable", "zip", "installer", "signing"},
                        "publish.windows.");
      config.publish.windows.portable =
          BoolValue(*windows, "portable", true, "publish.windows.");
      config.publish.windows.zip =
          BoolValue(*windows, "zip", true, "publish.windows.");
      if (const auto* installer =
              OptionalObject(*windows, "installer", "publish.windows.")) {
        RejectUnknownKeys(*installer, {"enabled", "desktop_shortcut",
                                       "start_menu", "iscc"},
                          "publish.windows.installer.");
        config.publish.windows.installer.enabled = BoolValue(
            *installer, "enabled", false, "publish.windows.installer.");
        config.publish.windows.installer.desktop_shortcut = BoolValue(
            *installer, "desktop_shortcut", true,
            "publish.windows.installer.");
        config.publish.windows.installer.start_menu = BoolValue(
            *installer, "start_menu", true,
            "publish.windows.installer.");
        config.publish.windows.installer.iscc = ResolveConfigPath(
            directory,
            StringValue(*installer, "iscc", {},
                        "publish.windows.installer."),
            "publish.windows.installer.iscc", true);
      }
      if (const auto* signing =
              OptionalObject(*windows, "signing", "publish.windows.")) {
        RejectUnknownKeys(*signing, {"enabled", "certificate_thumbprint",
                                     "timestamp_url", "signtool"},
                          "publish.windows.signing.");
        config.publish.windows.signing.enabled = BoolValue(
            *signing, "enabled", false, "publish.windows.signing.");
        config.publish.windows.signing.certificate_thumbprint = StringValue(
            *signing, "certificate_thumbprint", {},
            "publish.windows.signing.");
        config.publish.windows.signing.timestamp_url = StringValue(
            *signing, "timestamp_url", {}, "publish.windows.signing.");
        config.publish.windows.signing.signtool = ResolveConfigPath(
            directory,
            StringValue(*signing, "signtool", {},
                        "publish.windows.signing."),
            "publish.windows.signing.signtool", true);
      }
    }
    ValidateSigningConfig(config.publish.windows.signing);

    if (const auto* linux =
            OptionalObject(publish_value, "linux", "publish.")) {
      RejectUnknownKeys(*linux, {"tar_gz", "deb"}, "publish.linux.");
      config.publish.linux.tar_gz =
          BoolValue(*linux, "tar_gz", true, "publish.linux.");
      config.publish.linux.deb =
          BoolValue(*linux, "deb", false, "publish.linux.");
    }
    return config;
  } catch (const Error&) {
    throw;
  } catch (const std::exception& error) {
    throw Error(std::string("Invalid lwweb.json: ") + error.what());
  }
}

}  // namespace lwweb
