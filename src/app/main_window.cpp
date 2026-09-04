#include "lwweb/app/main_window.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/pe_version.h"
#include "lwweb/packer/packer.h"
#include "lwweb/version.h"

#include <CommCtrl.h>
#include <Dwmapi.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <Uxtheme.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

constexpr wchar_t kClassName[] = L"lw.Web2App.Packer";
constexpr wchar_t kIpcSettingsClassName[] = L"lw.Web2App.IpcSettings";
constexpr wchar_t kProjectUrl[] = L"https://github.com/lxw112190/lw.Web2App";
constexpr wchar_t kReleasesUrl[] = L"https://github.com/lxw112190/lw.Web2App/releases";
constexpr int kClientWidth = 1120;
constexpr int kClientHeight = 720;
constexpr int kSponsorResourceId = 201;
constexpr COLORREF kBackground = RGB(244, 247, 252);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kPrimary = RGB(39, 94, 246);
constexpr COLORREF kPrimaryPressed = RGB(30, 78, 220);
constexpr COLORREF kText = RGB(28, 39, 60);
constexpr COLORREF kMuted = RGB(101, 116, 139);
constexpr COLORREF kBorder = RGB(218, 226, 238);
constexpr COLORREF kHeader = RGB(234, 241, 255);
constexpr COLORREF kSuccess = RGB(17, 142, 86);
constexpr COLORREF kFailure = RGB(198, 57, 57);

enum ControlId {
  kModeLocal = 100,
  kModeUrl,
  kSource,
  kBrowseSource,
  kEntry,
  kStartPath,
  kBackendProxy,
  kBackendOrigin,
  kTitle,
  kProductName,
  kFileDescription,
  kCompanyName,
  kVersionEdit,
  kCopyright,
  kIcon,
  kBrowseIcon,
  kDefaultIcon,
  kWidth,
  kHeight,
  kFullscreen,
  kResizable,
  kSpa,
  kLogging,
  kDebugLogging,
  kIpcEnabled,
  kIpcSettings,
  kIpcSummary,
  kOutput,
  kBrowseOutput,
  kPack,
  kSourceLabel,
  kModeHint,
  kEntryLabel,
  kStartPathLabel,
  kBackendOriginLabel,
  kIconHint,
  kSponsorCaption,
  kProjectHome,
  kLatestRelease,
  kHeaderVersion,
};

enum class FontRole { Title, Section, Body, Small };

// 保存一个 Win32 子控件的 96-DPI 逻辑位置与字体角色。
struct ControlLayout {
  HWND control = nullptr;
  RECT logical{};
  FontRole font = FontRole::Body;
};

// 打包器主窗口持有的 GDI 资源、DPI 布局、预览图和表单交互状态。
// 所有线程只通过消息向该对象回传结果，GDI 对象在 WM_DESTROY 统一释放。
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
  HBITMAP icon_preview = nullptr;
  HBITMAP sponsor_qr = nullptr;
  HICON default_icon = nullptr;
  RECT left_card{28, 96, 526, 594};
  RECT right_card{538, 96, 1092, 594};
  RECT output_card{28, 602, 1092, 692};
  RECT icon_preview_rect{656, 374, 724, 442};
  RECT sponsor_rect{48, 400, 160, 512};
  RECT sponsor_divider{48, 382, 506, 383};
  RECT status_rect{48, 663, 824, 684};
  std::wstring status = L"准备就绪 · 请选择网页目录";
  COLORREF status_color = kSuccess;
  COLORREF icon_hint_color = kMuted;
  bool busy = false;
  bool syncing_metadata = false;
  bool product_name_edited = false;
  bool file_description_edited = false;
  bool ipc_enabled = false;
  std::vector<std::string> ipc_capabilities;
  std::vector<std::filesystem::path> ipc_filesystem_roots;
};

constexpr UINT kPackProgressMessage = WM_APP + 1;
constexpr UINT kPackFinishedMessage = WM_APP + 2;

// GUI 的默认输出与打包器放在同一目录树下，使用独立 out 子目录，既方便
// 用户查找，也确保默认生成文件不会覆盖正在运行的 lw.Web2App.exe。
std::filesystem::path DefaultOutputPath() {
  return CurrentExecutablePath().parent_path() / L"out" / L"MyWebApp.exe";
}

// 工作线程只写入该结果对象，窗口线程负责显示消息并恢复控件状态。
struct PackResult {
  bool success = false;
  std::filesystem::path output;
  std::wstring error;
};

int Scale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

RECT ScaleRect(RECT value, UINT dpi) {
  return {Scale(value.left, dpi), Scale(value.top, dpi), Scale(value.right, dpi),
          Scale(value.bottom, dpi)};
}

HFONT FontForRole(const State& state, FontRole role) {
  switch (role) {
    case FontRole::Title: return state.title_font;
    case FontRole::Section: return state.section_font;
    case FontRole::Small: return state.small_font;
    default: return state.body_font;
  }
}

FontRole RoleForFont(const State& state, HFONT font) {
  if (font == state.title_font) return FontRole::Title;
  if (font == state.section_font) return FontRole::Section;
  if (font == state.small_font) return FontRole::Small;
  return FontRole::Body;
}

