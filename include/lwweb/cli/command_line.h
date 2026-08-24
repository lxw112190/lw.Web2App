#pragma once

#include "lwweb/packer/packer.h"
#include "lwweb/publish/publisher.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace lwweb {

// CLI 的平台差异只影响帮助文字和产物提示；参数解析与 PackOptions 构建完全共享。
enum class CliPlatform { Windows, Linux };

enum class CliAction { Help, Inspect, Pack, Publish };

// 统一解析 Windows/Linux 入口传入的 UTF-8 参数，避免两套 CLI 随功能演进漂移。
struct CliCommand {
  CliAction action = CliAction::Help;
  std::filesystem::path inspect_target;
  PackOptions pack;
  PublishOptions publish;
};

CliCommand ParseCommandLine(const std::vector<std::string>& args,
                            const std::filesystem::path& runner);
std::string CommandLineHelp(CliPlatform platform);
int RunCommandLine(const std::vector<std::string>& args,
                   const std::filesystem::path& runner, CliPlatform platform,
                   std::ostream& output);

}  // namespace lwweb
