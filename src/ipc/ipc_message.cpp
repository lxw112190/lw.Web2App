#include "lwweb/ipc/ipc_message.h"

namespace lwweb {

IpcException::IpcException(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

IpcRequest ParseIpcRequest(const std::string& text) {
  if (text.size() > kMaxIpcMessageSize)
    throw IpcException("INVALID_REQUEST", "IPC message exceeds the 1 MiB limit");
  try {
    const auto value = nlohmann::json::parse(text);
    if (!value.is_object() || value.value("v", 0) != 1 ||
        value.value("kind", "") != "request")
      throw IpcException("INVALID_REQUEST", "Invalid IPC protocol envelope");
    if (!value.contains("id") || !value["id"].is_string() ||
        !value.contains("method") || !value["method"].is_string())
      throw IpcException("INVALID_REQUEST", "IPC id and method must be strings");
    IpcRequest request;
    request.id = value["id"].get<std::string>();
    request.method = value["method"].get<std::string>();
    request.params = value.value("params", nlohmann::json::object());
    if (request.id.empty() || request.id.size() > kMaxIpcIdentifierSize ||
        request.method.empty() || request.method.size() > kMaxIpcIdentifierSize)
      throw IpcException("INVALID_REQUEST", "IPC id or method is outside the size limit");
    if (!request.params.is_object() && !request.params.is_null())
      throw IpcException("INVALID_REQUEST", "IPC params must be an object or null");
    if (request.params.is_null()) request.params = nlohmann::json::object();
    return request;
  } catch (const IpcException&) {
    throw;
  } catch (const std::exception&) {
    throw IpcException("INVALID_REQUEST", "Malformed IPC JSON request");
  }
}

nlohmann::json IpcResponseToJson(const IpcResponse& response) {
  nlohmann::json value = {{"v", 1}, {"kind", "response"},
                          {"id", response.id}, {"ok", response.ok}};
  if (response.ok)
    value["result"] = response.result;
  else
    value["error"] = {{"code", response.error.code},
                      {"message", response.error.message}};
  return value;
}

std::string SerializeIpcResponse(const IpcResponse& response) {
  return IpcResponseToJson(response).dump();
}

IpcResponse MakeIpcError(std::string id, std::string code, std::string message) {
  IpcResponse response;
  response.id = std::move(id);
  response.error = {std::move(code), std::move(message)};
  return response;
}

}  // namespace lwweb
