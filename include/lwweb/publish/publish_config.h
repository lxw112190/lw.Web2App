#pragma once

#include "lwweb/packer/packer.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace lwweb {

// 项目在发布目录、Release 元数据和安装器中使用的稳定身份信息。
// version 第一版沿用 Windows PE 的一至四段纯数字版本规则。
struct ProjectAppConfig {
  std::string id;
  std::string name;
  std::string version;
  std::string company;
  std::string description;
  std::string copyright;
  std::filesystem::path icon;
};

// Windows Installer 的声明式开关。具体 Inno Setup 构建在后续阶段实现，
// 但路径从一开始就按 lwweb.json 所在目录解析，避免 CI 与 IDE 行为漂移。
struct WindowsInstallerConfig {
  bool enabled = false;
  bool desktop_shortcut = true;
  bool start_menu = true;
  std::filesystem::path iscc;
};

// Windows 发布目标。Portable 与 ZIP 保持独立，签名设置只引用证书存储区
// 指纹，不保存 PFX 或任何密码。
struct WindowsPublishConfig {
  bool portable = true;
  bool zip = true;
  WindowsInstallerConfig installer;
  SigningConfig signing;
};

// Ubuntu 发布目标。首个 publish 阶段先消费 tar_gz，DEB 由后续独立阶段实现。
struct LinuxPublishConfig {
  bool tar_gz = true;
  bool deb = false;
};

// lwweb.json 的 publish 节；output 是已经相对配置文件目录解析的绝对路径。
struct PublishConfig {
  std::filesystem::path output;
  WindowsPublishConfig windows;
  LinuxPublishConfig linux;
};

// 完整项目配置。pack 是可直接交给后续 Publisher 的打包模板，runner 和
// output 由运行 publish 命令时补充；它不是写入 Runtime 的 Manifest JSON。
struct ProjectConfig {
  std::uint32_t schema = 1;
  std::filesystem::path config_file;
  ProjectAppConfig app;
  PackOptions pack;
  PublishConfig publish;
};

// 读取并严格校验 lwweb.json。未知字段会被拒绝，所有相对路径都以配置文件
// 所在目录为基准，任何 password/token/secret/PFX 字段都会明确失败。
ProjectConfig LoadProjectConfig(const std::filesystem::path& config_file);

}  // namespace lwweb