HFONT MakeFont(int height, int weight, UINT dpi) {
  return CreateFontW(-Scale(height, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Microsoft YaHei UI");
}

void RecreateFonts(State& state) {
  const auto old_title = state.title_font;
  const auto old_section = state.section_font;
  const auto old_body = state.body_font;
  const auto old_small = state.small_font;
  state.title_font = MakeFont(24, FW_SEMIBOLD, state.dpi);
  state.section_font = MakeFont(17, FW_SEMIBOLD, state.dpi);
  state.body_font = MakeFont(14, FW_NORMAL, state.dpi);
  state.small_font = MakeFont(12, FW_NORMAL, state.dpi);
  for (const auto& item : state.controls)
    SendMessageW(item.control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(FontForRole(state, item.font)), TRUE);
  if (old_title) DeleteObject(old_title);
  if (old_section) DeleteObject(old_section);
  if (old_body) DeleteObject(old_body);
  if (old_small) DeleteObject(old_small);
}

HWND AddControl(State& state, const wchar_t* kind, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id = 0,
                HFONT font = nullptr, DWORD extended_style = 0) {
  const auto control = CreateWindowExW(
      extended_style, kind, text, WS_CHILD | WS_VISIBLE | style, Scale(x, state.dpi),
      Scale(y, state.dpi), Scale(width, state.dpi), Scale(height, state.dpi),
      state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (!control) throw Error("Cannot create a Windows GUI control");
  const auto role = RoleForFont(state, font);
  SendMessageW(control, WM_SETFONT,
               reinterpret_cast<WPARAM>(FontForRole(state, role)), TRUE);
  SetWindowTheme(control, L"Explorer", nullptr);
  state.controls.push_back({control, RECT{x, y, x + width, y + height}, role});
  return control;
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
  return AddControl(state, L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y, width,
                    height, id, font);
}

HWND AddEdit(State& state, const wchar_t* text, int x, int y, int width, int id,
             const wchar_t* cue = nullptr, DWORD extra_style = 0) {
  const auto edit = AddControl(state, L"EDIT", text,
                               WS_TABSTOP | ES_AUTOHSCROLL | extra_style, x, y,
                               width, 30, id, state.body_font, WS_EX_CLIENTEDGE);
  if (cue) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
  return edit;
}

HWND AddButton(State& state, const wchar_t* text, int x, int y, int width,
               int height, int id) {
  return AddControl(state, L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, x, y,
                    width, height, id);
}

HWND AddCombo(State& state, int x, int y, int width, int id) {
  return AddControl(state, L"COMBOBOX", L"",
                    WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                    x, y, width, 220, id, state.body_font, WS_EX_CLIENTEDGE);
}

std::wstring Text(HWND window, int id) {
  const auto control = GetDlgItem(window, id);
  const auto length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<std::size_t>(length));
  return value;
}

std::wstring Trim(std::wstring value) {
  const auto first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const auto last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1U);
}

void SetDynamicLabelText(HWND window, int id, const std::wstring& text) {
  const auto label = GetDlgItem(window, id);
  SetWindowTextW(label, text.c_str());
  RedrawWindow(label, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void UpdateSuggestedStartPath(HWND window) {
  const auto entry = WideToUtf8(Text(window, kEntry));
  if (entry.empty()) return;
  try {
    SetWindowTextW(GetDlgItem(window, kStartPath),
                   Utf8ToWide(SuggestedStartPath(entry)).c_str());
  } catch (...) {
  }
}

void RefreshHtmlEntries(HWND window) {
  const auto combo = GetDlgItem(window, kEntry);
  const auto previous = Text(window, kEntry);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  std::vector<std::string> entries;
  try {
    const auto source = Text(window, kSource);
    if (!source.empty()) entries = FindHtmlEntries(std::filesystem::path(source));
  } catch (...) {
  }
  if (entries.empty()) entries.push_back("index.html");
  int selection = 0;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto wide = Utf8ToWide(entries[index]);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    if (wide == previous) selection = static_cast<int>(index);
  }
  SendMessageW(combo, CB_SETCURSEL, selection, 0);
  UpdateSuggestedStartPath(window);
}

std::filesystem::path PickFolder(
    HWND owner, const wchar_t* title = L"选择网页构建产物目录") {
  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) return {};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(title);
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
             ? std::filesystem::path(buffer) : std::filesystem::path{};
}

enum IpcDialogControlId {
  kIpcPresetReadOnly = 300,
  kIpcPresetBrowse,
  kIpcPresetManage,
  kIpcRootList,
  kIpcAddRoot,
  kIpcRemoveRoot,
  kIpcCapabilityFirst = 320,
};

// GUI 文案与 Manifest 能力名的唯一映射。界面只展示客户容易理解的名称，
// 写入 Payload 时仍使用稳定的 Native IPC capability 标识。
struct IpcCapabilityControl {
  int id;
  const wchar_t* label;
  const char* capability;
};

constexpr std::array<IpcCapabilityControl, 16> kIpcCapabilityControls{{
    {kIpcCapabilityFirst + 0, L"获取应用信息", "app.info"},
    {kIpcCapabilityFirst + 1, L"获取系统目录", "app.paths"},
    {kIpcCapabilityFirst + 2, L"选择本地文件", "dialog.file"},
    {kIpcCapabilityFirst + 3, L"选择本地文件夹", "dialog.directory"},
    {kIpcCapabilityFirst + 4, L"检查文件是否存在", "fs.exists"},
    {kIpcCapabilityFirst + 5, L"浏览文件夹内容", "fs.list"},
    {kIpcCapabilityFirst + 6, L"预览授权文件", "fs.read"},
    {kIpcCapabilityFirst + 7, L"创建文件夹", "fs.mkdir"},
    {kIpcCapabilityFirst + 8, L"复制文件", "fs.copy"},
    {kIpcCapabilityFirst + 9, L"移动文件", "fs.move"},
    {kIpcCapabilityFirst + 10, L"移入回收站（推荐）", "fs.trash"},
    {kIpcCapabilityFirst + 11, L"永久删除（高风险）", "fs.delete"},
    {kIpcCapabilityFirst + 12, L"监听文件变化", "fs.watch"},
    {kIpcCapabilityFirst + 13, L"控制应用窗口", "window.control"},
    {kIpcCapabilityFirst + 14, L"允许应用退出", "app.lifecycle"},
    {kIpcCapabilityFirst + 15, L"系统托盘", "tray"},
}};

// Native IPC 权限弹窗的临时状态。只有点击“保存”后才会回写主窗口，
// 关闭或取消不会改变当前打包配置。
struct IpcDialogState {
  HWND window = nullptr;
  State* owner_state = nullptr;
  UINT dpi = 96;
  HFONT title_font = nullptr;
  HFONT body_font = nullptr;
  HFONT small_font = nullptr;
  bool accepted = false;
  std::vector<std::string> capabilities;
  std::vector<std::filesystem::path> roots;
};

bool HasIpcCapability(const std::vector<std::string>& capabilities,
                      const char* capability) {
  return std::find(capabilities.begin(), capabilities.end(), capability) !=
         capabilities.end();
}

HWND AddIpcDialogControl(IpcDialogState& state, const wchar_t* kind,
                         const wchar_t* text, DWORD style, int x, int y,
                         int width, int height, int id, HFONT font,
                         DWORD extended_style = 0) {
  const auto control = CreateWindowExW(
      extended_style, kind, text, WS_CHILD | WS_VISIBLE | style,
      Scale(x, state.dpi), Scale(y, state.dpi), Scale(width, state.dpi),
      Scale(height, state.dpi), state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (!control) throw Error("Cannot create the IPC settings control");
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SetWindowTheme(control, L"Explorer", nullptr);
  return control;
}

void SetIpcDialogPreset(HWND window,
                        const std::vector<std::string>& capabilities) {
  for (const auto& item : kIpcCapabilityControls)
    CheckDlgButton(window, item.id,
                   HasIpcCapability(capabilities, item.capability)
                       ? BST_CHECKED
                       : BST_UNCHECKED);
}

std::vector<std::string> CollectIpcDialogCapabilities(HWND window) {
  std::vector<std::string> capabilities;
  for (const auto& item : kIpcCapabilityControls) {
    if (IsDlgButtonChecked(window, item.id) == BST_CHECKED)
      capabilities.emplace_back(item.capability);
  }
  return capabilities;
}

void RefreshIpcRootList(IpcDialogState& state) {
  const auto list = GetDlgItem(state.window, kIpcRootList);
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  for (const auto& root : state.roots) {
    const auto value = root.wstring();
    SendMessageW(list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
  }
  EnableWindow(GetDlgItem(state.window, kIpcRemoveRoot), !state.roots.empty());
}

void BuildIpcSettingsInterface(IpcDialogState& state) {
  state.title_font = MakeFont(20, FW_SEMIBOLD, state.dpi);
  state.body_font = MakeFont(14, FW_NORMAL, state.dpi);
  state.small_font = MakeFont(12, FW_NORMAL, state.dpi);
  AddIpcDialogControl(state, L"STATIC", L"本地交互能力（Native IPC）",
                      SS_LEFT | SS_NOPREFIX, 24, 20, 420, 30, 0,
                      state.title_font);
  AddIpcDialogControl(
      state, L"STATIC",
      L"仅授权受信任的本地网页；网页需通过 window.lw.invoke() 主动调用。",
      SS_LEFT | SS_NOPREFIX, 24, 54, 650, 22, 0, state.small_font);

  AddIpcDialogControl(state, L"STATIC", L"常用方案", SS_LEFT | SS_NOPREFIX,
                      24, 88, 90, 22, 0, state.body_font);
  AddIpcDialogControl(state, L"BUTTON", L"只读文件查看", WS_TABSTOP,
                      116, 82, 150, 32, kIpcPresetReadOnly, state.body_font);
  AddIpcDialogControl(state, L"BUTTON", L"文件夹浏览", WS_TABSTOP,
                      276, 82, 150, 32, kIpcPresetBrowse, state.body_font);
  AddIpcDialogControl(state, L"BUTTON", L"安全文件管理", WS_TABSTOP,
                      436, 82, 160, 32, kIpcPresetManage, state.body_font);

  AddIpcDialogControl(state, L"STATIC", L"授权能力", SS_LEFT | SS_NOPREFIX,
                      24, 130, 120, 22, 0, state.body_font);
  for (std::size_t index = 0; index < kIpcCapabilityControls.size(); ++index) {
    const auto& item = kIpcCapabilityControls[index];
    // Keep the fixed-width dialog within bounds; the tray capability occupies the sixth row.
    const int column = static_cast<int>(index % 3);
    const int row = static_cast<int>(index / 3);
    AddIpcDialogControl(state, L"BUTTON", item.label,
                        WS_TABSTOP | BS_AUTOCHECKBOX,
                        32 + column * 214, 158 + row * 32, 205, 24,
                        item.id, state.body_font);
  }

  AddIpcDialogControl(state, L"STATIC", L"固定授权目录（可选，最多 32 个）",
                      SS_LEFT | SS_NOPREFIX, 24, 360, 300, 22, 0,
                      state.body_font);
  AddIpcDialogControl(
      state, L"LISTBOX", L"",
      WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
      24, 388, 522, 132, kIpcRootList, state.body_font, WS_EX_CLIENTEDGE);
  AddIpcDialogControl(state, L"BUTTON", L"添加目录", WS_TABSTOP,
                      560, 388, 126, 34, kIpcAddRoot, state.body_font);
  AddIpcDialogControl(state, L"BUTTON", L"移除所选", WS_TABSTOP,
                      560, 432, 126, 34, kIpcRemoveRoot, state.body_font);
  AddIpcDialogControl(
      state, L"STATIC",
      L"安全提示：固定目录会授予网页持续访问权限；删除、移动等写操作请按最小权限开启。",
      SS_LEFT | SS_NOPREFIX, 24, 538, 662, 38, 0, state.small_font);
  AddIpcDialogControl(state, L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON,
                      454, 592, 110, 36, IDOK, state.body_font);
  AddIpcDialogControl(state, L"BUTTON", L"取消", WS_TABSTOP,
                      576, 592, 110, 36, IDCANCEL, state.body_font);
  SetIpcDialogPreset(state.window, state.capabilities);
  RefreshIpcRootList(state);
}

LRESULT CALLBACK IpcSettingsWindowProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<IpcDialogState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    state = reinterpret_cast<IpcDialogState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
  }
  switch (message) {
    case WM_COMMAND:
      if (!state) break;
      switch (LOWORD(wparam)) {
        case kIpcPresetReadOnly:
          SetIpcDialogPreset(window, {"app.info", "dialog.file"});
          return 0;
        case kIpcPresetBrowse:
          SetIpcDialogPreset(
              window, {"app.info", "dialog.directory", "fs.exists", "fs.list",
                       "fs.read"});
          return 0;
        case kIpcPresetManage: {
          std::vector<std::string> all;
          for (const auto& item : kIpcCapabilityControls)
            if (std::strcmp(item.capability, "fs.delete") != 0 &&
                std::strcmp(item.capability, "window.control") != 0 &&
                std::strcmp(item.capability, "app.lifecycle") != 0 &&
                std::strcmp(item.capability, "tray") != 0)
              all.emplace_back(item.capability);
          SetIpcDialogPreset(window, all);
          return 0;
        }
        case kIpcAddRoot: {
          const auto selected = PickFolder(window, L"添加 Native IPC 固定授权目录");
          if (selected.empty()) return 0;
          std::error_code path_error;
          const auto absolute = std::filesystem::absolute(selected, path_error);
          if (path_error) {
            MessageBoxW(window, L"无法解析所选目录，请重新选择。",
                        L"Native IPC", MB_OK | MB_ICONWARNING);
            return 0;
          }
          const auto normalized = absolute.lexically_normal();
          const auto duplicate = std::find_if(
              state->roots.begin(), state->roots.end(),
              [&](const std::filesystem::path& existing) {
                return CompareStringOrdinal(existing.c_str(), -1,
                                            normalized.c_str(), -1, TRUE) ==
                       CSTR_EQUAL;
              });
          if (duplicate == state->roots.end()) {
            if (state->roots.size() >= 32) {
              MessageBoxW(window, L"固定授权目录最多只能添加 32 个。",
                          L"Native IPC", MB_OK | MB_ICONWARNING);
            } else {
              state->roots.push_back(normalized);
              RefreshIpcRootList(*state);
            }
          }
          return 0;
        }
        case kIpcRemoveRoot: {
          const auto selection = static_cast<int>(SendDlgItemMessageW(
              window, kIpcRootList, LB_GETCURSEL, 0, 0));
          if (selection != LB_ERR &&
              static_cast<std::size_t>(selection) < state->roots.size()) {
            state->roots.erase(state->roots.begin() + selection);
            RefreshIpcRootList(*state);
          }
          return 0;
        }
        case IDOK: {
          const auto capabilities = CollectIpcDialogCapabilities(window);
          if (state->owner_state && state->owner_state->ipc_enabled &&
              capabilities.empty()) {
            MessageBoxW(window, L"已启用 Native IPC，请至少选择一项授权能力。",
                        L"Native IPC", MB_OK | MB_ICONWARNING);
            return 0;
          }
          state->capabilities = capabilities;
          state->accepted = true;
          DestroyWindow(window);
          return 0;
        }
        case IDCANCEL:
          DestroyWindow(window);
          return 0;
      }
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    }
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      return DefWindowProcW(window, message, wparam, lparam);
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool ShowIpcSettings(HWND owner, State& owner_state) {
  static bool class_registered = false;
  if (!class_registered) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = IpcSettingsWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(1));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kIpcSettingsClassName;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      throw Error("Cannot register the IPC settings window");
    class_registered = true;
  }

  IpcDialogState dialog_state;
  dialog_state.owner_state = &owner_state;
  dialog_state.dpi = GetDpiForWindow(owner);
  dialog_state.capabilities = owner_state.ipc_capabilities;
  dialog_state.roots = owner_state.ipc_filesystem_roots;
  constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  RECT bounds{0, 0, Scale(712, dialog_state.dpi), Scale(672, dialog_state.dpi)};
  AdjustWindowRectExForDpi(&bounds, style, FALSE, extended_style,
                           dialog_state.dpi);
  RECT parent{};
  GetWindowRect(owner, &parent);
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  const int x = parent.left + (parent.right - parent.left - width) / 2;
  const int y = parent.top + (parent.bottom - parent.top - height) / 2;
  const auto window = CreateWindowExW(
      extended_style, kIpcSettingsClassName, L"配置 Native IPC 权限", style,
      x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr),
      &dialog_state);
  if (!window) throw Error("Cannot create the IPC settings window");

  try {
    BuildIpcSettingsInterface(dialog_state);
  } catch (...) {
    DestroyWindow(window);
    if (dialog_state.title_font) DeleteObject(dialog_state.title_font);
    if (dialog_state.body_font) DeleteObject(dialog_state.body_font);
    if (dialog_state.small_font) DeleteObject(dialog_state.small_font);
    throw;
  }
  EnableWindow(owner, FALSE);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  MSG message{};
  while (IsWindow(window)) {
    const auto result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      if (IsWindow(window)) DestroyWindow(window);
      if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  DeleteObject(dialog_state.title_font);
  DeleteObject(dialog_state.body_font);
  DeleteObject(dialog_state.small_font);
  if (dialog_state.accepted) {
    owner_state.ipc_capabilities = std::move(dialog_state.capabilities);
    owner_state.ipc_filesystem_roots = std::move(dialog_state.roots);
  }
  return dialog_state.accepted;
}

