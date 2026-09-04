#pragma once

#include <filesystem>
#include <string>

namespace lwweb {

// 解析运行时允许暴露给网页的系统目录。返回路径只用于定位，
// 不会自动授予 fs.* 访问权限；文件操作仍必须通过 Manifest 根目录或 Session Grant。
class SystemPaths {
 public:
  static std::filesystem::path Resolve(const std::string& name,
                                       const std::string& app_id);
};

}  // namespace lwweb
