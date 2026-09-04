#include "lwweb/cli/command_line.h"
#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/runtime/local_file_grant.h"
#include "lwweb/runtime/resource_server.h"
#include "lwweb/runtime/single_instance.h"
#include "lwweb/version.h"

#include <gtk/gtk.h>
#include <openssl/rand.h>
#include <webkit2/webkit2.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

std::filesystem::path XdgPath(const char* variable, const char* fallback) {
  if (const auto* value = std::getenv(variable); value && *value) return value;
  const auto* home = std::getenv("HOME");
  if (!home || !*home) throw Error("HOME is not defined");
  return std::filesystem::path(home) / fallback;
}

const char* FirstEnvironmentValue(const std::vector<const char*>& names) {
  for (const auto* name : names) {
    if (const auto* value = std::getenv(name); value && *value) return value;
  }
  return nullptr;
}

std::vector<std::string> ProxyIgnoreHosts() {
  const auto* value = FirstEnvironmentValue({"no_proxy", "NO_PROXY"});
  if (!value) return {};
  std::vector<std::string> hosts;
  std::string list(value);
  for (std::size_t begin = 0; begin <= list.size();) {
    const auto comma = list.find(',', begin);
    const auto end = comma == std::string::npos ? list.size() : comma;
    auto first = begin;
    while (first < end && std::isspace(static_cast<unsigned char>(list[first]))) ++first;
    auto last = end;
    while (last > first && std::isspace(static_cast<unsigned char>(list[last - 1]))) --last;
    if (last > first) hosts.emplace_back(list.substr(first, last - first));
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return hosts;
}

void ConfigureEnvironmentProxy(WebKitWebsiteDataManager* manager, Logger& logger) {
  const auto* all_proxy = FirstEnvironmentValue({"all_proxy", "ALL_PROXY"});
  const auto* http_proxy = FirstEnvironmentValue({"http_proxy", "HTTP_PROXY"});
  const auto* https_proxy = FirstEnvironmentValue({"https_proxy", "HTTPS_PROXY"});
  if (!all_proxy && !http_proxy && !https_proxy) return;
  const auto ignore_hosts = ProxyIgnoreHosts();
  std::vector<const char*> ignore_host_pointers;
  ignore_host_pointers.reserve(ignore_hosts.size() + 1);
  for (const auto& host : ignore_hosts) ignore_host_pointers.push_back(host.c_str());
  ignore_host_pointers.push_back(nullptr);
  auto* settings = webkit_network_proxy_settings_new(
      all_proxy, ignore_hosts.empty() ? nullptr : ignore_host_pointers.data());
  if (http_proxy)
    webkit_network_proxy_settings_add_proxy_for_scheme(settings, "http", http_proxy);
  if (https_proxy)
    webkit_network_proxy_settings_add_proxy_for_scheme(settings, "https", https_proxy);
  webkit_website_data_manager_set_network_proxy_settings(
      manager, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, settings);
  webkit_network_proxy_settings_free(settings);
  logger.Info("WebKitGTK proxy configured from environment");
}

std::string DefaultOutputPath() {
  return (CurrentExecutablePath().parent_path() / "out" / "MyWebApp").string();
}

std::string RandomIpcTransportToken() {
  unsigned char bytes[32]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    throw Error("Cannot generate the Native IPC session token");
  static constexpr char hex[] = "0123456789abcdef";
  std::string result(sizeof(bytes) * 2, '0');
  for (std::size_t i = 0; i < sizeof(bytes); ++i) {
    result[i * 2] = hex[bytes[i] >> 4];
    result[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  return result;
}

// GTK/WebKitGTK 信号回调共享的轻量运行状态；对象生命周期覆盖整个 gtk_main 循环。
struct RuntimeState {
  Logger* logger = nullptr;
  GtkWidget* window = nullptr;
  WebKitWebView* view = nullptr;
  std::shared_ptr<IpcDispatcher> ipc_dispatcher;
  std::string local_origin;
  std::string ipc_transport_token;
  bool fullscreen = false;
  bool always_on_top = false;
  std::string close_behavior = "exit";
};

void OnRuntimeDestroy(GtkWidget*, gpointer) { gtk_main_quit(); }

gboolean OnRuntimeDelete(GtkWidget* window, GdkEvent*, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  if (state && state->close_behavior == "hide") {
    gtk_widget_hide(window);
    return TRUE;
  }
  return FALSE;
}

gboolean QuitRuntime(gpointer data) {
  gtk_widget_destroy(GTK_WIDGET(data));
  return G_SOURCE_REMOVE;
}

nlohmann::json WindowState(GtkWidget* window, const RuntimeState& state) {
  bool minimized = false;
  bool maximized = false;
  if (auto* native = gtk_widget_get_window(window)) {
    const auto flags = gdk_window_get_state(native);
    minimized = (flags & GDK_WINDOW_STATE_ICONIFIED) != 0;
    maximized = (flags & GDK_WINDOW_STATE_MAXIMIZED) != 0;
  }
  return {{"visible", gtk_widget_get_visible(window) != FALSE},
          {"minimized", minimized},
          {"maximized", maximized},
          {"fullscreen", state.fullscreen},
          {"alwaysOnTop", state.always_on_top},
          {"closeBehavior", state.close_behavior}};
}

void ControlWindow(GtkWidget* window, RuntimeState& state, const std::string& method,
                   const nlohmann::json& params) {
  auto* native = GTK_WINDOW(window);
  if (method == "window.show") {
    gtk_widget_show(window);
  } else if (method == "window.hide") {
    gtk_widget_hide(window);
  } else if (method == "window.minimize") {
    gtk_window_iconify(native);
  } else if (method == "window.maximize") {
    gtk_window_maximize(native);
  } else if (method == "window.restore") {
    gtk_window_deiconify(native);
    gtk_window_unmaximize(native);
  } else if (method == "window.focus") {
    gtk_widget_show(window);
    gtk_window_deiconify(native);
    gtk_window_present(native);
  } else if (method == "window.setAlwaysOnTop") {
    state.always_on_top = params.at("enabled").get<bool>();
    gtk_window_set_keep_above(native, state.always_on_top);
  } else if (method == "window.setCloseBehavior") {
    state.close_behavior = params.at("behavior").get<std::string>();
  }
}

gboolean OnRuntimeKey(GtkWidget* window, GdkEventKey* event, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  if (event->keyval == GDK_KEY_F11) {
    state->fullscreen = !state->fullscreen;
    if (state->fullscreen)
      gtk_window_fullscreen(GTK_WINDOW(window));
    else
      gtk_window_unfullscreen(GTK_WINDOW(window));
    return TRUE;
  }
  if (event->keyval == GDK_KEY_Escape && state->fullscreen) {
    state->fullscreen = false;
    gtk_window_unfullscreen(GTK_WINDOW(window));
    return TRUE;
  }
  return FALSE;
}

void OnLoadChanged(WebKitWebView* webview, WebKitLoadEvent event, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  if (event == WEBKIT_LOAD_STARTED) {
    const auto* uri = webkit_web_view_get_uri(webview);
    state->logger->Info(std::string("Navigate: ") + (uri ? uri : ""));
  } else if (event == WEBKIT_LOAD_FINISHED) {
    state->logger->Info("Navigation completed");
  }
}

gboolean OnLoadFailed(WebKitWebView*, WebKitLoadEvent, const char* uri, GError* error,
                      gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  state->logger->Error("Navigation failed: " + std::string(uri ? uri : "") +
                       " - " + (error ? error->message : "unknown error"));
  return FALSE;
}

void OnWebProcessTerminated(WebKitWebView*, WebKitWebProcessTerminationReason reason,
                            gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  state->logger->Error("WebKit web process terminated, reason=" + std::to_string(reason));
}

void OnScriptMessage(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  auto* value = webkit_javascript_result_get_js_value(result);
  auto* text = jsc_value_to_string(value);
  state->logger->Error(std::string("[WEB-ERROR] ") + (text ? text : "unknown error"));
  g_free(text);
}

void SendIpcResponse(WebKitWebView* view, const IpcResponse& response) {
  const auto script = "window.__lwIpcReceive&&window.__lwIpcReceive(" +
                      SerializeIpcResponse(response) + ")";
  webkit_web_view_run_javascript(view, script.c_str(), nullptr, nullptr, nullptr);
}

void SendIpcEvent(WebKitWebView* view, const std::string& event,
                  const nlohmann::json& data) {
  const auto script = "window.__lwIpcReceive&&window.__lwIpcReceive(" +
                      SerializeIpcEvent({event, data}) + ")";
  webkit_web_view_run_javascript(view, script.c_str(), nullptr, nullptr, nullptr);
}

struct LinuxIpcResponse {
  WebKitWebView* view = nullptr;
  std::shared_ptr<IpcDispatcher> dispatcher;
  Logger* logger = nullptr;
  std::string method;
  IpcResponse response;
};

struct LinuxIpcEvent {
  WebKitWebView* view = nullptr;
  std::string event;
  nlohmann::json data;
};

gboolean ApplyIpcEvent(gpointer data) {
  std::unique_ptr<LinuxIpcEvent> pending(static_cast<LinuxIpcEvent*>(data));
  try {
    SendIpcEvent(pending->view, pending->event, pending->data);
  } catch (const std::exception&) {
    // Event delivery is deliberately best effort; malformed/oversized data is dropped.
  }
  g_object_unref(pending->view);
  return G_SOURCE_REMOVE;
}

gboolean ApplyIpcResponse(gpointer data) {
  std::unique_ptr<LinuxIpcResponse> pending(static_cast<LinuxIpcResponse*>(data));
  pending->dispatcher->End(pending->response.id);
  if (pending->response.ok)
    pending->logger->Info("IPC result: " + pending->method + " OK");
  else
    pending->logger->Warn("IPC failed: " + pending->method + " - " +
                          pending->response.error.code);
  SendIpcResponse(pending->view, pending->response);
  g_object_unref(pending->view);
  return G_SOURCE_REMOVE;
}

std::optional<std::filesystem::path> ChooseIpcDirectory(GtkWidget* owner) {
  auto* dialog = gtk_file_chooser_dialog_new(
      "选择授权目录", GTK_WINDOW(owner), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
      "取消", GTK_RESPONSE_CANCEL, "选择", GTK_RESPONSE_ACCEPT, nullptr);
  const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
  if (response != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(dialog);
    return std::nullopt;
  }
  auto* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
  gtk_widget_destroy(dialog);
  if (!filename)
    throw IpcException("IO_ERROR", "Cannot read selected directory");
  std::filesystem::path selected(filename);
  g_free(filename);
  return selected;
}

std::optional<std::vector<std::filesystem::path>> ChooseIpcFiles(
    GtkWidget* owner, const OpenFileDialogOptions& selection) {
  auto* dialog = gtk_file_chooser_dialog_new(
      "选择本地文件", GTK_WINDOW(owner), GTK_FILE_CHOOSER_ACTION_OPEN,
      "取消", GTK_RESPONSE_CANCEL, "选择", GTK_RESPONSE_ACCEPT, nullptr);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog),
                                       selection.multiple);
  for (const auto& filter_config : selection.filters) {
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filter_config.name.c_str());
    for (const auto& extension : filter_config.extensions) {
      const auto pattern = extension == "*" ? "*" : "*." + extension;
      gtk_file_filter_add_pattern(filter, pattern.c_str());
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
  }
  const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
  if (response != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(dialog);
    return std::nullopt;
  }
  auto* filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
  gtk_widget_destroy(dialog);
  std::vector<std::filesystem::path> selected;
  for (auto* item = filenames; item; item = item->next) {
    if (item->data) selected.emplace_back(static_cast<const char*>(item->data));
  }
  g_slist_free_full(filenames, g_free);
  if (selected.size() > 256)
    throw IpcException("IO_ERROR", "Selected file count is invalid");
  return selected;
}

void MoveFileToTrash(const std::filesystem::path& path) {
  auto* file = g_file_new_for_path(path.c_str());
  if (!file)
    throw IpcException("UNSUPPORTED", "System Trash is unavailable");
  GError* error = nullptr;
  const bool moved = g_file_trash(file, nullptr, &error);
  g_object_unref(file);
  if (!moved) {
    const bool unsupported = error && error->domain == G_IO_ERROR &&
                             error->code == G_IO_ERROR_NOT_SUPPORTED;
    g_clear_error(&error);
    throw IpcException(unsupported ? "UNSUPPORTED" : "IO_ERROR",
                       unsupported ? "System Trash is unavailable"
                                   : "Move to Trash failed");
  }
}

void OnIpcScriptMessage(WebKitUserContentManager*, WebKitJavascriptResult* result,
                        gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  if (!state->ipc_dispatcher) return;
  const auto* current_uri = webkit_web_view_get_uri(state->view);
  if (!current_uri || !IsAllowedIpcSource(current_uri, state->local_origin)) {
    state->logger->Warn("Rejected Native IPC message from an untrusted origin");
    return;
  }
  auto* value = webkit_javascript_result_get_js_value(result);
  auto* raw = jsc_value_to_string(value);
  const std::string text = raw ? raw : "";
  g_free(raw);
  const auto prefix = state->ipc_transport_token + ":";
  if (state->ipc_transport_token.empty() || text.rfind(prefix, 0) != 0) {
    state->logger->Warn("Rejected Native IPC message without the session transport token");
    return;
  }
  IpcRequest request;
  try {
    request = ParseIpcRequest(text.substr(prefix.size()));
  } catch (const IpcException& error) {
    SendIpcResponse(state->view, MakeIpcError("", error.Code(), error.what()));
    return;
  }
  if (!state->ipc_dispatcher->TryBegin(request.id)) {
    SendIpcResponse(state->view,
                    MakeIpcError(request.id, "BUSY",
                                 "Duplicate or excessive pending request"));
    return;
  }
  state->logger->Info("IPC request: " + request.method);
  if (state->ipc_dispatcher->ExecutionFor(request.method) == IpcExecution::Worker) {
    auto* view = WEBKIT_WEB_VIEW(g_object_ref(state->view));
    const auto dispatcher = state->ipc_dispatcher;
    auto* logger = state->logger;
    std::thread([view, dispatcher, logger, request = std::move(request)]() mutable {
      auto pending = std::make_unique<LinuxIpcResponse>();
      pending->view = view;
      pending->dispatcher = dispatcher;
      pending->logger = logger;
      pending->method = request.method;
      pending->response = dispatcher->Dispatch(request);
      g_idle_add(ApplyIpcResponse, pending.release());
    }).detach();
  } else {
    const auto response = state->ipc_dispatcher->Dispatch(request);
    state->ipc_dispatcher->End(request.id);
    if (response.ok)
      state->logger->Info("IPC result: " + request.method + " OK");
    else
      state->logger->Warn("IPC failed: " + request.method + " - " +
                          response.error.code);
    SendIpcResponse(state->view, response);
  }
}

gboolean OnRuntimeDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision,
                               WebKitPolicyDecisionType type, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  if (!state->ipc_dispatcher || type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;
  auto* navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  auto* action = webkit_navigation_policy_decision_get_navigation_action(navigation);
  auto* request = webkit_navigation_action_get_request(action);
  const auto* uri = webkit_uri_request_get_uri(request);
  if (uri && IsAllowedIpcSource(uri, state->local_origin)) return FALSE;
  webkit_policy_decision_ignore(decision);
  state->logger->Warn("Blocked external navigation while Native IPC is enabled");
  return TRUE;
}

// 在下载真正写盘前交给用户选择目标位置；不把文件名和本地路径写入日志，
// 避免旧系统把 Token 放进下载 URL 或文件名时造成信息泄露。
gboolean OnDownloadDecideDestination(WebKitDownload* download,
                                     const gchar* suggested_filename, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  auto* dialog = gtk_file_chooser_dialog_new(
      "保存下载文件", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_SAVE,
      "取消", GTK_RESPONSE_CANCEL, "保存", GTK_RESPONSE_ACCEPT, nullptr);
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
  auto* basename = g_path_get_basename(
      suggested_filename && *suggested_filename ? suggested_filename : "download");
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), basename);
  g_free(basename);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(dialog);
    webkit_download_cancel(download);
    state->logger->Info("Download canceled by user");
    return TRUE;
  }
  auto* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
  gtk_widget_destroy(dialog);
  GError* error = nullptr;
  auto* destination = g_filename_to_uri(filename, nullptr, &error);
  g_free(filename);
  if (!destination) {
    state->logger->Error(std::string("Cannot prepare download destination: ") +
                         (error ? error->message : "unknown error"));
    g_clear_error(&error);
    webkit_download_cancel(download);
    return TRUE;
  }
  webkit_download_set_allow_overwrite(download, TRUE);
  webkit_download_set_destination(download, destination);
  g_free(destination);
  state->logger->Info("Download accepted by user");
  return TRUE;
}

