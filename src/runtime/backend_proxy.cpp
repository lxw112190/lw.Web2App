#include "lwweb/runtime/backend_proxy.h"

#include "lwweb/common/error.h"
#include "lwweb/common/logging.h"
#include "lwweb/runtime/proxy_stream.h"
#include "lwweb/runtime/resource_server.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
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

bool IsDownloadResponse(const httplib::Headers& headers) {
  const auto range = headers.equal_range("Content-Disposition");
  for (auto item = range.first; item != range.second; ++item) {
    if (Lower(item->second).find("attachment") != std::string::npos) return true;
  }
  return false;
}

struct UpstreamResponse {
  int status = -1;
  std::string reason;
  httplib::Headers headers;
  std::string body;
  std::string error;
  bool headers_ready = false;
  bool finished = false;
  bool success = false;
  bool download = false;
  bool response_too_large = false;
  std::mutex mutex;
  std::condition_variable changed;
};

struct UpstreamSnapshot {
  int status = -1;
  std::string reason;
  httplib::Headers headers;
  bool download = false;
};

UpstreamSnapshot SnapshotHeaders(UpstreamResponse& upstream) {
  std::lock_guard lock(upstream.mutex);
  return {upstream.status, upstream.reason, upstream.headers, upstream.download};
}

bool ApplyUpstreamHeaders(const UpstreamSnapshot& upstream,
                          const BackendProxyConfig& config,
                          const HttpOrigin& origin,
                          httplib::Response& response,
                          bool streaming) {
  response.status = upstream.status;
  response.reason = upstream.reason;
  const auto connection_tokens = ConnectionTokens(upstream.headers);
  for (const auto& header : upstream.headers) {
    const auto lower = Lower(header.first);
    if (IsHopByHopHeader(header.first, connection_tokens) || lower == "location" ||
        (!streaming && lower == "content-length") ||
        (streaming && lower == "content-type"))
      continue;
    if (lower == "set-cookie") {
      response.headers.emplace(header.first, RewriteCookie(header.second, config.prefix));
    } else if (lower.rfind("access-control-", 0) != 0) {
      response.headers.emplace(header.first, header.second);
    }
  }
  const auto location = upstream.headers.find("Location");
  if (location != upstream.headers.end()) {
    const auto rewritten = RewriteLocation(location->second, origin.ToString(), config.prefix);
    if (!rewritten) return false;
    response.set_header("Location", *rewritten);
  }
  response.set_header("X-Content-Type-Options", "nosniff");
  return true;
}

