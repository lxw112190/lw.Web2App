#include "lwweb/common/file_utils.h"
#include "lwweb/publish/publish_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectoryGuard {
  std::filesystem::path path;
  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output) throw std::runtime_error("cannot write project config fixture");
}

bool ConfigRejected(const std::filesystem::path& path,
                    const std::string& text) {
  WriteText(path, text);
  try {
    (void)lwweb::LoadProjectConfig(path);
    return false;
  } catch (...) {
    return true;
  }
}

}  // namespace

void RunPublishConfigTests() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() /
                    ("lwweb-project-config-" + unique);
  TempDirectoryGuard cleanup{root};
  const auto project = root / "project";
  const auto config_directory = project / "config";
  std::filesystem::create_directories(project / "web");
  WriteText(project / "web" / "index.html", "<!doctype html><title>test</title>");
  WriteText(project / "icon.png", "fixture");

  const auto config_path = config_directory / "lwweb.json";
  const std::string complete = R"JSON({
    "schema": 1,
    "app": {
      "id": "com.example.project",
      "name": "ProjectApp",
      "version": "1.2.0",
      "company": "Example Company",
      "description": "Example desktop application",
      "copyright": "Copyright 2026",
      "icon": "../icon.png"
    },
    "web": {
      "source": "../web",
      "entry": "index.html",
      "start_path": "/login"
    },
    "window": {
      "width": 1440,
      "height": 900,
      "fullscreen": false,
      "resizable": false
    },
    "runtime": {
      "spa_fallback": true,
      "devtools": true,
      "logging": {
        "enabled": true,
        "level": "debug",
        "max_file_size": 1048576,
        "max_files": 3
      },
      "backend_proxy": {
        "enabled": true,
        "origin": "http://192.0.2.10:8080",
        "prefix": "/__lw_proxy__",
        "connect_timeout_ms": 3000,
        "read_timeout_ms": 20000,
        "max_request_size": 1048576,
        "max_response_size": 8388608
      },
      "external_links": {"policy": "browser"},
      "ipc": {
        "enabled": true,
        "capabilities": ["app.info", "dialog.directory", "fs.list"],
        "filesystem_roots": ["${PICTURES}"]
      }
    },
    "publish": {
      "output": "../release",
      "windows": {
        "portable": true,
        "zip": true,
        "installer": {
          "enabled": true,
          "desktop_shortcut": false,
          "start_menu": true,
          "iscc": "../tools/ISCC.exe"
        },
        "signing": {
          "enabled": true,
          "certificate_thumbprint": "00112233445566778899AABBCCDDEEFF00112233",
          "timestamp_url": "https://timestamp.example.com",
          "signtool": "../tools/signtool.exe"
        }
      },
      "linux": {
        "tar_gz": true,
        "deb": true
      }
    }
  })JSON";
  WriteText(config_path, std::string("\xEF\xBB\xBF") + complete);
  const auto config = lwweb::LoadProjectConfig(config_path);
  Check(config.schema == 1 && config.app.id == "com.example.project" &&
            config.app.name == "ProjectApp" && config.app.version == "1.2.0",
        "project identity is parsed");
  Check(config.pack.source_directory ==
            std::filesystem::absolute(project / "web").lexically_normal() &&
            config.app.icon ==
                std::filesystem::absolute(project / "icon.png").lexically_normal() &&
            config.publish.output ==
                std::filesystem::absolute(project / "release").lexically_normal(),
        "project paths resolve against the config directory");
  Check(config.pack.manifest.app_id == "com.example.project" &&
            config.pack.manifest.title == "ProjectApp" &&
            config.pack.manifest.start_path == "/login" &&
            config.pack.manifest.width == 1440 &&
            config.pack.manifest.height == 900 &&
            !config.pack.manifest.fullscreen &&
            !config.pack.manifest.resizable,
        "app and window settings map to PackOptions");
  Check(config.pack.manifest.logging.level == "debug" &&
            config.pack.manifest.logging.max_file_size == 1048576 &&
            config.pack.manifest.logging.max_files == 3 &&
            config.pack.manifest.backend_proxy.enabled &&
            config.pack.manifest.external_links.policy == "browser" &&
            config.pack.manifest.ipc.enabled &&
            config.pack.manifest.ipc.capabilities.size() == 3,
        "runtime settings map to the existing validated manifest");
  Check(config.pack.metadata.version == L"1.2.0.0" &&
            config.pack.metadata.file_description ==
                L"Example desktop application" &&
            !config.pack.signing.enabled,
        "PE metadata is prepared while publish signing remains separate");
  Check(config.publish.windows.portable && config.publish.windows.zip &&
            config.publish.windows.installer.enabled &&
            !config.publish.windows.installer.desktop_shortcut &&
            config.publish.windows.signing.enabled &&
            config.publish.windows.signing.signtool ==
                std::filesystem::absolute(project / "tools" / "signtool.exe")
                    .lexically_normal() &&
            config.publish.linux.tar_gz && config.publish.linux.deb,
        "platform publish settings are parsed");

  const auto minimal_path = root / "minimal" / "lwweb.json";
  WriteText(minimal_path, R"JSON({
    "schema": 1,
    "app": {"id": "com.example.minimal", "name": "Minimal", "version": "2"},
    "web": {"source": "./dist"}
  })JSON");
  const auto minimal = lwweb::LoadProjectConfig(minimal_path);
  Check(minimal.pack.manifest.entry == "index.html" &&
            minimal.pack.manifest.start_path == "/" &&
            minimal.pack.manifest.fullscreen &&
            minimal.publish.windows.portable && minimal.publish.windows.zip &&
            minimal.publish.linux.tar_gz && !minimal.publish.linux.deb,
        "project config applies stable defaults");

  const std::string prefix = R"JSON({
    "schema": 1,
    "app": {"id": "com.example.bad", "name": "Bad", "version": "1.0"},
    "web": {"source": "./dist"},
  )JSON";
  Check(ConfigRejected(root / "bad-schema.json", R"JSON({
          "schema": 2,
          "app": {"id": "com.example.bad", "name": "Bad", "version": "1"},
          "web": {"source": "./dist"}
        })JSON"),
        "unsupported project schema is rejected");
  Check(ConfigRejected(root / "unknown.json", R"JSON({
          "schema": 1,
          "app": {"id": "com.example.bad", "name": "Bad", "version": "1",
                  "publisher_name": "typo"},
          "web": {"source": "./dist"}
        })JSON"),
        "unknown project fields are rejected");
  Check(ConfigRejected(root / "secret.json", R"JSON({
          "schema": 1,
          "app": {"id": "com.example.bad", "name": "Bad", "version": "1"},
          "web": {"source": "./dist"},
          "publish": {"windows": {"signing": {"enabled": true,
            "certificate_thumbprint": "00112233445566778899AABBCCDDEEFF00112233",
            "pfx_password": "must-not-be-stored"}}}
        })JSON"),
        "secret fields are rejected before ordinary schema errors");
  Check(ConfigRejected(root / "missing-thumbprint.json", R"JSON({
          "schema": 1,
          "app": {"id": "com.example.bad", "name": "Bad", "version": "1"},
          "web": {"source": "./dist"},
          "publish": {"windows": {"signing": {"enabled": true}}}
        })JSON"),
        "enabled signing requires a certificate thumbprint");
  Check(ConfigRejected(root / "unsupported-capability.json", R"JSON({
          "schema": 1,
          "app": {"id": "com.example.bad", "name": "Bad", "version": "1"},
          "web": {"source": "./dist"},
          "runtime": {"ipc": {"enabled": true,
            "capabilities": ["shell.exec"]}}
        })JSON"),
        "project config reuses Native IPC capability validation");
  Check(ConfigRejected(root / "malformed.json", prefix + "}"),
        "malformed project JSON is rejected");
}
