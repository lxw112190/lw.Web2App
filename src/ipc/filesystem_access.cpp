#include "lwweb/ipc/filesystem_access.h"

#include "lwweb/ipc/ipc_message.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace lwweb {
namespace {

std::string RequiredString(const nlohmann::json& params, const char* name) {
  const auto found = params.find(name);
  if (found == params.end() || !found->is_string() ||
      found->get_ref<const std::string&>().empty())
    throw IpcException("INVALID_ARGUMENT",
                       std::string(name) + " must be a non-empty string");
  return found->get<std::string>();
}

bool OptionalBool(const nlohmann::json& params, const char* name,
                  bool fallback = false) {
  const auto found = params.find(name);
  if (found == params.end()) return fallback;
  if (!found->is_boolean())
    throw IpcException("INVALID_ARGUMENT",
                       std::string(name) + " must be a boolean");
  return found->get<bool>();
}

std::string PathText(const std::filesystem::path& path) {
  return path.u8string();
}

IpcException FilesystemError(const std::error_code& error,
                             const char* operation) {
  if (error == std::errc::no_such_file_or_directory)
    return IpcException("NOT_FOUND",
                        std::string(operation) + " failed: path not found");
  if (error == std::errc::file_exists)
    return IpcException("ALREADY_EXISTS",
                        std::string(operation) + " failed: target exists");
  if (error == std::errc::permission_denied)
    return IpcException("PERMISSION_DENIED",
                        std::string(operation) + " was denied");
  return IpcException("IO_ERROR", std::string(operation) + " failed");
}

bool IsCrossDeviceMoveError(const std::error_code& error) {
#ifdef _WIN32
  return error.category() == std::system_category() &&
         error.value() == ERROR_NOT_SAME_DEVICE;
#else
  return error == std::errc::cross_device_link;
#endif
}

std::filesystem::path MoveStagingPath(
    const std::filesystem::path& destination) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::ostringstream name;
  name << ".lwweb-move-" << std::hex << ticks << '-'
       << sequence.fetch_add(1, std::memory_order_relaxed) << ".tmp";
  return destination.parent_path() / std::filesystem::u8path(name.str());
}

void PublishStagedMove(const std::filesystem::path& staging,
                       const std::filesystem::path& destination,
                       bool overwrite) {
  std::error_code error;
#ifdef _WIN32
  const DWORD flags = MOVEFILE_WRITE_THROUGH |
                      (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
  if (!MoveFileExW(staging.c_str(), destination.c_str(), flags))
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
#else
  if (overwrite) {
    std::filesystem::rename(staging, destination, error);
  } else {
    // staging 与 destination 位于同一目录，因此硬链接发布是同文件系统的
    // 原子 no-replace 操作，避免 rename 在 POSIX 上静默覆盖竞态创建的文件。
    std::filesystem::create_hard_link(staging, destination, error);
    if (!error) {
      std::error_code remove_error;
      if (!std::filesystem::remove(staging, remove_error) || remove_error)
        error = remove_error ? remove_error
                             : std::make_error_code(std::errc::io_error);
    }
  }
#endif
  if (error) throw FilesystemError(error, "Move publish");
}

}  // namespace

