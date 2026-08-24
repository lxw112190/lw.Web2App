#include "lwweb/packer/payload.h"
#include "lwweb/packer/packer.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/runtime/local_file_grant.h"
#include "lwweb/runtime/resource_server.h"
#ifdef _WIN32
#include "lwweb/pe/pe_resources.h"
#endif

#include <httplib.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectoryGuard {
  std::filesystem::path path;
  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool BoundsRejected(const lwweb::PayloadFooter& footer, std::uint64_t file_size) {
  try {
    lwweb::ValidatePayloadBounds(footer, file_size);
    return false;
  } catch (...) {
    return true;
  }
}

struct ListeningServerGuard {
  std::unique_ptr<httplib::Server> server;
  std::thread thread;

  bool Start(std::uint16_t port) {
    server = std::make_unique<httplib::Server>();
    server->new_task_queue = [] { return new httplib::ThreadPool(2); };
#ifdef _WIN32
    server->set_socket_options([](auto socket) {
      const BOOL exclusive = TRUE;
      (void)setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    });
#else
    server->set_socket_options([](auto) {});
#endif
    if (!server->bind_to_port("127.0.0.1", port)) {
      server.reset();
      return false;
    }
    thread = std::thread([this] { server->listen_after_bind(); });
    server->wait_until_ready();
    return true;
  }

  ~ListeningServerGuard() {
    if (server) server->stop();
    if (thread.joinable()) thread.join();
  }
};

struct BackendServerGuard {
  httplib::Server server;
  std::thread thread;
  int port = 0;

  template <typename Configure>
  bool Bind(Configure configure) {
    server.new_task_queue = [] { return new httplib::ThreadPool(2); };
    configure(server);
    port = server.bind_to_any_port("127.0.0.1");
    return port > 0;
  }

  void Listen() {
    thread = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
  }

  ~BackendServerGuard() {
    server.stop();
    if (thread.joinable()) thread.join();
  }
};
}

