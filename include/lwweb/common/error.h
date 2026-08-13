#pragma once

#include <stdexcept>
#include <string>

namespace lwweb {

// lw.Web2App 的统一业务异常类型。
// 核心库抛出的可预期错误均使用该类型，调用方可直接向 GUI 或 CLI 展示 what()。
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace lwweb
