#pragma once

#include "lwweb/packer/manifest.h"
#include "lwweb/packer/payload.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace lwweb {

// 打包和读取 ZIP 时使用的防御性资源上限。
// 这些限制用于避免异常大文件、超量条目及 ZIP 解压炸弹耗尽系统资源。
struct SecurityLimits {
  std::uint64_t max_file_size = 512ull * 1024 * 1024;
  std::uint64_t max_total_size = 2ull * 1024 * 1024 * 1024;
  std::uint32_t max_file_count = 100000;
};

// 写入目标 EXE 版本资源和图标资源的 Windows PE 元数据。
// version 使用最多四段、每段不超过 65535 的数字版本格式。
struct PeMetadata {
  std::wstring product_name;
  std::wstring company_name;
  std::wstring file_description;
  std::wstring version = L"1.0.0.0";
  std::wstring copyright;
  std::filesystem::path icon;
};

// 一次打包任务所需的全部输入、输出和进度回调。
// runner 可以是原始打包器，也可以是已经携带 Payload 的生成程序。
struct PackOptions {
  std::filesystem::path runner;
  std::filesystem::path source_directory;
  std::filesystem::path output;
  Manifest manifest;
  PeMetadata metadata;
  SecurityLimits limits;
  std::function<void(const std::string&)> progress;
};

PreparedPayload PreparePayload(const PackOptions& options,
                               Manifest manifest);
void PackApplication(const PackOptions& options);

}  // namespace lwweb