void OnDownloadFinished(WebKitDownload* download, gpointer data) {
  if (!g_object_get_data(G_OBJECT(download), "lw-download-failed"))
    static_cast<RuntimeState*>(data)->logger->Info("Download completed");
}

void OnDownloadFailed(WebKitDownload* download, GError* error, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  g_object_set_data(G_OBJECT(download), "lw-download-failed", GINT_TO_POINTER(1));
  if (error && error->domain == WEBKIT_DOWNLOAD_ERROR &&
      error->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)
    return;
  state->logger->Warn("Download interrupted, reason=" +
                      std::to_string(error ? error->code : -1));
}

void OnDownloadStarted(WebKitWebContext*, WebKitDownload* download, gpointer data) {
  auto* state = static_cast<RuntimeState*>(data);
  state->logger->Info("Download started");
  g_signal_connect(download, "decide-destination",
                   G_CALLBACK(OnDownloadDecideDestination), state);
  g_signal_connect(download, "finished", G_CALLBACK(OnDownloadFinished), state);
  g_signal_connect(download, "failed", G_CALLBACK(OnDownloadFailed), state);
}

std::string FrontendErrorBridge() {
  return R"JS((function(){
    function send(kind,args){try{window.webkit.messageHandlers.lwWebError.postMessage(
      kind+': '+Array.prototype.map.call(args,function(v){
        if(v&&v.stack)return v.stack; try{return typeof v==='string'?v:JSON.stringify(v)}catch(_){return String(v)}
      }).join(' '));}catch(_){}}
    var oldError=console.error; console.error=function(){send('console.error',arguments);return oldError.apply(console,arguments)};
    window.addEventListener('error',function(e){send('Uncaught',[e.message,e.filename+':'+e.lineno+':'+e.colno,e.error])});
    window.addEventListener('unhandledrejection',function(e){send('Unhandled rejection',[e.reason])});
  })();)JS";
}

