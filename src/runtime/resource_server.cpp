#include "lwweb/runtime/resource_server.h"

#include "lwweb/common/error.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/runtime/backend_proxy.h"
#include "lwweb/runtime/local_file_grant.h"

#include <httplib.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <regex>

namespace lwweb {
namespace {

std::string RegexEscape(const std::string& value) {
  static const std::regex special(R"([.^$|()\[\]{}*+?\\])");
  return std::regex_replace(value, special, R"(\$&)");
}

constexpr char kLocalFilePrefix[] = "/__lw_file__";

std::optional<std::string> LocalFileGrantId(const std::string& path) {
  const std::string prefix = std::string(kLocalFilePrefix) + "/";
  if (path.rfind(prefix, 0) != 0) return std::nullopt;
  const auto remainder = path.substr(prefix.size());
  const auto separator = remainder.find('/');
  if (separator != 32 || separator + 1 >= remainder.size()) return std::nullopt;
  const auto id = remainder.substr(0, separator);
  if (!std::all_of(id.begin(), id.end(), [](unsigned char character) {
        return std::isdigit(character) || (character >= 'a' && character <= 'f');
      }))
    return std::nullopt;
  const auto display_name = remainder.substr(separator + 1);
  if (display_name == "." || display_name == ".." ||
      display_name.find('/') != std::string::npos ||
      display_name.find('\\') != std::string::npos ||
      std::any_of(display_name.begin(), display_name.end(), [](unsigned char character) {
        return character == 0 || character < 0x20 || character == 0x7f;
      }))
    return std::nullopt;
  return id;
}

bool ValidSingleRange(const httplib::Request& request, std::uint64_t size,
                      std::uint64_t& offset, std::uint64_t& end) {
  if (request.ranges.empty()) {
    offset = 0;
    end = size ? size - 1 : 0;
    return true;
  }
  if (request.ranges.size() != 1 || size == 0) return false;
  const auto first = request.ranges.front().first;
  const auto last = request.ranges.front().second;
  if (first == -1) {
    if (last <= 0 || static_cast<std::uint64_t>(last) > size) return false;
    offset = size - static_cast<std::uint64_t>(last);
    end = size - 1;
    return true;
  }
  if (first < 0 || static_cast<std::uint64_t>(first) >= size) return false;
  offset = static_cast<std::uint64_t>(first);
  if (last == -1 || static_cast<std::uint64_t>(last) >= size)
    end = size - 1;
  else if (last < first)
    return false;
  else
    end = static_cast<std::uint64_t>(last);
  return true;
}

void SetLocalFileHeaders(httplib::Response& response) {
  response.set_header("Accept-Ranges", "bytes");
  response.set_header("Cache-Control", "private, no-store");
  response.set_header("X-Content-Type-Options", "nosniff");
  response.set_header("Referrer-Policy", "no-referrer");
  response.set_header("Cross-Origin-Resource-Policy", "same-origin");
}

void HandleLocalFile(const httplib::Request& request, httplib::Response& response,
                     std::uint16_t port,
                     const std::shared_ptr<LocalFileGrantManager>& grants,
                     const Logger* logger) {
  if (!IsExpectedResourceHost(request.get_header_value("Host"), port)) {
    response.status = 403;
    return;
  }
  SetLocalFileHeaders(response);
  const auto id = LocalFileGrantId(request.path);
  const auto grant = id && grants ? grants->Find(*id) : std::nullopt;
  if (!grant) {
    response.status = 404;
    return;
  }

  std::uint64_t offset = 0;
  std::uint64_t end = 0;
  if (!ValidSingleRange(request, grant->size, offset, end)) {
    response.status = 416;
    response.set_header("Content-Range", "bytes */" + std::to_string(grant->size));
    return;
  }
  if (logger) {
    if (request.ranges.empty())
      logger->Info("Local file request: full");
    else
      logger->Info("Local file request: range " + std::to_string(offset) + "-" +
                   std::to_string(end));
  }

  const auto path = grant->path;
  response.set_content_provider(
      static_cast<std::size_t>(grant->size), grant->mime,
      [path](std::size_t stream_offset, std::size_t length,
             httplib::DataSink& sink) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        input.seekg(static_cast<std::streamoff>(stream_offset));
        if (!input) return false;
        std::array<char, 64 * 1024> buffer{};
        auto remaining = length;
        while (remaining > 0) {
          const auto part = (std::min)(remaining, buffer.size());
          input.read(buffer.data(), static_cast<std::streamsize>(part));
          if (static_cast<std::size_t>(input.gcount()) != part) return false;
          if (!sink.write(buffer.data(), part)) return false;
          remaining -= part;
        }
        return true;
      });
}

}  // namespace

