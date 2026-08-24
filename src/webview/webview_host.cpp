#include "lwweb/webview/webview_host.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/sha256.h"
#include "lwweb/ipc/ipc_dispatcher.h"
#include "lwweb/version.h"

#include <ShlObj.h>
#include <ShObjIdl.h>

#include <filesystem>
#include <iterator>
#include <optional>
#include <thread>

using Microsoft::WRL::Callback;

namespace lwweb {
namespace {

std::string RuntimeAppId(const Manifest& manifest) {
  if (IsValidAppId(manifest.app_id)) return manifest.app_id;
  return "legacy-" + HexDigest(Sha256(
      reinterpret_cast<const std::uint8_t*>(manifest.title.data()), manifest.title.size()))
                         .substr(0, 24);
}

std::wstring UserDataFolder(const Manifest& manifest) {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                  &local_app_data)))
    throw Error("Cannot locate the local application data directory");
  const std::filesystem::path root(local_app_data);
  CoTaskMemFree(local_app_data);
  const auto folder = root / L"lw.Web2App" / L"apps" /
                      Utf8ToWide(RuntimeAppId(manifest)) / L"WebView2";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) throw Error("Cannot create the WebView2 user data directory");
  return folder.wstring();
}

// 使用 Windows Shell 的系统“另存为”窗口选择下载目标，保留 WebView2
// 从 Content-Disposition 推导出的文件名，并支持长路径与 Unicode。
std::optional<std::wstring> ChooseDownloadPath(HWND owner,
                                               const std::wstring& suggested_path,
                                               bool& dialog_available) {
  dialog_available = false;
  Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  dialog_available = true;

  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(dialog->GetOptions(&options)))
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR);
  dialog->SetTitle(L"保存下载文件");

  const std::filesystem::path suggested(suggested_path);
  auto filename = suggested.filename().wstring();
  if (filename.empty()) filename = L"download";
  dialog->SetFileName(filename.c_str());
  const auto parent = suggested.parent_path();
  if (!parent.empty()) {
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr,
                                               IID_PPV_ARGS(&folder))))
      dialog->SetDefaultFolder(folder.Get());
  }

  const auto shown = dialog->Show(owner);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
  if (FAILED(shown)) {
    dialog_available = false;
    return std::nullopt;
  }
  Microsoft::WRL::ComPtr<IShellItem> result;
  if (FAILED(dialog->GetResult(&result))) return std::nullopt;
  PWSTR path = nullptr;
  if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
    return std::nullopt;
  std::wstring selected(path);
  CoTaskMemFree(path);
  return selected;
}

std::optional<std::filesystem::path> ChooseDirectory(HWND owner) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    throw IpcException("UNSUPPORTED", "System directory dialog is unavailable");
  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(dialog->GetOptions(&options)))
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                       FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
  dialog->SetTitle(L"选择授权目录");
  const auto shown = dialog->Show(owner);
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
  if (FAILED(shown))
    throw IpcException("IO_ERROR", "System directory dialog failed");
  Microsoft::WRL::ComPtr<IShellItem> result;
  if (FAILED(dialog->GetResult(&result)))
    throw IpcException("IO_ERROR", "Cannot read selected directory");
  PWSTR path = nullptr;
  if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path)
    throw IpcException("IO_ERROR", "Selected item is not a filesystem directory");
  std::filesystem::path selected(path);
  CoTaskMemFree(path);
  return selected;
}

struct PendingIpcResponse {
  std::shared_ptr<IpcDispatcher> dispatcher;
  std::string method;
  IpcResponse response;
};

void LogIpcResult(const Logger* logger, const std::string& method,
                  const IpcResponse& response) {
  if (!logger) return;
  if (response.ok)
    logger->Info("IPC result: " + method + " OK");
  else
    logger->Warn("IPC failed: " + method + " - " + response.error.code);
}

}  // namespace

WebViewHost::~WebViewHost() {
  if (controller_) controller_->Close();
}

