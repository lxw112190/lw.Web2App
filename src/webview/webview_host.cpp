#include "lwweb/webview/webview_host.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/sha256.h"

#include <ShlObj.h>

#include <filesystem>
#include <iterator>

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

}  // namespace

WebViewHost::~WebViewHost() {
  if (controller_) controller_->Close();
}

void WebViewHost::Create(HWND window, const std::wstring& url, const Manifest& manifest,
                         std::function<void(const std::wstring&)> on_error,
                         const Logger* logger) {
  window_ = window;
  url_ = url;
  manifest_ = manifest;
  on_error_ = std::move(on_error);
  logger_ = logger;
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
                                         }
                                         return S_OK;
                                       }).Get(),
                                   &message_token);
                               webview_->AddScriptToExecuteOnDocumentCreated(
                                   LR"JS((()=>{const p='__lw_web_error__';const s=v=>{try{return typeof v==='string'?v:JSON.stringify(v)}catch(_){return String(v)}};const e=console.error.bind(console);console.error=(...a)=>{e(...a);chrome.webview.postMessage(p+a.map(s).join(' '))};addEventListener('error',x=>chrome.webview.postMessage(p+(x.message||'Uncaught error')+(x.filename?' at '+x.filename+':'+x.lineno:'')));addEventListener('unhandledrejection',x=>chrome.webview.postMessage(p+'Unhandled rejection: '+s(x.reason)))})())JS",
                                   nullptr);
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

}  // namespace lwweb