HBITMAP CreateBitmapFromWicSource(IWICImagingFactory* factory,
                                  IWICBitmapSource* source, UINT pixel_width,
                                  UINT pixel_height, const char* context) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(source, pixel_width, pixel_height,
                                WICBitmapInterpolationModeFant)) ||
      FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)))
    throw std::runtime_error(std::string("Unable to prepare ") + context);
  const UINT stride = pixel_width * 4U;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * pixel_height);
  if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                   pixels.data())))
    throw std::runtime_error(std::string("Unable to read ") + context + " pixels");
  for (std::size_t index = 0; index < pixels.size(); index += 4U) {
    const auto alpha = static_cast<unsigned int>(pixels[index + 3U]);
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      const auto value = static_cast<unsigned int>(pixels[index + channel]);
      pixels[index + channel] = static_cast<std::uint8_t>(
          (value * alpha + 255U * (255U - alpha) + 127U) / 255U);
    }
    pixels[index + 3U] = 255U;
  }
  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = static_cast<LONG>(pixel_width);
  bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(pixel_height);
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;
  void* bitmap_pixels = nullptr;
  const auto bitmap = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS,
                                       &bitmap_pixels, nullptr, 0);
  if (!bitmap || !bitmap_pixels)
    throw std::runtime_error(std::string("Unable to create ") + context + " bitmap");
  std::memcpy(bitmap_pixels, pixels.data(), pixels.size());
  return bitmap;
}

