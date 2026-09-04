#include "lwweb/app/main_window.h"

#include "lwweb/cli/command_line.h"
#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/ipc/ipc_message.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/pe/pe_resources.h"
#include "lwweb/runtime/local_file_grant.h"
#include "lwweb/runtime/resource_server.h"
#include "lwweb/runtime/single_instance.h"
#include "lwweb/webview/webview_host.h"
#include "lwweb/version.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lwweb {
namespace {

// 生成应用运行期间绑定到主窗口的服务与 WebView2 对象。
// 成员声明顺序保证 WebView2 先销毁、本地 HTTP 服务随后停止。
struct RuntimeState {
  Logger logger;
  std::unique_ptr<SingleInstanceGuard> instance_guard;
  std::shared_ptr<LocalFileGrantManager> file_grants;
  std::unique_ptr<ResourceServer> server;
  std::unique_ptr<WebViewHost> webview;
  bool fullscreen = false;
  std::string close_behavior = "exit";
  bool tray_created = false;
  UINT activate_message = 0;
  NOTIFYICONDATAW tray_icon{sizeof(NOTIFYICONDATAW)};
  UINT taskbar_created_message = 0;
  std::vector<nlohmann::json> tray_menu;
  std::function<void(const std::string&, const nlohmann::json&)> emit_event;
  DWORD windowed_style = 0;
  DWORD windowed_ex_style = 0;
  WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
};

constexpr UINT kRuntimeTrayMessage = WM_APP + 0x4a4;
constexpr UINT kRuntimeTrayCommandFirst = 0x6200;

void UpdateTrayModel(RuntimeState& state, const nlohmann::json& params) {
  state.tray_menu.clear();
  if (const auto menu = params.find("menu"); menu != params.end())
    for (const auto& item : *menu) state.tray_menu.push_back(item);
}

void UpdateTrayTooltip(RuntimeState& state, const nlohmann::json& params) {
  const auto tooltip = params.value("tooltip", std::string("lw.Web2App"));
  const auto wide = Utf8ToWide(tooltip);
  std::wcsncpy(state.tray_icon.szTip, wide.c_str(),
               (sizeof(state.tray_icon.szTip) / sizeof(wchar_t)) - 1);
  state.tray_icon.szTip[(sizeof(state.tray_icon.szTip) / sizeof(wchar_t)) - 1] = L'\0';
}

nlohmann::json CreateTray(HWND window, HINSTANCE instance, RuntimeState& state,
                          const nlohmann::json& params) {
  if (state.tray_created) {
    state.tray_menu.clear();
    UpdateTrayModel(state, params);
    UpdateTrayTooltip(state, params);
    state.tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &state.tray_icon))
      throw IpcException("IO_ERROR", "Cannot update the system tray icon");
    return {{"created", true}, {"updated", true}};
  }
  state.tray_icon = NOTIFYICONDATAW{sizeof(NOTIFYICONDATAW)};
  state.tray_icon.hWnd = window;
  state.tray_icon.uID = 1;
  state.tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  state.tray_icon.uCallbackMessage = kRuntimeTrayMessage;
  state.tray_icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  UpdateTrayTooltip(state, params);
  UpdateTrayModel(state, params);
  if (!Shell_NotifyIconW(NIM_ADD, &state.tray_icon))
    throw IpcException("IO_ERROR", "Cannot create the system tray icon");
  state.tray_created = true;
  return {{"created", true}};
}

nlohmann::json UpdateTray(RuntimeState& state, const nlohmann::json& params) {
  if (!state.tray_created)
    throw IpcException("INVALID_STATE", "Create the system tray before updating it");
  UpdateTrayModel(state, params);
  UpdateTrayTooltip(state, params);
  state.tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  if (!Shell_NotifyIconW(NIM_MODIFY, &state.tray_icon))
    throw IpcException("IO_ERROR", "Cannot update the system tray icon");
  return {{"updated", true}};
}

nlohmann::json DestroyTray(RuntimeState& state) {
  if (!state.tray_created) return {{"destroyed", false}};
  Shell_NotifyIconW(NIM_DELETE, &state.tray_icon);
  state.tray_created = false;
  state.tray_menu.clear();
  return {{"destroyed", true}};
}

void EmitTrayEvent(RuntimeState& state, const std::string& event,
                   const nlohmann::json& data) {
  if (state.emit_event) (void)state.emit_event(event, data);
}

void ShowTrayMenu(HWND window, RuntimeState& state) {
  if (!state.tray_created) return;
  const auto menu = CreatePopupMenu();
  if (!menu) return;
  std::unordered_map<UINT, std::string> command_ids;
  UINT command = kRuntimeTrayCommandFirst;
  for (const auto& item : state.tray_menu) {
    if (item.value("type", "") == "separator") {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    const auto id = command++;
    command_ids.emplace(id, item.value("id", ""));
    UINT flags = MF_STRING;
    if (!item.value("enabled", true)) flags |= MF_GRAYED;
    if (item.value("checked", false)) flags |= MF_CHECKED;
    const auto label = Utf8ToWide(item.value("label", ""));
    AppendMenuW(menu, flags, id, label.c_str());
  }
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(window);
  const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                       point.x, point.y, 0, window, nullptr);
  DestroyMenu(menu);
  const auto found = command_ids.find(selected);
  if (found != command_ids.end()) EmitTrayEvent(state, "tray.menu", {{"id", found->second}});
}

