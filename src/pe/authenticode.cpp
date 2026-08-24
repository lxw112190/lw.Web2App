#include "lwweb/pe/authenticode.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/packer.h"

#include <Windows.h>
#include <Softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace lwweb {
namespace {

struct SecurityDirectoryInfo {
  std::uint64_t directory_offset = 0;
  std::uint64_t certificate_offset = 0;
  std::uint64_t certificate_size = 0;
};

template <typename Value>
Value ReadAt(std::ifstream& input, std::uint64_t offset,
             const char* description) {
  Value value{};
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!input) throw Error(std::string("Cannot read PE ") + description);
  return value;
}

SecurityDirectoryInfo ReadSecurityDirectory(
    const std::filesystem::path& executable) {
  const auto file_size = FileSize(executable);
  std::ifstream input(executable, std::ios::binary);
  if (!input) throw Error("Cannot open PE executable");
  const auto dos = ReadAt<IMAGE_DOS_HEADER>(input, 0, "DOS header");
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
    throw Error("PE DOS header is invalid");
  const auto nt_offset = static_cast<std::uint64_t>(dos.e_lfanew);
  if (nt_offset > file_size || sizeof(DWORD) > file_size - nt_offset)
    throw Error("PE header offset is outside the file");
  const auto signature = ReadAt<DWORD>(input, nt_offset, "signature");
  if (signature != IMAGE_NT_SIGNATURE) throw Error("PE signature is invalid");
  const auto file_header_offset = nt_offset + sizeof(DWORD);
  const auto file_header =
      ReadAt<IMAGE_FILE_HEADER>(input, file_header_offset, "file header");
  const auto optional_offset = file_header_offset + sizeof(IMAGE_FILE_HEADER);
  if (optional_offset > file_size ||
      file_header.SizeOfOptionalHeader > file_size - optional_offset)
    throw Error("PE optional header is outside the file");
  const auto magic = ReadAt<WORD>(input, optional_offset,
                                  "optional header magic");
  std::size_t data_directory_offset = 0;
  if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    data_directory_offset =
        offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
        IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY);
  } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    data_directory_offset =
        offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
        IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY);
  } else {
    throw Error("PE optional header magic is unsupported");
  }
  if (data_directory_offset + sizeof(IMAGE_DATA_DIRECTORY) >
      file_header.SizeOfOptionalHeader)
    throw Error("PE Security Directory is missing");
  const auto directory_file_offset = optional_offset + data_directory_offset;
  const auto directory = ReadAt<IMAGE_DATA_DIRECTORY>(
      input, directory_file_offset, "Security Directory");
  if (directory.VirtualAddress == 0 && directory.Size == 0)
    return {directory_file_offset, 0, 0};
  if (directory.VirtualAddress == 0 || directory.Size == 0)
    throw Error("PE Security Directory is inconsistent");

  const auto certificate_offset =
      static_cast<std::uint64_t>(directory.VirtualAddress);
  const auto certificate_size = static_cast<std::uint64_t>(directory.Size);
  if ((certificate_offset & 7u) != 0 ||
      certificate_offset < optional_offset + file_header.SizeOfOptionalHeader ||
      certificate_offset > file_size ||
      certificate_size > file_size - certificate_offset ||
      certificate_offset + certificate_size != file_size)
    throw Error("Authenticode Certificate Table is not a valid PE tail");
  if (certificate_size < sizeof(WIN_CERTIFICATE))
    throw Error("Authenticode Certificate Table is truncated");
  const auto certificate = ReadAt<WIN_CERTIFICATE>(
      input, certificate_offset, "certificate header");
  if (certificate.dwLength < sizeof(WIN_CERTIFICATE) ||
      certificate.dwLength > certificate_size ||
      certificate.wRevision != WIN_CERT_REVISION_2_0 ||
      certificate.wCertificateType != WIN_CERT_TYPE_PKCS_SIGNED_DATA)
    throw Error("Authenticode certificate header is invalid");
  return {directory_file_offset, certificate_offset, certificate_size};
}

std::string NormalizeThumbprint(const std::string& value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isspace(byte)) continue;
    if (!std::isxdigit(byte))
      throw Error("Code-signing certificate thumbprint must be hexadecimal");
    normalized.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
  }
  if (normalized.size() != 40)
    throw Error("Code-signing certificate thumbprint must contain 40 SHA-1 hex digits");
  return normalized;
}

std::wstring QuoteArgument(const std::wstring& value) {
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (const auto character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'\"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(character);
    }
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

void RunTool(const std::filesystem::path& executable,
             const std::vector<std::wstring>& arguments,
             const char* operation) {
  std::wstring command = QuoteArgument(executable.wstring());
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command += QuoteArgument(argument);
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                      nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup, &process))
    throw Error(std::string("Cannot start ") + operation + ": " +
                WindowsErrorMessage(GetLastError()));
  CloseHandle(process.hThread);
  const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  const bool read_exit = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  CloseHandle(process.hProcess);
  if (wait != WAIT_OBJECT_0 || !read_exit)
    throw Error(std::string(operation) + " did not finish correctly");
  if (exit_code != 0)
    throw Error(std::string(operation) + " failed with exit code " +
                std::to_string(exit_code));
}

