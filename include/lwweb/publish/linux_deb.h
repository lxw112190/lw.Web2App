#pragma once

#include <filesystem>
#include <string>

namespace lwweb {

// 生成应用专属 DEB 所需的稳定身份和文件输入。application 必须是已经写入
// Payload 的最终 ELF；icon 为空时使用内置默认 SVG，当前支持 PNG 或 SVG。
struct LinuxDebBuildOptions {
  std::filesystem::path application;
  std::filesystem::path icon;
  std::filesystem::path output_directory;
  std::string app_id;
  std::string app_name;
  std::string app_version;
  std::string publisher;
  std::string description;
};

// DEB 构建结果同时返回稳定包名和实际产物路径，供 Publisher 写入发布清单。
struct LinuxDebBuildResult {
  std::filesystem::path package;
  std::string package_name;
  std::string executable_name;
  std::string dependencies;
};

// 使用 dpkg-shlibdeps 从最终 ELF 推导依赖，再由 dpkg-deb 构建 amd64 包。
// 任何工具失败或产物校验失败都会抛出 Error，不会留下看似完整的 DEB。
LinuxDebBuildResult BuildLinuxDeb(const LinuxDebBuildOptions& options);

}  // namespace lwweb