void HandleTrayMessage(HWND window, RuntimeState& state, LPARAM lparam) {
  switch (static_cast<UINT>(lparam)) {
    case WM_LBUTTONUP:
      EmitTrayEvent(state, "tray.click", {{"button", "left"}, {"clicks", 1}});
      break;
    case WM_LBUTTONDBLCLK:
      EmitTrayEvent(state, "tray.click", {{"button", "left"}, {"clicks", 2}});
      break;
    case WM_RBUTTONUP:
      ShowTrayMenu(window, state);
      break;
  }
}

nlohmann::json WindowState(HWND window, const RuntimeState& state) {
  const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
  return {{"visible", IsWindowVisible(window) != FALSE},
          {"minimized", IsIconic(window) != FALSE},
          {"maximized", IsZoomed(window) != FALSE},
          {"fullscreen", state.fullscreen},
          {"alwaysOnTop", (ex_style & WS_EX_TOPMOST) != 0},
          {"closeBehavior", state.close_behavior}};
}

void ControlWindow(HWND window, RuntimeState& state, const std::string& method,
                   const nlohmann::json& params) {
  if (method == "window.show") {
    ShowWindow(window, SW_SHOW);
  } else if (method == "window.hide") {
    ShowWindow(window, SW_HIDE);
  } else if (method == "window.minimize") {
    ShowWindow(window, SW_MINIMIZE);
  } else if (method == "window.maximize") {
    ShowWindow(window, SW_MAXIMIZE);
  } else if (method == "window.restore") {
    ShowWindow(window, SW_RESTORE);
  } else if (method == "window.focus") {
    ShowWindow(window, SW_SHOW);
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    SetFocus(window);
  } else if (method == "window.setAlwaysOnTop") {
    const bool enabled = params.at("enabled").get<bool>();
    SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  } else if (method == "window.setCloseBehavior") {
    state.close_behavior = params.at("behavior").get<std::string>();
  }
}

void SetFullscreen(HWND window, RuntimeState& state, bool fullscreen) {
  if (state.fullscreen == fullscreen) return;
  if (fullscreen) {
    state.windowed_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    state.windowed_ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    state.windowed_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(window, &state.windowed_placement);

    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowLongPtrW(window, GWL_STYLE,
                      state.windowed_style & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW));
    SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtrW(window, GWL_STYLE, state.windowed_style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, state.windowed_ex_style);
    SetWindowPlacement(window, &state.windowed_placement);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
  }
  state.fullscreen = fullscreen;
}

