#pragma once

#include "lwweb/packer/manifest.h"

#include <Windows.h>
#include <Unknwn.h>
#include <WebView2.h>
#include <wrl.h>

#include <functional>
#include <memory>
#include <string>

namespace lwweb {

class Logger;
class IpcDispatcher;
class LocalFileGrantManager;

constexpr UINT kWebViewIpcResponseMessage = WM_APP + 0x4a1;

// 管理 WebView2 Environment、Controller 与网页导航生命周期。
// 对象必须在创建它的 UI 线程上使用，并且其生命周期不能短于宿主窗口。
class WebViewHost {
 public:
  WebViewHost() = default;
  ~WebViewHost();
  WebViewHost(const WebViewHost&) = delete;
  WebViewHost& operator=(const WebViewHost&) = delete;

  void Create(HWND window, const std::wstring& url, const std::string& local_origin,
              const Manifest& manifest,
              std::function<void(const std::wstring&)> on_error,
              std::function<void(bool)> on_fullscreen_changed,
              const Logger* logger = nullptr,
              std::shared_ptr<LocalFileGrantManager> file_grants = nullptr);
  void Resize();
  bool HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam);

 private:
  HWND window_ = nullptr;
  Manifest manifest_;
  std::wstring url_;
  std::string local_origin_;
  std::wstring user_data_folder_;
  std::function<void(const std::wstring&)> on_error_;
  std::function<void(bool)> on_fullscreen_changed_;
  const Logger* logger_ = nullptr;
  std::shared_ptr<IpcDispatcher> ipc_dispatcher_;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
  Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

}  // namespace lwweb
