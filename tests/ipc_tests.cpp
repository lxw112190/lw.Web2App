#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/ipc/ipc_message.h"
#include "lwweb/ipc/ipc_permissions.h"
#include "lwweb/packer/manifest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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
}

void RunIpcTests() {
  const auto request = lwweb::ParseIpcRequest(
      R"({"v":1,"kind":"request","id":"42","method":"app.getInfo","params":{}})");
  Check(request.id == "42" && request.method == "app.getInfo",
        "IPC request protocol parsed");
  bool malformed_rejected = false;
  try {
    (void)lwweb::ParseIpcRequest(R"({"v":1,"kind":"request","id":1})");
  } catch (const lwweb::IpcException& error) {
    malformed_rejected = error.Code() == "INVALID_REQUEST";
  }
  Check(malformed_rejected, "malformed IPC request rejected with stable error code");
  bool oversized_rejected = false;
  try {
    (void)lwweb::ParseIpcRequest(std::string(lwweb::kMaxIpcMessageSize + 1, 'x'));
  } catch (const lwweb::IpcException& error) {
    oversized_rejected = error.Code() == "INVALID_REQUEST";
  }
  Check(oversized_rejected, "oversized IPC request rejected");

  Check(lwweb::IsAllowedIpcSource("http://127.0.0.1:53182/login.html",
                                  "http://127.0.0.1:53182"),
        "exact local origin accepted");
  Check(!lwweb::IsAllowedIpcSource("http://127.0.0.1:53183/login.html",
                                   "http://127.0.0.1:53182"),
        "different local port rejected");
  Check(!lwweb::IsAllowedIpcSource("http://127.0.0.1.evil.test:53182/",
                                   "http://127.0.0.1:53182"),
        "origin prefix spoof rejected");

  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto root = std::filesystem::temp_directory_path() / ("lwweb-ipc-" + unique);
  TempDirectoryGuard cleanup{root};
  const auto grant_root =
      std::filesystem::temp_directory_path() / ("lwweb-ipc-grant-" + unique);
  TempDirectoryGuard grant_cleanup{grant_root};
  std::filesystem::create_directories(root / "folder");
  std::filesystem::create_directories(grant_root);
  {
    std::ofstream(root / "folder" / "before.txt") << "ipc";
  }

  lwweb::Manifest manifest;
  manifest.app_id = "test.ipc.app";
  manifest.title = "IPC Test";
  manifest.ipc.enabled = true;
  manifest.ipc.capabilities = {"app.info", "dialog.directory", "fs.exists",
                               "fs.list", "fs.copy", "fs.move", "fs.delete"};
  manifest.ipc.filesystem_roots = {root.u8string()};
  lwweb::ValidateManifest(manifest);
  const auto round_trip =
      lwweb::ParseManifest(lwweb::SerializeManifest(manifest));
  Check(round_trip.ipc.enabled && round_trip.ipc.capabilities.size() == 7 &&
            round_trip.ipc.filesystem_roots.size() == 1,
        "IPC manifest configuration round-trips");

  lwweb::IpcRuntimeServices services;
  services.platform = "windows";
  services.runtime_version = "0.test";
  services.select_directory = [grant_root] { return std::optional(grant_root); };
  lwweb::IpcDispatcher dispatcher(manifest, services);
  const auto info = dispatcher.Dispatch(request);
  Check(info.ok && info.result["appId"] == "test.ipc.app" &&
            info.result["platform"] == "windows" && info.result["arch"] == "x64",
        "app.getInfo returns stable cross-platform fields");
  Check(dispatcher.TryBegin("same"), "first IPC request ID reserved");
  Check(!dispatcher.TryBegin("same"), "duplicate pending IPC request ID rejected");
  dispatcher.End("same");
  const auto selected = dispatcher.Dispatch({"dialog", "dialog.selectDirectory", {}});
  Check(selected.ok && selected.result["path"] == grant_root.u8string(),
        "directory dialog creates a session grant");
  const auto granted_list = dispatcher.Dispatch(
      {"grant-list", "fs.list", {{"path", grant_root.u8string()}}});
  Check(granted_list.ok, "session grant authorizes a selected directory");

  auto list = dispatcher.Dispatch({"list", "fs.list", {{"path", (root / "folder").u8string()}}});
  Check(list.ok && list.result["entries"].size() == 1,
        "fs.list reads an authorized directory");
  auto moved = dispatcher.Dispatch(
      {"move", "fs.move", {{"from", (root / "folder" / "before.txt").u8string()},
                              {"to", (root / "folder" / "after.txt").u8string()}}});
  Check(moved.ok && std::filesystem::exists(root / "folder" / "after.txt"),
        "fs.move operates inside an authorized root");
  auto removed = dispatcher.Dispatch(
      {"delete", "fs.delete", {{"path", (root / "folder" / "after.txt").u8string()}}});
  Check(removed.ok && !std::filesystem::exists(root / "folder" / "after.txt"),
        "fs.delete operates inside an authorized root");
  const auto outside = root.parent_path() / "lwweb-ipc-outside.txt";
  std::ofstream(outside) << "outside";
  auto denied = dispatcher.Dispatch(
      {"denied", "fs.delete", {{"path", outside.u8string()}}});
  Check(!denied.ok && denied.error.code == "PERMISSION_DENIED" &&
            std::filesystem::exists(outside),
        "filesystem operation outside roots is denied");
  auto traversal_denied = dispatcher.Dispatch(
      {"traversal", "fs.list", {{"path", (root / "folder" / ".." / ".." / outside.filename()).u8string()}}});
  Check(!traversal_denied.ok &&
            (traversal_denied.error.code == "PERMISSION_DENIED" ||
             traversal_denied.error.code == "NOT_FOUND"),
        "filesystem traversal cannot escape an authorized root");
  std::error_code ignored;
  std::filesystem::remove(outside, ignored);

  const auto external_directory =
      std::filesystem::temp_directory_path() / ("lwweb-ipc-external-" + unique);
  TempDirectoryGuard external_cleanup{external_directory};
  std::filesystem::create_directories(external_directory);
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(external_directory, root / "escape-link",
                                             symlink_error);
  if (!symlink_error) {
    auto symlink_denied = dispatcher.Dispatch(
        {"symlink", "fs.list", {{"path", (root / "escape-link").u8string()}}});
    Check(!symlink_denied.ok && symlink_denied.error.code == "PERMISSION_DENIED",
          "symbolic link cannot escape an authorized root");
  }

  lwweb::Manifest limited = manifest;
  limited.ipc.capabilities = {"app.info"};
  lwweb::IpcDispatcher limited_dispatcher(limited, services);
  auto capability_denied = limited_dispatcher.Dispatch(
      {"denied-capability", "fs.list", {{"path", root.u8string()}}});
  Check(!capability_denied.ok && capability_denied.error.code == "PERMISSION_DENIED",
        "missing method capability is denied");
  auto unknown = dispatcher.Dispatch({"unknown", "unknown.method", {}});
  Check(!unknown.ok && unknown.error.code == "METHOD_NOT_FOUND",
        "unknown IPC method returns stable error");
  lwweb::IpcRuntimeServices canceled_services;
  canceled_services.platform = "windows";
  canceled_services.select_directory = [] {
    return std::optional<std::filesystem::path>{};
  };
  lwweb::Manifest dialog_only = manifest;
  dialog_only.ipc.capabilities = {"dialog.directory"};
  lwweb::IpcDispatcher canceled_dispatcher(dialog_only, canceled_services);
  const auto canceled = canceled_dispatcher.Dispatch(
      {"cancel", "dialog.selectDirectory", {}});
  Check(!canceled.ok && canceled.error.code == "USER_CANCELLED",
        "canceled directory dialog returns stable error");

  lwweb::Manifest url_manifest;
  url_manifest.mode = lwweb::AppMode::Url;
  url_manifest.url = "https://example.com";
  url_manifest.ipc.enabled = true;
  url_manifest.ipc.capabilities = {"app.info"};
  bool url_ipc_rejected = false;
  try {
    lwweb::ValidateManifest(url_manifest);
  } catch (...) {
    url_ipc_rejected = true;
  }
  Check(url_ipc_rejected, "URL mode cannot enable Native IPC");

  const auto bridge = lwweb::BuildIpcBridgeScript("windows", "windows");
  Check(bridge.find("window,'lw'") != std::string::npos &&
            bridge.find("chrome.webview.postMessage") != std::string::npos,
        "Windows bridge exposes the stable window.lw API");
  const auto linux_bridge =
      lwweb::BuildIpcBridgeScript("linux", "linux", "session-secret");
  Check(linux_bridge.find("session-secret:") != std::string::npos &&
            linux_bridge.find("messageHandlers.lwIpc") != std::string::npos,
        "Linux bridge binds messages to a per-process session token");
}