int RunPayloadApp(const LoadedPayload& payload) {
  Logger logger;
  try {
    logger = Logger::Runtime(payload.manifest);
  } catch (const std::exception& error) {
    std::cerr << "Runtime log initialization failed: " << error.what() << '\n';
  }
  logger.Info(std::string("lw.WebRuntime ") + kVersion + " (Linux)");
  logger.Info("Payload format: " +
              std::string(payload.footer.version == kPayloadVersion ? "LWWEB002" : "LWWEB001"));
  logger.Info("Payload verification OK");
  const auto app_id = EffectiveAppId(payload.manifest);
  logger.Info("App ID: " + app_id);
  logger.Info("Entry: " + (payload.manifest.mode == AppMode::Local ? payload.manifest.entry
                                                                    : payload.manifest.url));
  if (payload.manifest.mode == AppMode::Local)
    logger.Info("Start path: " + payload.manifest.start_path);
  SingleInstanceGuard instance_guard(app_id);

  std::unique_ptr<ResourceServer> server;
  auto file_grants = std::make_shared<LocalFileGrantManager>(&logger);
  std::string navigation;
  std::string local_origin;
  if (payload.manifest.mode == AppMode::Local) {
    server = std::make_unique<ResourceServer>(payload, SecurityLimits{}, &logger,
                                              file_grants);
    auto base = server->Start();
    local_origin = base;
    while (!local_origin.empty() && local_origin.back() == '/') local_origin.pop_back();
    navigation = BuildLocalStartUrl(base, payload.manifest.start_path);
  } else {
    navigation = payload.manifest.url;
  }

  if (!gtk_init_check(nullptr, nullptr)) throw Error("Cannot initialize GTK: no graphical display");
  const auto data_dir = XdgPath("XDG_DATA_HOME", ".local/share") / "lw.Web2App" / "apps" /
                        app_id / "webkitgtk";
  const auto cache_dir = XdgPath("XDG_CACHE_HOME", ".cache") / "lw.Web2App" / "apps" /
                         app_id / "webkitgtk";
  std::filesystem::create_directories(data_dir);
  std::filesystem::create_directories(cache_dir);

  auto* data_manager = webkit_website_data_manager_new(
      "base-data-directory", data_dir.c_str(), "base-cache-directory", cache_dir.c_str(),
      nullptr);
  if (payload.manifest.mode == AppMode::Url) ConfigureEnvironmentProxy(data_manager, logger);
  auto* context = webkit_web_context_new_with_website_data_manager(data_manager);
  auto* content = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(content, "lwWebError");
  auto* script = webkit_user_script_new(FrontendErrorBridge().c_str(),
                                        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr,
                                        nullptr);
  webkit_user_content_manager_add_script(content, script);
  webkit_user_script_unref(script);
  std::string ipc_transport_token;
  if (payload.manifest.ipc.enabled) {
    webkit_user_content_manager_register_script_message_handler(content, "lwIpc");
    ipc_transport_token = RandomIpcTransportToken();
    const auto bridge_text =
        BuildIpcBridgeScript("linux", "linux", ipc_transport_token);
    auto* ipc_script = webkit_user_script_new(
        bridge_text.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(content, ipc_script);
    webkit_user_script_unref(ipc_script);
  }

  auto* view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context,
                                             "user-content-manager", content, nullptr));
  auto* settings = webkit_web_view_get_settings(view);
  webkit_settings_set_enable_developer_extras(settings, payload.manifest.devtools);

  // WebView 的 GObject 属性持有 context 与脚本管理器；由 GTK 控件树统一结束生命周期。
  auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), payload.manifest.title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window), static_cast<int>(payload.manifest.width),
                              static_cast<int>(payload.manifest.height));
  gtk_window_set_resizable(GTK_WINDOW(window), payload.manifest.resizable);
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(view));

  RuntimeState state;
  state.logger = &logger;
  state.window = window;
  state.view = view;
  state.local_origin = local_origin;
  state.ipc_transport_token = ipc_transport_token;
  state.fullscreen = payload.manifest.fullscreen;
  if (payload.manifest.ipc.enabled) {
    IpcRuntimeServices services;
    services.platform = "linux";
    services.runtime_version = kVersion;
    services.select_directory = [window] { return ChooseIpcDirectory(window); };
    services.open_files = [window](const OpenFileDialogOptions& options) {
      return ChooseIpcFiles(window, options);
    };
    services.trash_file = MoveFileToTrash;
    services.get_window_state = [window, &state] { return WindowState(window, state); };
    services.control_window = [window, &state](const std::string& method,
                                               const nlohmann::json& params) {
      ControlWindow(window, state, method, params);
    };
    services.request_quit = [window] { g_idle_add(QuitRuntime, window); };
    services.emit_event = [view](const std::string& event, const nlohmann::json& data) {
      auto* pending = new LinuxIpcEvent{WEBKIT_WEB_VIEW(g_object_ref(view)), event, data};
      g_idle_add(ApplyIpcEvent, pending);
    };
    services.file_grants = file_grants;
    state.ipc_dispatcher =
        std::make_shared<IpcDispatcher>(payload.manifest, std::move(services));
    logger.Info("Native IPC bridge enabled");
  }
  g_signal_connect(window, "destroy", G_CALLBACK(OnRuntimeDestroy), nullptr);
  g_signal_connect(window, "delete-event", G_CALLBACK(OnRuntimeDelete), &state);
  g_signal_connect(window, "key-press-event", G_CALLBACK(OnRuntimeKey), &state);
  g_signal_connect(view, "load-changed", G_CALLBACK(OnLoadChanged), &state);
  g_signal_connect(view, "load-failed", G_CALLBACK(OnLoadFailed), &state);
  g_signal_connect(view, "web-process-terminated", G_CALLBACK(OnWebProcessTerminated), &state);
  g_signal_connect(view, "decide-policy", G_CALLBACK(OnRuntimeDecidePolicy), &state);
  g_signal_connect(content, "script-message-received::lwWebError", G_CALLBACK(OnScriptMessage),
                   &state);
  if (state.ipc_dispatcher)
    g_signal_connect(content, "script-message-received::lwIpc",
                     G_CALLBACK(OnIpcScriptMessage), &state);
  g_signal_connect(context, "download-started", G_CALLBACK(OnDownloadStarted), &state);

  logger.Info("WebKitGTK Runtime: " + std::to_string(webkit_get_major_version()) + "." +
              std::to_string(webkit_get_minor_version()) + "." +
              std::to_string(webkit_get_micro_version()));
  logger.Info("WebKitGTK initialized");
  logger.Info("User data: " + data_dir.string());
  logger.Info("Cache: " + cache_dir.string());
  logger.Info("Logs: " + logger.File().string());
  webkit_web_view_load_uri(view, navigation.c_str());
  gtk_widget_show_all(window);
  if (state.fullscreen) gtk_window_fullscreen(GTK_WINDOW(window));
  gtk_main();

  logger.Info("Application closed");
  logger.Flush();
  g_object_unref(content);
  g_object_unref(context);
  g_object_unref(data_manager);
  return 0;
}