void RunPayloadTests() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto log_file = std::filesystem::temp_directory_path() /
                        ("lwweb-logging-test-" + unique) / "app.log";
  TempDirectoryGuard log_cleanup{log_file.parent_path()};
  lwweb::LoggingConfig rotation;
  rotation.max_file_size = 64 * 1024;
  rotation.max_files = 2;
  {
    auto logger = lwweb::Logger::Rotating("lwweb-test", log_file, rotation);
    for (int i = 0; i < 3000; ++i)
      logger.Info("rotation smoke test payload 0123456789012345678901234567890123456789");
    logger.Flush();
  }
  Check(std::filesystem::is_regular_file(log_file), "rotating logger writes a file");
  const auto rotated1 = log_file.parent_path() / "app.1.log";
  const auto rotated2 = log_file.parent_path() / "app.2.log";
  const auto rotated3 = log_file.parent_path() / "app.3.log";
  Check(std::filesystem::is_regular_file(rotated1),
        "rotating logger creates the first archive");
  Check(std::filesystem::is_regular_file(rotated2),
        "rotating logger retains the configured archive count");
  Check(!std::filesystem::exists(rotated3),
        "rotating logger does not exceed the configured archive count");
  Check(std::filesystem::file_size(log_file) <= rotation.max_file_size,
        "current rotating log respects its size limit");
  lwweb::PayloadFooter input;
  input.flags = lwweb::kPayloadHasZip;
  input.payload_offset = 0x0123456789ABCDEFu;
  input.payload_size = 123456;
  input.manifest_offset = input.payload_offset + input.payload_size;
  input.manifest_size = 99;
  for (std::size_t i = 0; i < input.sha256.size(); ++i)
    input.sha256[i] = static_cast<std::uint8_t>(i);
  const auto output = lwweb::DecodeFooter(lwweb::EncodeFooter(input));
  Check(output.version == lwweb::kPayloadVersion, "footer version round-trip");
  Check(output.flags == input.flags, "footer flags round-trip");
  Check(output.payload_offset == input.payload_offset, "footer offset round-trip");
  Check(output.payload_size == input.payload_size, "footer size round-trip");
  Check(output.manifest_offset == input.manifest_offset, "manifest offset round-trip");
  Check(output.manifest_size == input.manifest_size, "manifest size round-trip");
  Check(output.sha256 == input.sha256, "digest round-trip");

  lwweb::PayloadFooter valid_bounds;
  valid_bounds.payload_offset = 100;
  valid_bounds.payload_size = 200;
  valid_bounds.manifest_offset = 300;
  valid_bounds.manifest_size = 50;
  lwweb::ValidatePayloadBounds(valid_bounds, 350 + lwweb::kPayloadFooterSize);
  Check(BoundsRejected(valid_bounds, lwweb::kPayloadFooterSize - 1),
        "file smaller than footer rejected");
  auto invalid_bounds = valid_bounds;
  invalid_bounds.version = 99;
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "unsupported footer version rejected");
  invalid_bounds = valid_bounds;
  invalid_bounds.payload_offset = std::numeric_limits<std::uint64_t>::max();
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "payload offset overflow rejected");
  invalid_bounds = valid_bounds;
  invalid_bounds.payload_size = std::numeric_limits<std::uint64_t>::max();
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "payload size overflow rejected");
  invalid_bounds = valid_bounds;
  invalid_bounds.manifest_offset = 301;
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "manifest gap rejected");
  invalid_bounds = valid_bounds;
  invalid_bounds.manifest_size = 49;
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "manifest not adjacent to footer rejected");
  invalid_bounds = valid_bounds;
  invalid_bounds.payload_size = 251;
  invalid_bounds.manifest_offset = 351;
  Check(BoundsRejected(invalid_bounds, 350 + lwweb::kPayloadFooterSize),
        "payload beyond footer rejected");
  const auto base = std::filesystem::temp_directory_path() / "lwweb-integration-test";
  std::error_code ignored;
  std::filesystem::remove_all(base, ignored);
  std::filesystem::create_directories(base / "site" / "assets");
  {
#ifdef _WIN32
    std::filesystem::copy_file(lwweb::CurrentExecutablePath(),
                               base / "runner.exe");
#else
    std::ofstream runner(base / "runner.exe", std::ios::binary);
    runner << "MZ fake runner prefix";
#endif
    std::ofstream html(base / "site" / "index.html", std::ios::binary);
    html << "<!doctype html><title>test</title>";
    std::ofstream css(base / "site" / "assets" / "app.css", std::ios::binary);
    css << "body{color:red}";
  }
  lwweb::PackOptions pack;
  pack.runner = base / "runner.exe";
  pack.source_directory = base / "site";
  pack.output = base / "packed.exe";
  pack.manifest.title = "Integration Test";
  std::atomic<int> backend_requests{0};
  std::string backend_target;
  std::string backend_body;
  std::string backend_origin_header;
  std::string backend_range_header;
  const std::string download_data(2 * 1024 * 1024, 'D');
  std::string local_file_data(2 * 1024 * 1024, '\0');
  for (std::size_t i = 0; i < local_file_data.size(); ++i)
    local_file_data[i] = static_cast<char>(i % 251);
  const auto local_file_path =
      base / std::filesystem::u8path(u8"本地文档.pdf");
  {
    std::ofstream local_file(local_file_path, std::ios::binary);
    local_file.write(local_file_data.data(),
                     static_cast<std::streamsize>(local_file_data.size()));
  }
  BackendServerGuard backend;
  Check(backend.Bind([&](httplib::Server& server) {
          server.Post("/sysUser/login", [&](const httplib::Request& request,
                                             httplib::Response& response) {
            ++backend_requests;
            backend_target = request.target;
            backend_body = request.body;
            backend_origin_header = request.get_header_value("Origin");
            response.status = 200;
            response.set_header("Set-Cookie",
                                "JSESSIONID=test; Domain=127.0.0.1; Path=/; HttpOnly");
            response.set_content(R"({"success":true})", "application/json");
          });
          server.Get("/external-redirect", [](const httplib::Request&,
                                                httplib::Response& response) {
            response.set_redirect("http://example.com/escape");
          });
          server.Get("/downloads/report", [&](const httplib::Request& request,
                                                httplib::Response& response) {
            backend_range_header = request.get_header_value("Range");
            response.set_header(
                "Content-Disposition",
                "attachment; filename*=UTF-8''proxy-download-%E6%B5%8B%E8%AF%95.bin");
            response.set_header("Accept-Ranges", "bytes");
            response.set_header("Cache-Control", "no-store");
            response.set_content_provider(
                download_data.size(), "application/octet-stream",
                [&](std::size_t offset, std::size_t length,
                    httplib::DataSink& sink) {
                  return sink.write(download_data.data() + offset, length);
                });
          });
          server.Get("/oversized-api", [](const httplib::Request&,
                                            httplib::Response& response) {
            response.set_content(std::string(2048, 'J'), "application/json");
          });
        }),
        "mock legacy backend starts");
  pack.manifest.backend_proxy.enabled = true;
  pack.manifest.backend_proxy.origin =
      "http://127.0.0.1:" + std::to_string(backend.port);
  pack.manifest.backend_proxy.max_response_size = 1024;
  std::unique_ptr<ListeningServerGuard> port_blocker;
  for (int attempt = 0; attempt < 32 && !port_blocker; ++attempt) {
    const auto app_id = "test-port-" + unique + "-" + std::to_string(attempt);
    auto candidate = std::make_unique<ListeningServerGuard>();
    if (candidate->Start(lwweb::StableAppPort(app_id))) {
      pack.manifest.app_id = app_id;
      port_blocker = std::move(candidate);
    }
  }
  Check(port_blocker != nullptr, "test reserves a preferred resource port");
  Check(lwweb::IsCanonicalArchivePath(pack.manifest.entry), "pack entry is canonical");
  lwweb::ValidateManifest(pack.manifest);
  {
    std::ofstream previous(pack.output, std::ios::binary);
    previous << "previous application";
  }

  // Payload 先于 Runner 组装完成，签名阶段因此可以稳定取得最终摘要。
  // AppendPreparedPayload 必须写入参与摘要计算的同一份 Manifest 字节。
  const auto prepared = lwweb::PreparePayload(pack, pack.manifest);
  Check(prepared.file_count == 2 && prepared.source_size > 0 &&
            prepared.compressed_size == std::filesystem::file_size(prepared.zip),
        "prepared payload exposes ZIP statistics before executable assembly");
  Check(prepared.sha256 ==
            lwweb::Sha256FileWithSuffix(prepared.zip, prepared.manifest_json),
        "prepared payload digest covers exact ZIP and manifest bytes");
  Check((prepared.flags & lwweb::kPayloadHasZip) != 0,
        "prepared local payload records the ZIP flag");

  const auto prepared_output = base / "prepared.exe";
  const auto compatibility_output = base / "compatibility.exe";
  {
    std::ofstream prepared_runner(prepared_output, std::ios::binary);
    prepared_runner << "MZ prepared runner";
  }
  std::filesystem::copy_file(prepared_output, compatibility_output);
  lwweb::AppendPreparedPayload(prepared_output, prepared);
  lwweb::AppendPayload(compatibility_output, prepared.zip, pack.manifest,
                       prepared.flags);
  Check(lwweb::ReadFileBytes(prepared_output) ==
            lwweb::ReadFileBytes(compatibility_output),
        "prepared and compatibility append APIs produce identical LWWEB002 bytes");
  const auto prepared_loaded = lwweb::LoadPayload(prepared_output);
  Check(prepared_loaded.footer.sha256 == prepared.sha256 &&
            prepared_loaded.manifest.title == pack.manifest.title,
        "prepared payload round-trips through the existing V2 loader");

  auto changed = prepared;
  changed.zip = base / "changed-payload.zip";
  std::filesystem::copy_file(prepared.zip, changed.zip);
  {
    std::ofstream stream(changed.zip, std::ios::binary | std::ios::app);
    stream << 'x';
  }
  const auto changed_output = base / "changed.exe";
  {
    std::ofstream changed_runner(changed_output, std::ios::binary);
    changed_runner << "MZ changed runner";
  }
  bool changed_payload_rejected = false;
  try {
    lwweb::AppendPreparedPayload(changed_output, changed);
  } catch (...) {
    changed_payload_rejected = true;
  }
  Check(changed_payload_rejected,
        "prepared payload mutation is rejected before footer publication");

  lwweb::PackOptions url_options;
  url_options.output = base / "url-prepared.exe";
  lwweb::Manifest url_manifest;
  url_manifest.mode = lwweb::AppMode::Url;
  url_manifest.url = "https://example.com/app";
  const auto prepared_url = lwweb::PreparePayload(url_options, url_manifest);
  Check(prepared_url.zip.empty() &&
            (prepared_url.flags & lwweb::kPayloadUrlMode) != 0 &&
            prepared_url.sha256 == lwweb::Sha256(
                reinterpret_cast<const std::uint8_t*>(
                    prepared_url.manifest_json.data()),
                prepared_url.manifest_json.size()),
        "URL mode prepares a manifest-only LWWEB002 payload");
  std::filesystem::remove(prepared.zip, ignored);

  lwweb::PackApplication(pack);
  const auto loaded = lwweb::LoadPayload(pack.output);