std::filesystem::path SearchPathForSignTool() {
  const auto required =
      SearchPathW(nullptr, L"signtool.exe", nullptr, 0, nullptr, nullptr);
  if (required == 0) return {};
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
  if (SearchPathW(nullptr, L"signtool.exe", nullptr,
                  static_cast<DWORD>(buffer.size()), buffer.data(), nullptr) == 0)
    return {};
  return std::filesystem::path(buffer.data());
}

std::filesystem::path WindowsKitsSignTool() {
  std::array<wchar_t, 32768> program_files{};
  const auto length = GetEnvironmentVariableW(
      L"ProgramFiles(x86)", program_files.data(),
      static_cast<DWORD>(program_files.size()));
  const auto root = length > 0 && length < program_files.size()
                        ? std::filesystem::path(program_files.data()) /
                              L"Windows Kits" / L"10" / L"bin"
                        : std::filesystem::path(L"C:\\Program Files (x86)") /
                              L"Windows Kits" / L"10" / L"bin";
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) return {};
  const auto direct = root / L"x64" / L"signtool.exe";
  if (std::filesystem::is_regular_file(direct, error)) return direct;
  std::vector<std::filesystem::path> candidates;
  for (std::filesystem::directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error)) continue;
    const auto candidate = iterator->path() / L"x64" / L"signtool.exe";
    if (std::filesystem::is_regular_file(candidate, error))
      candidates.push_back(candidate);
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
              return left.parent_path().parent_path().filename().wstring() >
                     right.parent_path().parent_path().filename().wstring();
            });
  return candidates.empty() ? std::filesystem::path{} : candidates.front();
}

}  // namespace

bool HasAuthenticodeSignature(const std::filesystem::path& executable) {
  const auto directory = ReadSecurityDirectory(executable);
  return directory.certificate_size != 0;
}

std::optional<std::uint64_t> AuthenticodeContentEnd(
    const std::filesystem::path& executable) {
  const auto directory = ReadSecurityDirectory(executable);
  if (directory.certificate_size == 0) return std::nullopt;
  return directory.certificate_offset;
}

bool StripAuthenticodeSignature(const std::filesystem::path& executable) {
  const auto directory = ReadSecurityDirectory(executable);
  if (directory.certificate_size == 0) return false;
  {
    std::fstream stream(executable,
                        std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) throw Error("Cannot open PE executable for signature removal");
    const IMAGE_DATA_DIRECTORY empty{};
    stream.seekp(static_cast<std::streamoff>(directory.directory_offset));
    stream.write(reinterpret_cast<const char*>(&empty), sizeof(empty));
    stream.flush();
    if (!stream) throw Error("Cannot clear PE Security Directory");
  }
  std::error_code error;
  std::filesystem::resize_file(executable, directory.certificate_offset,
                               error);
  if (error)
    throw Error("Cannot remove Authenticode Certificate Table: " +
                error.message());
  return true;
}

std::filesystem::path FindSignTool(
    const std::filesystem::path& configured) {
  std::error_code error;
  if (!configured.empty()) {
    const auto absolute = std::filesystem::absolute(configured, error);
    if (error || !std::filesystem::is_regular_file(absolute, error))
      throw Error("Configured SignTool does not exist");
    return absolute;
  }
  const auto from_path = SearchPathForSignTool();
  if (!from_path.empty()) return from_path;
  const auto from_kits = WindowsKitsSignTool();
  if (!from_kits.empty()) return from_kits;
  throw Error("SignTool was not found. Install the Windows SDK or specify --signtool");
}

void SignAuthenticode(const std::filesystem::path& executable,
                      const SigningConfig& config) {
  if (!config.enabled)
    throw Error("Authenticode signing is not enabled");
  const auto thumbprint = NormalizeThumbprint(config.certificate_thumbprint);
  if (!config.timestamp_url.empty() &&
      !IsSupportedHttpUrl(config.timestamp_url))
    throw Error("Timestamp URL must use http:// or https://");
  const auto signtool = FindSignTool(config.signtool);
  std::vector<std::wstring> arguments = {
      L"sign", L"/sha1", Utf8ToWide(thumbprint), L"/fd", L"SHA256"};
  if (!config.timestamp_url.empty()) {
    arguments.insert(arguments.end(),
                     {L"/tr", Utf8ToWide(config.timestamp_url), L"/td",
                      L"SHA256"});
  }
  arguments.push_back(executable.wstring());
  RunTool(signtool, arguments, "SignTool signing");
  if (config.verify_after_sign) VerifyAuthenticodeSignature(executable);
}

void VerifyAuthenticodeSignature(
    const std::filesystem::path& executable) {
  auto path = executable.wstring();
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const auto status = WinVerifyTrust(nullptr, &policy, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  (void)WinVerifyTrust(nullptr, &policy, &trust);
  if (status != ERROR_SUCCESS) {
    std::ostringstream message;
    message << "Authenticode verification failed, HRESULT=0x" << std::hex
            << std::uppercase
            << static_cast<unsigned long>(status);
    throw Error(message.str());
  }
}

}  // namespace lwweb
