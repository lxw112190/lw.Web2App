#include "lwweb/runtime/backend_proxy.h"

#include "lwweb/common/error.h"
#include "lwweb/common/logging.h"
#include "lwweb/runtime/resource_server.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string Trim(std::string value) {
  const auto whitespace = [](unsigned char character) { return std::isspace(character); };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), whitespace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(),
              value.end());
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin(),
                    [](unsigned char left, unsigned char right) {
                      return std::tolower(left) == std::tolower(right);
                    });
}

std::unordered_set<std::string> ConnectionTokens(const httplib::Headers& headers) {
  std::unordered_set<std::string> result;
  const auto range = headers.equal_range("Connection");
  for (auto item = range.first; item != range.second; ++item) {
    std::stringstream stream(item->second);
    std::string token;
    while (std::getline(stream, token, ',')) result.insert(Lower(Trim(token)));
  }
  return result;
}

bool IsHopByHopHeader(const std::string& name,
                      const std::unordered_set<std::string>& connection_tokens) {
  const auto lower = Lower(name);
  static const std::unordered_set<std::string> blocked = {
      "connection",          "keep-alive",       "proxy-authenticate",
      "proxy-authorization", "te",               "trailer",
      "transfer-encoding",   "upgrade"};
  return blocked.count(lower) != 0 || connection_tokens.count(lower) != 0;
}

bool IsAllowedMethod(const std::string& method) {
  static const std::unordered_set<std::string> allowed = {
      "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
  return allowed.count(method) != 0;
}

bool IsTrustedPageRequest(const httplib::Request& request,
                          const std::string& local_origin) {
  const auto fetch_site = Lower(request.get_header_value("Sec-Fetch-Site"));
  if (!fetch_site.empty() && fetch_site != "same-origin") return false;

  auto origin = request.get_header_value("Origin");
  while (origin.size() > 1 && origin.back() == '/') origin.pop_back();
  if (!origin.empty() && origin != local_origin) return false;

  const auto referer = request.get_header_value("Referer");
  if (!referer.empty() && referer != local_origin &&
      !StartsWith(referer, local_origin + "/"))
    return false;
  return true;
}

std::string ProxyTarget(const httplib::Request& request,
                        const BackendProxyConfig& config) {
  auto path = request.path.substr(config.prefix.size());
  if (path.empty()) path = "/";
  if (path.front() != '/') path.insert(path.begin(), '/');
  const auto query = request.target.find('?');
  if (query != std::string::npos) path += request.target.substr(query);
  return path;
}

std::string RewriteCookie(const std::string& input, const std::string& prefix) {
  std::stringstream stream(input);
  std::string part;
  std::vector<std::string> parts;
  bool path_written = false;
  while (std::getline(stream, part, ';')) {
    part = Trim(part);
    if (part.empty()) continue;
    const auto equals = part.find('=');
    const auto name = Lower(Trim(part.substr(0, equals)));
    if (name == "domain") continue;
    if (name == "path") {
      auto path = equals == std::string::npos ? "/" : Trim(part.substr(equals + 1));
      if (path.empty() || path.front() != '/') path = "/";
      part = "Path=" + prefix + (path == "/" ? "" : path);
      path_written = true;
    }
    parts.push_back(std::move(part));
  }
  if (!path_written) parts.push_back("Path=" + prefix);
  std::string result;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index) result += "; ";
    result += parts[index];
  }
  return result;
}

std::optional<std::string> RewriteLocation(const std::string& location,
                                           const std::string& backend_origin,
                                           const std::string& prefix) {
  if (location.empty()) return std::string{};
  if (location.front() == '/' && (location.size() == 1 || location[1] != '/'))
    return prefix + location;
  if (location == backend_origin) return prefix + "/";
  const auto origin_with_slash = backend_origin + "/";
  if (StartsWithCaseInsensitive(location, origin_with_slash))
    return prefix + location.substr(backend_origin.size());
  return std::nullopt;
}

void SetProxyError(httplib::Response& response, int status, const std::string& message) {
  response.headers.clear();
  response.body.clear();
  response.reason.clear();
  response.status = status;
  response.set_content(message, "text/plain; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.set_header("X-Content-Type-Options", "nosniff");
}

}  // namespace

BackendProxy::BackendProxy(BackendProxyConfig config, const Logger* logger)
    : config_(std::move(config)), logger_(logger) {
  const auto parsed = ParseHttpOrigin(config_.origin);
  if (!parsed || parsed->scheme != "http")
    throw Error("Backend proxy currently requires a valid http:// origin");
  origin_ = *parsed;
}

bool BackendProxy::Matches(const std::string& path) const {
  return path == config_.prefix || StartsWith(path, config_.prefix + "/");
}

