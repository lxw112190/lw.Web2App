#include "lwweb/common/file_utils.h"
#include "lwweb/common/sha256.h"
#include "lwweb/packer/payload.h"
#include "lwweb/publish/publisher.h"

#include <nlohmann/json.hpp>
#include <miniz.h>

#include <chrono>
#include <cstring>
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
  output << text;
  if (!output) throw std::runtime_error("cannot write publisher fixture");
}

std::string Digest(const std::filesystem::path& path) {
  const auto size = lwweb::FileSize(path);
  return lwweb::HexDigest(lwweb::Sha256FileRange(path, 0, size));
}

}  // namespace

void RunPublisherTests() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() /
                    ("lwweb-publisher-" + unique);
  TempDirectoryGuard cleanup{root};
  WriteText(root / "web" / "index.html", "<!doctype html><title>publish</title>");
  const auto config = root / "lwweb.json";
  WriteText(config, R"JSON({
    "schema": 1,
    "app": {
      "id": "com.example.publish-test",
      "name": "Publish Test",
      "version": "1.2.3",
      "company": "Example",
      "description": "Publisher integration test"
    },
    "web": {"source": "./web", "entry": "index.html"},
    "publish": {
      "output": "./release",
      "windows": {"portable": true, "zip": true},
      "linux": {"tar_gz": true, "deb": false}
    }
  })JSON");

  lwweb::PublishOptions options;
  options.runner = lwweb::CurrentExecutablePath();
  options.config_file = config;
#ifdef _WIN32
  options.platform = lwweb::PublishPlatform::Windows;
  const std::string platform = "windows";
  const std::string application = "Publish Test.exe";
  const std::string archive = "Publish Test-1.2.3-windows-x64.zip";
#else
  options.platform = lwweb::PublishPlatform::Linux;
  const std::string platform = "linux";
  const std::string application = "Publish Test";
  const std::string archive = "Publish Test-1.2.3-linux-x64.tar.gz";
#endif
  const auto result = lwweb::PublishProject(options);
  Check(std::filesystem::is_directory(result.directory),
        "publish creates final release directory");
  Check(std::filesystem::is_regular_file(result.directory / application),
        "publish creates portable application");
  Check(std::filesystem::is_regular_file(result.directory / archive),
        "publish creates platform archive");
  Check(lwweb::LoadPayload(result.directory / application).manifest.title ==
            "Publish Test",
        "published portable application contains configured payload");

  const auto release_json = nlohmann::json::parse(
      lwweb::ReadFileText(result.directory / "RELEASE_INFO.json"));
  Check(release_json.at("format") == "lw-web2app-release" &&
            release_json.at("app").at("version") == "1.2.3" &&
            release_json.at("artifacts").size() == 2,
        "release metadata describes project and artifacts");
  Check(release_json.at("artifacts").at(0).at("platform") == platform &&
            release_json.at("artifacts").at(0).at("sha256") ==
                Digest(result.directory / application) &&
            release_json.at("artifacts").at(1).at("sha256") ==
                Digest(result.directory / archive),
        "release metadata contains verified artifact digests");
  const auto sums = lwweb::ReadFileText(result.directory / "SHA256SUMS.txt");
  Check(sums.find(Digest(result.directory / application) + "  " + application) !=
            std::string::npos &&
            sums.find(Digest(result.directory / archive) + "  " + archive) !=
                std::string::npos,
        "checksum file covers portable and archive artifacts");

  WriteText(result.directory / "stale-release.marker", "replace on success");
  const auto replaced = lwweb::PublishProject(options);
  Check(replaced.directory == result.directory &&
            !std::filesystem::exists(result.directory / "stale-release.marker") &&
            std::filesystem::is_regular_file(result.directory / application),
        "successful republish atomically replaces the same-version directory");

  auto linux_options = options;
  linux_options.platform = lwweb::PublishPlatform::Linux;
  linux_options.output_override = root / "linux-release";
  const auto linux_result = lwweb::PublishProject(linux_options);
  const auto tar_gzip =
      linux_result.directory / "Publish Test-1.2.3-linux-x64.tar.gz";
  const auto gzip = lwweb::ReadFileBytes(tar_gzip);
  Check(gzip.size() > 18 && gzip[0] == 0x1f && gzip[1] == 0x8b &&
            gzip[2] == 0x08,
        "Linux publish creates a GZip stream");
  std::size_t tar_size = 0;
  void* tar_memory = tinfl_decompress_mem_to_heap(
      gzip.data() + 10, gzip.size() - 18, &tar_size, 0);
  Check(tar_memory != nullptr && tar_size >= 1536,
        "Linux publish archive contains a valid raw DEFLATE stream");
  const auto* tar = static_cast<const unsigned char*>(tar_memory);
  const std::string tar_name(reinterpret_cast<const char*>(tar),
                             std::char_traits<char>::length(
                                 reinterpret_cast<const char*>(tar)));
  const bool valid_tar = tar_name == "Publish Test" &&
                         std::memcmp(tar + 257, "ustar", 5) == 0;
  const auto trailer = gzip.size() - 8;
  const auto stored_crc = static_cast<mz_ulong>(gzip[trailer]) |
                          (static_cast<mz_ulong>(gzip[trailer + 1]) << 8) |
                          (static_cast<mz_ulong>(gzip[trailer + 2]) << 16) |
                          (static_cast<mz_ulong>(gzip[trailer + 3]) << 24);
  const auto actual_crc = mz_crc32(MZ_CRC32_INIT, tar, tar_size);
  mz_free(tar_memory);
  Check(valid_tar && stored_crc == actual_crc,
        "Linux tar.gz has a valid USTAR header and GZip CRC");