// Linux 打包器窗口的控件集合；仅保存 GTK 所拥有控件的非拥有指针。
struct GuiState {
  GtkWidget* window{};
  GtkWidget* local_radio{};
  GtkWidget* source_label{};
  GtkWidget* source{};
  GtkWidget* browse_source{};
  GtkWidget* entry{};
  GtkWidget* start_path{};
  GtkWidget* title{};
  GtkWidget* width{};
  GtkWidget* height{};
  GtkWidget* fullscreen{};
  GtkWidget* resizable{};
  GtkWidget* spa{};
  GtkWidget* logging{};
  GtkWidget* debug{};
  GtkWidget* backend_proxy{};
  GtkWidget* backend_origin{};
  GtkWidget* output{};
  GtkWidget* status{};
  GtkWidget* pack{};
  bool busy = false;
};

struct GuiProgressUpdate {
  GuiState* state{};
  std::string message;
};

struct GuiPackResult {
  GuiState* state{};
  bool success = false;
  std::string output;
  std::string error;
};

const char* EntryText(GtkWidget* entry) { return gtk_entry_get_text(GTK_ENTRY(entry)); }

std::string ComboText(GtkWidget* combo) {
  auto* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
  if (!text) return {};
  std::string result(text);
  g_free(text);
  return result;
}

