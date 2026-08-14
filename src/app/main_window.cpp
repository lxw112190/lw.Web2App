#include "lwweb/app/main_window.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/packer.h"

#include <CommCtrl.h>
#include <Dwmapi.h>
#include <ShlObj.h>
#include <Uxtheme.h>
#include <commdlg.h>

#include <filesystem>
#include <string>
#include <vector>

namespace lwweb {
namespace {

constexpr wchar_t kClassName[] = L"lw.Web2App.Packer";
constexpr COLORREF kBackground = RGB(244, 247, 252);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kPrimary = RGB(39, 94, 246);
constexpr COLORREF kPrimaryHover = RGB(30, 78, 220);
constexpr COLORREF kText = RGB(28, 39, 60);
constexpr COLORREF kMuted = RGB(101, 116, 139);
constexpr COLORREF kBorder = RGB(218, 226, 238);
constexpr COLORREF kHeader = RGB(234, 241, 255);
constexpr COLORREF kSuccess = RGB(17, 142, 86);

enum ControlId {
  kModeLocal = 100,
  kModeUrl,
  kSource,
  kBrowseSource,
  kTitle,
  kWidth,
  kHeight,
  kResizable,
  kFullscreen,
  kSpa,
  kLogging,
  kDebugLogging,
  kIcon,
  kBrowseIcon,
  kOutput,
  kBrowseOutput,
  kPack,
  kSourceLabel,
  kModeHint,
};

enum class FontRole { Title, Section, Body, Small };

struct ControlLayout {
  HWND control = nullptr;
  RECT logical{};
  FontRole font = FontRole::Body;
};

// 打包器主窗口持有的字体、画刷、DPI 布局和交互状态。
// 所有 GDI 对象都在 WM_DESTROY 中统一释放，避免窗口重复打开时泄漏。
struct State {
  HWND window = nullptr;
  UINT dpi = 96;
  HFONT title_font = nullptr;
  HFONT section_font = nullptr;
  HFONT body_font = nullptr;
  HFONT small_font = nullptr;
  HBRUSH background_brush = nullptr;
  HBRUSH card_brush = nullptr;
  std::vector<ControlLayout> controls;
  std::wstring status = L"准备就绪 · 请选择网页目录和输出位置";
  bool busy = false;
};

constexpr RECT kStatusRect{48, 731, 712, 757};

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

RECT ScaleRect(RECT value, UINT dpi) {
  return {Scale(value.left, dpi), Scale(value.top, dpi), Scale(value.right, dpi),
          Scale(value.bottom, dpi)};
}

HFONT FontForRole(const State& state, FontRole role) {
  switch (role) {
    case FontRole::Title:
      return state.title_font;
    case FontRole::Section:
      return state.section_font;
    case FontRole::Small:
      return state.small_font;
    default:
      return state.body_font;
  }
}

FontRole RoleForFont(const State& state, HFONT font) {
  if (font == state.title_font) return FontRole::Title;
  if (font == state.section_font) return FontRole::Section;
  if (font == state.small_font) return FontRole::Small;
  return FontRole::Body;
}

HFONT MakeFont(int height, int weight, UINT dpi) {
  return CreateFontW(-Scale(height, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

HWND AddControl(State& state, const wchar_t* kind, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id = 0,
                HFONT font = nullptr, DWORD extended_style = 0) {
  const auto control = CreateWindowExW(
      extended_style, kind, text, WS_CHILD | WS_VISIBLE | style, Scale(x, state.dpi),
      Scale(y, state.dpi), Scale(width, state.dpi), Scale(height, state.dpi),
      state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  const auto role = RoleForFont(state, font);
  SendMessageW(control, WM_SETFONT,
               reinterpret_cast<WPARAM>(FontForRole(state, role)), TRUE);
  SetWindowTheme(control, L"Explorer", nullptr);
  state.controls.push_back(
      {control, RECT{x, y, x + width, y + height}, role});
  return control;
}

void RecreateFonts(State& state) {
  const auto old_title = state.title_font;
  const auto old_section = state.section_font;
  const auto old_body = state.body_font;
  const auto old_small = state.small_font;
  state.title_font = MakeFont(30, FW_SEMIBOLD, state.dpi);
  state.section_font = MakeFont(18, FW_SEMIBOLD, state.dpi);
  state.body_font = MakeFont(16, FW_NORMAL, state.dpi);
  state.small_font = MakeFont(14, FW_NORMAL, state.dpi);
  for (const auto& item : state.controls)
    SendMessageW(item.control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(FontForRole(state, item.font)), TRUE);
  if (old_title) DeleteObject(old_title);
  if (old_section) DeleteObject(old_section);
  if (old_body) DeleteObject(old_body);
  if (old_small) DeleteObject(old_small);
}

void LayoutControls(const State& state) {
  for (const auto& item : state.controls) {
    const auto rect = ScaleRect(item.logical, state.dpi);
    MoveWindow(item.control, rect.left, rect.top, rect.right - rect.left,
               rect.bottom - rect.top, TRUE);
  }
}

HWND AddLabel(State& state, const wchar_t* text, int x, int y, int width,
              int height, int id = 0, HFONT font = nullptr) {
  return AddControl(state, L"STATIC", text, SS_LEFT, x, y, width, height, id, font);
}

HWND AddEdit(State& state, const wchar_t* text, int x, int y, int width, int id,
             const wchar_t* cue = nullptr, DWORD extra_style = 0) {
  const auto edit = AddControl(state, L"EDIT", text,
                               WS_TABSTOP | ES_AUTOHSCROLL | extra_style, x, y, width,
                               34, id, state.body_font, WS_EX_CLIENTEDGE);
  if (cue) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
  return edit;
}

HWND AddButton(State& state, const wchar_t* text, int x, int y, int width,
               int height, int id) {
  return AddControl(state, L"BUTTON", text,
                    WS_TABSTOP | BS_OWNERDRAW, x, y, width, height, id);
}

std::wstring Text(HWND window, int id) {
  const auto control = GetDlgItem(window, id);
  const auto length = GetWindowTextLengthW(control);
  std::wstring value(length + 1, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  value.resize(length);
  return value;
}

std::filesystem::path PickFolder(HWND owner) {
  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog))))
    return {};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"选择网页构建产物目录");
  std::filesystem::path result;
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}

std::filesystem::path PickFile(HWND owner, const wchar_t* title,
                               const wchar_t* filter, bool save) {
  wchar_t buffer[32768]{};
  OPENFILENAMEW value{sizeof(value)};
  value.hwndOwner = owner;
  value.lpstrTitle = title;
  value.lpstrFilter = filter;
  value.lpstrFile = buffer;
  value.nMaxFile = static_cast<DWORD>(std::size(buffer));
  value.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  value.lpstrDefExt = save ? L"exe" : nullptr;
  return (save ? GetSaveFileNameW(&value) : GetOpenFileNameW(&value))
             ? std::filesystem::path(buffer)
             : std::filesystem::path{};
}

std::wstring LocalizeProgress(const std::string& message) {
  if (message == "Copying runner") return L"正在准备通用运行器…";
  if (message == "Writing PE metadata" || message == "Writing platform metadata")
    return L"正在写入图标和版本信息…";
  if (message == "Compressing static resources") return L"正在压缩网页资源…";
  if (message == "Appending payload and SHA-256") return L"正在写入 Payload 并计算 SHA-256…";
  if (message == "Publishing output") return L"正在发布最终应用…";
  if (message == "Done") return L"生成完成，可以运行目标 EXE。";
  return Utf8ToWide(message);
}

void SetStatus(HWND window, const std::wstring& text) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return;
  state->status = text;
  // 状态由父窗口统一绘制；擦除并同步刷新完整固定区域，避免透明 STATIC
  // 和嵌套消息泵在快速更新时保留旧字形。
  const auto status_rect = ScaleRect(kStatusRect, state->dpi);
  RedrawWindow(window, &status_rect, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

void UpdateMode(HWND window) {
  const bool local = IsDlgButtonChecked(window, kModeLocal) == BST_CHECKED;
  SetWindowTextW(GetDlgItem(window, kSourceLabel),
                 local ? L"网页目录" : L"在线网址");
  SetWindowTextW(GetDlgItem(window, kModeHint),
                 local ? L"选择 Vite、Vue、React 或普通 HTML 的构建产物目录"
                       : L"输入以 http:// 或 https:// 开头的网页地址");
  SendMessageW(GetDlgItem(window, kSource), EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(local ? L"例如：D:\\project\\dist"
                                               : L"例如：https://example.com"));
  EnableWindow(GetDlgItem(window, kBrowseSource), local);
  InvalidateRect(window, nullptr, TRUE);
}

void Pack(HWND window) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state || state->busy) return;
  state->busy = true;
  EnableWindow(GetDlgItem(window, kPack), FALSE);
  SetWindowTextW(GetDlgItem(window, kPack), L"正在生成…");
  try {
    PackOptions options;
    options.runner = CurrentExecutablePath();
    options.output = Text(window, kOutput);
    options.manifest.title = WideToUtf8(Text(window, kTitle));
    options.manifest.width = std::stoul(Text(window, kWidth));
    options.manifest.height = std::stoul(Text(window, kHeight));
    options.manifest.resizable = IsDlgButtonChecked(window, kResizable) == BST_CHECKED;
    options.manifest.fullscreen = IsDlgButtonChecked(window, kFullscreen) == BST_CHECKED;
    options.manifest.spa_fallback = IsDlgButtonChecked(window, kSpa) == BST_CHECKED;
    options.manifest.logging.enabled = IsDlgButtonChecked(window, kLogging) == BST_CHECKED;
    options.manifest.logging.level =
        IsDlgButtonChecked(window, kDebugLogging) == BST_CHECKED ? "debug" : "info";
    options.metadata.product_name = Text(window, kTitle);
    options.metadata.file_description = Text(window, kTitle);
    const auto icon = Text(window, kIcon);
    if (!icon.empty()) options.metadata.icon = icon;
    if (IsDlgButtonChecked(window, kModeUrl) == BST_CHECKED) {
      options.manifest.mode = AppMode::Url;
      options.manifest.url = WideToUtf8(Text(window, kSource));
    } else {
      options.manifest.mode = AppMode::Local;
      options.source_directory = Text(window, kSource);
      const auto entry = FindDefaultEntry(options.source_directory);
      options.manifest.entry = WideToUtf8(
          std::filesystem::relative(entry, options.source_directory).generic_wstring());
    }
    options.progress = [window](const std::string& message) {
      SetStatus(window, LocalizeProgress(message));
      MSG event{};
      while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&event);
        DispatchMessageW(&event);
      }
    };
    PackApplication(options);
    MessageBoxW(window, (L"EXE 已成功生成：\n\n" + options.output.wstring()).c_str(),
                L"lw.Web2App", MB_OK | MB_ICONINFORMATION);
  } catch (const std::exception& error) {
    const auto message = Utf8ToWide(error.what());
    SetStatus(window, L"生成失败：" + message);
    MessageBoxW(window, message.c_str(), L"生成失败", MB_OK | MB_ICONERROR);
  }
  state->busy = false;
  EnableWindow(GetDlgItem(window, kPack), TRUE);
  SetWindowTextW(GetDlgItem(window, kPack), L"生成 Windows EXE");
  InvalidateRect(GetDlgItem(window, kPack), nullptr, TRUE);
}

