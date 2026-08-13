#pragma once

#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/runtime/resource_cache.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace httplib {
class Server;
}

namespace lwweb {

// 为嵌入 EXE 的 ZIP 建立中央目录索引，并按资源路径独立解压。
// 构造阶段会执行条目数量、路径和解压尺寸等安全检查。
class ZipResourceStore {
 public:
  ZipResourceStore(const LoadedPayload& payload, SecurityLimits limits = {});
  ~ZipResourceStore();
  ZipResourceStore(const ZipResourceStore&) = delete;
  ZipResourceStore& operator=(const ZipResourceStore&) = delete;

  bool Exists(const std::string& path) const;
  std::vector<std::uint8_t> Read(const std::string& path);

 private:
  // 隐藏 miniz、文件流和同步状态，避免第三方类型泄漏到公开头文件。
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 仅监听 127.0.0.1 随机端口的静态资源 HTTP 服务。
// 服务严格校验 Host，并为不存在的路径按配置提供 SPA fallback。
class ResourceServer {
 public:
  ResourceServer(const LoadedPayload& payload, SecurityLimits limits = {});
  ~ResourceServer();
  ResourceServer(const ResourceServer&) = delete;
  ResourceServer& operator=(const ResourceServer&) = delete;

  std::string Start();
  void Stop();

 private:
  LoadedPayload payload_;
  SecurityLimits limits_;
  std::unique_ptr<ZipResourceStore> store_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  int port_ = 0;
};

}  // namespace lwweb