void BackendProxy::Handle(const httplib::Request& request,
                          httplib::Response& response,
                          std::uint16_t local_port) const {
  const auto started = std::chrono::steady_clock::now();
  const auto host = request.get_header_value("Host");
  if (!IsExpectedResourceHost(host, local_port)) {
    SetProxyError(response, 403, "Invalid local application host");
    return;
  }
  const auto local_origin = "http://" + host;
  if (!IsTrustedPageRequest(request, local_origin)) {
    if (logger_) logger_->Warn("Blocked cross-site backend proxy request");
    SetProxyError(response, 403, "Cross-site backend proxy request rejected");
    return;
  }
  if (!IsAllowedMethod(request.method)) {
    SetProxyError(response, 405, "Unsupported backend proxy method");
    response.set_header("Allow", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
    return;
  }
  if (request.body.size() > config_.max_request_size) {
    SetProxyError(response, 413, "Backend proxy request is too large");
    return;
  }

  httplib::Request outbound;
  outbound.method = request.method;
  outbound.path = ProxyTarget(request, config_);
  outbound.body = request.body;
  const auto connection_tokens = ConnectionTokens(request.headers);
  for (const auto& header : request.headers) {
    const auto lower = Lower(header.first);
    if (IsHopByHopHeader(header.first, connection_tokens) || lower == "host" ||
        lower == "content-length" || lower == "origin" || lower == "referer" ||
        lower.rfind("sec-fetch-", 0) == 0)
      continue;
    outbound.headers.emplace(header.first, header.second);
  }
  if (!outbound.body.empty())
    outbound.headers.emplace("Content-Length", std::to_string(outbound.body.size()));
  if (request.has_header("Origin")) outbound.headers.emplace("Origin", origin_.ToString());
  if (request.has_header("Referer")) outbound.headers.emplace("Referer", origin_.ToString() + "/");

  std::string body;
  bool response_too_large = false;
  outbound.response_handler = [&](const httplib::Response& upstream) {
    const auto content_length = upstream.get_header_value_u64("Content-Length", 0);
    if (content_length > config_.max_response_size) {
      response_too_large = true;
      return false;
    }
    return true;
  };
  outbound.content_receiver = [&](const char* data, std::size_t length,
                                  std::size_t, std::size_t) {
    if (length > config_.max_response_size - body.size()) {
      response_too_large = true;
      return false;
    }
    body.append(data, length);
    return true;
  };

  httplib::Client client(origin_.ToString());
  client.set_connection_timeout(std::chrono::milliseconds(config_.connect_timeout_ms));
  client.set_read_timeout(std::chrono::milliseconds(config_.read_timeout_ms));
  client.set_write_timeout(std::chrono::milliseconds(config_.read_timeout_ms));
  client.set_max_timeout(std::chrono::milliseconds(config_.read_timeout_ms));
  client.set_follow_location(false);
  client.set_keep_alive(false);
  client.set_path_encode(false);
  const auto upstream = client.send(outbound);
  if (!upstream) {
    if (logger_)
      logger_->Warn("Backend proxy request failed: " + request.method + " " +
                    request.path + " (" + httplib::to_string(upstream.error()) + ")");
    SetProxyError(response, 502,
                  response_too_large ? "Backend proxy response is too large"
                                     : "Backend is unavailable");
    return;
  }

  response.status = upstream->status;
  response.reason = upstream->reason;
  response.body = std::move(body);
  const auto upstream_connection_tokens = ConnectionTokens(upstream->headers);
  for (const auto& header : upstream->headers) {
    const auto lower = Lower(header.first);
    if (IsHopByHopHeader(header.first, upstream_connection_tokens) ||
        lower == "content-length" || lower == "location")
      continue;
    if (lower == "set-cookie") {
      response.headers.emplace(header.first, RewriteCookie(header.second, config_.prefix));
    } else if (lower.rfind("access-control-", 0) != 0) {
      response.headers.emplace(header.first, header.second);
    }
  }
  if (upstream->has_header("Location")) {
    const auto rewritten = RewriteLocation(upstream->get_header_value("Location"),
                                           origin_.ToString(), config_.prefix);
    if (!rewritten) {
      if (logger_) logger_->Warn("Backend proxy rejected a cross-origin redirect");
      SetProxyError(response, 502, "Backend redirect target is not allowed");
      return;
    }
    response.set_header("Location", *rewritten);
  }
  response.set_header("X-Content-Type-Options", "nosniff");

  if (logger_ && logger_->DebugEnabled()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    logger_->Debug("Proxy " + request.method + " " + request.path + " -> " +
                   std::to_string(response.status) + " in " + std::to_string(elapsed) +
                   " ms");
  }
}

}  // namespace lwweb
