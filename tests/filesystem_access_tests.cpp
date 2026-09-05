#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/ipc/filesystem_access.h"
#include "lwweb/ipc/ipc_message.h"
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

  const auto fallback_source = root / "folder" / "fallback-source.txt";
  const auto fallback_destination =
      root / "folder" / "fallback-destination.txt";
  std::ofstream(fallback_source) << "fallback";
  lwweb::ipc_detail::MoveRegularFileByCopyAndDelete(
      fallback_source, fallback_destination, false);
  Check(!std::filesystem::exists(fallback_source) &&
            ReadText(fallback_destination) == "fallback",
        "cross-filesystem move fallback publishes a complete copy before removing the source");

  std::ofstream(fallback_source) << "new";
  bool fallback_exists_rejected = false;
  try {
    lwweb::ipc_detail::MoveRegularFileByCopyAndDelete(
        fallback_source, fallback_destination, false);
  } catch (const lwweb::IpcException& error) {
    fallback_exists_rejected = error.Code() == "ALREADY_EXISTS";
  }
  Check(fallback_exists_rejected &&
            ReadText(fallback_source) == "new" &&
            ReadText(fallback_destination) == "fallback",
        "cross-filesystem move fallback preserves both files when overwrite is disabled");

  lwweb::ipc_detail::MoveRegularFileByCopyAndDelete(
      fallback_source, fallback_destination, true);
  Check(!std::filesystem::exists(fallback_source) &&
            ReadText(fallback_destination) == "new",
        "cross-filesystem move fallback replaces the destination only when requested");

  // A configured root may be absent at startup; its lexical and resolved
  // boundaries remain bound to the declared location.
  const auto pending_root = root / "future-root";
  Check(!std::filesystem::exists(pending_root),
        "pending-root test starts with a missing directory");
  lwweb::Manifest pending_manifest = manifest;
  pending_manifest.ipc.capabilities = {"fs.exists", "fs.list", "fs.mkdir"};
  pending_manifest.ipc.filesystem_roots = {pending_root.u8string()};
  lwweb::IpcDispatcher pending_dispatcher(pending_manifest, services);
  auto pending_exists = pending_dispatcher.Dispatch(
      {"pending-root", "fs.exists", {{"path", pending_root.u8string()}}});
  auto pending_child = pending_dispatcher.Dispatch(
      {"pending-child", "fs.exists",
       {{"path", (pending_root / "usage.jsonl").u8string()}}});
  Check(pending_exists.ok && pending_exists.result["exists"] == false &&
            pending_child.ok && pending_child.result["exists"] == false,
        "missing configured root and child report exists=false");
  auto pending_sibling = pending_dispatcher.Dispatch(
      {"pending-sibling", "fs.exists",
       {{"path", (root / "future-root-other").u8string()}}});
  Check(!pending_sibling.ok && pending_sibling.error.code == "PERMISSION_DENIED",
        "pending root rejects a sibling with a colliding textual prefix");
  auto pending_mkdir = pending_dispatcher.Dispatch(
      {"pending-mkdir", "fs.mkdir", {{"path", pending_root.u8string()}}});
  Check(pending_mkdir.ok && std::filesystem::is_directory(pending_root),
        "pending root can be created through its pre-authorization");
  std::ofstream(pending_root / "usage.jsonl") << "{}";
  auto pending_list = pending_dispatcher.Dispatch(
      {"pending-list", "fs.list", {{"path", pending_root.u8string()}}});
  Check(pending_list.ok && pending_list.result["entries"].size() == 1,
        "pending root becomes usable without rebuilding the dispatcher");

  const auto redirected_root = root / "redirected-root";
  const auto redirected_target = outside / "redirect-target";
  Check(!std::filesystem::exists(redirected_root),
        "redirected-root test starts with a missing directory");
  lwweb::Manifest redirected_manifest = pending_manifest;
  redirected_manifest.ipc.filesystem_roots = {redirected_root.u8string()};
  lwweb::IpcDispatcher redirected_dispatcher(redirected_manifest, services);
  std::error_code link_error;
  std::filesystem::create_directory_symlink(outside, redirected_root, link_error);
  if (!link_error) {
    auto escaped = redirected_dispatcher.Dispatch(
        {"redirected", "fs.exists", {{"path", redirected_root.u8string()}}});
    Check(!escaped.ok && escaped.error.code == "PERMISSION_DENIED",
          "pending root rejects a symlink redirect outside its policy boundary");
  }
}