void DrawRoundedPanel(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
  const auto fill_brush = CreateSolidBrush(fill);
  const auto border_pen = CreatePen(PS_SOLID, 1, border);
  const auto old_brush = SelectObject(dc, fill_brush);
  const auto old_pen = SelectObject(dc, border_pen);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(border_pen);
  DeleteObject(fill_brush);
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
  const bool primary = item.CtlID == kPack;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool focused = (item.itemState & ODS_FOCUS) != 0;
  COLORREF fill = kCard;
  COLORREF border = kBorder;
  COLORREF text = kText;
  if (primary) {
    fill = disabled ? RGB(151, 176, 242) : (pressed ? kPrimaryHover : kPrimary);
    border = fill;
    text = RGB(255, 255, 255);
  } else if (pressed) {
    fill = RGB(237, 242, 250);
  }
  const auto dpi = GetDpiForWindow(item.hwndItem);
  DrawRoundedPanel(item.hDC, item.rcItem, fill, border,
                   Scale(primary ? 12 : 9, dpi));
  wchar_t caption[128]{};
  GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, text);
  SelectObject(item.hDC, reinterpret_cast<HFONT>(
                               SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
  auto text_rect = item.rcItem;
  DrawTextW(item.hDC, caption, -1, &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if (focused && !primary) {
    InflateRect(&text_rect, -4, -4);
    DrawFocusRect(item.hDC, &text_rect);
  }
}

void BuildInterface(State& state) {
  AddLabel(state, L"lw.Web2App", 34, 22, 300, 38, 0, state.title_font);
  AddLabel(state, L"把网页构建产物快速转换为轻量 Windows 应用", 35, 63, 510, 24,
           0, state.small_font);
  AddLabel(state, L"C++17 · Win32 · WebView2", 535, 39, 190, 22, 0,
           state.small_font);

  AddLabel(state, L"01  网页来源", 48, 126, 200, 28, 0, state.section_font);
  AddControl(state, L"BUTTON", L"本地静态目录", WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
             48, 162, 145, 26, kModeLocal);
  AddControl(state, L"BUTTON", L"在线网址", WS_TABSTOP | BS_AUTORADIOBUTTON,
             208, 162, 120, 26, kModeUrl);
  CheckRadioButton(state.window, kModeLocal, kModeUrl, kModeLocal);
  AddLabel(state, L"网页目录", 48, 202, 120, 22, kSourceLabel, state.small_font);
  AddLabel(state, L"选择 Vite、Vue、React 或普通 HTML 的构建产物目录",
           141, 203, 480, 22, kModeHint, state.small_font);
  AddEdit(state, L"", 48, 228, 548, kSource, L"例如：D:\\project\\dist");
  AddButton(state, L"选择目录", 610, 228, 102, 34, kBrowseSource);

  AddLabel(state, L"02  应用设置", 48, 313, 200, 28, 0, state.section_font);
  AddLabel(state, L"应用名称", 48, 355, 120, 22, 0, state.small_font);
  AddEdit(state, L"我的网页应用", 48, 379, 664, kTitle, L"显示在窗口标题栏中的名称");
  AddLabel(state, L"窗口尺寸", 48, 427, 120, 22, 0, state.small_font);
  AddEdit(state, L"1280", 48, 451, 92, kWidth, nullptr, ES_NUMBER);
  AddLabel(state, L"×", 150, 457, 24, 22, 0, state.section_font);
  AddEdit(state, L"800", 180, 451, 92, kHeight, nullptr, ES_NUMBER);
  AddControl(state, L"BUTTON", L"默认全屏显示", WS_TABSTOP | BS_AUTOCHECKBOX,
             309, 454, 132, 26, kFullscreen);
  AddControl(state, L"BUTTON", L"允许调整大小", WS_TABSTOP | BS_AUTOCHECKBOX,
             443, 454, 130, 26, kResizable);
  AddControl(state, L"BUTTON", L"SPA 路由回退", WS_TABSTOP | BS_AUTOCHECKBOX,
             576, 454, 130, 26, kSpa);
  CheckDlgButton(state.window, kFullscreen, BST_CHECKED);
  CheckDlgButton(state.window, kResizable, BST_CHECKED);
  CheckDlgButton(state.window, kSpa, BST_CHECKED);

  AddLabel(state, L"应用图标（PNG / ICO，可选）", 48, 499, 240, 22, 0,
           state.small_font);
  AddControl(state, L"BUTTON", L"启用运行日志", WS_TABSTOP | BS_AUTOCHECKBOX,
             330, 496, 140, 26, kLogging);
  AddControl(state, L"BUTTON", L"详细日志", WS_TABSTOP | BS_AUTOCHECKBOX,
             500, 496, 110, 26, kDebugLogging);
  CheckDlgButton(state.window, kLogging, BST_CHECKED);
  AddEdit(state, L"", 48, 523, 548, kIcon, L"留空则使用默认图标");
  AddButton(state, L"选择图标", 610, 523, 102, 34, kBrowseIcon);
  AddLabel(state, L"输出位置", 48, 571, 120, 22, 0, state.small_font);
  AddEdit(state, L"", 48, 595, 548, kOutput, L"选择生成的 .exe 文件位置");
  AddButton(state, L"选择位置", 610, 595, 102, 34, kBrowseOutput);

  AddButton(state, L"生成 Windows EXE", 48, 668, 664, 48, kPack);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_CREATE: {
      state = new State{};
      state->window = window;
      state->dpi = GetDpiForWindow(window);
      RecreateFonts(*state);
      state->background_brush = CreateSolidBrush(kBackground);
      state->card_brush = CreateSolidBrush(kCard);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      BuildInterface(*state);
      return 0;
    }
    case WM_DPICHANGED:
      if (state) {
        state->dpi = HIWORD(wparam);
        RecreateFonts(*state);
        LayoutControls(*state);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      }
      return 0;
    case WM_COMMAND:
      if (!state) break;
      switch (LOWORD(wparam)) {
        case kModeLocal:
        case kModeUrl:
          UpdateMode(window);
          return 0;
        case kLogging:
          EnableWindow(GetDlgItem(window, kDebugLogging),
                       IsDlgButtonChecked(window, kLogging) == BST_CHECKED);
          return 0;
        case kBrowseSource: {
          const auto path = PickFolder(window);
          if (!path.empty()) SetWindowTextW(GetDlgItem(window, kSource), path.c_str());
          return 0;
        }
        case kBrowseIcon: {
          const auto path = PickFile(
              window, L"选择应用图标",
              L"图标文件 (*.png;*.ico)\0*.png;*.ico\0所有文件\0*.*\0", false);
          if (!path.empty()) SetWindowTextW(GetDlgItem(window, kIcon), path.c_str());
          return 0;
        }
        case kBrowseOutput: {
          const auto path = PickFile(window, L"选择 EXE 输出位置",
                                     L"Windows 可执行文件 (*.exe)\0*.exe\0", true);
          if (!path.empty()) SetWindowTextW(GetDlgItem(window, kOutput), path.c_str());
          return 0;
        }
        case kPack:
          Pack(window);
          return 0;
      }
      break;
    case WM_DRAWITEM:
      DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
      return TRUE;
    case WM_CTLCOLORSTATIC: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      const auto control = reinterpret_cast<HWND>(lparam);
      const auto id = GetDlgCtrlID(control);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, id == kModeHint ? kMuted : kText);
      return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLORBTN: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const auto dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      FillRect(dc, &client, state ? state->background_brush
                                  : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
      RECT header{0, 0, client.right, state ? Scale(105, state->dpi) : 105};
      const auto header_brush = CreateSolidBrush(kHeader);
      FillRect(dc, &header, header_brush);
      DeleteObject(header_brush);
      const auto dpi = state ? state->dpi : 96;
      DrawRoundedPanel(dc, ScaleRect(RECT{28, 109, 732, 286}, dpi), kCard, kBorder,
                       Scale(18, dpi));
      DrawRoundedPanel(dc, ScaleRect(RECT{28, 296, 732, 648}, dpi), kCard, kBorder,
                       Scale(18, dpi));
      if (state) {
        // 永远先覆盖整个状态区域，再绘制一次当前文本。
        const auto scaled_status = ScaleRect(kStatusRect, state->dpi);
        FillRect(dc, &scaled_status, state->background_brush);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, kBackground);
        SetTextColor(dc, kSuccess);
        SelectObject(dc, state->small_font);
        auto status_rect = scaled_status;
        DrawTextW(dc, state->status.c_str(), -1, &status_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      if (state) {
        DeleteObject(state->title_font);
        DeleteObject(state->section_font);
        DeleteObject(state->body_font);
        DeleteObject(state->small_font);
        DeleteObject(state->background_brush);
        DeleteObject(state->card_brush);
        delete state;
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int RunPackerGui(HINSTANCE instance) {
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = kClassName;
  if (!RegisterClassExW(&window_class)) throw Error("Cannot register the packer window");
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  const auto dpi = GetDpiForSystem();
  RECT bounds{0, 0, Scale(760, dpi), Scale(781, dpi)};
  AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_CONTROLPARENT, dpi);
  const auto window = CreateWindowExW(
      WS_EX_CONTROLPARENT, kClassName, L"lw.Web2App · 网页转 Windows 应用",
      style, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
      bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
  if (!window) throw Error("Cannot create the packer window");
  const DWORD corner = 2;  // DWMWCP_ROUND
  DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

}  // namespace lwweb