std::uint16_t StableAppPort(const std::string& app_id) {
  // Stable FNV-1a avoids std::hash's implementation-defined result while
  // distributing applications across the complete private port range.
  std::uint32_t hash = 2166136261u;
  for (const auto value : app_id) {
    hash ^= static_cast<unsigned char>(value);
    hash *= 16777619u;
  }
  return static_cast<std::uint16_t>(49152u + (hash & 0x3fffu));
}

bool IsExpectedResourceHost(const std::string& host, std::uint16_t port) {
  return host == "127.0.0.1:" + std::to_string(port);
}

std::string BuildLocalStartUrl(const std::string& origin, const std::string& start_path) {
  if (!IsSafeStartPath(start_path)) throw Error("Unsafe manifest start path");
  auto base = origin;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + start_path;
}

// ZipResourceStore 的私有实现，负责 miniz 回调、宽路径文件流、索引和缓存。
struct ZipResourceStore::Impl {
  LoadedPayload payload;
  SecurityLimits limits;
  mz_zip_archive zip{};
  std::unordered_map<std::string, mz_uint> entries;
  ResourceCache cache;
  std::mutex mutex;
  std::ifstream file;
  const Logger* logger = nullptr;

  Impl(const LoadedPayload& loaded, SecurityLimits security, const Logger* log)
      : payload(loaded), limits(security), logger(log) {
    if (!(payload.footer.flags & kPayloadHasZip)) throw Error("Payload has no local ZIP archive");
    file.open(payload.executable, std::ios::binary);
    if (!file) throw Error("Cannot open executable for ZIP access");
    zip.m_pIO_opaque = this;
    zip.m_pRead = [](void* opaque, mz_uint64 offset, void* buffer, size_t size) -> size_t {
      auto* self = static_cast<Impl*>(opaque);
      if (offset > self->payload.footer.payload_size ||
          size > self->payload.footer.payload_size - offset)
        return 0;
      self->file.clear();
      self->file.seekg(static_cast<std::streamoff>(self->payload.footer.payload_offset + offset));
      if (!self->file) return 0;
      self->file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
      return static_cast<size_t>(self->file.gcount());
    };
    if (!mz_zip_reader_init(&zip, payload.footer.payload_size, 0))
      throw Error("Cannot open embedded ZIP archive");

    const auto count = mz_zip_reader_get_num_files(&zip);
    if (count > limits.max_file_count) throw Error("ZIP resource count exceeds the safety limit");
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
      mz_zip_archive_file_stat stat{};
      if (!mz_zip_reader_file_stat(&zip, i, &stat)) throw Error("Cannot inspect ZIP entry");
      if (stat.m_is_directory) continue;
      const auto normalized = NormalizeArchivePath(stat.m_filename ? stat.m_filename : "");
      if (!normalized) throw Error("ZIP contains an unsafe path");
      if (stat.m_uncomp_size > limits.max_file_size)
        throw Error("ZIP entry exceeds the per-file safety limit");
      if (stat.m_uncomp_size > limits.max_total_size - total)
        throw Error("ZIP exceeds the total uncompressed size limit");
      total += stat.m_uncomp_size;
      if (!entries.emplace(*normalized, i).second) throw Error("ZIP contains duplicate paths");
      if (payload.manifest.backend_proxy.enabled) {
        const auto reserved = payload.manifest.backend_proxy.prefix.substr(1);
        if (*normalized == reserved || normalized->rfind(reserved + "/", 0) == 0)
          throw Error("ZIP resource conflicts with the backend proxy prefix");
      }
      if (*normalized == "__lw_file__" ||
          normalized->rfind("__lw_file__/", 0) == 0)
        throw Error("ZIP resource conflicts with the local file bridge prefix");
    }
  }

  ~Impl() { mz_zip_reader_end(&zip); }
};

ZipResourceStore::ZipResourceStore(const LoadedPayload& payload, SecurityLimits limits,
                                   const Logger* logger)
    : impl_(std::make_unique<Impl>(payload, limits, logger)) {}