#ifdef _WIN32
  const auto packed_binding = lwweb::ReadPePayloadBinding(pack.output);
  Check(!packed_binding,
        "unsigned Windows pack does not claim a signed payload binding");

  // 模拟攻击者同时修改 Manifest 与 Footer SHA-256。单独的 LWWEB002
  // 校验可以通过；Binding 的拒绝场景由签名集成测试覆盖。
  const auto rebound_tamper = base / "binding-tamper.exe";
  std::filesystem::copy_file(pack.output, rebound_tamper);
  auto tampered_footer = loaded.footer;
  {
    std::fstream executable(rebound_tamper,
                            std::ios::binary | std::ios::in | std::ios::out);
    executable.seekg(
        static_cast<std::streamoff>(tampered_footer.manifest_offset));
    std::string manifest_text(
        static_cast<std::size_t>(tampered_footer.manifest_size), '\0');
    executable.read(manifest_text.data(),
                    static_cast<std::streamsize>(manifest_text.size()));
    const auto title = manifest_text.find("Integration Test");
    Check(title != std::string::npos,
          "binding tamper fixture locates the manifest title");
    manifest_text[title] = 'J';
    executable.seekp(static_cast<std::streamoff>(
        tampered_footer.manifest_offset + title));
    executable.write(manifest_text.data() + title, 1);
  }
  tampered_footer.sha256 = lwweb::Sha256FileRange(
      rebound_tamper, tampered_footer.payload_offset,
      tampered_footer.payload_size + tampered_footer.manifest_size);
  const auto tampered_footer_bytes = lwweb::EncodeFooter(tampered_footer);
  {
    std::fstream executable(rebound_tamper,
                            std::ios::binary | std::ios::in | std::ios::out);
    executable.seekp(static_cast<std::streamoff>(
        std::filesystem::file_size(rebound_tamper) -
        lwweb::kPayloadFooterSize));
    executable.write(
        reinterpret_cast<const char*>(tampered_footer_bytes.data()),
        static_cast<std::streamsize>(tampered_footer_bytes.size()));
  }
  const auto footer_only_verified = lwweb::LoadPayload(rebound_tamper);
  Check(footer_only_verified.manifest.title == "Jntegration Test",
        "recomputed Footer passes the standalone LWWEB002 check");
  lwweb::VerifyPePayloadBinding(rebound_tamper,
                                footer_only_verified.footer.sha256);