#ifndef _WIN32
  auto deb_config = nlohmann::json::parse(lwweb::ReadFileText(config));
  deb_config["publish"]["linux"]["tar_gz"] = false;
  deb_config["publish"]["linux"]["deb"] = true;
  WriteText(config, deb_config.dump(2));
  auto deb_options = linux_options;
  deb_options.output_override = root / "deb-release";
  const auto deb_result = lwweb::PublishProject(deb_options);
  const auto deb = deb_result.directory / "publish-test_1.2.3_amd64.deb";
  const auto deb_bytes = lwweb::ReadFileBytes(deb);
  Check(deb_bytes.size() > 8 &&
            std::memcmp(deb_bytes.data(), "!<arch>\n", 8) == 0 &&
            deb_result.release.artifacts.size() == 2 &&
            deb_result.release.artifacts[0].type == "portable" &&
            deb_result.release.artifacts[1].type == "deb" &&
            deb_result.release.artifacts[1].sha256 == Digest(deb),
        "Linux publish builds, validates, and records a generated-app DEB");
  WriteText(deb_result.directory / "deb-release.marker",
            "preserve on DEB failure");
  deb_config["app"]["icon"] = "./missing-icon.png";
  WriteText(config, deb_config.dump(2));
  bool deb_failed = false;
  try {
    (void)lwweb::PublishProject(deb_options);
  } catch (...) {
    deb_failed = true;
  }
  Check(deb_failed &&
            lwweb::ReadFileText(deb_result.directory / "deb-release.marker") ==
                "preserve on DEB failure",
        "DEB failure preserves the previous complete release");
#endif

#ifdef _WIN32
  auto installer_config = nlohmann::json::parse(lwweb::ReadFileText(config));
  installer_config["publish"]["windows"]["zip"] = false;
  installer_config["publish"]["windows"]["installer"] = {
      {"enabled", true},
      {"desktop_shortcut", false},
      {"start_menu", true},
      {"iscc", lwweb::CurrentExecutablePath().u8string()}};
  WriteText(config, installer_config.dump(2));
  auto installer_options = options;
  installer_options.output_override = root / "installer-release";
  const auto installer_result = lwweb::PublishProject(installer_options);
  const auto setup =
      installer_result.directory / "Publish Test-Setup-1.2.3.exe";
  Check(std::filesystem::is_regular_file(setup) &&
            !std::filesystem::exists(installer_result.directory /
                                     ".lwweb-installer.iss") &&
            installer_result.release.artifacts.size() == 2 &&
            installer_result.release.artifacts[0].type == "portable" &&
            installer_result.release.artifacts[1].type == "installer" &&
            installer_result.release.artifacts[1].sha256 == Digest(setup),
        "Windows publish builds, validates, and records an Installer artifact");
  WriteText(installer_result.directory / "installer-release.marker",
            "preserve on ISCC failure");
  installer_config["publish"]["windows"]["installer"]["iscc"] =
      (root / "missing-ISCC.exe").u8string();
  WriteText(config, installer_config.dump(2));
  bool installer_failed = false;
  try {
    (void)lwweb::PublishProject(installer_options);
  } catch (...) {
    installer_failed = true;
  }
  Check(installer_failed &&
            lwweb::ReadFileText(installer_result.directory /
                                "installer-release.marker") ==
                "preserve on ISCC failure",
        "ISCC failure preserves the previous complete release");
#endif

  WriteText(result.directory / "previous-release.marker", "keep on failure");
  WriteText(config, R"JSON({
    "schema": 1,
    "app": {"id": "com.example.publish-test", "name": "Publish Test",
            "version": "1.2.3"},
    "web": {"source": "./web", "entry": "missing.html"},
    "publish": {"output": "./release"}
  })JSON");
  bool failed = false;
  try {
    (void)lwweb::PublishProject(options);
  } catch (...) {
    failed = true;
  }
  Check(failed, "publish rejects a missing entry page");
  Check(lwweb::ReadFileText(result.directory / "previous-release.marker") ==
            "keep on failure",
        "failed publish preserves previous release directory");
  for (const auto& item : std::filesystem::directory_iterator(root / "release"))
    Check(item.path().filename().u8string().rfind(".lwweb-", 0) != 0,
          "failed publish cleans temporary directories");
}