ZipResourceStore::~ZipResourceStore() = default;

bool ZipResourceStore::Exists(const std::string& path) const {
  const auto normalized = NormalizeArchivePath(path);
  return normalized && impl_->entries.find(*normalized) != impl_->entries.end();
}

std::vector<std::uint8_t> ZipResourceStore::Read(const std::string& path) {
  const auto normalized = NormalizeArchivePath(path);
  if (!normalized) throw Error("Unsafe resource path");
  if (const auto cached = impl_->cache.Get(*normalized)) {
    if (impl_->logger && impl_->logger->DebugEnabled())
      impl_->logger->Debug("ZIP cache hit: " + *normalized);
    return *cached;
  }
  if (impl_->logger && impl_->logger->DebugEnabled())
    impl_->logger->Debug("ZIP cache miss: " + *normalized);
  const auto it = impl_->entries.find(*normalized);
  if (it == impl_->entries.end()) throw Error("Resource was not found");

  mz_zip_archive_file_stat stat{};
  std::vector<std::uint8_t> bytes;
  {
    std::lock_guard lock(impl_->mutex);
    if (!mz_zip_reader_file_stat(&impl_->zip, it->second, &stat))
      throw Error("Cannot inspect ZIP entry");
    bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!bytes.empty() && !mz_zip_reader_extract_to_mem(&impl_->zip, it->second, bytes.data(),
                                                        bytes.size(), 0))
      throw Error("Cannot decompress ZIP resource");
  }
  impl_->cache.Put(*normalized, bytes);
  return bytes;
}

ResourceServer::ResourceServer(const LoadedPayload& payload, SecurityLimits limits,
                               const Logger* logger,
                               std::shared_ptr<LocalFileGrantManager> file_grants)
    : payload_(payload), limits_(limits), file_grants_(std::move(file_grants)),
      logger_(logger) {}

ResourceServer::~ResourceServer() { Stop(); }