void UpdateSuggestedStartPath(GuiState* state) {
  const auto entry = ComboText(state->entry);
  if (entry.empty()) return;
  try {
    gtk_entry_set_text(GTK_ENTRY(state->start_path), SuggestedStartPath(entry).c_str());
  } catch (...) {
  }
}

void RefreshHtmlEntries(GuiState* state) {
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(state->entry));
  std::vector<std::string> entries;
  try {
    const std::string source = EntryText(state->source);
    if (!source.empty()) entries = FindHtmlEntries(std::filesystem::u8path(source));
  } catch (...) {
  }
  if (entries.empty()) entries.push_back("index.html");
  for (const auto& entry : entries)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->entry), entry.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(state->entry), 0);
  UpdateSuggestedStartPath(state);
}

void SetStatus(GuiState* state, const std::string& message, bool error = false) {
  gtk_label_set_text(GTK_LABEL(state->status), message.c_str());
  auto* context = gtk_widget_get_style_context(state->status);
  gtk_style_context_remove_class(context, error ? "success" : "error");
  gtk_style_context_add_class(context, error ? "error" : "success");
}

void ChoosePath(GtkWidget* parent, GtkWidget* entry, GtkFileChooserAction action,
                const char* title) {
  auto* dialog = gtk_file_chooser_dialog_new(
      title, GTK_WINDOW(parent), action, "取消", GTK_RESPONSE_CANCEL,
      action == GTK_FILE_CHOOSER_ACTION_SAVE ? "保存" : "选择", GTK_RESPONSE_ACCEPT,
      nullptr);
  if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
  }
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    auto* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_entry_set_text(GTK_ENTRY(entry), filename);
    g_free(filename);
  }
  gtk_widget_destroy(dialog);
}

