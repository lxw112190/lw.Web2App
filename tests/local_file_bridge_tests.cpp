#include "lwweb/common/file_utils.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"
#include "lwweb/runtime/local_file_grant.h"
#include "lwweb/runtime/resource_server.h"

#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// 独立 HTTP 集成测试的临时目录；ResourceServer 先析构，随后再清理载荷与授权文件。
struct TempDirectoryGuard {
  std::filesystem::path path;

  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void WriteFile(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("cannot write local file bridge fixture");
}

lwweb::LoadedPayload BuildPayload(const std::filesystem::path& base) {
  std::filesystem::create_directories(base / "site");
  WriteFile(base / "site" / "index.html", "<!doctype html><title>bridge</title>");
#ifdef _WIN32
  std::filesystem::copy_file(lwweb::CurrentExecutablePath(), base / "runner.exe",
                             std::filesystem::copy_options::overwrite_existing);
#else
  WriteFile(base / "runner", "ELF test runner prefix");
#endif

  lwweb::PackOptions options;
#ifdef _WIN32
  options.runner = base / "runner.exe";
  options.output = base / "packed.exe";
#else
  options.runner = base / "runner";
  options.output = base / "packed";
#endif
  options.source_directory = base / "site";
  options.manifest.title = "Local File Bridge Test";
  options.manifest.app_id = "local-file-bridge-http-test";
  lwweb::PackApplication(options);
  return lwweb::LoadPayload(options.output);
}

std::string ClientOrigin(const std::string& address) {
  return address.empty() || address.back() != '/' ? address
                                                   : address.substr(0, address.size() - 1);
}

}  // namespace

void RunLocalFileBridgeTests() {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto base = std::filesystem::temp_directory_path() /
                    ("lwweb-local-file-bridge-test-" + unique);
  TempDirectoryGuard cleanup{base};
  std::filesystem::create_directories(base);

  const auto payload = BuildPayload(base);
  auto grants = std::make_shared<lwweb::LocalFileGrantManager>();

  std::string file_bytes(64, '\0');
  for (std::size_t index = 0; index < file_bytes.size(); ++index)
    file_bytes[index] = static_cast<char>(index);
  const auto file_path = base / std::filesystem::u8path(u8"本地文档-测试.bin");
  WriteFile(file_path, file_bytes);
  const auto file_grant = grants->Create(file_path);

  const auto empty_path = base / std::filesystem::u8path(u8"空文件.txt");
  WriteFile(empty_path, {});
  const auto empty_grant = grants->Create(empty_path);

  const auto changing_path = base / "changing.bin";
  WriteFile(changing_path, "1234");
  const auto changing_grant = grants->Create(changing_path);

  lwweb::ResourceServer server(payload, {}, nullptr, grants);
  const auto address = server.Start();
  httplib::Client client(ClientOrigin(address));

  const auto whole = client.Get(file_grant.url);
  Check(whole && whole->status == 200 && whole->body == file_bytes,
        "local file bridge GET returns the complete file");
  Check(whole->get_header_value("Content-Length") == std::to_string(file_bytes.size()),
        "local file bridge GET returns Content-Length");

  const auto head = client.Head(file_grant.url);
  Check(head && head->status == 200 && head->body.empty() &&
            head->get_header_value("Content-Length") == std::to_string(file_bytes.size()),
        "local file bridge HEAD returns metadata without a body");

  const auto bounded =
      client.Get(file_grant.url, httplib::Headers{{"Range", "bytes=0-9"}});
  Check(bounded && bounded->status == 206 && bounded->body == file_bytes.substr(0, 10) &&
            bounded->get_header_value("Content-Range") == "bytes 0-9/64",
        "local file bridge supports a bounded byte range");

  const auto open_ended =
      client.Get(file_grant.url, httplib::Headers{{"Range", "bytes=10-"}});
  Check(open_ended && open_ended->status == 206 &&
            open_ended->body == file_bytes.substr(10),
        "local file bridge supports an open-ended byte range");

  const auto suffix =
      client.Get(file_grant.url, httplib::Headers{{"Range", "bytes=-10"}});
  Check(suffix && suffix->status == 206 && suffix->body == file_bytes.substr(54),
        "local file bridge supports a suffix byte range");

  const auto out_of_bounds =
      client.Get(file_grant.url, httplib::Headers{{"Range", "bytes=100-"}});
  Check(out_of_bounds && out_of_bounds->status == 416 &&
            out_of_bounds->get_header_value("Content-Range") == "bytes */64",
        "local file bridge rejects an unsatisfiable byte range");

  const auto multiple = client.Get(
      file_grant.url, httplib::Headers{{"Range", "bytes=0-9,20-29"}});
  Check(multiple && multiple->status == 416,
        "local file bridge rejects multipart ranges");

  const auto unknown = client.Get(
      "/__lw_file__/0123456789abcdef0123456789abcdef/missing.bin");
  Check(unknown && unknown->status == 404,
        "local file bridge rejects an unknown grant token");

  const auto write = client.Post(file_grant.url, "change", "text/plain");
  Check(write && write->status == 405 &&
            write->get_header_value("Allow") == "GET, HEAD",
        "local file bridge rejects non-read methods");

  const auto wrong_host =
      client.Get(file_grant.url, httplib::Headers{{"Host", "evil.example"}});
  Check(wrong_host && wrong_host->status == 403,
        "local file bridge rejects an unexpected Host header");

  const auto empty = client.Get(empty_grant.url);
  Check(empty && empty->status == 200 && empty->body.empty() &&
            empty->get_header_value("Content-Length") == "0",
        "local file bridge serves a zero-byte file");

  WriteFile(changing_path, "12345");
  const auto changed = client.Get(changing_grant.url);
  Check(changed && changed->status == 404,
        "local file bridge invalidates a grant after the file size changes");

  Check(grants->Revoke(file_grant.id), "local file bridge revokes a live grant");
  const auto revoked = client.Get(file_grant.url);
  Check(revoked && revoked->status == 404,
        "local file bridge returns 404 after grant revocation");
}
