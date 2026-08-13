#include "lwweb/runtime/resource_server.h"

#include "lwweb/common/error.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"

#include <httplib.h>
#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace lwweb {

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
                               const Logger* logger)
    : payload_(payload), limits_(limits), logger_(logger) {}

ResourceServer::~ResourceServer() { Stop(); }

std::string ResourceServer::Start() {
  if (thread_.joinable()) return "http://127.0.0.1:" + std::to_string(port_) + "/";
  store_ = std::make_unique<ZipResourceStore>(payload_, limits_, logger_);
  server_ = std::make_unique<httplib::Server>();
  server_->set_payload_max_length(1024);
  server_->Get("/health", [](const httplib::Request&, httplib::Response& response) {
    response.set_content("ok", "text/plain; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
  });
  server_->Get("/.*", [this](const httplib::Request& request, httplib::Response& response) {
    if (logger_ && logger_->DebugEnabled()) logger_->Debug("GET " + request.path);
    const auto expected = "127.0.0.1:" + std::to_string(port_);
    const auto host = request.get_header_value("Host");
    if (host != expected) {
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
  port_ = StableAppPort(EffectiveAppId(payload_.manifest));
  if (!server_->bind_to_port("127.0.0.1", port_))
    throw Error("Cannot bind the stable local resource server port " +
                std::to_string(port_) + "; another instance may already be running");
  thread_ = std::thread([this] { server_->listen_after_bind(); });
  if (logger_) logger_->Info("Resource server: 127.0.0.1:" + std::to_string(port_));
  return "http://127.0.0.1:" + std::to_string(port_) + "/";
}

void ResourceServer::Stop() {
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
  server_.reset();
  store_.reset();
  port_ = 0;
}

}  // namespace lwweb
