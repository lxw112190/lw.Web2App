#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace lwweb {

// 以隐藏窗口运行一个受信任的 Windows 外部工具。参数使用 Windows 标准规则
// 单独转义；stdout/stderr 会被限量收集，并在失败或超时时附加到异常信息。
// 超时后会终止子进程，避免打包、签名或发布流程永久等待。
void RunWindowsExternalTool(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    std::chrono::milliseconds timeout, const std::string& operation);

}  // namespace lwweb