namespace ipc_detail {

void MoveRegularFileByCopyAndDelete(
    const std::filesystem::path& source,
    const std::filesystem::path& destination, bool overwrite) {
  std::error_code error;
  const auto source_status = std::filesystem::symlink_status(source, error);
  if (error) throw FilesystemError(error, "Move fallback");
  if (!std::filesystem::is_regular_file(source_status))
    throw IpcException(
        "UNSUPPORTED",
        "Cross-filesystem fs.move currently supports regular files only");

  std::error_code comparison_error;
  if (std::filesystem::equivalent(source, destination, comparison_error) &&
      !comparison_error)
    throw IpcException("INVALID_ARGUMENT",
                       "Move source and destination must differ");
  if (std::filesystem::exists(destination, error) && !overwrite)
    throw IpcException("ALREADY_EXISTS",
                       "Move destination already exists");
  if (error) throw FilesystemError(error, "Move fallback");

  std::filesystem::path staging;
  for (int attempt = 0; attempt < 64; ++attempt) {
    staging = MoveStagingPath(destination);
    error.clear();
    if (std::filesystem::copy_file(source, staging,
                                   std::filesystem::copy_options::none,
                                   error))
      break;
    if (error == std::errc::file_exists) continue;
    throw FilesystemError(error, "Move fallback copy");
  }
  if (staging.empty() || error) {
    throw IpcException("IO_ERROR",
                       "Move fallback could not create a staging file");
  }

  struct StagingCleanup {
    std::filesystem::path path;
    ~StagingCleanup() {
      if (path.empty()) return;
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{staging};

  PublishStagedMove(staging, destination, overwrite);
  cleanup.path.clear();

  error.clear();
  if (!std::filesystem::remove(source, error) || error) {
    if (!error) error = std::make_error_code(std::errc::io_error);
    // 目标已经完整发布，绝不因源文件删除失败而删除目标。此时返回错误，
    // 调用方可以安全地识别“两个完整副本仍存在”的可恢复状态。
    throw FilesystemError(error, "Move source removal");
  }
}

}  // namespace ipc_detail

IpcFilesystemAccess::IpcFilesystemAccess(
    std::shared_ptr<IpcFilesystemPermissions> permissions)
    : permissions_(std::move(permissions)) {
  if (!permissions_)
    throw IpcException("INTERNAL_ERROR",
                       "Filesystem permission service is unavailable");
}

void IpcFilesystemAccess::GrantDirectory(
    const std::filesystem::path& directory) {
  permissions_->AddSessionGrant(directory);
}

nlohmann::json IpcFilesystemAccess::Exists(
    const nlohmann::json& params) const {
  const auto path = permissions_->RequireDestination(
      RequiredString(params, "path"));
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) throw FilesystemError(error, "Exists check");
  return {{"exists", exists}};
}

nlohmann::json IpcFilesystemAccess::List(
    const nlohmann::json& params) const {
  const auto path = permissions_->RequireExisting(
      RequiredString(params, "path"));
  std::error_code error;
  if (!std::filesystem::is_directory(path, error))
    throw IpcException("INVALID_ARGUMENT",
                       "fs.list path must be a directory");
  nlohmann::json entries = nlohmann::json::array();
  std::size_t count = 0;
  for (std::filesystem::directory_iterator iterator(path, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (++count > 10000)
      throw IpcException("IO_ERROR",
                         "Directory entry count exceeds the safety limit");
    const auto& item = *iterator;
    const auto status = item.symlink_status(error);
    if (error) break;
    nlohmann::json entry = {
        {"name", item.path().filename().u8string()},
        {"path", item.path().u8string()},
        {"type", std::filesystem::is_symlink(status)
                     ? "symlink"
                     : std::filesystem::is_directory(status) ? "directory"
                                                              : "file"}};
    if (std::filesystem::is_regular_file(status)) {
      const auto size = item.file_size(error);
      if (!error) entry["size"] = size;
    }
    entries.push_back(std::move(entry));
  }
  if (error) throw FilesystemError(error, "Directory listing");
  return {{"entries", std::move(entries)}};
}

nlohmann::json IpcFilesystemAccess::Copy(
    const nlohmann::json& params) const {
  const auto source = permissions_->RequireExisting(
      RequiredString(params, "from"));
  const auto destination = permissions_->RequireDestination(
      RequiredString(params, "to"));
  const bool overwrite = OptionalBool(params, "overwrite");

  std::error_code error;
  const auto source_status = std::filesystem::symlink_status(source, error);
  if (error) throw FilesystemError(error, "Copy");
  if (!std::filesystem::is_regular_file(source_status))
    throw IpcException("UNSUPPORTED",
                       "fs.copy currently supports regular files only");
  if (std::filesystem::exists(destination, error)) {
    std::error_code comparison_error;
    if (std::filesystem::equivalent(source, destination, comparison_error) &&
        !comparison_error)
      throw IpcException("INVALID_ARGUMENT",
                         "Copy source and destination must differ");
    if (!overwrite)
      throw IpcException("ALREADY_EXISTS",
                         "Copy destination already exists");
  }
  if (error) throw FilesystemError(error, "Copy");

  const auto options = overwrite
                           ? std::filesystem::copy_options::overwrite_existing
                           : std::filesystem::copy_options::none;
  if (!std::filesystem::copy_file(source, destination, options, error)) {
    if (!error)
      throw IpcException("IO_ERROR", "Copy did not create the destination");
    throw FilesystemError(error, "Copy");
  }
  return {{"path", PathText(destination)}};
}

nlohmann::json IpcFilesystemAccess::Move(
    const nlohmann::json& params) const {
  const auto source = permissions_->RequireExisting(
      RequiredString(params, "from"));
  const auto destination = permissions_->RequireDestination(
      RequiredString(params, "to"));
  const bool overwrite = OptionalBool(params, "overwrite");
  std::error_code comparison_error;
  if (std::filesystem::equivalent(source, destination, comparison_error) &&
      !comparison_error)
    throw IpcException("INVALID_ARGUMENT",
                       "Move source and destination must differ");
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    if (!overwrite)
      throw IpcException("ALREADY_EXISTS",
                         "Move destination already exists");
  }
  if (error) throw FilesystemError(error, "Move");
#ifdef _WIN32
  const DWORD flags = MOVEFILE_WRITE_THROUGH |
                      (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
  if (!MoveFileExW(source.c_str(), destination.c_str(), flags)) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
  }
#else
  std::filesystem::rename(source, destination, error);
#endif
  if (IsCrossDeviceMoveError(error)) {
    ipc_detail::MoveRegularFileByCopyAndDelete(source, destination, overwrite);
    error.clear();
  }
  if (error) throw FilesystemError(error, "Move");
  return {{"path", PathText(destination)}};
}

nlohmann::json IpcFilesystemAccess::Delete(
    const nlohmann::json& params) const {
  const auto path = permissions_->RequireExisting(
      RequiredString(params, "path"));
  const bool recursive = OptionalBool(params, "recursive");
  std::error_code error;
  if (recursive)
    (void)std::filesystem::remove_all(path, error);
  else if (!std::filesystem::remove(path, error) && !error)
    throw IpcException("IO_ERROR", "Directory is not empty");
  if (error) throw FilesystemError(error, "Delete");
  return nlohmann::json::object();
}

}  // namespace lwweb
