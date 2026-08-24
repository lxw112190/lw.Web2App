#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/packer/manifest.h"

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

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

}  // namespace

void RunFilesystemAccessTests() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() /
                    ("lwweb-filesystem-" + unique);
  const auto outside = std::filesystem::temp_directory_path() /
                       ("lwweb-filesystem-outside-" + unique);
  TempDirectoryGuard cleanup{root};
  TempDirectoryGuard outside_cleanup{outside};
  std::filesystem::create_directories(root / "folder");
  std::filesystem::create_directories(outside);
  std::ofstream(root / "folder" / "source.txt") << "first";
  std::ofstream(outside / "outside.txt") << "outside";

  lwweb::Manifest manifest;
  manifest.app_id = "test.filesystem.app";
  manifest.ipc.enabled = true;
  manifest.ipc.capabilities = {"fs.exists", "fs.copy"};
  manifest.ipc.filesystem_roots = {root.u8string()};
  lwweb::ValidateManifest(manifest);
  lwweb::IpcRuntimeServices services;
  services.platform = "windows";
  lwweb::IpcDispatcher dispatcher(manifest, services);

  const auto source = root / "folder" / "source.txt";
  const auto destination = root / "folder" / "copy.txt";
  auto exists = dispatcher.Dispatch(
      {"exists", "fs.exists", {{"path", source.u8string()}}});
  Check(exists.ok && exists.result["exists"] == true,
        "fs.exists reports an authorized existing path");
  auto missing = dispatcher.Dispatch(
      {"missing", "fs.exists",
       {{"path", (root / "folder" / "missing.txt").u8string()}}});
  Check(missing.ok && missing.result["exists"] == false,
        "fs.exists reports an authorized missing path");

  auto copied = dispatcher.Dispatch(
      {"copy", "fs.copy",
       {{"from", source.u8string()}, {"to", destination.u8string()}}});
  Check(copied.ok && ReadText(destination) == "first",
        "fs.copy copies a regular file inside an authorized root");
  auto duplicate = dispatcher.Dispatch(
      {"copy-again", "fs.copy",
       {{"from", source.u8string()}, {"to", destination.u8string()}}});
  Check(!duplicate.ok && duplicate.error.code == "ALREADY_EXISTS",
        "fs.copy refuses to overwrite by default");

  std::ofstream(source, std::ios::trunc) << "second";
  auto overwritten = dispatcher.Dispatch(
      {"copy-overwrite", "fs.copy",
       {{"from", source.u8string()},
        {"to", destination.u8string()},
        {"overwrite", true}}});
  Check(overwritten.ok && ReadText(destination) == "second",
        "fs.copy overwrites only when explicitly requested");

  auto source_denied = dispatcher.Dispatch(
      {"copy-source-denied", "fs.copy",
       {{"from", (outside / "outside.txt").u8string()},
        {"to", (root / "folder" / "outside-copy.txt").u8string()}}});
  Check(!source_denied.ok && source_denied.error.code == "PERMISSION_DENIED",
        "fs.copy rejects a source outside authorized roots");
  auto destination_denied = dispatcher.Dispatch(
      {"copy-destination-denied", "fs.copy",
       {{"from", source.u8string()},
        {"to", (outside / "copied.txt").u8string()}}});
  Check(!destination_denied.ok &&
            destination_denied.error.code == "PERMISSION_DENIED" &&
            !std::filesystem::exists(outside / "copied.txt"),
        "fs.copy rejects a destination outside authorized roots");
  auto directory_denied = dispatcher.Dispatch(
      {"copy-directory", "fs.copy",
       {{"from", (root / "folder").u8string()},
        {"to", (root / "folder-copy").u8string()}}});
  Check(!directory_denied.ok && directory_denied.error.code == "UNSUPPORTED",
        "fs.copy does not recursively copy directories in the first version");

  lwweb::Manifest limited = manifest;
  limited.ipc.capabilities = {"fs.exists"};
  lwweb::IpcDispatcher limited_dispatcher(limited, services);
  auto capability_denied = limited_dispatcher.Dispatch(
      {"copy-capability-denied", "fs.copy",
       {{"from", source.u8string()},
        {"to", (root / "folder" / "denied.txt").u8string()}}});
  Check(!capability_denied.ok &&
            capability_denied.error.code == "PERMISSION_DENIED",
        "fs.copy requires its dedicated capability");
}
