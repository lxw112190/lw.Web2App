#pragma once

#include "lwweb/publish/release_info.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace lwweb {

enum class PublishPlatform { Windows, Linux };

// publish 命令的运行时参数。配置文件内部的相对路径以配置文件为基准，
// 而 output_override 是显式 CLI 参数，按当前工作目录解析。
struct PublishOptions {
  std::filesystem::path runner;
  std::filesystem::path config_file = "lwweb.json";
  std::optional<std::filesystem::path> output_override;
  PublishPlatform platform = PublishPlatform::Windows;
  std::function<void(const std::string&)> progress;
};

// 成功发布后的目录和清单。directory 只有在所有产物校验完成并原子发布后
// 才会返回，因此调用方不会看到构建中的临时目录。
struct PublishResult {
  std::filesystem::path directory;
  ReleaseInfo release;
};

PublishResult PublishProject(const PublishOptions& options);

}  // namespace lwweb