void OnBrowseSource(GtkButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  ChoosePath(state->window, state->source, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
             "选择网站目录");
  RefreshHtmlEntries(state);
}

gboolean OnSourceFocusOut(GtkWidget*, GdkEvent*, gpointer data) {
  RefreshHtmlEntries(static_cast<GuiState*>(data));
  return FALSE;
}

void OnEntryChanged(GtkComboBox*, gpointer data) {
  UpdateSuggestedStartPath(static_cast<GuiState*>(data));
}

void OnBrowseOutput(GtkButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  ChoosePath(state->window, state->output, GTK_FILE_CHOOSER_ACTION_SAVE,
             "保存 Linux 应用");
}

void OnModeChanged(GtkToggleButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  const bool local = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->local_radio));
  gtk_label_set_text(GTK_LABEL(state->source_label), local ? "网站目录" : "网站 URL");
  gtk_widget_set_sensitive(state->browse_source, local);
  gtk_widget_set_sensitive(state->entry, local);
  gtk_widget_set_sensitive(state->start_path, local);
  gtk_widget_set_sensitive(state->spa, local);
  gtk_widget_set_sensitive(state->backend_proxy, local);
  gtk_widget_set_sensitive(
      state->backend_origin,
      local && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->backend_proxy)));
}

void OnBackendProxyChanged(GtkToggleButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  const bool local = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->local_radio));
  gtk_widget_set_sensitive(
      state->backend_origin,
      local && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->backend_proxy)));
}

void ShowError(GtkWidget* parent, const std::string& message) {
  auto* dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                        message.c_str());
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

gboolean ApplyProgressUpdate(gpointer data) {
  std::unique_ptr<GuiProgressUpdate> update(static_cast<GuiProgressUpdate*>(data));
  SetStatus(update->state, update->message);
  return G_SOURCE_REMOVE;
}

