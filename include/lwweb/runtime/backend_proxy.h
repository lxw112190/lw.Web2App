#pragma once

#include "lwweb/common/http_origin.h"
#include "lwweb/packer/manifest.h"

#include <cstdint>
#include <string>

namespace httplib {
struct Request;
struct Response;
}  // namespace httplib

namespace lwweb {

class Logger;

// 将固定前缀下的同源浏览器请求转发到 Manifest 指定的唯一 HTTP Origin。
// 该类负责 Host/来源校验、Header 过滤、Cookie/重定向改写与大小、超时限制。
class BackendProxy {
 public:
  BackendProxy(BackendProxyConfig config, const Logger* logger = nullptr);

  bool Matches(const std::string& path) const;
  void Handle(const httplib::Request& request, httplib::Response& response,
              std::uint16_t local_port) const;

 private:
  BackendProxyConfig config_;
  HttpOrigin origin_;
  const Logger* logger_ = nullptr;
};

}  // namespace lwweb
