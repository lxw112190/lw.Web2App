#include "lwweb/webview/webview_host.h"

using Microsoft::WRL::Callback;

namespace lwweb {

WebViewHost::~WebViewHost() {
  if (controller_) controller_->Close();
}

void WebViewHost::Create(HWND window, const std::wstring& url, const Manifest& manifest,
                         std::function<void(const std::wstring&)> on_error) {
  window_ = window;
  url_ = url;
  manifest_ = manifest;
  on_error_ = std::move(on_error);
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              on_error_(L"WebView2 Runtime is unavailable. Install the Evergreen Runtime and retry.");
              return result;
            }
            return environment->CreateCoreWebView2Controller(
                window_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                             [this](HRESULT controller_result,
                                    ICoreWebView2Controller* controller) -> HRESULT {
                               if (FAILED(controller_result) || !controller) {
                                 on_error_(L"Cannot create the WebView2 controller.");
                                 return controller_result;
                               }
                               controller_ = controller;
                               controller_->get_CoreWebView2(&webview_);
                               Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                               if (SUCCEEDED(webview_->get_Settings(&settings))) {
                                 settings->put_AreDevToolsEnabled(manifest_.devtools);
                                 settings->put_AreDefaultContextMenusEnabled(manifest_.devtools);
                                 settings->put_IsStatusBarEnabled(FALSE);
                               }
                               Resize();
                               webview_->Navigate(url_.c_str());
                               return S_OK;
                             }).Get());
          }).Get());
  if (FAILED(started)) on_error_(L"Cannot initialize WebView2.");
}

void WebViewHost::Resize() {
  if (!controller_ || !window_) return;
  RECT bounds{};
  GetClientRect(window_, &bounds);
  controller_->put_Bounds(bounds);
}

}  // namespace lwweb