#endif
  backend.Listen();
  Check(loaded.manifest.title == "Integration Test", "packed manifest loads");
  Check(loaded.manifest.start_path == "/", "packed manifest stores default start path");
  Check(loaded.manifest.backend_proxy.enabled,
        "controlled backend proxy is stored in the package");
  Check(loaded.manifest.fullscreen, "new packages start fullscreen by default");
  Check(loaded.manifest.logging.enabled, "runtime logging enabled by default");
  Check(loaded.manifest.logging.max_file_size == 2ull * 1024 * 1024,
        "runtime log rotation size");
  Check(loaded.manifest.logging.max_files == 5, "runtime log rotation count");
  Check(lwweb::IsValidAppId(loaded.manifest.app_id), "generated app ID is valid");
  const auto stable_port = lwweb::StableAppPort(loaded.manifest.app_id);
  Check(stable_port >= 49152, "stable app port uses the private range");
  Check(stable_port == lwweb::StableAppPort(loaded.manifest.app_id),
        "stable app port is deterministic");
  Check(lwweb::StableAppPort("app-port-a") != lwweb::StableAppPort("app-port-b"),
        "different app IDs distribute across ports");
  {
    auto file_grants = std::make_shared<lwweb::LocalFileGrantManager>();
    const auto local_grant = file_grants->Create(local_file_path);
    Check(local_grant.id.size() == 32 &&
              local_grant.url.find(local_file_path.parent_path().u8string()) ==
                  std::string::npos,
          "local file grant uses an opaque URL without the disk path");
    lwweb::ResourceServer fallback_server(loaded, {}, nullptr, file_grants);
    const auto address = fallback_server.Start();
    Check(address != "http://127.0.0.1:" + std::to_string(stable_port) + "/",
          "occupied preferred port selects a fallback port");
    httplib::Client application(address.substr(0, address.size() - 1));
    httplib::Headers page_headers = {
        {"Origin", address.substr(0, address.size() - 1)},
        {"Referer", address + "login.html"},
        {"Sec-Fetch-Site", "same-origin"}};
    const auto local_full = application.Get(local_grant.url);
    Check(local_full && local_full->status == 200 &&
              local_full->body == local_file_data,
          "local file bridge streams a complete granted file");
    Check(local_full->get_header_value("Accept-Ranges") == "bytes" &&
              local_full->get_header_value("Cache-Control") == "private, no-store" &&
              local_full->get_header_value("X-Content-Type-Options") == "nosniff",
          "local file bridge emits private read-only response headers");
    const auto local_head = application.Head(local_grant.url);
    Check(local_head && local_head->status == 200 && local_head->body.empty() &&
              local_head->get_header_value("Content-Length") ==
                  std::to_string(local_file_data.size()) &&
              local_head->get_header_value("Content-Type") == "application/pdf",
          "local file bridge HEAD returns metadata without a body");
    httplib::Headers local_range_headers = {{"Range", "bytes=65536-131071"}};
    const auto local_range = application.Get(local_grant.url, local_range_headers);
    Check(local_range && local_range->status == 206 &&
              local_range->body == local_file_data.substr(65536, 65536) &&
              local_range->get_header_value("Content-Range") ==
                  "bytes 65536-131071/" + std::to_string(local_file_data.size()),
          "local file bridge streams a bounded single byte range");
    const auto local_open_range = application.Get(
        local_grant.url, httplib::Headers{{"Range", "bytes=1048576-"}});
    Check(local_open_range && local_open_range->status == 206 &&
              local_open_range->body == local_file_data.substr(1048576),
          "local file bridge supports open-ended byte ranges");
    const auto local_suffix_range = application.Get(
        local_grant.url, httplib::Headers{{"Range", "bytes=-65536"}});
    Check(local_suffix_range && local_suffix_range->status == 206 &&
              local_suffix_range->body ==
                  local_file_data.substr(local_file_data.size() - 65536),
          "local file bridge supports suffix byte ranges");
    const auto invalid_local_range = application.Get(
        local_grant.url, httplib::Headers{{"Range", "bytes=9999999-"}});
    Check(invalid_local_range && invalid_local_range->status == 416 &&
              invalid_local_range->get_header_value("Content-Range") ==
                  "bytes */" + std::to_string(local_file_data.size()),
          "local file bridge rejects an unsatisfiable byte range");
    const auto multiple_local_range = application.Get(
        local_grant.url,
        httplib::Headers{{"Range", "bytes=0-99,200-299"}});
    Check(multiple_local_range && multiple_local_range->status == 416,
          "local file bridge rejects multipart ranges in the first version");
    const auto display_separator = local_grant.url.find_last_of('/');
    const auto changed_display_url =
        local_grant.url.substr(0, display_separator + 1) + "other-name.pdf";
    const auto changed_display = application.Get(changed_display_url);
    Check(changed_display && changed_display->status == 200 &&
              changed_display->body == local_file_data,
          "display filename never changes the file bound to a grant token");
    const auto token_start = std::string("/__lw_file__/").size();
    const auto grant_token = local_grant.url.substr(token_start, 32);
    const auto traversal = application.Get(
        "/__lw_file__/" + grant_token + "/../other.pdf");
    Check(traversal && traversal->status == 404,
          "local file bridge rejects traversal in the display path");
    const auto unknown_grant = application.Get(
        "/__lw_file__/0123456789abcdef0123456789abcdef/missing.pdf");
    Check(unknown_grant && unknown_grant->status == 404,
          "unknown local file grant returns 404");
    const auto local_write = application.Post(local_grant.url, "change", "text/plain");
    Check(local_write && local_write->status == 405 &&
              local_write->get_header_value("Allow") == "GET, HEAD",
          "local file bridge rejects non-read methods before reading a body");
    const auto login = application.Post(
        "/__lw_proxy__/sysUser/login?source=test", page_headers,
        std::string(R"({"message":"proxy-test"})"),
        "application/json");
    Check(login && login->status == 200 && login->body == R"({"success":true})",
          "controlled proxy forwards a legacy login POST");
    Check(backend_target == "/sysUser/login?source=test",
          "proxy preserves backend query parameters");
    Check(backend_body == R"({"message":"proxy-test"})",
          "proxy preserves request body");
    Check(backend_origin_header ==
              "http://127.0.0.1:" + std::to_string(backend.port),
          "proxy rewrites browser origin for the backend");
    const auto cookie = login->get_header_value("Set-Cookie");
    Check(cookie.find("Domain=") == std::string::npos,
          "proxy removes backend cookie domain");
    Check(cookie.find("Path=/__lw_proxy__") != std::string::npos,
          "proxy scopes backend cookie to its local prefix");
    const auto requests_after_login = backend_requests.load();
    const auto blocked = application.Post(
        "/__lw_proxy__/sysUser/login", {{"Origin", "http://evil.example"}},
        R"({"message":"blocked"})", "application/json");
    Check(blocked && blocked->status == 403,
          "cross-site proxy request is rejected");
    Check(backend_requests.load() == requests_after_login,
          "blocked request never reaches the backend");
    const auto redirect = application.Get("/__lw_proxy__/external-redirect", page_headers);
    Check(redirect && redirect->status == 502,
          "cross-origin backend redirect is rejected");
    const auto oversized = application.Get("/__lw_proxy__/oversized-api", page_headers);
    Check(oversized && oversized->status == 502,
          "oversized non-download response remains bounded");
    const auto download = application.Get("/__lw_proxy__/downloads/report", page_headers);
    Check(download && download->status == 200 && download->body == download_data,
          "attachment response streams beyond the buffered API limit");
    const auto download_disposition =
        download->get_header_value("Content-Disposition");
    Check(download_disposition ==
              "attachment; filename*=UTF-8''proxy-download-%E6%B5%8B%E8%AF%95.bin",
          "download content disposition and RFC 5987 filename are preserved");
    Check(download->get_header_value("Content-Length") ==
              std::to_string(download_data.size()),
          "download content length is preserved");
    httplib::Headers range_headers = page_headers;
    range_headers.emplace("Range", "bytes=1024-2047");
    const auto partial =
        application.Get("/__lw_proxy__/downloads/report", range_headers);
    Check(partial && partial->status == 206 && partial->body.size() == 1024 &&
              partial->body == download_data.substr(1024, 1024),
          "download byte range is forwarded without local double slicing");
    Check(backend_range_header == "bytes=1024-2047",
          "download Range header reaches the backend");
    Check(partial->get_header_value("Content-Range") ==
              "bytes 1024-2047/" + std::to_string(download_data.size()),
          "download Content-Range is preserved");
    Check(file_grants->Revoke(local_grant.id),
          "local file grant can be explicitly revoked");
    const auto revoked_local_file = application.Get(local_grant.url);
    Check(revoked_local_file && revoked_local_file->status == 404,
          "revoked local file grant becomes unavailable immediately");
    fallback_server.Stop();
  }
  lwweb::ZipResourceStore store(loaded);
  Check(store.Exists("index.html"), "ZIP index includes entry");
  Check(store.Exists("assets/app.css"), "ZIP index includes asset");
  const auto resource = store.Read("assets/app.css");
  Check(std::string(resource.begin(), resource.end()) == "body{color:red}",
        "ZIP resource extracts on demand");
  Check(!store.Exists("../index.html"), "unsafe runtime path rejected");

  std::filesystem::create_directories(base / "site" / "__lw_file__");
  std::ofstream(base / "site" / "__lw_file__" / "collision.txt") << "reserved";
  auto reserved_pack = pack;
  reserved_pack.output = base / "reserved-route.exe";
  bool reserved_resource_rejected = false;
  try {
    lwweb::PackApplication(reserved_pack);
  } catch (...) {
    reserved_resource_rejected = true;
  }
  Check(reserved_resource_rejected,
        "packaging rejects resources under the local file bridge route");
  std::filesystem::remove_all(base / "site" / "__lw_file__", ignored);

  // ZIP 阶段故意失败时，已经存在的正式产物必须保持逐字节不变，且暂存文件
  // 必须被清理。这覆盖“临时文件完整构建 + 成功后原子替换”的回归行为。
  const auto before_failed_pack = lwweb::ReadFileBytes(pack.output);
  pack.limits.max_file_count = 1;
  bool failed_pack_rejected = false;
  try {
    lwweb::PackApplication(pack);
  } catch (...) {
    failed_pack_rejected = true;
  }
  Check(failed_pack_rejected, "pack failure is reported");
  Check(lwweb::ReadFileBytes(pack.output) == before_failed_pack,
        "failed pack preserves the existing output");
  for (const auto& item : std::filesystem::directory_iterator(base)) {
    const auto name = item.path().filename().u8string();
    Check(name.find(".lwweb-building-") == std::string::npos,
          "failed pack cleans staging artifacts");
  }
  pack.limits.max_file_count = 100000;

  {
    std::fstream executable(pack.output, std::ios::binary | std::ios::in | std::ios::out);
    executable.seekg(static_cast<std::streamoff>(loaded.footer.manifest_offset));
    char byte = 0;
    executable.read(&byte, 1);
    byte ^= 1;
    executable.seekp(static_cast<std::streamoff>(loaded.footer.manifest_offset));
    executable.write(&byte, 1);
  }
  bool tampering_rejected = false;
  try {
    (void)lwweb::LoadPayload(pack.output);
  } catch (...) {
    tampering_rejected = true;
  }
  Check(tampering_rejected, "manifest tampering rejected by V2 content hash");

  const auto legacy = lwweb::ParseManifest(
      R"({"format":"lw-web-app","version":1,"mode":"local","entry":"index.html","title":"Legacy"})");
  Check(!legacy.fullscreen, "legacy manifest remains windowed");
  Check(!legacy.logging.enabled, "legacy manifest does not unexpectedly enable logging");
  Check(legacy.start_path == "/", "legacy manifest defaults to root start path");
  Check(!legacy.backend_proxy.enabled,
        "legacy manifest keeps the controlled backend proxy disabled");
  lwweb::Manifest proxy_manifest;
  proxy_manifest.backend_proxy.enabled = true;
  proxy_manifest.backend_proxy.origin = "http://127.0.0.1:18080";
  const auto proxy_round_trip =
      lwweb::ParseManifest(lwweb::SerializeManifest(proxy_manifest));
  Check(proxy_round_trip.backend_proxy.enabled &&
            proxy_round_trip.backend_proxy.origin == "http://127.0.0.1:18080",
        "controlled backend proxy survives manifest round trip");
  auto reserved_proxy_manifest = proxy_manifest;
  reserved_proxy_manifest.backend_proxy.prefix = "/__lw_file__/proxy";
  bool reserved_proxy_rejected = false;
  try {
    lwweb::ValidateManifest(reserved_proxy_manifest);
  } catch (...) {
    reserved_proxy_rejected = true;
  }
  Check(reserved_proxy_rejected,
        "backend proxy cannot overlap the local file bridge route");
  std::filesystem::remove_all(base, ignored);
}
