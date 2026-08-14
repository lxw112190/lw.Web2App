#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lwweb {

// 已完成语法校验的 HTTP(S) Origin；不包含路径、查询参数和用户信息。
struct HttpOrigin {
  std::string scheme;
  std::string host;
  std::uint16_t port = 0;
  bool explicit_port = false;

  std::string Authority() const;
  std::string ToString() const;
};

std::optional<HttpOrigin> ParseHttpOrigin(const std::string& value);

}  // namespace lwweb