HBITMAP CreateIconPreviewBitmap(const std::filesystem::path& source, UINT pixel_size) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)))
    throw Error("Cannot decode icon preview");
  return CreateBitmapFromWicSource(factory.Get(), frame.Get(), pixel_size, pixel_size,
                                   "icon preview");
}

HBITMAP CreateSponsorQrBitmap() {
  using Microsoft::WRL::ComPtr;
  const auto module = GetModuleHandleW(nullptr);
  const auto resource = FindResourceW(module, MAKEINTRESOURCEW(kSponsorResourceId), RT_RCDATA);
  if (!resource) throw Error("Cannot find embedded sponsor image");
  const auto resource_size = SizeofResource(module, resource);
  const auto loaded = LoadResource(module, resource);
  auto* bytes = static_cast<BYTE*>(LockResource(loaded));
  if (!loaded || !bytes || resource_size == 0) throw Error("Cannot read embedded sponsor image");
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapClipper> clipper;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(bytes, resource_size)) ||
      FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                              WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(factory->CreateBitmapClipper(&clipper)))
    throw Error("Cannot decode embedded sponsor image");
  UINT width = 0;
  UINT height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
    throw Error("Embedded sponsor image has invalid dimensions");
  const auto crop_size = std::min(width * 52U / 100U, height * 38U / 100U);
  WICRect crop{};
  crop.X = static_cast<INT>((width - crop_size) / 2U);
  crop.Y = static_cast<INT>(std::min(height * 23U / 100U, height - crop_size));
  crop.Width = static_cast<INT>(crop_size);
  crop.Height = static_cast<INT>(crop_size);
  if (FAILED(clipper->Initialize(frame.Get(), &crop))) throw Error("Cannot crop sponsor image");
  return CreateBitmapFromWicSource(factory.Get(), clipper.Get(), 192, 192, "sponsor QR");
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

