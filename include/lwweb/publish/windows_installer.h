#pragma once

#include <filesystem>
#include <string>

namespace lwweb {

// 生成 Inno Setup 安装器所需的全部显式输入。application 必须是已经完成
// Payload Binding 和 Authenticode（如启用）的最终 Portable EXE。
struct WindowsInstallerBuildOptions {
  std::filesystem::path application;
  std::filesystem::path output_directory;
  std::filesystem::path configured_iscc;
  std::string output_basename;
  std::string app_id;
  std::string app_name;
  std::string app_version;
  std::string publisher;
  bool desktop_shortcut = true;
  bool start_menu = true;
};

// 按“显式配置、PATH、常见 Inno Setup 安装目录”的顺序查找 ISCC.exe。
// 显式配置无效时直接报错，不会退回另一个编译器掩盖配置错误。
std::filesystem::path FindInnoSetupCompiler(
    const std::filesystem::path& configured = {});

// 生成临时 UTF-8 .iss、调用 ISCC.exe，并返回已经验证为 PE 的 Setup EXE。
// 临时脚本无论成功失败都会删除，产物仍位于 Publisher 的暂存目录中。
std::filesystem::path BuildWindowsInstaller(
    const WindowsInstallerBuildOptions& options);

}  // namespace lwweb
