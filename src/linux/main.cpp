#include "lwweb/cli/command_line.h"
#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/runtime/resource_server.h"
#include "lwweb/runtime/single_instance.h"
#include "lwweb/version.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
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
  const auto* home = std::getenv("HOME");
  return (std::filesystem::path(home && *home ? home : ".") / "MyWebApp").string();
}

// GTK/WebKitGTK 信号回调共享的轻量运行状态；对象生命周期覆盖整个 gtk_main 循环。
struct RuntimeState {
  Logger* logger = nullptr;
  bool fullscreen = false;
};

void OnRuntimeDestroy(GtkWidget*, gpointer) { gtk_main_quit(); }

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
  SingleInstanceGuard instance_guard(app_id);

  std::unique_ptr<ResourceServer> server;
  std::string navigation;
  if (payload.manifest.mode == AppMode::Local) {
    server = std::make_unique<ResourceServer>(payload, SecurityLimits{}, &logger);
    navigation = server->Start();
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

  RuntimeState state{&logger, payload.manifest.fullscreen};
  g_signal_connect(window, "destroy", G_CALLBACK(OnRuntimeDestroy), nullptr);
  g_signal_connect(window, "key-press-event", G_CALLBACK(OnRuntimeKey), &state);
  g_signal_connect(view, "load-changed", G_CALLBACK(OnLoadChanged), &state);
  g_signal_connect(view, "load-failed", G_CALLBACK(OnLoadFailed), &state);
  g_signal_connect(view, "web-process-terminated", G_CALLBACK(OnWebProcessTerminated), &state);
  g_signal_connect(content, "script-message-received::lwWebError", G_CALLBACK(OnScriptMessage),
                   &state);

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
  GtkWidget* title{};
  GtkWidget* width{};
  GtkWidget* height{};
  GtkWidget* fullscreen{};
  GtkWidget* resizable{};
  GtkWidget* spa{};
  GtkWidget* logging{};
  GtkWidget* debug{};
  GtkWidget* output{};
  GtkWidget* status{};
  GtkWidget* pack{};
};

const char* EntryText(GtkWidget* entry) { return gtk_entry_get_text(GTK_ENTRY(entry)); }

void SetStatus(GuiState* state, const std::string& message, bool error = false) {
  gtk_label_set_text(GTK_LABEL(state->status), message.c_str());
  auto* context = gtk_widget_get_style_context(state->status);
  gtk_style_context_remove_class(context, error ? "success" : "error");
  gtk_style_context_add_class(context, error ? "error" : "success");
  while (gtk_events_pending()) gtk_main_iteration();
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
  gtk_widget_set_sensitive(state->spa, local);
}

void ShowError(GtkWidget* parent, const std::string& message) {
  auto* dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                        message.c_str());
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

void OnPack(GtkButton*, gpointer data) {
  auto* state = static_cast<GuiState*>(data);
  gtk_widget_set_sensitive(state->pack, FALSE);
  try {
    PackOptions options;
    options.runner = CurrentExecutablePath();
    options.output = EntryText(state->output);
    const bool local = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->local_radio));
    options.manifest.mode = local ? AppMode::Local : AppMode::Url;
    if (local) {
      options.source_directory = EntryText(state->source);
      options.manifest.entry =
          std::filesystem::relative(FindDefaultEntry(options.source_directory),
                                    options.source_directory)
              .generic_string();
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
    options.progress = [state](const std::string& message) { SetStatus(state, message); };
    SetStatus(state, "正在生成 Linux 应用…");
    PackApplication(options);
    SetStatus(state, "生成成功：" + options.output.string());
  } catch (const std::exception& error) {
    SetStatus(state, "生成失败：" + std::string(error.what()), true);
    ShowError(state->window, error.what());
  }
  gtk_widget_set_sensitive(state->pack, TRUE);
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
  gtk_window_set_default_size(GTK_WINDOW(state->window), 760, 600);
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
  state->title = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->title), "我的网页应用");
  AttachRow(grid, 1, "窗口标题", state->title);
  state->width = gtk_entry_new();
  state->height = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->width), "1280");
  gtk_entry_set_text(GTK_ENTRY(state->height), "800");
  AttachRow(grid, 2, "窗口宽度", state->width);
  AttachRow(grid, 3, "窗口高度", state->height);
  state->fullscreen = gtk_check_button_new_with_label("默认全屏（F11 切换，Esc 退出全屏）");
  state->resizable = gtk_check_button_new_with_label("允许调整窗口大小");
  state->spa = gtk_check_button_new_with_label("启用 SPA fallback");
  state->logging = gtk_check_button_new_with_label("启用运行日志");
  state->debug = gtk_check_button_new_with_label("详细日志（DEBUG）");
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
  AttachRow(grid, 4, "运行选项", checks);
  state->output = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(state->output), DefaultOutputPath().c_str());
  auto* browse_output = gtk_button_new_with_label("浏览…");
  AttachRow(grid, 5, "输出文件", state->output, browse_output);

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
  g_signal_connect(state->browse_source, "clicked", G_CALLBACK(OnBrowseSource), state.get());
  g_signal_connect(browse_output, "clicked", G_CALLBACK(OnBrowseOutput), state.get());
  g_signal_connect(state->local_radio, "toggled", G_CALLBACK(OnModeChanged), state.get());
  g_signal_connect(state->pack, "clicked", G_CALLBACK(OnPack), state.get());
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