void DrawBitmapInRect(HDC dc, HBITMAP bitmap, RECT rect) {
  if (!bitmap) return;
  BITMAP info{};
  if (GetObjectW(bitmap, sizeof(info), &info) == 0) return;
  const auto memory_dc = CreateCompatibleDC(dc);
  if (!memory_dc) return;
  const auto old_bitmap = SelectObject(memory_dc, bitmap);
  const auto old_mode = SetStretchBltMode(dc, HALFTONE);
  StretchBlt(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
             memory_dc, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
  SetStretchBltMode(dc, old_mode);
  SelectObject(memory_dc, old_bitmap);
  DeleteDC(memory_dc);
}

void DrawIconPreview(HDC dc, const State& state) {
  const auto rect = ScaleRect(state.icon_preview_rect, state.dpi);
  DrawRoundedPanel(dc, rect, kCard, kBorder, Scale(9, state.dpi));
  auto content = rect;
  InflateRect(&content, -Scale(6, state.dpi), -Scale(6, state.dpi));
  if (state.icon_preview) DrawBitmapInRect(dc, state.icon_preview, content);
  else if (state.default_icon)
    DrawIconEx(dc, content.left, content.top, state.default_icon,
               content.right - content.left, content.bottom - content.top, 0,
               nullptr, DI_NORMAL);
  else {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMuted);
    SelectObject(dc, state.small_font);
    DrawTextW(dc, L"默认", -1, &content,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
}

void DrawSponsorQr(HDC dc, const State& state) {
  const auto rect = ScaleRect(state.sponsor_rect, state.dpi);
  DrawRoundedPanel(dc, rect, kCard, kBorder, Scale(8, state.dpi));
  auto content = rect;
  InflateRect(&content, -Scale(4, state.dpi), -Scale(4, state.dpi));
  if (state.sponsor_qr) DrawBitmapInRect(dc, state.sponsor_qr, content);
  else {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMuted);
    SelectObject(dc, state.small_font);
    DrawTextW(dc, L"二维码不可用", -1, &content,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item) {
  const bool primary = item.CtlID == kPack;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  COLORREF fill = kCard;
  COLORREF border = kBorder;
  COLORREF text = kText;
  if (primary) {
    fill = disabled ? RGB(151, 176, 242) : (pressed ? kPrimaryPressed : kPrimary);
    border = fill;
    text = RGB(255, 255, 255);
  } else if (disabled) {
    fill = RGB(245, 247, 250);
    text = kMuted;
  } else if (pressed) fill = RGB(237, 242, 250);
  const auto dpi = GetDpiForWindow(item.hwndItem);
  DrawRoundedPanel(item.hDC, item.rcItem, fill, border, Scale(primary ? 10 : 8, dpi));
  wchar_t caption[128]{};
  GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, text);
  SelectObject(item.hDC, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
  auto text_rect = item.rcItem;
  DrawTextW(item.hDC, caption, -1, &text_rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void SetStatus(HWND window, const std::wstring& text, COLORREF color = kSuccess) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return;
  state->status = text;
  state->status_color = color;
  const auto rect = ScaleRect(state->status_rect, state->dpi);
  RedrawWindow(window, &rect, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

// 打包成功后打开资源管理器并选中产物，便于用户立即运行、复制或分发。
// 文件名不能包含双引号，因此用 /select 的带引号参数不会产生额外命令片段。
void RevealOutputInExplorer(const std::filesystem::path& output) {
  const auto parameters = L"/select,\"" + std::filesystem::absolute(output).wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void OpenWebLink(HWND owner, const wchar_t* url) {
  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(owner, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32)
    MessageBoxW(owner, L"无法打开浏览器，请检查系统默认浏览器设置。",
                L"lw.Web2App", MB_OK | MB_ICONWARNING);
}

void RefreshIpcSummary(HWND window) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return;
  const bool local = IsDlgButtonChecked(window, kModeLocal) == BST_CHECKED;
  std::wstring summary;
  if (!local) {
    summary = L"在线网址模式不支持 Native IPC";
  } else if (!state->ipc_enabled) {
    summary = L"默认关闭 · 仅供受信任的本地网页使用";
  } else {
    summary = L"已授权 " + std::to_wstring(state->ipc_capabilities.size()) +
              L" 项能力 · 固定目录 " +
              std::to_wstring(state->ipc_filesystem_roots.size()) + L" 个";
  }
  SetDynamicLabelText(window, kIpcSummary, summary);
}

void RefreshIconPreview(State& state) {
  const auto icon_text = Trim(Text(state.window, kIcon));
  HBITMAP replacement = nullptr;
  std::wstring hint = L"使用内置默认图标";
  COLORREF hint_color = kMuted;
  if (!icon_text.empty()) {
    try {
      const std::filesystem::path path(icon_text);
      if (!std::filesystem::is_regular_file(path)) throw Error("Icon file not found");
      auto extension = path.extension().wstring();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
      if (extension != L".png" && extension != L".ico") throw Error("Icon must be PNG or ICO");
      replacement = CreateIconPreviewBitmap(path, static_cast<UINT>(Scale(56, state.dpi)));
      hint = L"图标有效 · 已自动适配";
      hint_color = kSuccess;
    } catch (...) {
      hint = L"图标不可用 · 请选择有效的 PNG 或 ICO 文件";
      hint_color = kFailure;
    }
  }
  if (state.icon_preview) DeleteObject(state.icon_preview);
  state.icon_preview = replacement;
  state.icon_hint_color = hint_color;
  SetDynamicLabelText(state.window, kIconHint, hint);
  EnableWindow(GetDlgItem(state.window, kDefaultIcon), !icon_text.empty());
  const auto rect = ScaleRect(state.icon_preview_rect, state.dpi);
  RedrawWindow(state.window, &rect, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

std::wstring LocalizeProgress(const std::string& message) {
  if (message == "Copying runner") return L"正在准备通用运行器…";
  if (message == "Writing PE metadata" || message == "Writing platform metadata")
    return L"正在写入图标和版本信息…";
  if (message == "Compressing static resources") return L"正在压缩网页资源…";
  if (message == "Preparing payload") return L"正在准备应用 Payload…";
  if (message == "Signing application") return L"正在执行 Authenticode 签名…";
  if (message == "Appending payload and SHA-256" ||
      message == "Appending prepared payload")
    return L"正在写入已校验的 Payload…";
  if (message == "Publishing output") return L"正在发布最终应用…";
  if (message == "Done") return L"生成完成，可以运行目标 EXE。";
  return Utf8ToWide(message);
}

void UpdateMode(HWND window) {
  const bool local = IsDlgButtonChecked(window, kModeLocal) == BST_CHECKED;
  SetDynamicLabelText(window, kSourceLabel, local ? L"网页目录" : L"在线网址");
  SetDynamicLabelText(window, kModeHint,
                      local ? L"选择 HTML / Vue / React / Vite 构建产物目录"
                            : L"输入以 http:// 或 https:// 开头的网页地址");
  SendMessageW(GetDlgItem(window, kSource), EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(local ? L"例如：D:\\project\\dist"
                                               : L"例如：https://example.com"));
  EnableWindow(GetDlgItem(window, kBrowseSource), local);
  EnableWindow(GetDlgItem(window, kEntry), local);
  EnableWindow(GetDlgItem(window, kStartPath), local);
  EnableWindow(GetDlgItem(window, kSpa), local);
  EnableWindow(GetDlgItem(window, kEntryLabel), local);
  EnableWindow(GetDlgItem(window, kStartPathLabel), local);
  EnableWindow(GetDlgItem(window, kBackendProxy), local);
  const bool proxy = local && IsDlgButtonChecked(window, kBackendProxy) == BST_CHECKED;
  EnableWindow(GetDlgItem(window, kBackendOrigin), proxy);
  EnableWindow(GetDlgItem(window, kBackendOriginLabel), proxy);
  EnableWindow(GetDlgItem(window, kIpcEnabled), local);
  EnableWindow(GetDlgItem(window, kIpcSettings), local);
  RefreshIpcSummary(window);
  InvalidateRect(window, nullptr, TRUE);
}

std::uint32_t ParseWindowDimension(HWND window, int id, const wchar_t* label) {
  const auto text = Trim(Text(window, id));
  try {
    std::size_t consumed = 0;
    const auto value = std::stoul(text, &consumed);
    if (consumed != text.size() || value == 0 || value > 16384) throw std::runtime_error("invalid");
    return static_cast<std::uint32_t>(value);
  } catch (...) {
    throw Error(WideToUtf8(std::wstring(label) + L"必须是 1 到 16384 之间的整数"));
  }
}

std::filesystem::path ValidateIcon(HWND window) {
  const auto value = Trim(Text(window, kIcon));
  if (value.empty()) return {};
  const std::filesystem::path path(value);
  if (!std::filesystem::is_regular_file(path)) throw Error("选择的图标文件不存在");
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
  if (extension != L".png" && extension != L".ico") throw Error("应用图标只支持 PNG 或 ICO 文件");
  return path;
}

void Pack(HWND window) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state || state->busy) return;
  try {
    PackOptions options;
    options.runner = CurrentExecutablePath();
    options.output = Trim(Text(window, kOutput));
    if (options.output.empty()) throw Error("请选择 EXE 输出位置");
    const auto title = Trim(Text(window, kTitle));
    if (title.empty()) throw Error("应用名称不能为空");
    options.manifest.title = WideToUtf8(title);
    options.manifest.width = ParseWindowDimension(window, kWidth, L"窗口宽度");
    options.manifest.height = ParseWindowDimension(window, kHeight, L"窗口高度");
    options.manifest.resizable = IsDlgButtonChecked(window, kResizable) == BST_CHECKED;
    options.manifest.fullscreen = IsDlgButtonChecked(window, kFullscreen) == BST_CHECKED;
    options.manifest.spa_fallback = IsDlgButtonChecked(window, kSpa) == BST_CHECKED;
    options.manifest.backend_proxy.enabled =
        IsDlgButtonChecked(window, kModeLocal) == BST_CHECKED &&
        IsDlgButtonChecked(window, kBackendProxy) == BST_CHECKED;
    if (options.manifest.backend_proxy.enabled) {
      const auto origin = Trim(Text(window, kBackendOrigin));
      if (origin.empty()) throw Error("启用后台代理后必须填写后台地址");
      options.manifest.backend_proxy.origin = WideToUtf8(origin);
    }
    options.manifest.logging.enabled = IsDlgButtonChecked(window, kLogging) == BST_CHECKED;
    options.manifest.logging.level =
        IsDlgButtonChecked(window, kDebugLogging) == BST_CHECKED ? "debug" : "info";
    const bool local_mode =
        IsDlgButtonChecked(window, kModeLocal) == BST_CHECKED;
    options.manifest.ipc.enabled = local_mode && state->ipc_enabled;
    if (options.manifest.ipc.enabled) {
      if (state->ipc_capabilities.empty())
        throw Error("启用 Native IPC 后必须至少授权一项能力");
      options.manifest.ipc.capabilities = state->ipc_capabilities;
      for (const auto& root : state->ipc_filesystem_roots)
        options.manifest.ipc.filesystem_roots.push_back(root.u8string());
    }
    auto product_name = Trim(Text(window, kProductName));
    auto file_description = Trim(Text(window, kFileDescription));
    if (product_name.empty()) product_name = title;
    if (file_description.empty()) file_description = title;
    options.metadata.product_name = std::move(product_name);
    options.metadata.file_description = std::move(file_description);
    options.metadata.company_name = Trim(Text(window, kCompanyName));
    options.metadata.version = NormalizePeVersion(Trim(Text(window, kVersionEdit)));
    options.metadata.copyright = Trim(Text(window, kCopyright));
    options.metadata.icon = ValidateIcon(window);
    SetWindowTextW(GetDlgItem(window, kVersionEdit), options.metadata.version.c_str());
    if (IsDlgButtonChecked(window, kModeUrl) == BST_CHECKED) {
      options.manifest.mode = AppMode::Url;
      options.manifest.url = WideToUtf8(Trim(Text(window, kSource)));
      if (options.manifest.url.empty()) throw Error("在线网址不能为空");
    } else {
      options.manifest.mode = AppMode::Local;
      options.source_directory = Trim(Text(window, kSource));
      if (options.source_directory.empty()) throw Error("请选择网页目录");
      options.manifest.entry = WideToUtf8(Trim(Text(window, kEntry)));
      options.manifest.start_path = WideToUtf8(Trim(Text(window, kStartPath)));
    }
    state->busy = true;
    EnableWindow(GetDlgItem(window, kPack), FALSE);
    SetWindowTextW(GetDlgItem(window, kPack), L"正在生成…");
    SetStatus(window, L"正在准备打包任务…");
    options.progress = [window](const std::string& message) {
      auto update = std::make_unique<std::wstring>(LocalizeProgress(message));
      if (PostMessageW(window, kPackProgressMessage, 0, reinterpret_cast<LPARAM>(update.get())))
        update.release();
    };
    std::thread([window, options = std::move(options)]() mutable {
      auto result = std::make_unique<PackResult>();
      result->output = options.output;
      try { PackApplication(options); result->success = true; }
      catch (const std::exception& error) { result->error = Utf8ToWide(error.what()); }
      catch (...) { result->error = L"发生未知打包错误"; }
      if (PostMessageW(window, kPackFinishedMessage, 0, reinterpret_cast<LPARAM>(result.get())))
        result.release();
    }).detach();
  } catch (const std::exception& error) {
    const auto message = Utf8ToWide(error.what());
    SetStatus(window, L"生成失败：" + message, kFailure);
    MessageBoxW(window, message.c_str(), L"生成失败", MB_OK | MB_ICONERROR);
    state->busy = false;
    EnableWindow(GetDlgItem(window, kPack), TRUE);
    SetWindowTextW(GetDlgItem(window, kPack), L"生成 Windows EXE");
    InvalidateRect(GetDlgItem(window, kPack), nullptr, TRUE);
  }
}

void BuildInterface(State& state) {
  AddLabel(state, L"lw.Web2App", 34, 12, 260, 32, 0, state.title_font);
  AddLabel(state, L"把网页构建产物快速转换为轻量 Windows 应用", 35, 45, 500, 20, 0, state.small_font);
  const auto version_text = L"Windows GUI · v" + Utf8ToWide(kVersion);
  AddLabel(state, version_text.c_str(), 890, 27, 190, 20, kHeaderVersion, state.small_font);

  AddLabel(state, L"01  网页来源", 48, 111, 200, 25, 0, state.section_font);
  AddControl(state, L"BUTTON", L"本地静态目录", WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
             48, 143, 142, 24, kModeLocal);
  AddControl(state, L"BUTTON", L"在线网址", WS_TABSTOP | BS_AUTORADIOBUTTON,
             200, 143, 104, 24, kModeUrl);
  CheckRadioButton(state.window, kModeLocal, kModeUrl, kModeLocal);
  AddLabel(state, L"网页目录", 48, 174, 90, 20, kSourceLabel, state.small_font);
  AddLabel(state, L"HTML / Vue / React / Vite 构建目录", 134, 174, 270, 20,
           kModeHint, state.small_font);
  AddEdit(state, L"", 48, 196, 344, kSource, L"例如：D:\\project\\dist");
  AddButton(state, L"选择目录", 400, 196, 106, 30, kBrowseSource);
  AddLabel(state, L"启动页（仅扫描根目录）", 48, 236, 210, 20, kEntryLabel, state.small_font);
  AddLabel(state, L"启动路径", 286, 236, 120, 20, kStartPathLabel, state.small_font);
  AddCombo(state, 48, 258, 220, kEntry);
  AddEdit(state, L"/", 286, 258, 220, kStartPath, L"例如：/login 或 /#/login");
  AddControl(state, L"BUTTON", L"启用受控后台代理", WS_TABSTOP | BS_AUTOCHECKBOX,
             48, 300, 180, 24, kBackendProxy);
  AddLabel(state, L"解决旧系统跨域与局域网访问限制", 232, 301, 274, 20, 0, state.small_font);
  AddLabel(state, L"后台地址", 48, 330, 88, 20, kBackendOriginLabel, state.small_font);
  AddEdit(state, L"", 136, 326, 370, kBackendOrigin, L"例如：http://192.0.2.10:8080");
  AddLabel(state, L"联系与支持", 176, 400, 150, 24, 0, state.section_font);
  AddLabel(state,
           L"作者：天天代码码天天\r\nQQ：819069052\r\nQQ群：758616458\r\n扫码支持项目维护",
           176, 430, 330, 76, kSponsorCaption, state.small_font);
  AddButton(state, L"GitHub 项目主页", 48, 536, 216, 30, kProjectHome);
  AddButton(state, L"查看最新版本", 274, 536, 232, 30, kLatestRelease);

  AddLabel(state, L"02  应用设置", 558, 111, 200, 25, 0, state.section_font);
  AddLabel(state, L"应用名称", 558, 148, 88, 20, 0, state.small_font);
  AddEdit(state, L"我的网页应用", 656, 143, 416, kTitle, L"窗口标题栏显示的名称");
  AddLabel(state, L"产品名称", 558, 184, 88, 20, 0, state.small_font);
  AddEdit(state, L"我的网页应用", 656, 179, 416, kProductName, L"EXE 属性中的 ProductName");
  AddLabel(state, L"文件说明", 558, 220, 88, 20, 0, state.small_font);
  AddEdit(state, L"我的网页应用", 656, 215, 416, kFileDescription, L"EXE 属性中的 FileDescription");
  AddLabel(state, L"公司名称", 558, 256, 88, 20, 0, state.small_font);
  AddEdit(state, L"天天代码码天天", 656, 251, 416, kCompanyName, L"EXE 属性中的 CompanyName");
  AddLabel(state, L"版本号", 558, 292, 88, 20, 0, state.small_font);
  AddEdit(state, L"1.0.0.0", 656, 287, 416, kVersionEdit, L"1 到 4 段数字，每段不超过 65535");
  AddLabel(state, L"版权信息", 558, 328, 88, 20, 0, state.small_font);
  AddEdit(state, L"Copyright © 天天代码码天天", 656, 323, 416, kCopyright,
          L"EXE 属性中的 LegalCopyright");
  AddLabel(state, L"应用图标", 558, 382, 88, 20, 0, state.small_font);
  AddEdit(state, L"", 734, 374, 230, kIcon, L"留空使用默认图标");
  AddButton(state, L"选择图标", 972, 374, 100, 30, kBrowseIcon);
  AddLabel(state, L"使用内置默认图标", 734, 410, 230,
           20, kIconHint, state.small_font);
  AddButton(state, L"默认图标", 972, 406, 100, 30, kDefaultIcon);
  AddLabel(state, L"窗口尺寸", 558, 454, 88, 20, 0, state.small_font);
  AddEdit(state, L"1280", 656, 447, 82, kWidth, nullptr, ES_NUMBER);
  AddLabel(state, L"×", 746, 453, 20, 20, 0, state.section_font);
  AddEdit(state, L"800", 772, 447, 82, kHeight, nullptr, ES_NUMBER);
  AddControl(state, L"BUTTON", L"默认全屏", WS_TABSTOP | BS_AUTOCHECKBOX,
             874, 450, 104, 24, kFullscreen);
  AddControl(state, L"BUTTON", L"可调整大小", WS_TABSTOP | BS_AUTOCHECKBOX,
             976, 450, 104, 24, kResizable);
  AddControl(state, L"BUTTON", L"SPA 路由回退", WS_TABSTOP | BS_AUTOCHECKBOX,
             558, 488, 130, 24, kSpa);
  AddControl(state, L"BUTTON", L"启用运行日志", WS_TABSTOP | BS_AUTOCHECKBOX,
             704, 488, 132, 24, kLogging);
  AddControl(state, L"BUTTON", L"详细日志", WS_TABSTOP | BS_AUTOCHECKBOX,
             850, 488, 106, 24, kDebugLogging);
  AddControl(state, L"BUTTON", L"启用本地交互（Native IPC）",
             WS_TABSTOP | BS_AUTOCHECKBOX, 558, 522, 276, 24, kIpcEnabled);
  AddButton(state, L"配置 IPC 权限…", 850, 520, 222, 34, kIpcSettings);
  AddLabel(state, L"默认关闭 · 仅供受信任的本地网页使用", 558, 554,
           276, 20, kIpcSummary, state.small_font);
  CheckDlgButton(state.window, kFullscreen, BST_CHECKED);
  CheckDlgButton(state.window, kResizable, BST_CHECKED);
  CheckDlgButton(state.window, kSpa, BST_CHECKED);
  CheckDlgButton(state.window, kLogging, BST_CHECKED);

  AddLabel(state, L"输出位置", 48, 613, 86, 20, 0, state.small_font);
  const auto default_output = DefaultOutputPath().wstring();
  AddEdit(state, default_output.c_str(), 134, 608, 566, kOutput,
          L"选择生成的 .exe 文件位置");
  AddButton(state, L"选择位置", 710, 608, 104, 30, kBrowseOutput);
  AddButton(state, L"生成 Windows EXE", 834, 618, 238, 50, kPack);
  EnableWindow(GetDlgItem(state.window, kBackendOrigin), FALSE);
  EnableWindow(GetDlgItem(state.window, kBackendOriginLabel), FALSE);
  RefreshIpcSummary(state.window);
  RefreshHtmlEntries(state.window);
  state.product_name_edited = false;
  state.file_description_edited = false;
}

void SyncMetadataFromTitle(State& state) {
  if (state.syncing_metadata) return;
  state.syncing_metadata = true;
  const auto title = Text(state.window, kTitle);
  if (!state.product_name_edited) SetWindowTextW(GetDlgItem(state.window, kProductName), title.c_str());
  if (!state.file_description_edited)
    SetWindowTextW(GetDlgItem(state.window, kFileDescription), title.c_str());
  state.syncing_metadata = false;
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
      state->default_icon = static_cast<HICON>(LoadImageW(
          GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
          Scale(56, state->dpi), Scale(56, state->dpi), LR_DEFAULTCOLOR));
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      BuildInterface(*state);
      try { state->sponsor_qr = CreateSponsorQrBitmap(); } catch (...) {}
      RefreshIconPreview(*state);
      return 0;
    }
    case WM_DPICHANGED:
      if (state) {
        state->dpi = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        RecreateFonts(*state);
        LayoutControls(*state);
        if (state->default_icon) DestroyIcon(state->default_icon);
        state->default_icon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            Scale(56, state->dpi), Scale(56, state->dpi), LR_DEFAULTCOLOR));
        RefreshIconPreview(*state);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      }
      return 0;
    case kPackProgressMessage: {
      std::unique_ptr<std::wstring> update(reinterpret_cast<std::wstring*>(lparam));
      if (update) SetStatus(window, *update);
      return 0;
    }
    case kPackFinishedMessage: {
      std::unique_ptr<PackResult> result(reinterpret_cast<PackResult*>(lparam));
      if (!state || !result) return 0;
      state->busy = false;
      EnableWindow(GetDlgItem(window, kPack), TRUE);
      SetWindowTextW(GetDlgItem(window, kPack), L"生成 Windows EXE");
      InvalidateRect(GetDlgItem(window, kPack), nullptr, TRUE);
      if (result->success) {
        SetStatus(window, L"生成完成，可以运行目标 EXE。");
        RevealOutputInExplorer(result->output);
        MessageBoxW(window, (L"EXE 已成功生成：\n\n" + result->output.wstring()).c_str(),
                    L"lw.Web2App", MB_OK | MB_ICONINFORMATION);
      } else {
        SetStatus(window, L"生成失败：" + result->error, kFailure);
        MessageBoxW(window, result->error.c_str(), L"生成失败", MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    case WM_COMMAND:
      if (!state) break;
      switch (LOWORD(wparam)) {
        case kModeLocal:
        case kModeUrl: UpdateMode(window); return 0;
        case kLogging:
          EnableWindow(GetDlgItem(window, kDebugLogging),
                       IsDlgButtonChecked(window, kLogging) == BST_CHECKED);
          return 0;
        case kIpcEnabled:
          state->ipc_enabled =
              IsDlgButtonChecked(window, kIpcEnabled) == BST_CHECKED;
          if (state->ipc_enabled && state->ipc_capabilities.empty())
            state->ipc_capabilities = {"app.info", "dialog.file"};
          RefreshIpcSummary(window);
          return 0;
        case kIpcSettings:
          try {
            if (ShowIpcSettings(window, *state)) RefreshIpcSummary(window);
          } catch (const std::exception& error) {
            const auto error_message = Utf8ToWide(error.what());
            MessageBoxW(window, error_message.c_str(), L"Native IPC 配置失败",
                        MB_OK | MB_ICONERROR);
          }
          return 0;
        case kBackendProxy: UpdateMode(window); return 0;
        case kBrowseSource: {
          const auto path = PickFolder(window);
          if (!path.empty()) {
            SetWindowTextW(GetDlgItem(window, kSource), path.c_str());
            RefreshHtmlEntries(window);
          }
          return 0;
        }
        case kSource:
          if (HIWORD(wparam) == EN_KILLFOCUS) RefreshHtmlEntries(window);
          return 0;
        case kEntry:
          if (HIWORD(wparam) == CBN_SELCHANGE) UpdateSuggestedStartPath(window);
          return 0;
        case kTitle:
          if (HIWORD(wparam) == EN_CHANGE) SyncMetadataFromTitle(*state);
          return 0;
        case kProductName:
          if (HIWORD(wparam) == EN_CHANGE && !state->syncing_metadata)
            state->product_name_edited = true;
          return 0;
        case kFileDescription:
          if (HIWORD(wparam) == EN_CHANGE && !state->syncing_metadata)
            state->file_description_edited = true;
          return 0;
        case kIcon:
          if (HIWORD(wparam) == EN_KILLFOCUS) RefreshIconPreview(*state);
          return 0;
        case kBrowseIcon: {
          const auto path = PickFile(window, L"选择应用图标",
              L"图标文件 (*.png;*.ico)\0*.png;*.ico\0所有文件\0*.*\0", false);
          if (!path.empty()) {
            SetWindowTextW(GetDlgItem(window, kIcon), path.c_str());
            RefreshIconPreview(*state);
          }
          return 0;
        }
        case kDefaultIcon:
          SetWindowTextW(GetDlgItem(window, kIcon), L"");
          RefreshIconPreview(*state);
          return 0;
        case kProjectHome: OpenWebLink(window, kProjectUrl); return 0;
        case kLatestRelease: OpenWebLink(window, kReleasesUrl); return 0;
        case kBrowseOutput: {
          const auto path = PickFile(window, L"选择 EXE 输出位置",
                                     L"Windows 可执行文件 (*.exe)\0*.exe\0", true);
          if (!path.empty()) SetWindowTextW(GetDlgItem(window, kOutput), path.c_str());
          return 0;
        }
        case kPack: Pack(window); return 0;
      }
      break;
    case WM_DRAWITEM:
      DrawOwnerButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
      return TRUE;
    case WM_CTLCOLORSTATIC: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
      SetBkMode(dc, TRANSPARENT);
      COLORREF color = kText;
      if (id == kModeHint || id == kSponsorCaption || id == kHeaderVersion ||
          id == kIpcSummary)
        color = kMuted;
      if (state && id == kIconHint) color = state->icon_hint_color;
      SetTextColor(dc, color);
      if (state && id == kIconHint) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, kCard);
        return reinterpret_cast<INT_PTR>(state->card_brush);
      }
      return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLORBTN: {
      const auto dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }
    case WM_ERASEBKGND: return TRUE;
    case WM_CLOSE:
      if (state && state->busy) {
        MessageBoxW(window, L"正在生成应用，请等待任务完成后再关闭窗口。",
                    L"lw.Web2App", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const auto dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      FillRect(dc, &client, state ? state->background_brush
                                  : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
      const auto dpi = state ? state->dpi : 96;
      RECT header{0, 0, client.right, Scale(72, dpi)};
      const auto header_brush = CreateSolidBrush(kHeader);
      FillRect(dc, &header, header_brush);
      DeleteObject(header_brush);
      if (state) {
        DrawRoundedPanel(dc, ScaleRect(state->left_card, dpi), kCard, kBorder, Scale(16, dpi));
        DrawRoundedPanel(dc, ScaleRect(state->right_card, dpi), kCard, kBorder, Scale(16, dpi));
        DrawRoundedPanel(dc, ScaleRect(state->output_card, dpi), kCard, kBorder, Scale(14, dpi));
        const auto divider = ScaleRect(state->sponsor_divider, dpi);
        const auto divider_pen = CreatePen(PS_SOLID, 1, kBorder);
        const auto old_pen = SelectObject(dc, divider_pen);
        MoveToEx(dc, divider.left, divider.top, nullptr);
        LineTo(dc, divider.right, divider.bottom);
        SelectObject(dc, old_pen);
        DeleteObject(divider_pen);
        DrawSponsorQr(dc, *state);
        DrawIconPreview(dc, *state);
        const auto scaled_status = ScaleRect(state->status_rect, dpi);
        FillRect(dc, &scaled_status, state->background_brush);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, kBackground);
        SetTextColor(dc, state->status_color);
        SelectObject(dc, state->small_font);
        auto status_rect = scaled_status;
        DrawTextW(dc, state->status.c_str(), -1, &status_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
        if (state->icon_preview) DeleteObject(state->icon_preview);
        if (state->sponsor_qr) DeleteObject(state->sponsor_qr);
        if (state->default_icon) DestroyIcon(state->default_icon);
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
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
  window_class.hIconSm = static_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = kClassName;
  if (!RegisterClassExW(&window_class)) throw Error("Cannot register the packer window");
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  const auto dpi = GetDpiForSystem();
  RECT bounds{0, 0, Scale(kClientWidth, dpi), Scale(kClientHeight, dpi)};
  AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_CONTROLPARENT, dpi);
  const auto width = bounds.right - bounds.left;
  const auto height = bounds.bottom - bounds.top;
  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const auto x = work_area.left + std::max(0L, (work_area.right - work_area.left - width) / 2L);
  const auto y = work_area.top + std::max(0L, (work_area.bottom - work_area.top - height) / 2L);
  const auto window = CreateWindowExW(
      WS_EX_CONTROLPARENT, kClassName, L"lw.Web2App · 网页转 Windows 应用",
      style, x, y, width, height, nullptr, nullptr, instance, nullptr);
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
