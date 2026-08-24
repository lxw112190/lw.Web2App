#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace lwweb {

class Logger;

// 仅在当前 Runtime 进程中有效的本地文件授权。path 只供 Native HTTP
// 数据面使用，绝不能序列化到网页、Manifest 或常规日志。
struct LocalFileGrant {
  std::string id;
  std::string name;
  std::uint64_t size = 0;
  std::string mime;
  std::string url;
  std::filesystem::path path;
};

// 维护“随机授权 ID -> 规范化本地文件”的会话映射。授权不落盘，进程退出
// 后自动销毁；每次 HTTP 查找都会确认文件身份信息仍与授权创建时一致。
class LocalFileGrantManager {
 public:
  explicit LocalFileGrantManager(const Logger* logger = nullptr);

  LocalFileGrant Create(const std::filesystem::path& selected);
  std::optional<LocalFileGrant> Find(const std::string& id) const;
  bool Revoke(const std::string& id);
  std::size_t Count() const;

 private:
  const Logger* logger_ = nullptr;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, LocalFileGrant> grants_;
};

}  // namespace lwweb
