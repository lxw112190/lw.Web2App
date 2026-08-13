#pragma once

#include "lwweb/packer/manifest.h"

#include <Windows.h>
#include <Unknwn.h>
#include <WebView2.h>
#include <wrl.h>

#include <functional>
#include <string>

namespace lwweb {

// 管理 WebView2 Environment、Controller 与网页导航生命周期。
// 对象必须在创建它的 UI 线程上使用，并且其生命周期不能短于宿主窗口。
class WebViewHost {
 public:
  WebViewHost() = default;
  ~WebViewHost();
  WebViewHost(const WebViewHost&) = delete;
  WebViewHost& operator=(const WebViewHost&) = delete;

  void Create(HWND window, const std::wstring& url, const Manifest& manifest,
              std::function<void(const std::wstring&)> on_error);
  void Resize();

 private:
  HWND window_ = nullptr;
  Manifest manifest_;
  std::wstring url_;
  std::function<void(const std::wstring&)> on_error_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

}  // namespace lwweb