std::string ResourceServer::Start() {
  if (thread_.joinable()) return "http://127.0.0.1:" + std::to_string(port_) + "/";
  store_ = std::make_unique<ZipResourceStore>(payload_, limits_, logger_);
  if (payload_.manifest.backend_proxy.enabled)
    backend_proxy_ =
        std::make_unique<BackendProxy>(payload_.manifest.backend_proxy, logger_);
  const auto make_server = [this] {
    auto server = std::make_unique<httplib::Server>();
    // cpp-httplib 默认按 CPU 核数创建线程；桌面应用使用固定小线程池可避免高核数机器
    // 为每个生成应用预留过多线程，同时仍允许同步 XHR 与多个静态资源并发。
    server->new_task_queue = [] { return new httplib::ThreadPool(8); };
#ifdef _WIN32
    // Windows 的 SO_REUSEADDR 允许第二个进程绑定同一端口，会破坏 Host/origin
    // 隔离。独占绑定让端口冲突可靠失败，再由候选端口逻辑处理回退。
    server->set_socket_options([](auto socket) {
      const BOOL exclusive = TRUE;
      (void)setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    });
#else
    // 本地应用端口不需要 SO_REUSEPORT；保持独占可可靠检测其他进程占用。
    server->set_socket_options([](auto) {});
#endif
    server->set_payload_max_length(backend_proxy_
                                       ? static_cast<std::size_t>(
                                             payload_.manifest.backend_proxy.max_request_size)
                                       : 1024u);
    const auto reject_local_file_write =
        [this](const httplib::Request& request, httplib::Response& response) {
          if (!IsExpectedResourceHost(request.get_header_value("Host"),
                                      static_cast<std::uint16_t>(port_))) {
            response.status = 403;
          } else {
            response.status = 405;
            response.set_header("Allow", "GET, HEAD");
            SetLocalFileHeaders(response);
          }
        };
    if (backend_proxy_) {
      const auto pattern = "^" +
                           RegexEscape(payload_.manifest.backend_proxy.prefix) +
                           "(?:/.*)?$";
      const auto handler = [this](const httplib::Request& request,
                                  httplib::Response& response) {
        backend_proxy_->Handle(request, response,
                               static_cast<std::uint16_t>(port_));
      };
      server->Get(pattern, handler);
      server->Post(pattern, handler);
      server->Put(pattern, handler);
      server->Patch(pattern, handler);
      server->Delete(pattern, handler);
      server->Options(pattern, handler);
    }
    server->Get("/health", [this](const httplib::Request& request,
                                  httplib::Response& response) {
      if (!IsExpectedResourceHost(request.get_header_value("Host"),
                                  static_cast<std::uint16_t>(port_))) {
        response.status = 403;
        return;
      }
      response.set_content("ok", "text/plain; charset=utf-8");
      response.set_header("Cache-Control", "no-store");
    });
    server->Get("^/__lw_file__(?:/.*)?$",
                [this](const httplib::Request& request,
                       httplib::Response& response) {
                  HandleLocalFile(request, response,
                                  static_cast<std::uint16_t>(port_),
                                  file_grants_, logger_);
                });
    // Register every ordinary write verb so the bridge reliably returns 405
    // instead of the library's default 404. Do not reject these requests from a
    // pre-routing handler: replying before cpp-httplib consumes the small,
    // globally bounded request body can reset the Windows connection and make
    // the client lose the 405 response.
    const std::string local_file_pattern = "^/__lw_file__(?:/.*)?$";
    server->Post(local_file_pattern, reject_local_file_write);
    server->Put(local_file_pattern, reject_local_file_write);
    server->Patch(local_file_pattern, reject_local_file_write);
    server->Delete(local_file_pattern, reject_local_file_write);
    server->Options(local_file_pattern, reject_local_file_write);
    server->Get("/.*", [this](const httplib::Request& request, httplib::Response& response) {
      if (logger_ && logger_->DebugEnabled()) logger_->Debug("GET " + request.path);
      const auto host = request.get_header_value("Host");
      if (!IsExpectedResourceHost(host, static_cast<std::uint16_t>(port_))) {
        response.status = 403;
        return;
      }
      std::string path = request.path;
      if (!path.empty() && path.front() == '/') path.erase(path.begin());
      if (path.empty()) path = payload_.manifest.entry;
      auto normalized = NormalizeArchivePath(path);
      if (!normalized) {
        response.status = 400;
        return;
      }
      if (!store_->Exists(*normalized)) {
        if (!payload_.manifest.spa_fallback) {
          response.status = 404;
          return;
        }
        normalized = NormalizeArchivePath(payload_.manifest.entry);
        if (logger_ && logger_->DebugEnabled())
          logger_->Debug("SPA fallback: " + request.path);
      }
      try {
        auto bytes = store_->Read(*normalized);
        response.set_content(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                             MimeTypeForPath(*normalized));
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "no-referrer");
        response.set_header("Cross-Origin-Resource-Policy", "same-origin");
        response.set_header("Content-Security-Policy",
                            "default-src 'self' data: blob:; script-src 'self' 'unsafe-inline' "
                            "'unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: "
                            "blob:; media-src 'self' data: blob:; connect-src 'self' https: wss:");
      } catch (...) {
        response.status = 500;
      }
    });
    return server;
  };
  const auto preferred_port = StableAppPort(EffectiveAppId(payload_.manifest));
  constexpr int kPortAttempts = 64;
  for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
    // 257 与 16384 互质，可在动态端口范围内形成稳定且不重复的候选序列。
    port_ = 49152 + ((preferred_port - 49152 + attempt * 257) & 0x3fff);
    server_ = make_server();
    if (server_->bind_to_port("127.0.0.1", port_)) break;
    server_.reset();
    port_ = 0;
  }
  if (!port_)
    throw Error("Cannot bind a local resource server port after " +
                std::to_string(kPortAttempts) + " attempts");
  if (logger_ && port_ != preferred_port)
    logger_->Warn("Preferred resource port " + std::to_string(preferred_port) +
                  " was occupied; using " + std::to_string(port_));
  thread_ = std::thread([this] { server_->listen_after_bind(); });
  server_->wait_until_ready();
  if (logger_) logger_->Info("Resource server: 127.0.0.1:" + std::to_string(port_));
  if (logger_ && backend_proxy_)
    logger_->Info("Backend proxy: " + payload_.manifest.backend_proxy.prefix + " -> " +
                  payload_.manifest.backend_proxy.origin);
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

void ResourceServer::Stop() {
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
  server_.reset();
  backend_proxy_.reset();
  store_.reset();
  port_ = 0;
}

}  // namespace lwweb