gboolean ApplyPackResult(gpointer data) {
  std::unique_ptr<GuiPackResult> result(static_cast<GuiPackResult*>(data));
  result->state->busy = false;
  gtk_widget_set_sensitive(result->state->pack, TRUE);
  if (result->success) {
    SetStatus(result->state, "生成成功：" + result->output);
  } else {
    SetStatus(result->state, "生成失败：" + result->error, true);
    ShowError(result->state->window, result->error);
  }
  return G_SOURCE_REMOVE;
}

gboolean OnPackerDelete(GtkWidget*, GdkEvent*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  if (!state->busy) return FALSE;
  SetStatus(state, "正在生成应用，请等待任务完成后再关闭窗口。");
  return TRUE;
}

void OnPack(GtkButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  if (state->busy) return;
  try {
    PackOptions options;
    options.runner = CurrentExecutablePath();
    options.output = EntryText(state->output);
    const bool local = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->local_radio));
    options.manifest.mode = local ? AppMode::Local : AppMode::Url;
    if (local) {
      options.source_directory = EntryText(state->source);
      options.manifest.entry = ComboText(state->entry);
      options.manifest.start_path = EntryText(state->start_path);
    } else {
      options.manifest.url = EntryText(state->source);
    }
    options.manifest.title = EntryText(state->title);
    options.manifest.width = std::stoul(EntryText(state->width));
    options.manifest.height = std::stoul(EntryText(state->height));
    options.manifest.fullscreen =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->fullscreen));
    options.manifest.resizable =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->resizable));
    options.manifest.spa_fallback =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->spa));
    options.manifest.logging.enabled =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->logging));
    options.manifest.logging.level =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->debug)) ? "debug" : "info";
    options.manifest.backend_proxy.enabled =
        local && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->backend_proxy));
    if (options.manifest.backend_proxy.enabled)
      options.manifest.backend_proxy.origin = EntryText(state->backend_origin);
    options.progress = [state](const std::string& message) {
      g_idle_add(ApplyProgressUpdate, new GuiProgressUpdate{state, message});
    };
    state->busy = true;
    gtk_widget_set_sensitive(state->pack, FALSE);
    SetStatus(state, "正在生成 Linux 应用…");
    std::thread([state, options = std::move(options)]() mutable {
      auto result = std::make_unique<GuiPackResult>();
      result->state = state;
      result->output = options.output.string();
      try {
        PackApplication(options);
        result->success = true;
      } catch (const std::exception& error) {
        result->error = error.what();
      } catch (...) {
        result->error = "发生未知打包错误";
      }
      g_idle_add(ApplyPackResult, result.release());
    }).detach();
  } catch (const std::exception& error) {
    state->busy = false;
    SetStatus(state, "生成失败：" + std::string(error.what()), true);
    ShowError(state->window, error.what());
    gtk_widget_set_sensitive(state->pack, TRUE);
  }
}

void AttachRow(GtkGrid* grid, int row, const char* label, GtkWidget* widget,
               GtkWidget* tail = nullptr) {
  auto* text = gtk_label_new(label);
  gtk_widget_set_halign(text, GTK_ALIGN_START);
  gtk_grid_attach(grid, text, 0, row, 1, 1);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_grid_attach(grid, widget, 1, row, tail ? 1 : 2, 1);
  if (tail) gtk_grid_attach(grid, tail, 2, row, 1, 1);
}

