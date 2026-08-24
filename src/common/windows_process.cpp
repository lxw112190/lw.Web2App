#include "lwweb/common/windows_process.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

constexpr std::uintmax_t kMaxCapturedToolOutput = 64u * 1024u;

// 简单的 Win32 HANDLE 所有权封装，确保启动、等待和异常路径都不会泄漏句柄。
class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  ~UniqueHandle() { Reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  HANDLE Get() const { return handle_; }
  bool Valid() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  void Reset(HANDLE handle = nullptr) {
    if (Valid()) CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

// 管理用于收集外部工具输出的临时文件；文件内容只在失败时进入错误信息。
class TemporaryOutputFile {
 public:
  TemporaryOutputFile() {
    std::vector<wchar_t> directory(32768, L'\0');
    const auto length = GetTempPathW(static_cast<DWORD>(directory.size()),
                                     directory.data());
    if (length == 0 || length >= directory.size())
      throw Error("Cannot locate the Windows temporary directory: " +
                  WindowsErrorMessage(GetLastError()));
    std::vector<wchar_t> filename(MAX_PATH + 1, L'\0');
    if (GetTempFileNameW(directory.data(), L"LWP", 0, filename.data()) == 0)
      throw Error("Cannot create external tool output file: " +
                  WindowsErrorMessage(GetLastError()));
    path_ = filename.data();
  }

  ~TemporaryOutputFile() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::wstring QuoteWindowsArgument(const std::wstring& value) {
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

std::string ReadCapturedOutput(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0) return {};
  const auto retained = std::min(size, kMaxCapturedToolOutput);
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  if (size > retained)
    input.seekg(static_cast<std::streamoff>(size - retained));
  std::string output(static_cast<std::size_t>(retained), '\0');
  input.read(output.data(), static_cast<std::streamsize>(output.size()));
  output.resize(static_cast<std::size_t>(input.gcount()));
  std::replace(output.begin(), output.end(), '\0', '?');
  while (!output.empty() &&
         std::isspace(static_cast<unsigned char>(output.back())))
    output.pop_back();
  if (size > retained) output.insert(0, "[earlier output truncated]\n");
  return output;
}

std::string OutputSuffix(const std::string& output) {
  return output.empty() ? std::string{} : "\nCaptured output:\n" + output;
}

DWORD TimeoutMilliseconds(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0)
    throw Error("External tool timeout must be positive");
  const auto maximum = static_cast<long long>(
      std::numeric_limits<DWORD>::max() - 1u);
  return static_cast<DWORD>(std::min(timeout.count(), maximum));
}

}  // namespace

void RunWindowsExternalTool(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    std::chrono::milliseconds timeout, const std::string& operation) {
  if (operation.empty()) throw Error("External tool operation is empty");
  const auto timeout_ms = TimeoutMilliseconds(timeout);
  std::wstring command = QuoteWindowsArgument(executable.wstring());
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command += QuoteWindowsArgument(argument);
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  TemporaryOutputFile output_file;
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  UniqueHandle output(CreateFileW(
      output_file.Path().c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security,
      CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (!output.Valid())
    throw Error("Cannot open external tool output file: " +
                WindowsErrorMessage(GetLastError()));
  UniqueHandle input(CreateFileW(L"NUL", GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, OPEN_EXISTING, 0, nullptr));
  if (!input.Valid())
    throw Error("Cannot open external tool input: " +
                WindowsErrorMessage(GetLastError()));

  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input.Get();
  startup.hStdOutput = output.Get();
  startup.hStdError = output.Get();
  PROCESS_INFORMATION raw_process{};
  const auto directory = working_directory.empty()
                             ? std::wstring{}
                             : working_directory.wstring();
  if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                      nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                      directory.empty() ? nullptr : directory.c_str(),
                      &startup, &raw_process))
    throw Error("Cannot start " + operation + ": " +
                WindowsErrorMessage(GetLastError()));
  UniqueHandle process(raw_process.hProcess);
  UniqueHandle thread(raw_process.hThread);
  output.Reset();
  input.Reset();

  const auto wait = WaitForSingleObject(process.Get(), timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    (void)TerminateProcess(process.Get(), ERROR_TIMEOUT);
    (void)WaitForSingleObject(process.Get(), 5000);
    const auto captured = ReadCapturedOutput(output_file.Path());
    const auto timeout_seconds =
        (static_cast<std::uint64_t>(timeout_ms) + 999u) / 1000u;
    throw Error(operation + " timed out after " +
                std::to_string(timeout_seconds) + " seconds" +
                OutputSuffix(captured));
  }
  if (wait != WAIT_OBJECT_0) {
    const auto wait_error = GetLastError();
    (void)TerminateProcess(process.Get(), 1);
    (void)WaitForSingleObject(process.Get(), 5000);
    throw Error(operation + " wait failed: " +
                WindowsErrorMessage(wait_error));
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process.Get(), &exit_code))
    throw Error(operation + " exit code is unavailable: " +
                WindowsErrorMessage(GetLastError()));
  if (exit_code != 0) {
    const auto captured = ReadCapturedOutput(output_file.Path());
    throw Error(operation + " failed with exit code " +
                std::to_string(exit_code) + OutputSuffix(captured));
  }
}

}  // namespace lwweb
