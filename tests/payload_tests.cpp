#include "lwweb/packer/payload.h"
#include "lwweb/packer/packer.h"
#include "lwweb/common/logging.h"
#include "lwweb/runtime/resource_server.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunPayloadTests() {
  const auto log_file = std::filesystem::temp_directory_path() / L"lwweb-logging-test" /
                        L"app.log";
  auto logger = lwweb::Logger::Rotating("lwweb-test", log_file, {});
  logger.Info("logging smoke test");
  logger.Flush();
  Check(std::filesystem::is_regular_file(log_file), "rotating logger writes a file");
  lwweb::PayloadFooter input;
  input.flags = lwweb::kPayloadHasZip;
  input.payload_offset = 0x0123456789ABCDEFu;
  input.payload_size = 123456;
  input.manifest_offset = input.payload_offset + input.payload_size;
  input.manifest_size = 99;
  for (std::size_t i = 0; i < input.sha256.size(); ++i)
    input.sha256[i] = static_cast<std::uint8_t>(i);
  const auto output = lwweb::DecodeFooter(lwweb::EncodeFooter(input));
  Check(output.version == lwweb::kPayloadVersion, "footer version round-trip");
  Check(output.flags == input.flags, "footer flags round-trip");
  Check(output.payload_offset == input.payload_offset, "footer offset round-trip");
  Check(output.payload_size == input.payload_size, "footer size round-trip");
  Check(output.manifest_offset == input.manifest_offset, "manifest offset round-trip");
  Check(output.manifest_size == input.manifest_size, "manifest size round-trip");
  Check(output.sha256 == input.sha256, "digest round-trip");

  const auto base = std::filesystem::temp_directory_path() / L"lwweb-integration-test";
  std::error_code ignored;
  std::filesystem::remove_all(base, ignored);
  std::filesystem::create_directories(base / L"site" / L"assets");
  {
    std::ofstream runner(base / L"runner.exe", std::ios::binary);
    runner << "MZ fake runner prefix";
    std::ofstream html(base / L"site" / L"index.html", std::ios::binary);
    html << "<!doctype html><title>test</title>";
    std::ofstream css(base / L"site" / L"assets" / L"app.css", std::ios::binary);
    css << "body{color:red}";
  }
  lwweb::PackOptions pack;
  pack.runner = base / L"runner.exe";
  pack.source_directory = base / L"site";
  pack.output = base / L"packed.exe";
  pack.manifest.title = "Integration Test";
  lwweb::PackApplication(pack);
  const auto loaded = lwweb::LoadPayload(pack.output);
  Check(loaded.manifest.title == "Integration Test", "packed manifest loads");
  Check(loaded.manifest.fullscreen, "new packages start fullscreen by default");
  Check(loaded.manifest.logging.enabled, "runtime logging enabled by default");
  Check(loaded.manifest.logging.max_file_size == 2ull * 1024 * 1024,
        "runtime log rotation size");
  Check(loaded.manifest.logging.max_files == 5, "runtime log rotation count");
  Check(lwweb::IsValidAppId(loaded.manifest.app_id), "generated app ID is valid");
  lwweb::ZipResourceStore store(loaded);
  Check(store.Exists("index.html"), "ZIP index includes entry");
  Check(store.Exists("assets/app.css"), "ZIP index includes asset");
  const auto resource = store.Read("assets/app.css");
  Check(std::string(resource.begin(), resource.end()) == "body{color:red}",
        "ZIP resource extracts on demand");
  Check(!store.Exists("../index.html"), "unsafe runtime path rejected");

  {
    std::fstream executable(pack.output, std::ios::binary | std::ios::in | std::ios::out);
    executable.seekg(static_cast<std::streamoff>(loaded.footer.manifest_offset));
    char byte = 0;
    executable.read(&byte, 1);
    byte ^= 1;
    executable.seekp(static_cast<std::streamoff>(loaded.footer.manifest_offset));
    executable.write(&byte, 1);
  }
  bool tampering_rejected = false;
  try {
    (void)lwweb::LoadPayload(pack.output);
  } catch (...) {
    tampering_rejected = true;
  }
  Check(tampering_rejected, "manifest tampering rejected by V2 content hash");

  const auto legacy = lwweb::ParseManifest(
      R"({"format":"lw-web-app","version":1,"mode":"local","entry":"index.html","title":"Legacy"})");
  Check(!legacy.fullscreen, "legacy manifest remains windowed");
  Check(!legacy.logging.enabled, "legacy manifest does not unexpectedly enable logging");
  std::filesystem::remove_all(base, ignored);
}