int RunPackerGui() {
  if (!gtk_init_check(nullptr, nullptr)) throw Error("Cannot initialize GTK: no graphical display");
  auto state = std::make_unique<GuiState>();
  state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(state->window), "lw.Web2App - Linux 网页应用打包器");
  gtk_window_set_default_size(GTK_WINDOW(state->window), 760, 740);
  gtk_container_set_border_width(GTK_CONTAINER(state->window), 24);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_container_add(GTK_CONTAINER(state->window), root);
  auto* heading = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(heading),
                       "<span size='x-large' weight='bold'>lw.Web2App</span>\n"
                       "<span foreground='#64748b'>将静态网站打包为 Ubuntu 桌面应用</span>");
  gtk_widget_set_halign(heading, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(root), heading, FALSE, FALSE, 0);

  auto* modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  state->local_radio = gtk_radio_button_new_with_label(nullptr, "本地静态网站");
  auto* url_radio = gtk_radio_button_new_with_label_from_widget(
      GTK_RADIO_BUTTON(state->local_radio), "在线 URL");
  gtk_box_pack_start(GTK_BOX(modes), state->local_radio, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(modes), url_radio, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), modes, FALSE, FALSE, 0);

  auto* grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(grid, 12);
  gtk_grid_set_column_spacing(grid, 12);
  gtk_box_pack_start(GTK_BOX(root), GTK_WIDGET(grid), TRUE, TRUE, 0);
  state->source_label = gtk_label_new("网站目录");
  gtk_widget_set_halign(state->source_label, GTK_ALIGN_START);
  state->source = gtk_entry_new();
  state->browse_source = gtk_button_new_with_label("浏览…");
  gtk_grid_attach(grid, state->source_label, 0, 0, 1, 1);
  gtk_widget_set_hexpand(state->source, TRUE);
  gtk_grid_attach(grid, state->source, 1, 0, 1, 1);
  gtk_grid_attach(grid, state->browse_source, 2, 0, 1, 1);
  state->entry = gtk_combo_box_text_new();
  AttachRow(grid, 1, "启动页", state->entry);
  state->start_path = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->start_path), "/");
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->start_path), "/、/login 或 /#/login");
  AttachRow(grid, 2, "启动路径", state->start_path);
  state->title = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->title), "我的网页应用");
  AttachRow(grid, 3, "窗口标题", state->title);
  state->width = gtk_entry_new();
  state->height = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->width), "1280");
  gtk_entry_set_text(GTK_ENTRY(state->height), "800");
  AttachRow(grid, 4, "窗口宽度", state->width);
  AttachRow(grid, 5, "窗口高度", state->height);
  state->fullscreen = gtk_check_button_new_with_label("默认全屏（F11 切换，Esc 退出全屏）");
  state->resizable = gtk_check_button_new_with_label("允许调整窗口大小");
  state->spa = gtk_check_button_new_with_label("启用 SPA fallback");
  state->logging = gtk_check_button_new_with_label("启用运行日志");
  state->debug = gtk_check_button_new_with_label("详细日志（DEBUG）");
  state->backend_proxy = gtk_check_button_new_with_label("HTTP 后台代理");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->fullscreen), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->resizable), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->spa), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->logging), TRUE);
  auto* checks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_pack_start(GTK_BOX(checks), state->fullscreen, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(checks), state->resizable, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(checks), state->spa, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(checks), state->logging, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(checks), state->debug, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(checks), state->backend_proxy, FALSE, FALSE, 0);
  AttachRow(grid, 6, "运行选项", checks);
  state->backend_origin = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->backend_origin),
                                 "例如：http://192.0.2.10:8080");
  gtk_widget_set_sensitive(state->backend_origin, FALSE);
  AttachRow(grid, 7, "后台地址", state->backend_origin);
  state->output = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->output), DefaultOutputPath().c_str());
  auto* browse_output = gtk_button_new_with_label("浏览…");
  AttachRow(grid, 8, "输出文件", state->output, browse_output);

  state->pack = gtk_button_new_with_label("生成 Linux 应用");
  gtk_widget_set_name(state->pack, "primary-button");
  gtk_widget_set_size_request(state->pack, -1, 46);
  gtk_box_pack_start(GTK_BOX(root), state->pack, FALSE, FALSE, 0);
  state->status = gtk_label_new("就绪");
  gtk_label_set_line_wrap(GTK_LABEL(state->status), TRUE);
  gtk_widget_set_halign(state->status, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(root), state->status, FALSE, FALSE, 0);

  auto* css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      css, "#primary-button{background:#2563eb;color:white;border-radius:8px;font-weight:bold;}"
           ".success{color:#15803d;}.error{color:#b91c1c;}",
      -1, nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                            GTK_STYLE_PROVIDER(css),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  g_signal_connect(state->window, "destroy", G_CALLBACK(OnRuntimeDestroy), nullptr);
  g_signal_connect(state->window, "delete-event", G_CALLBACK(OnPackerDelete), state.get());
  g_signal_connect(state->browse_source, "clicked", G_CALLBACK(OnBrowseSource), state.get());
  g_signal_connect(state->source, "focus-out-event", G_CALLBACK(OnSourceFocusOut), state.get());
  g_signal_connect(state->entry, "changed", G_CALLBACK(OnEntryChanged), state.get());
  g_signal_connect(browse_output, "clicked", G_CALLBACK(OnBrowseOutput), state.get());
  g_signal_connect(state->local_radio, "toggled", G_CALLBACK(OnModeChanged), state.get());
  g_signal_connect(state->backend_proxy, "toggled", G_CALLBACK(OnBackendProxyChanged),
                   state.get());
  g_signal_connect(state->pack, "clicked", G_CALLBACK(OnPack), state.get());
  RefreshHtmlEntries(state.get());
  gtk_widget_show_all(state->window);
  gtk_main();
  return 0;
}

void ShowLauncherError(const std::string& message) {
  std::cerr << "lw.Web2App: " << message << '\n';
  if (!gtk_init_check(nullptr, nullptr)) return;
  auto* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                        GTK_BUTTONS_OK, "%s", message.c_str());
  gtk_window_set_title(GTK_WINDOW(dialog), "lw.Web2App");
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

}  // namespace
}  // namespace lwweb

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv, argv + argc);
    if (argc > 1)
      return lwweb::RunCommandLine(args, lwweb::CurrentExecutablePath(),
                                   lwweb::CliPlatform::Linux, std::cout);
    const auto executable = lwweb::CurrentExecutablePath();
    if (lwweb::HasPayload(executable)) return lwweb::RunPayloadApp(lwweb::LoadPayload(executable));
    return lwweb::RunPackerGui();
  } catch (const std::exception& error) {
    try {
      auto logger = lwweb::Logger::Rotating(
          "lw.Web2App.Launcher", lwweb::LocalAppDataRoot() / "logs" / "launcher.log", {});
      logger.Error(error.what());
      logger.Flush();
    } catch (...) {
    }
    lwweb::ShowLauncherError(error.what());
    return 1;
  }
}