std::string HeaderValue(const httplib::Headers& headers, const std::string& name,
                        const std::string& fallback = {}) {
  const auto item = headers.find(name);
  return item == headers.end() ? fallback : item->second;
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

  // Range 已原样放入 outbound.headers，由真实后台决定 206/Content-Range。
  // cpp-httplib 还会基于解析后的 ranges 再裁剪一次本地响应，因此必须清除它。
  if (request.has_header("Range"))
    const_cast<httplib::Request&>(request).ranges.clear();

  const auto config = config_;
  const auto origin = origin_;
  const auto* logger = logger_;
  const auto method = request.method;
  auto state = std::make_shared<UpstreamResponse>();
  auto stream = std::make_shared<ProxyStream>();
  auto worker = std::make_shared<std::thread>(
      [state, stream, config, origin, outbound = std::move(outbound)]() mutable {
        bool download = false;
        outbound.response_handler = [&](const httplib::Response& upstream) {
          download = IsDownloadResponse(upstream.headers) && outbound.method != "HEAD";
          const auto content_length =
              upstream.get_header_value_u64("Content-Length", 0);
          const bool too_large = !download && content_length > config.max_response_size;
          {
            std::lock_guard lock(state->mutex);
            state->status = upstream.status;
            state->reason = upstream.reason;
            state->headers = upstream.headers;
            state->download = download;
            state->response_too_large = too_large;
            state->headers_ready = true;
          }
          state->changed.notify_all();
          return !too_large;
        };
        outbound.content_receiver = [&](const char* data, std::size_t length,
                                        std::size_t, std::size_t) {
          if (download) return stream->Push(data, length);
          if (length > config.max_response_size - state->body.size()) {
            state->response_too_large = true;
            return false;
          }
          state->body.append(data, length);
          return true;
        };

        httplib::Client client(origin.ToString());
        client.set_connection_timeout(
            std::chrono::milliseconds(config.connect_timeout_ms));
        client.set_read_timeout(std::chrono::milliseconds(config.read_timeout_ms));
        client.set_write_timeout(std::chrono::milliseconds(config.read_timeout_ms));
        client.set_follow_location(false);
        client.set_keep_alive(false);
        client.set_path_encode(false);
        const auto result = client.send(outbound);
        if (download) {
          if (result)
            stream->Finish();
          else
            stream->Fail();
        }
        {
          std::lock_guard lock(state->mutex);
          state->success = static_cast<bool>(result);
          if (!result) state->error = httplib::to_string(result.error());
          state->finished = true;
        }
        state->changed.notify_all();
      });

  {
    std::unique_lock lock(state->mutex);
    state->changed.wait(lock, [&] { return state->headers_ready || state->finished; });
  }

  auto snapshot = SnapshotHeaders(*state);
  if (snapshot.status == -1) {
    if (worker->joinable()) worker->join();
    if (logger) logger->Warn("Backend proxy request failed before response headers");
    SetProxyError(response, 502, "Backend is unavailable");
    return;
  }

  if (!snapshot.download) {
    {
      std::unique_lock lock(state->mutex);
      state->changed.wait(lock, [&] { return state->finished; });
    }
    if (worker->joinable()) worker->join();
    if (!state->success) {
      if (logger)
        logger->Warn("Backend proxy request failed: " + method + " (" + state->error + ")");
      SetProxyError(response, 502,
                    state->response_too_large ? "Backend proxy response is too large"
                                              : "Backend is unavailable");
      return;
    }
    if (!ApplyUpstreamHeaders(snapshot, config, origin, response, false)) {
      if (logger) logger->Warn("Backend proxy rejected a cross-origin redirect");
      SetProxyError(response, 502, "Backend redirect target is not allowed");
      return;
    }
    response.body = std::move(state->body);
  } else {
    if (!ApplyUpstreamHeaders(snapshot, config, origin, response, true)) {
      stream->Cancel();
      if (worker->joinable()) worker->join();
      if (logger) logger->Warn("Backend proxy rejected a cross-origin redirect");
      SetProxyError(response, 502, "Backend redirect target is not allowed");
      return;
    }
    const auto content_type =
        HeaderValue(snapshot.headers, "Content-Type", "application/octet-stream");
    const bool has_length = snapshot.headers.find("Content-Length") != snapshot.headers.end();
    const auto length = has_length
                            ? snapshot.headers.find("Content-Length")->second
                            : std::string{};
    if (has_length && length == "0") {
      stream->Cancel();
      if (worker->joinable()) worker->join();
    } else {
      const auto provider = [stream](std::size_t, httplib::DataSink& sink) {
        std::string chunk;
        switch (stream->Read(chunk)) {
          case ProxyStreamReadResult::Data:
            return sink.write(chunk.data(), chunk.size());
          case ProxyStreamReadResult::Finished:
            sink.done();
            return true;
          case ProxyStreamReadResult::Failed:
            return false;
        }
        return false;
      };
      const auto releaser = [stream, worker, logger, method, started](bool success) {
        stream->Cancel();
        if (worker->joinable()) worker->join();
        if (logger) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count();
          logger->Info(std::string("Proxy download ") +
                       (success ? "completed: " : "interrupted: ") + method + " in " +
                       std::to_string(elapsed) + " ms");
        }
      };
      if (has_length)
        response.set_content_provider(content_type, provider, releaser);
      else
        response.set_chunked_content_provider(content_type, provider, releaser);
      if (logger)
        logger->Info("Proxy download started: " + method +
                     (has_length ? ", bytes=" + length : ", unknown length"));
    }
  }

  if (!snapshot.download && logger && logger->DebugEnabled()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    logger->Debug("Proxy " + method + " -> " + std::to_string(response.status) +
                  " in " + std::to_string(elapsed) + " ms");
  }
}

}  // namespace lwweb
