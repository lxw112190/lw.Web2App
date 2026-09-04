#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/ipc/ipc_message.h"
#include "lwweb/ipc/ipc_permissions.h"
#include "lwweb/packer/manifest.h"
#include "lwweb/runtime/local_file_grant.h"

#include <chrono>
#include <algorithm>
#include <condition_variable>
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
    std::ofstream(grant_root / std::filesystem::u8path(u8"本地文档.pdf"),
                  std::ios::binary) << "%PDF-local-file-bridge";
    std::ofstream(root / "folder" / "trash-me.jpg", std::ios::binary)
        << "jpeg-placeholder";
  }

  lwweb::Manifest manifest;
  manifest.app_id = "test.ipc.app";
  manifest.title = "IPC Test";
  manifest.ipc.enabled = true;
  manifest.ipc.capabilities = {
      "app.info", "app.paths", "dialog.directory", "dialog.file", "fs.exists", "fs.list", "fs.read",
      "fs.mkdir", "fs.copy", "fs.move", "fs.trash", "fs.delete", "fs.watch",
      "window.control", "app.lifecycle", "tray"};
  manifest.ipc.filesystem_roots = {root.u8string()};
  lwweb::ValidateManifest(manifest);
  const auto round_trip =
      lwweb::ParseManifest(lwweb::SerializeManifest(manifest));
  Check(round_trip.ipc.enabled && round_trip.ipc.capabilities.size() == 16 &&
            round_trip.ipc.filesystem_roots.size() == 1,
        "IPC manifest configuration round-trips");

  lwweb::IpcRuntimeServices services;
  services.platform = "windows";
  services.runtime_version = "0.test";
  std::string received_event;
  nlohmann::json received_event_data;
  std::mutex event_mutex;
  std::condition_variable event_ready;
  bool changed = false;
  services.emit_event = [&received_event, &received_event_data, &event_mutex,
                          &event_ready, &changed](const std::string& name,
                                                   const nlohmann::json& data) {
    {
      std::lock_guard lock(event_mutex);
      received_event = name;
      received_event_data = data;
      changed = name == "fs.changed";
    }
    event_ready.notify_one();
  };
  bool window_command_called = false;
  bool quit_requested = false;
  services.get_window_state = [] {
    return nlohmann::json{{"visible", true}, {"minimized", false},
                          {"maximized", false}, {"fullscreen", false},
                          {"alwaysOnTop", false}, {"closeBehavior", "exit"}};
  };
  services.control_window = [&window_command_called](const std::string& method,
                                                      const nlohmann::json&) {
    window_command_called = method == "window.focus";
  };
  services.request_quit = [&quit_requested] { quit_requested = true; };
  services.tray_create = [](const nlohmann::json&) {
    return nlohmann::json{{"created", true}};
  };
  services.tray_update = [](const nlohmann::json&) {
    return nlohmann::json{{"updated", true}};
  };
  services.tray_destroy = [] { return nlohmann::json{{"destroyed", true}}; };
  services.select_directory = [grant_root] { return std::optional(grant_root); };
  const auto selected_file = grant_root / std::filesystem::u8path(u8"本地文档.pdf");
  bool file_options_checked = false;
  services.open_files = [selected_file, &file_options_checked](
                            const lwweb::OpenFileDialogOptions& options) {
    file_options_checked = !options.multiple && options.filters.size() == 2 &&
                           options.filters[0].name == "PDF" &&
                           options.filters[0].extensions ==
                               std::vector<std::string>{"pdf"} &&
                           options.filters[1].extensions ==
                               std::vector<std::string>{"*"};
    return std::optional<std::vector<std::filesystem::path>>({selected_file});
  };
  const auto trashed_file = grant_root / "trashed.jpg";
  services.trash_file = [trashed_file](const std::filesystem::path& path) {
    std::filesystem::rename(path, trashed_file);
  };
  services.file_grants = std::make_shared<lwweb::LocalFileGrantManager>();
  lwweb::IpcDispatcher dispatcher(manifest, services);
  const auto info = dispatcher.Dispatch(request);
  Check(info.ok && info.result["appId"] == "test.ipc.app" &&
            info.result["platform"] == "windows" && info.result["arch"] == "x64",
        "app.getInfo returns stable cross-platform fields");
  const auto path_result = dispatcher.Dispatch(
      {"path", "app.getPath", {{"name", "appCache"}}});
  Check(path_result.ok && path_result.result["name"] == "appCache" &&
            !path_result.result["path"].get<std::string>().empty(),
        "app.getPath resolves an application-scoped cache directory");
  const auto window_state = dispatcher.Dispatch({"state", "window.getState", {}});
  Check(window_state.ok && window_state.result["closeBehavior"] == "exit",
        "window.getState returns platform window state");
  const auto focus = dispatcher.Dispatch({"focus", "window.focus", {}});
  Check(focus.ok && focus.result["ok"] == true && window_command_called,
        "window.focus uses the injected UI callback");
  const auto quit = dispatcher.Dispatch({"quit", "app.quit", {}});
  Check(quit.ok && quit.result["quitting"] == true && quit_requested,
        "app.quit schedules platform runtime shutdown");
  const auto tray = dispatcher.Dispatch(
      {"tray", "tray.create", {{"tooltip", "IPC test"},
                                 {"menu", {{{"id", "open"}, {"label", "打开"}},
                                            {{"type", "separator"}},
                                            {{"id", "exit"}, {"label", "退出"}}}}}});
  Check(tray.ok && tray.result["created"] == true,
        "tray.create validates and invokes the platform service");
  const auto invalid_tray = dispatcher.Dispatch(
      {"bad-tray", "tray.update", {{"menu", {{{"id", "dup"}, {"label", "A"}},
                                               {{"id", "dup"}, {"label", "B"}}}}}});
  Check(!invalid_tray.ok && invalid_tray.error.code == "INVALID_ARGUMENT",
        "tray menu rejects duplicate ids");
  Check(dispatcher.EmitEvent("fs.changed", {{"watcherId", "watch_1"}}) &&
            received_event == "fs.changed" && received_event_data["watcherId"] == "watch_1",
        "Native events are delivered through the injected transport");
  Check(!dispatcher.EmitEvent("invalid event", {}),
        "event transport does not report delivery when validation fails");
  {
    std::lock_guard lock(event_mutex);
    changed = false;
    received_event_data = nlohmann::json::object();
  }
  const auto watch = dispatcher.Dispatch(
      {"watch", "fs.watch", {{"path", root.u8string()}, {"recursive", true},
                               {"debounceMs", 50}}});
  if (!watch.ok) {
    throw std::runtime_error("fs.watch creates an authorized directory watcher: " +
                             watch.error.code + " " + watch.error.message);
  }
  Check(watch.result["watcherId"].get<std::string>().rfind("watch_", 0) == 0,
        "fs.watch creates an authorized directory watcher");
  std::ofstream(root / "folder" / "watch.txt") << "watch";
  {
    std::unique_lock lock(event_mutex);
    Check(event_ready.wait_for(lock, std::chrono::seconds(3), [&changed] { return changed; }),
          "fs.watch reports a created file through the Native event channel");
  }
  const bool watch_file_reported = std::any_of(
      received_event_data["changes"].begin(), received_event_data["changes"].end(),
      [](const nlohmann::json& change) {
        return change.value("relativePath", "") == "folder/watch.txt" &&
               change.value("type", "") == "created";
      });
  if (!watch_file_reported || received_event_data["overflow"] != false) {
    throw std::runtime_error("fs.watch returns bounded relative-path change metadata: " +
                             received_event_data.dump());
  }
  const auto unwatched = dispatcher.Dispatch(
      {"unwatch", "fs.unwatch", {{"watcherId", watch.result["watcherId"]}}});
  Check(unwatched.ok && unwatched.result["stopped"] == true,
        "fs.unwatch stops and joins the directory watcher");
  Check(dispatcher.TryBegin("same"), "first IPC request ID reserved");
  Check(!dispatcher.TryBegin("same"), "duplicate pending IPC request ID rejected");
  dispatcher.End("same");
  const auto selected = dispatcher.Dispatch({"dialog", "dialog.selectDirectory", {}});
  Check(selected.ok && selected.result["path"] == grant_root.u8string(),
        "directory dialog creates a session grant");
  const auto granted_list = dispatcher.Dispatch(
      {"grant-list", "fs.list", {{"path", grant_root.u8string()}}});
  Check(granted_list.ok && granted_list.result["entries"][0].contains("modifiedAt") &&
            granted_list.result["entries"][0]["mime"] == "application/pdf",
        "fs.list returns authorized entries with preview metadata");
  const auto read_grant = dispatcher.Dispatch(
      {"open-read", "fs.openRead", {{"path", selected_file.u8string()}}});
  Check(read_grant.ok && read_grant.result["name"] == u8"本地文档.pdf" &&
            read_grant.result["url"].get<std::string>().rfind("/__lw_file__/", 0) == 0 &&
            !read_grant.result.contains("path"),
        "fs.openRead creates an opaque HTTP grant for an authorized file");
  const auto read_grant_id = read_grant.result["id"].get<std::string>();
  const auto read_revoked = dispatcher.Dispatch(
      {"revoke-read", "file.revoke", {{"id", read_grant_id}}});
  Check(read_revoked.ok && read_revoked.result["revoked"] == true,
        "fs.read capability can revoke its local file grant");
  const auto opened = dispatcher.Dispatch(
      {"open-file", "dialog.openFile",
       {{"multiple", false},
        {"filters", {{{"name", "PDF"}, {"extensions", {".pdf"}}},
                     {{"name", "All"}, {"extensions", {"*"}}}}}}});
  Check(opened.ok && file_options_checked && opened.result["files"].size() == 1,
        "dialog.openFile validates filters and returns a stable files array");
  const auto public_file = opened.result["files"][0];
  Check(public_file["name"] == u8"本地文档.pdf" &&
            public_file["mime"] == "application/pdf" &&
            public_file["url"].get<std::string>().rfind("/__lw_file__/", 0) == 0 &&
            public_file.find("path") == public_file.end(),
        "file dialog response exposes metadata and URL without the local path");
  const auto grant_id = public_file["id"].get<std::string>();
  Check(grant_id.size() == 32 && services.file_grants->Find(grant_id).has_value(),
        "dialog.openFile creates a 128-bit session grant");
  const auto revoked = dispatcher.Dispatch(
      {"revoke-file", "file.revoke", {{"id", grant_id}}});
  Check(revoked.ok && revoked.result["revoked"] == true &&
            !services.file_grants->Find(grant_id),
        "file.revoke immediately removes a session grant");
  const auto invalid_filter = dispatcher.Dispatch(
      {"invalid-filter", "dialog.openFile",
       {{"filters", {{{"name", "unsafe"}, {"extensions", {"../pdf"}}}}}}});
  Check(!invalid_filter.ok && invalid_filter.error.code == "INVALID_ARGUMENT",
        "dialog.openFile rejects unsafe filter extensions before opening UI");

  auto list = dispatcher.Dispatch({"list", "fs.list", {{"path", (root / "folder").u8string()}}});
  Check(list.ok && list.result["entries"].size() == 3,
        "fs.list reads an authorized directory");
  const auto created_directory = root / "folder" / "selected";
  auto created = dispatcher.Dispatch(
      {"mkdir", "fs.mkdir", {{"path", created_directory.u8string()}}});
  Check(created.ok && std::filesystem::is_directory(created_directory),
        "fs.mkdir creates a direct child inside an authorized root");
  auto moved = dispatcher.Dispatch(
      {"move", "fs.move", {{"from", (root / "folder" / "before.txt").u8string()},
                              {"to", (root / "folder" / "after.txt").u8string()}}});
  Check(moved.ok && std::filesystem::exists(root / "folder" / "after.txt"),
        "fs.move operates inside an authorized root");
  auto removed = dispatcher.Dispatch(
      {"delete", "fs.delete", {{"path", (root / "folder" / "after.txt").u8string()}}});
  Check(removed.ok && !std::filesystem::exists(root / "folder" / "after.txt"),
        "fs.delete operates inside an authorized root");
  auto trashed = dispatcher.Dispatch(
      {"trash", "fs.trash", {{"path", (root / "folder" / "trash-me.jpg").u8string()}}});
  Check(trashed.ok && !std::filesystem::exists(root / "folder" / "trash-me.jpg") &&
            std::filesystem::exists(trashed_file),
        "fs.trash validates the file then delegates to the platform Trash service");
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
  lwweb::IpcRuntimeServices canceled_file_services;
  canceled_file_services.platform = "windows";
  canceled_file_services.file_grants =
      std::make_shared<lwweb::LocalFileGrantManager>();
  canceled_file_services.open_files = [](const lwweb::OpenFileDialogOptions&) {
    return std::optional<std::vector<std::filesystem::path>>{};
  };
  lwweb::Manifest file_dialog_only = manifest;
  file_dialog_only.ipc.capabilities = {"dialog.file"};
  lwweb::IpcDispatcher canceled_file_dispatcher(file_dialog_only,
                                                 canceled_file_services);
  const auto canceled_file = canceled_file_dispatcher.Dispatch(
      {"cancel-file", "dialog.openFile", {}});
  Check(!canceled_file.ok && canceled_file.error.code == "USER_CANCELLED",
        "canceled file dialog returns stable error");

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
