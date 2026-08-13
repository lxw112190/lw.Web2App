#include "lwweb/app/main_window.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/runtime/resource_server.h"
#include "lwweb/webview/webview_host.h"

#include <Windows.h>
#include <shellapi.h>

#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace lwweb {
namespace {

// 生成应用运行期间绑定到主窗口的服务与 WebView2 对象。
// 成员声明顺序保证 WebView2 先销毁、本地 HTTP 服务随后停止。
struct RuntimeState {
  Logger logger;
  std::unique_ptr<ResourceServer> server;
  std::unique_ptr<WebViewHost> webview;
  bool fullscreen = false;
  DWORD windowed_style = 0;
  DWORD windowed_ex_style = 0;
  WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
};

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
  if (message == WM_DESTROY) {
    if (state) {
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
  auto* state = new RuntimeState{};
  try {
    state->logger = Logger::Runtime(payload.manifest);
  } catch (const std::exception& error) {
    MessageBoxW(nullptr,
                (L"运行日志初始化失败，应用将继续运行：\n" + Utf8ToWide(error.what())).c_str(),
                L"lw.Web2App", MB_OK | MB_ICONWARNING);
  }
  state->logger.Info("lw.WebRuntime 0.1.0");
  state->logger.Info("Payload format: " +
                     std::string(payload.footer.version == kPayloadVersion ? "LWWEB002" :
                                                                           "LWWEB001"));
  state->logger.Info("Payload verification OK");
  state->logger.Info("App ID: " + EffectiveAppId(payload.manifest));
  state->logger.Info("Entry: " + (payload.manifest.mode == AppMode::Local
                                      ? payload.manifest.entry
                                      : payload.manifest.url));
  if (payload.manifest.mode == AppMode::Local) {
    state->server = std::make_unique<ResourceServer>(payload, SecurityLimits{},
                                                     &state->logger);
    navigation = Utf8ToWide(state->server->Start());
  } else {
    navigation = Utf8ToWide(payload.manifest.url);
  }
  const wchar_t class_name[] = L"lw.Web2App.Runtime";
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = RuntimeProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = class_name;
  if (!RegisterClassExW(&window_class)) throw Error("Cannot register runtime window");
  DWORD style = WS_OVERLAPPEDWINDOW;
  if (!payload.manifest.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
  RECT bounds{0, 0, static_cast<LONG>(payload.manifest.width),
              static_cast<LONG>(payload.manifest.height)};
  AdjustWindowRect(&bounds, style, FALSE);
  const auto window = CreateWindowExW(
      0, class_name, Utf8ToWide(payload.manifest.title).c_str(), style, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
      instance, nullptr);
  if (!window) {
    delete state;
    throw Error("Cannot create runtime window");
  }
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  state->webview = std::make_unique<WebViewHost>();
  state->webview->Create(window, navigation, payload.manifest, [window](const std::wstring& error) {
    MessageBoxW(window, error.c_str(), L"lw.Web2App", MB_OK | MB_ICONERROR);
    DestroyWindow(window);
  }, &state->logger);
  if (payload.manifest.fullscreen) SetFullscreen(window, *state, true);
  ShowWindow(window, SW_SHOW);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

std::wstring ArgumentValue(const std::vector<std::wstring>& args, const std::wstring& name,
                           const std::wstring& fallback = L"") {
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == name) return args[i + 1];
  return fallback;
}

bool HasArgument(const std::vector<std::wstring>& args, const std::wstring& name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

int RunCli(const std::vector<std::wstring>& args) {
  if (args.size() < 2 || args[1] == L"help" || args[1] == L"--help") {
    std::wcout << L"lw.Web2App\n\n"
                  L"  lwweb pack <directory> <output.exe> [--entry index.html] [--title App]\n"
                  L"             [--width 1280] [--height 800] [--icon app.ico] [--no-spa] [--windowed]\n"
                  L"             [--no-log | --debug-log]\n"
                  L"  lwweb pack-url <url> <output.exe> [--title App] [--windowed] [--no-log | --debug-log]\n"
                  L"  lwweb inspect <application.exe>\n";
    return 0;
  }
  if (args[1] == L"inspect" && args.size() >= 3) {
    const auto loaded = LoadPayload(args[2]);
    std::cout << SerializeManifest(loaded.manifest, true) << "\n";
    return 0;
  }
  if ((args[1] == L"pack" || args[1] == L"pack-url") && args.size() >= 4) {
    PackOptions options;
    options.runner = CurrentExecutablePath();
    options.output = args[3];
    options.manifest.mode = args[1] == L"pack" ? AppMode::Local : AppMode::Url;
    if (options.manifest.mode == AppMode::Local) {
      options.source_directory = args[2];
      auto entry = ArgumentValue(args, L"--entry");
      if (entry.empty()) entry = std::filesystem::relative(FindDefaultEntry(options.source_directory),
                                                           options.source_directory).generic_wstring();
      options.manifest.entry = WideToUtf8(entry);
    } else {
      options.manifest.url = WideToUtf8(args[2]);
    }
    options.manifest.title = WideToUtf8(ArgumentValue(args, L"--title", L"lw.Web2App App"));
    options.manifest.width = std::stoul(ArgumentValue(args, L"--width", L"1280"));
    options.manifest.height = std::stoul(ArgumentValue(args, L"--height", L"800"));
    options.manifest.fullscreen = !HasArgument(args, L"--windowed");
    options.manifest.spa_fallback = !HasArgument(args, L"--no-spa");
    options.manifest.devtools = HasArgument(args, L"--devtools");
    options.manifest.logging.enabled = !HasArgument(args, L"--no-log");
    options.manifest.logging.level = HasArgument(args, L"--debug-log") ? "debug" : "info";
    options.metadata.product_name = Utf8ToWide(options.manifest.title);
    options.metadata.file_description = options.metadata.product_name;
    options.metadata.icon = ArgumentValue(args, L"--icon");
    options.metadata.company_name = ArgumentValue(args, L"--company");
    options.metadata.version = ArgumentValue(args, L"--version", L"1.0.0.0");
    options.metadata.copyright = ArgumentValue(args, L"--copyright");
    options.progress = [](const std::string& message) { std::cout << message << "\n"; };
    PackApplication(options);
    return 0;
  }
  throw Error("Unknown or incomplete command; run with --help");
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
    std::vector<std::wstring> args(raw, raw + count);
    LocalFree(raw);
    if (count > 1) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
          FILE* stream = nullptr;
          freopen_s(&stream, "CONOUT$", "w", stdout);
          freopen_s(&stream, "CONOUT$", "w", stderr);
        }
        result = lwweb::RunCli(args);
    } else if (lwweb::HasPayload(executable)) {
      result = lwweb::RunPayloadApp(instance, lwweb::LoadPayload(executable));
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
