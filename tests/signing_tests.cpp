#include "lwweb/common/file_utils.h"
#include "lwweb/packer/packer.h"
#include "lwweb/packer/payload.h"

#ifdef _WIN32
#include "lwweb/pe/authenticode.h"

#include <Windows.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

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

#ifdef _WIN32
std::uint64_t SecurityDirectoryOffset(
    const std::filesystem::path& executable) {
  std::ifstream input(executable, std::ios::binary);
  IMAGE_DOS_HEADER dos{};
  input.read(reinterpret_cast<char*>(&dos), sizeof(dos));
  Check(input && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0,
        "signing fixture reads the DOS header");
  input.seekg(dos.e_lfanew + sizeof(DWORD));
  IMAGE_FILE_HEADER file_header{};
  input.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
  const auto optional_offset = static_cast<std::uint64_t>(dos.e_lfanew) +
                               sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
  WORD magic = 0;
  input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  Check(input && (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                  magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC),
        "signing fixture recognizes the optional header");
  const auto relative =
      magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
          ? offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY)
          : offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
                IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY);
  Check(relative + sizeof(IMAGE_DATA_DIRECTORY) <=
            file_header.SizeOfOptionalHeader,
        "signing fixture locates the Security Directory");
  return optional_offset + relative;
}

std::uint64_t AddFakeCertificateTable(
    const std::filesystem::path& executable, bool trailing_byte) {
  const auto current_size = std::filesystem::file_size(executable);
  const auto certificate_offset = (current_size + 7u) & ~std::uint64_t{7u};
  std::filesystem::resize_file(executable, certificate_offset);
  std::array<std::uint8_t, 16> certificate{};
  certificate[0] = static_cast<std::uint8_t>(certificate.size());
  certificate[4] = 0x00;
  certificate[5] = 0x02;
  certificate[6] = 0x02;
  certificate[7] = 0x00;
  {
    std::ofstream output(executable, std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(certificate.data()),
                 static_cast<std::streamsize>(certificate.size()));
    if (trailing_byte) output.put('x');
  }
  IMAGE_DATA_DIRECTORY directory{};
  directory.VirtualAddress = static_cast<DWORD>(certificate_offset);
  directory.Size = static_cast<DWORD>(certificate.size());
  {
    std::fstream output(executable,
                        std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(static_cast<std::streamoff>(
        SecurityDirectoryOffset(executable)));
    output.write(reinterpret_cast<const char*>(&directory),
                 sizeof(directory));
  }
  return certificate_offset;
}
#endif

}  // namespace

void RunSigningTests() {
#ifdef _WIN32
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto directory = std::filesystem::temp_directory_path() /
                         ("lwweb-signing-" + unique);
  TempDirectoryGuard cleanup{directory};
  std::filesystem::create_directories(directory);
  const auto unsigned_executable = directory / "unsigned.exe";
  std::filesystem::copy_file(lwweb::CurrentExecutablePath(),
                             unsigned_executable);
  if (lwweb::HasAuthenticodeSignature(unsigned_executable))
    (void)lwweb::StripAuthenticodeSignature(unsigned_executable);
  Check(!lwweb::HasAuthenticodeSignature(unsigned_executable),
        "unsigned PE has no Certificate Table");
  bool unsigned_verify_rejected = false;
  try {
    lwweb::VerifyAuthenticodeSignature(unsigned_executable);
  } catch (...) {
    unsigned_verify_rejected = true;
  }
  Check(unsigned_verify_rejected,
        "WinVerifyTrust rejects an unsigned executable");

  const auto fake_signed = directory / "fake-signed.exe";
  std::filesystem::copy_file(unsigned_executable, fake_signed);
  const auto certificate_offset =
      AddFakeCertificateTable(fake_signed, false);
  Check(lwweb::HasAuthenticodeSignature(fake_signed),
        "PE parser detects a declared Certificate Table");

  const auto site = directory / "site";
  std::filesystem::create_directories(site);
  {
    std::ofstream html(site / "index.html", std::ios::binary);
    html << "<!doctype html><title>signed runner</title>";
  }
  lwweb::PackOptions repack;
  repack.runner = fake_signed;
  repack.source_directory = site;
  repack.output = directory / "from-signed-runner.exe";
  repack.manifest.title = "From signed runner";
  lwweb::PackApplication(repack);
  const auto repacked_payload = lwweb::LoadPayload(repack.output);
  Check(repacked_payload.footer.payload_offset == certificate_offset,
        "unsigned packaging strips a signed bare Runner without copying its certificate");

  Check(lwweb::StripAuthenticodeSignature(fake_signed) &&
            !lwweb::HasAuthenticodeSignature(fake_signed) &&
            std::filesystem::file_size(fake_signed) == certificate_offset,
        "signature stripping clears the directory and truncates its tail");
  Check(!lwweb::StripAuthenticodeSignature(fake_signed),
        "signature stripping is idempotent for unsigned PE files");

  const auto signed_payload_layout = directory / "signed-payload-layout.exe";
  std::filesystem::copy_file(unsigned_executable, signed_payload_layout);
  lwweb::Manifest manifest;
  manifest.mode = lwweb::AppMode::Url;
  manifest.title = "Signed payload layout";
  manifest.url = "https://example.com";
  lwweb::AppendPayload(signed_payload_layout, {}, manifest,
                       lwweb::kPayloadUrlMode);
  const auto unsigned_payload = lwweb::LoadPayload(signed_payload_layout);
  const auto expected_prefix = unsigned_payload.footer.payload_offset;
  (void)AddFakeCertificateTable(signed_payload_layout, false);
  const auto signed_payload = lwweb::LoadPayload(signed_payload_layout);
  Check(signed_payload.footer.sha256 == unsigned_payload.footer.sha256 &&
            lwweb::RunnerPrefixSize(signed_payload_layout) == expected_prefix,
        "payload loader finds the Footer before an aligned Certificate Table");

  const auto malformed = directory / "malformed.exe";
  std::filesystem::copy_file(unsigned_executable, malformed);
  (void)AddFakeCertificateTable(malformed, true);
  bool malformed_rejected = false;
  try {
    (void)lwweb::StripAuthenticodeSignature(malformed);
  } catch (...) {
    malformed_rejected = true;
  }
  Check(malformed_rejected,
        "signature stripping rejects a Certificate Table that is not the PE tail");

  Check(std::filesystem::is_regular_file(lwweb::FindSignTool()),
        "SignTool discovery finds the installed Windows SDK tool");
  bool configured_tool_rejected = false;
  try {
    (void)lwweb::FindSignTool(directory / "missing-signtool.exe");
  } catch (...) {
    configured_tool_rejected = true;
  }
  Check(configured_tool_rejected,
        "an invalid configured SignTool path is rejected");
#endif
}
