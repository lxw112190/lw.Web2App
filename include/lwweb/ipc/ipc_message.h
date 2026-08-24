#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace lwweb {

constexpr std::size_t kMaxIpcMessageSize = 1024 * 1024;
constexpr std::size_t kMaxIpcIdentifierSize = 128;

struct IpcRequest {
  std::string id;
  std::string method;
  nlohmann::json params = nlohmann::json::object();
};

struct IpcError {
  std::string code;
  std::string message;
};

struct IpcResponse {
  std::string id;
  bool ok = false;
  nlohmann::json result;
  IpcError error;
};

// 带稳定错误码的 IPC 异常；message 可展示给调用页面，但不得包含敏感参数。
class IpcException : public std::runtime_error {
 public:
  IpcException(std::string code, std::string message);
  const std::string& Code() const noexcept { return code_; }

 private:
  std::string code_;
};

IpcRequest ParseIpcRequest(const std::string& text);
nlohmann::json IpcResponseToJson(const IpcResponse& response);
std::string SerializeIpcResponse(const IpcResponse& response);
IpcResponse MakeIpcError(std::string id, std::string code, std::string message);

}  // namespace lwweb