LRESULT CALLBACK RuntimeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<RuntimeState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_SIZE && state && state->webview) state->webview->Resize();
  if (state && state->webview &&
      state->webview->HandleWindowMessage(message, wparam, lparam))
    return 0;
  if (message == WM_DPICHANGED && state && !state->fullscreen) {
    const auto* suggested = reinterpret_cast<RECT*>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    return 0;
  }
  if (message == WM_KEYDOWN && state) {
    if (wparam == VK_F11) {
      SetFullscreen(window, *state, !state->fullscreen);
      return 0;
    }
    if (wparam == VK_ESCAPE && state->fullscreen) {
      SetFullscreen(window, *state, false);
      return 0;
    }
  }
  if (message == WM_CLOSE && state && state->close_behavior == "hide") {
    ShowWindow(window, SW_HIDE);
    return 0;
  }
  if (message == kRuntimeQuitMessage) {
    DestroyWindow(window);
    return 0;
  }
  if (state && state->activate_message != 0 && message == state->activate_message) {
    ShowWindow(window, SW_SHOW);
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    SetFocus(window);
    return 0;
  }
  if (state && message == kRuntimeTrayMessage) {
    HandleTrayMessage(window, *state, lparam);
    return 0;
  }
  if (state && state->taskbar_created_message != 0 &&
      message == state->taskbar_created_message && state->tray_created) {
    Shell_NotifyIconW(NIM_ADD, &state->tray_icon);
    return 0;
  }
  if (message == WM_DESTROY) {
    if (state) {
      DestroyTray(*state);
      delete state;
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int RunPayloadApp(HINSTANCE instance, const LoadedPayload& payload) {
  std::wstring navigation;
  std::string local_origin;
  auto state_owner = std::make_unique<RuntimeState>();
  auto* state = state_owner.get();
  try {
    state->logger = Logger::Runtime(payload.manifest);
  } catch (const std::exception& error) {
    MessageBoxW(nullptr,
                (L"运行日志初始化失败，应用将继续运行：\n" + Utf8ToWide(error.what())).c_str(),
                L"lw.Web2App", MB_OK | MB_ICONWARNING);
  }
  state->logger.Info(std::string("lw.WebRuntime ") + kVersion);
  state->logger.Info("Payload format: " +
                     std::string(payload.footer.version == kPayloadVersion ? "LWWEB002" :
                                                                           "LWWEB001"));
  state->logger.Info("Payload verification OK");
  state->logger.Info("App ID: " + EffectiveAppId(payload.manifest));
  state->logger.Info("Entry: " + (payload.manifest.mode == AppMode::Local
                                      ? payload.manifest.entry
                                      : payload.manifest.url));
  if (payload.manifest.mode == AppMode::Local)
    state->logger.Info("Start path: " + payload.manifest.start_path);
  state->instance_guard =
      std::make_unique<SingleInstanceGuard>(EffectiveAppId(payload.manifest));
  if (!state->instance_guard->IsPrimary()) return 0;
  if (payload.manifest.mode == AppMode::Local) {
    state->file_grants = std::make_shared<LocalFileGrantManager>(&state->logger);
    state->server = std::make_unique<ResourceServer>(payload, SecurityLimits{},
                                                     &state->logger,
                                                     state->file_grants);
    auto base = state->server->Start();
    local_origin = base;
    while (!local_origin.empty() && local_origin.back() == '/') local_origin.pop_back();
    navigation = Utf8ToWide(BuildLocalStartUrl(base, payload.manifest.start_path));
  } else {
    navigation = Utf8ToWide(payload.manifest.url);
  }
  const wchar_t class_name[] = L"lw.Web2App.Runtime";
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = RuntimeProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = class_name;
  if (!RegisterClassExW(&window_class)) throw Error("Cannot register runtime window");
  DWORD style = WS_OVERLAPPEDWINDOW;
  if (!payload.manifest.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
  const auto dpi = GetDpiForSystem();
  RECT bounds{0, 0, MulDiv(static_cast<int>(payload.manifest.width), dpi, 96),
              MulDiv(static_cast<int>(payload.manifest.height), dpi, 96)};
  AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, dpi);
  const auto window = CreateWindowExW(
      0, class_name, Utf8ToWide(payload.manifest.title).c_str(), style, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
      instance, nullptr);
  if (!window) {
    throw Error("Cannot create runtime window");
  }
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  state->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  const auto activation_name = L"lw.Web2App.Activate." +
                               Utf8ToWide(EffectiveAppId(payload.manifest));
  state->activate_message = RegisterWindowMessageW(activation_name.c_str());
  state->emit_event = [state](const std::string& event, const nlohmann::json& data) {
    if (state->webview) (void)state->webview->EmitIpcEvent(event, data);
  };
  state->webview = std::make_unique<WebViewHost>();
  try {
    state->webview->Create(
        window, navigation, local_origin, payload.manifest,
        [window](const std::wstring& error) {
          MessageBoxW(window, error.c_str(), L"lw.Web2App", MB_OK | MB_ICONERROR);
          DestroyWindow(window);
        },
        [window, state](bool fullscreen) { SetFullscreen(window, *state, fullscreen); },
        &state->logger, state->file_grants,
        [window, state] { return WindowState(window, *state); },
        [window, state](const std::string& method, const nlohmann::json& params) {
          ControlWindow(window, *state, method, params);
        },
        [window] { PostMessageW(window, kRuntimeQuitMessage, 0, 0); },
        [window, instance, state](const nlohmann::json& params) {
          return CreateTray(window, instance, *state, params);
        },
        [state](const nlohmann::json& params) { return UpdateTray(*state, params); },
        [state] { return DestroyTray(*state); });
  } catch (...) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
    throw;
  }
  state_owner.release();
  if (payload.manifest.fullscreen) SetFullscreen(window, *state, true);
  ShowWindow(window, SW_SHOW);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

}  // namespace
}  // namespace lwweb

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  int result = 1;
  try {
    const auto executable = lwweb::CurrentExecutablePath();
    int count = 0;
    auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> args;
    args.reserve(count);
    for (int i = 0; i < count; ++i) args.push_back(lwweb::WideToUtf8(raw[i]));
    LocalFree(raw);
    if (count > 1) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
          FILE* stream = nullptr;
          freopen_s(&stream, "CONOUT$", "w", stdout);
          freopen_s(&stream, "CONOUT$", "w", stderr);
        }
        result = lwweb::RunCommandLine(args, executable, lwweb::CliPlatform::Windows,
                                       std::cout);
    } else if (lwweb::HasPayload(executable)) {
      auto payload = lwweb::LoadPayload(executable);
      lwweb::VerifyPePayloadBinding(executable, payload.footer.sha256);
      result = lwweb::RunPayloadApp(instance, payload);
      } else {
        result = lwweb::RunPackerGui(instance);
      }
  } catch (const std::exception& error) {
    try {
      lwweb::LoggingConfig config;
      auto launcher_log = lwweb::Logger::Rotating(
          "lw.Web2App.Launcher", lwweb::LocalAppDataRoot() / L"logs" / L"launcher.log", config);
      launcher_log.Error(error.what());
      launcher_log.Flush();
    } catch (...) {
    }
    MessageBoxW(nullptr, lwweb::Utf8ToWide(error.what()).c_str(), L"lw.Web2App",
                MB_OK | MB_ICONERROR);
  }
  CoUninitialize();
  return result;
}
