#include "lwweb/common/file_utils.h"

#include "lwweb/common/error.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <codecvt>
#include <locale>
#endif

#include <fstream>
#include <limits>
#include <sstream>

namespace lwweb {

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw Error("Cannot open file: " + path.u8string());
  const auto end = input.tellg();
  if (end < 0) throw Error("Cannot determine file size");
  if (static_cast<unsigned long long>(end) > std::numeric_limits<std::size_t>::max())
    throw Error("File is too large for this process");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!input && !bytes.empty()) throw Error("Cannot read file");
  return bytes;
}

std::string ReadFileText(const std::filesystem::path& path) {
  const auto bytes = ReadFileBytes(path);
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void WriteFileBytes(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw Error("Cannot create file: " + path.u8string());
  if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
  if (!output) throw Error("Cannot write file");
}

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw Error("Cannot determine file size: " + error.message());
  return size;
}

std::filesystem::path CurrentExecutablePath() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size())
    throw Error("Cannot resolve current executable path");
  buffer.resize(length);
  return buffer;
#else
  std::vector<char> buffer(4096);
  for (;;) {
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) throw Error("Cannot resolve current executable path: " +
                                std::string(std::strerror(errno)));
    if (static_cast<std::size_t>(length) < buffer.size())
      return std::filesystem::path(std::string(buffer.data(), length));
    buffer.resize(buffer.size() * 2);
  }
#endif
}

std::string WideToUtf8(const std::wstring& value) {
#ifdef _WIN32
  if (value.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
  if (length <= 0) throw Error("Invalid UTF-16 text");
  std::string output(length, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), output.data(), length, nullptr,
                      nullptr);
  return output;
#else
  try {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(value);
  } catch (...) {
    throw Error("Invalid wide-character text");
  }
#endif
}

std::wstring Utf8ToWide(const std::string& value) {
#ifdef _WIN32
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) throw Error("Invalid UTF-8 text");
  std::wstring output(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), output.data(), length);
  return output;
#else
  try {
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(value);
  } catch (...) {
    throw Error("Invalid UTF-8 text");
  }
#endif
}

std::string WindowsErrorMessage(unsigned long error) {
#ifdef _WIN32
  wchar_t* text = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring message = length ? std::wstring(text, length) : L"Unknown error";
  if (text) LocalFree(text);
  return WideToUtf8(message);
#else
  return std::strerror(static_cast<int>(error));
#endif
}

}  // namespace lwweb