void WebViewHost::Create(HWND window, const std::wstring& url,
                         const std::string& local_origin, const Manifest& manifest,
                         std::function<void(const std::wstring&)> on_error,
                         std::function<void(bool)> on_fullscreen_changed,
                         const Logger* logger) {
  window_ = window;
  url_ = url;
  local_origin_ = local_origin;
  manifest_ = manifest;
  on_error_ = std::move(on_error);
  on_fullscreen_changed_ = std::move(on_fullscreen_changed);
  logger_ = logger;
  if (manifest_.ipc.enabled) {
    IpcRuntimeServices services;
    services.platform = "windows";
    services.runtime_version = kVersion;
    services.select_directory = [this] { return ChooseDirectory(window_); };
    ipc_dispatcher_ =
        std::make_shared<IpcDispatcher>(manifest_, std::move(services));
  }
  user_data_folder_ = UserDataFolder(manifest_);
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data_folder_.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              if (logger_) logger_->Error("Failed to create WebView2 environment, HRESULT=" +
                                          std::to_string(static_cast<unsigned long>(result)));
              on_error_(L"WebView2 Runtime is unavailable. Install the Evergreen Runtime and retry.");
              return result;
            }
            LPWSTR version = nullptr;
            if (SUCCEEDED(environment->get_BrowserVersionString(&version)) && version) {
              if (logger_) logger_->Info("WebView2 Runtime: " + WideToUtf8(version));
              CoTaskMemFree(version);
            }
            return environment->CreateCoreWebView2Controller(
                window_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                             [this](HRESULT controller_result,
                                    ICoreWebView2Controller* controller) -> HRESULT {
                               if (FAILED(controller_result) || !controller) {
                                 if (logger_) logger_->Error(
                                     "Failed to create WebView2 controller, HRESULT=" +
                                     std::to_string(static_cast<unsigned long>(controller_result)));
                                 on_error_(L"Cannot create the WebView2 controller.");
                                 return controller_result;
                               }
                               controller_ = controller;
                               EventRegistrationToken accelerator_token{};
                               controller_->add_AcceleratorKeyPressed(
                                   Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                       [this](ICoreWebView2Controller*,
                                              ICoreWebView2AcceleratorKeyPressedEventArgs* args)
                                           -> HRESULT {
                                         COREWEBVIEW2_KEY_EVENT_KIND kind{};
                                         UINT key = 0;
                                         args->get_KeyEventKind(&kind);
                                         args->get_VirtualKey(&key);
                                         const bool pressed =
                                             kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                             kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
                                         const auto style = static_cast<DWORD>(
                                             GetWindowLongPtrW(window_, GWL_STYLE));
                                         const bool fullscreen =
                                             (style & WS_OVERLAPPEDWINDOW) == 0;
                                         BOOL contains_fullscreen_element = FALSE;
                                         if (webview_)
                                           webview_->get_ContainsFullScreenElement(
                                               &contains_fullscreen_element);
                                         // 网页处于标准 Fullscreen API 状态时，让 WebView2
                                         // 自己处理 Esc；随后 change 事件会恢复 Native 窗口。
                                         if (pressed && key == VK_ESCAPE &&
                                             contains_fullscreen_element)
                                           return S_OK;
                                         if (pressed &&
                                             (key == VK_F11 || (key == VK_ESCAPE && fullscreen))) {
                                           args->put_Handled(TRUE);
                                           PostMessageW(window_, WM_KEYDOWN, key, 0);
                                         }
                                         return S_OK;
                                       })
                                       .Get(),
                                   &accelerator_token);
                               controller_->get_CoreWebView2(&webview_);
                               EventRegistrationToken fullscreen_token{};
                               webview_->add_ContainsFullScreenElementChanged(
                                   Callback<
                                       ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
                                       [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                         BOOL contains = FALSE;
                                         if (SUCCEEDED(sender->get_ContainsFullScreenElement(
                                                 &contains))) {
                                           if (logger_)
                                             logger_->Info(contains
                                                               ? "Web fullscreen entered"
                                                               : "Web fullscreen exited");
                                           if (on_fullscreen_changed_)
                                             on_fullscreen_changed_(contains != FALSE);
                                         }
                                         return S_OK;
                                       })
                                       .Get(),
                                   &fullscreen_token);
                               Microsoft::WRL::ComPtr<ICoreWebView2_4> webview4;
                               if (SUCCEEDED(webview_.As(&webview4))) {
                                 EventRegistrationToken download_token{};
                                 webview4->add_DownloadStarting(
                                     Callback<ICoreWebView2DownloadStartingEventHandler>(
                                         [this](ICoreWebView2*,
                                                ICoreWebView2DownloadStartingEventArgs* args)
                                             -> HRESULT {
                                           LPWSTR proposed = nullptr;
                                           std::wstring suggested;
                                           if (SUCCEEDED(args->get_ResultFilePath(&proposed)) &&
                                               proposed) {
                                             suggested = proposed;
                                             CoTaskMemFree(proposed);
                                           }
                                           bool dialog_available = false;
                                           const auto selected = ChooseDownloadPath(
                                               window_, suggested, dialog_available);
                                           if (selected) {
                                             args->put_ResultFilePath(selected->c_str());
                                             // 保留 WebView2 默认下载 UI，
                                             // 这样用户可以看到下载进度和完成状态。
                                             args->put_Handled(FALSE);
                                             if (logger_)
                                               logger_->Info("Download accepted by user");
                                           } else if (dialog_available) {
                                             args->put_Cancel(TRUE);
                                             args->put_Handled(TRUE);
                                             if (logger_)
                                               logger_->Info("Download canceled by user");
                                             return S_OK;
                                           } else if (logger_) {
                                             logger_->Warn(
                                                 "System save dialog unavailable; using "
                                                 "WebView2 download UI");
                                           }

                                           Microsoft::WRL::ComPtr<
                                               ICoreWebView2DownloadOperation>
                                               operation;
                                           if (SUCCEEDED(args->get_DownloadOperation(
                                                   &operation)) &&
                                               operation) {
                                             EventRegistrationToken state_token{};
                                             operation->add_StateChanged(
                                                 Callback<
                                                     ICoreWebView2StateChangedEventHandler>(
                                                     [this](
                                                         ICoreWebView2DownloadOperation* sender,
                                                         IUnknown*) -> HRESULT {
                                                       COREWEBVIEW2_DOWNLOAD_STATE state{};
                                                       if (FAILED(sender->get_State(&state)))
                                                         return S_OK;
                                                       if (state ==
                                                           COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
                                                         if (logger_)
                                                           logger_->Info(
                                                               "Download completed");
                                                       } else if (
                                                           state ==
                                                           COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
                                                         COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON
                                                         reason{};
                                                         sender->get_InterruptReason(&reason);
                                                         if (logger_)
                                                           logger_->Warn(
                                                               "Download interrupted, reason=" +
                                                               std::to_string(reason));
                                                       }
                                                       return S_OK;
                                                     })
                                                     .Get(),
                                                 &state_token);
                                           }
                                           return S_OK;
                                         })
                                         .Get(),
                                     &download_token);
                               } else if (logger_) {
                                 logger_->Warn(
                                     "WebView2 download event API unavailable; using default UI");
                               }
                               EventRegistrationToken navigation_token{};
                               webview_->add_NavigationCompleted(
                                   Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                       [this](ICoreWebView2*,
                                              ICoreWebView2NavigationCompletedEventArgs* args)
                                           -> HRESULT {
                                         BOOL success = FALSE;
                                         args->get_IsSuccess(&success);
                                         if (success) {
                                           if (logger_) logger_->Info("Navigation completed");
                                         } else if (logger_) {
                                           COREWEBVIEW2_WEB_ERROR_STATUS status{};
                                           args->get_WebErrorStatus(&status);
                                           logger_->Error("Navigation failed, WebErrorStatus=" +
                                                          std::to_string(status));
                                         }
                                         return S_OK;
                                       }).Get(),
                                   &navigation_token);
                               if (ipc_dispatcher_) {
                                 EventRegistrationToken starting_token{};
                                 webview_->add_NavigationStarting(
                                     Callback<ICoreWebView2NavigationStartingEventHandler>(
                                         [this](ICoreWebView2*,
                                                ICoreWebView2NavigationStartingEventArgs* args)
                                             -> HRESULT {
                                           LPWSTR uri = nullptr;
                                           if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                                             const auto allowed = IsAllowedIpcSource(
                                                 WideToUtf8(uri), local_origin_);
                                             CoTaskMemFree(uri);
                                             if (!allowed) {
                                               args->put_Cancel(TRUE);
                                               if (logger_)
                                                 logger_->Warn(
                                                     "Blocked external navigation while Native IPC is enabled");
                                             }
                                           }
                                           return S_OK;
                                         }).Get(),
                                     &starting_token);
                               }
                               EventRegistrationToken message_token{};
                               webview_->add_WebMessageReceived(
                                   Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                       [this](ICoreWebView2*,
                                              ICoreWebView2WebMessageReceivedEventArgs* args)
                                           -> HRESULT {
                                         LPWSTR message = nullptr;
                                         if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) &&
                                             message) {
                                           const std::wstring text(message);
                                           CoTaskMemFree(message);
                                           constexpr wchar_t prefix[] = L"__lw_web_error__";
                                           if (text.rfind(prefix, 0) == 0 && logger_)
                                             logger_->Error("[WEB-ERROR] " +
                                                            WideToUtf8(text.substr(
                                                                std::size(prefix) - 1)));
                                           return S_OK;
                                         }
                                         if (!ipc_dispatcher_) return S_OK;
                                         LPWSTR source = nullptr;
                                         if (FAILED(args->get_Source(&source)) || !source)
                                           return S_OK;
                                         const bool allowed = IsAllowedIpcSource(
                                             WideToUtf8(source), local_origin_);
                                         CoTaskMemFree(source);
                                         if (!allowed) {
                                           if (logger_)
                                             logger_->Warn("Rejected Native IPC message from an untrusted origin");
                                           return S_OK;
                                         }
                                         LPWSTR json = nullptr;
                                         if (FAILED(args->get_WebMessageAsJson(&json)) || !json)
                                           return S_OK;
                                         const auto text = WideToUtf8(json);
                                         CoTaskMemFree(json);
                                         IpcRequest request;
                                         try {
                                           request = ParseIpcRequest(text);
                                         } catch (const IpcException& error) {
                                           const auto response = SerializeIpcResponse(
                                               MakeIpcError("", error.Code(), error.what()));
                                           webview_->PostWebMessageAsJson(
                                               Utf8ToWide(response).c_str());
                                           return S_OK;
                                         }
                                         if (!ipc_dispatcher_->TryBegin(request.id)) {
                                           const auto response = SerializeIpcResponse(
                                               MakeIpcError(request.id, "BUSY",
                                                            "Duplicate or excessive pending request"));
                                           webview_->PostWebMessageAsJson(
                                               Utf8ToWide(response).c_str());
                                           return S_OK;
                                         }
                                         if (logger_)
                                           logger_->Info("IPC request: " + request.method);
                                         if (ipc_dispatcher_->ExecutionFor(request.method) ==
                                             IpcExecution::Worker) {
                                           const auto dispatcher = ipc_dispatcher_;
                                           const auto target = window_;
                                           std::thread([dispatcher, target,
                                                        request = std::move(request)]() mutable {
                                             auto pending = std::make_unique<PendingIpcResponse>();
                                             pending->dispatcher = dispatcher;
                                             pending->method = request.method;
                                             pending->response = dispatcher->Dispatch(request);
                                             if (!PostMessageW(target,
                                                               kWebViewIpcResponseMessage, 0,
                                                               reinterpret_cast<LPARAM>(
                                                                   pending.get()))) {
                                               dispatcher->End(request.id);
                                               return;
                                             }
                                             pending.release();
                                           }).detach();
                                         } else {
                                           const auto response =
                                               ipc_dispatcher_->Dispatch(request);
                                           ipc_dispatcher_->End(request.id);
                                           LogIpcResult(logger_, request.method, response);
                                           webview_->PostWebMessageAsJson(
                                               Utf8ToWide(SerializeIpcResponse(response)).c_str());
                                         }
                                         return S_OK;
                                       }).Get(),
                                   &message_token);
                               webview_->AddScriptToExecuteOnDocumentCreated(
                                   LR"JS((()=>{const p='__lw_web_error__';const s=v=>{try{return typeof v==='string'?v:JSON.stringify(v)}catch(_){return String(v)}};const e=console.error.bind(console);console.error=(...a)=>{e(...a);chrome.webview.postMessage(p+a.map(s).join(' '))};addEventListener('error',x=>chrome.webview.postMessage(p+(x.message||'Uncaught error')+(x.filename?' at '+x.filename+':'+x.lineno:'')));addEventListener('unhandledrejection',x=>chrome.webview.postMessage(p+'Unhandled rejection: '+s(x.reason)))})())JS",
                                   nullptr);
                               if (ipc_dispatcher_) {
                                 const auto bridge = Utf8ToWide(
                                     BuildIpcBridgeScript("windows", "windows"));
                                 webview_->AddScriptToExecuteOnDocumentCreated(
                                     bridge.c_str(), nullptr);
                                 if (logger_) logger_->Info("Native IPC bridge enabled");
                               }
                               Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                               if (SUCCEEDED(webview_->get_Settings(&settings))) {
                                 settings->put_AreDevToolsEnabled(manifest_.devtools);
                                 settings->put_AreDefaultContextMenusEnabled(manifest_.devtools);
                                 settings->put_IsStatusBarEnabled(FALSE);
                               }
                               Resize();
                               if (logger_) {
                                 logger_->Info("WebView2 initialized");
                                 logger_->Info("Navigate: " + WideToUtf8(url_));
                               }
                               webview_->Navigate(url_.c_str());
                               return S_OK;
                             }).Get());
          }).Get());
  if (FAILED(started)) {
    if (logger_) logger_->Error("Cannot initialize WebView2, HRESULT=" +
                                std::to_string(static_cast<unsigned long>(started)));
    on_error_(L"Cannot initialize WebView2.");
  }
}

void WebViewHost::Resize() {
  if (!controller_ || !window_) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  controller_->put_Bounds(bounds);
}

bool WebViewHost::HandleWindowMessage(UINT message, WPARAM, LPARAM lparam) {
  if (message != kWebViewIpcResponseMessage) return false;
  std::unique_ptr<PendingIpcResponse> pending(
      reinterpret_cast<PendingIpcResponse*>(lparam));
  if (!pending) return true;
  pending->dispatcher->End(pending->response.id);
  LogIpcResult(logger_, pending->method, pending->response);
  if (webview_) {
    const auto json = Utf8ToWide(SerializeIpcResponse(pending->response));
    webview_->PostWebMessageAsJson(json.c_str());
  }
  return true;
}

}  // namespace lwweb
