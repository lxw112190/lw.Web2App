#include "lwweb/ipc/ipc_permissions.h"

#include "lwweb/common/file_utils.h"
#include "lwweb/common/http_origin.h"
#include "lwweb/common/logging.h"
#include "lwweb/ipc/ipc_message.h"
#include "lwweb/runtime/system_paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <optional>
#include <utility>


namespace lwweb {
namespace {

std::filesystem::path ExpandRoot(const std::string& value, const std::string& app_id) {
  const auto home = SystemPaths::Resolve("home", app_id);
  const std::map<std::string, std::filesystem::path> variables = {
      {"${HOME}", home},
      {"${DESKTOP}", SystemPaths::Resolve("desktop", app_id)},
      {"${DOCUMENTS}", SystemPaths::Resolve("documents", app_id)},
      {"${PICTURES}", SystemPaths::Resolve("pictures", app_id)},
      {"${DOWNLOADS}", SystemPaths::Resolve("downloads", app_id)},
      {"${APP_DATA}", SystemPaths::Resolve("appData", app_id)},
      {"${APP_CACHE}", SystemPaths::Resolve("appCache", app_id)}};
  const auto exact = variables.find(value);
  if (exact != variables.end()) return exact->second;
  for (const auto& item : variables) {
    if (value.rfind(item.first + "/", 0) == 0 ||
        value.rfind(item.first + "\\", 0) == 0) {
      return item.second / std::filesystem::u8path(value.substr(item.first.size() + 1));
    }
  }
  return std::filesystem::u8path(value);
}

bool UnsafeWindowsNamespace(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto text = path.native();
  return text.rfind(L"\\\\?\\", 0) == 0 || text.rfind(L"\\\\.\\", 0) == 0 ||
         text.rfind(L"\\\\", 0) == 0 || text.find(L':', 2) != std::wstring::npos;
#else
  (void)path;
  return false;
#endif
}

std::filesystem::path CanonicalDirectory(const std::filesystem::path& path) {
  std::error_code error;
  auto result = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_directory(result, error))
    throw IpcException("NOT_FOUND", "Authorized filesystem root is unavailable");
  return result.lexically_normal();
}

struct ResolvedPrefix {
  std::filesystem::path existing;
  std::filesystem::path canonical;
  std::vector<std::filesystem::path> suffix;
};

ResolvedPrefix ResolveExistingPrefix(const std::filesystem::path& path) {
  auto current = path;
  std::vector<std::filesystem::path> suffix;
  std::error_code error;
  while (!current.empty() && !std::filesystem::exists(current, error)) {
    if (error) throw IpcException("NOT_FOUND", "Path parent is unavailable");
    const auto component = current.filename();
    if (!component.empty()) suffix.push_back(component);
    const auto parent = current.parent_path();
    if (parent == current) break;
    current = parent;
  }
  if (error || current.empty() || !std::filesystem::exists(current, error))
    throw IpcException("NOT_FOUND", "Path parent does not exist");
  auto canonical = std::filesystem::canonical(current, error);
  if (error) throw IpcException("NOT_FOUND", "Path parent does not exist");
  std::reverse(suffix.begin(), suffix.end());
  return {current, canonical.lexically_normal(), std::move(suffix)};
}

std::filesystem::path ResolveCandidatePath(const std::filesystem::path& path) {
  const auto prefix = ResolveExistingPrefix(path);
  auto resolved = prefix.canonical;
  for (const auto& component : prefix.suffix) resolved /= component;
  return resolved.lexically_normal();
}

bool ComponentEqual(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
#ifdef _WIN32
  auto a = left.native();
  auto b = right.native();
  std::transform(a.begin(), a.end(), a.begin(), ::towlower);
  std::transform(b.begin(), b.end(), b.begin(), ::towlower);
  return a == b;
#else
  return left == right;
#endif
}

bool IsDescendant(const std::filesystem::path& path,
                  const std::filesystem::path& root) {
  auto path_it = path.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || !ComponentEqual(*path_it, *root_it)) return false;
  }
  return true;
}

std::optional<HttpOrigin> SourceOrigin(const std::string& source) {
  const auto scheme = source.find("://");
  if (scheme == std::string::npos) return std::nullopt;
  const auto authority_end = source.find_first_of("/?#", scheme + 3);
  return ParseHttpOrigin(source.substr(0, authority_end));
}

}  // namespace

bool HasIpcCapability(const IpcConfig& config, const std::string& capability) {
  return config.enabled &&
         std::find(config.capabilities.begin(), config.capabilities.end(), capability) !=
             config.capabilities.end();
}

bool IsAllowedIpcSource(const std::string& source_url,
                        const std::string& local_origin) {
  const auto source = SourceOrigin(source_url);
  const auto expected = ParseHttpOrigin(local_origin);
  return source && expected && source->scheme == "http" &&
         source->host == "127.0.0.1" && source->scheme == expected->scheme &&
         source->host == expected->host && source->port == expected->port;
}

IpcFilesystemPermissions::IpcFilesystemPermissions(
    std::vector<std::string> configured_roots, std::string app_id) {
  for (const auto& configured : configured_roots) {
    auto root = ExpandRoot(configured, app_id);
    if (configured == "${APP_DATA}" || configured.rfind("${APP_DATA}/", 0) == 0 ||
        configured == "${APP_CACHE}" || configured.rfind("${APP_CACHE}/", 0) == 0) {
      std::error_code ignored;
      std::filesystem::create_directories(root, ignored);
    }
    if (root.empty() || !root.is_absolute() || UnsafeWindowsNamespace(root))
      throw IpcException("INVALID_ARGUMENT", "Configured filesystem root is invalid");
    std::error_code exists_error;
    if (std::filesystem::exists(root, exists_error)) {
      if (exists_error) throw IpcException("INVALID_ARGUMENT", "Configured filesystem root is invalid");
      configured_roots_.push_back(
          {root.lexically_normal(), CanonicalDirectory(root), false});
    } else {
      if (exists_error) throw IpcException("INVALID_ARGUMENT", "Configured filesystem root is invalid");
      configured_roots_.push_back(
          {root.lexically_normal(), ResolveCandidatePath(root), true});
    }
  }
}

void IpcFilesystemPermissions::AddSessionGrant(
    const std::filesystem::path& directory) {
  const auto canonical = CanonicalDirectory(directory);
  std::lock_guard lock(mutex_);
  if (std::find(session_grants_.begin(), session_grants_.end(), canonical) ==
      session_grants_.end())
    session_grants_.push_back(canonical);
}

bool IpcFilesystemPermissions::IsAuthorized(
    const std::filesystem::path& requested,
    const std::filesystem::path& resolved) const {
  for (const auto& root : configured_roots_) {
    if (root.pending_at_start) {
      if (IsDescendant(requested, root.declared_root) &&
          IsDescendant(resolved, root.policy_root))
        return true;
    } else if (IsDescendant(resolved, root.policy_root)) {
      return true;
    }
  }
  std::lock_guard lock(mutex_);
  for (const auto& root : session_grants_)
    if (IsDescendant(resolved, root)) return true;
  return false;
}

std::filesystem::path IpcFilesystemPermissions::RequireExisting(
    const std::string& utf8_path) const {
  if (utf8_path.empty() || utf8_path.find('\0') != std::string::npos)
    throw IpcException("INVALID_ARGUMENT", "Path is invalid");
  const auto requested = std::filesystem::absolute(std::filesystem::u8path(utf8_path))
                             .lexically_normal();
  if (!std::filesystem::u8path(utf8_path).is_absolute() ||
      UnsafeWindowsNamespace(requested))
    throw IpcException("INVALID_ARGUMENT", "Path must be an absolute local path");
  std::error_code error;
  const auto canonical = std::filesystem::canonical(requested, error);
  if (error) throw IpcException("NOT_FOUND", "Path does not exist");
  if (!IsAuthorized(requested, canonical))
    throw IpcException("PERMISSION_DENIED", "Path is outside authorized roots");
  return requested;
}

std::filesystem::path IpcFilesystemPermissions::RequireDestination(
    const std::string& utf8_path) const {
  if (utf8_path.empty() || utf8_path.find('\0') != std::string::npos)
    throw IpcException("INVALID_ARGUMENT", "Path is invalid");
  const auto input = std::filesystem::u8path(utf8_path);
  if (!input.is_absolute())
    throw IpcException("INVALID_ARGUMENT", "Path must be an absolute local path");
  const auto requested = std::filesystem::absolute(input).lexically_normal();
  if (UnsafeWindowsNamespace(requested))
    throw IpcException("INVALID_ARGUMENT", "Windows device and UNC paths are not supported");
  std::error_code error;
  const auto resolved = ResolveCandidatePath(requested);
  if (!IsAuthorized(requested, resolved))
    throw IpcException("PERMISSION_DENIED", "Destination is outside authorized roots");
  return requested;
}

std::vector<std::filesystem::path> IpcFilesystemPermissions::Roots() const {
  std::vector<std::filesystem::path> roots;
  roots.reserve(configured_roots_.size());
  for (const auto& root : configured_roots_) roots.push_back(root.policy_root);
  std::lock_guard lock(mutex_);
  roots.insert(roots.end(), session_grants_.begin(), session_grants_.end());
  return roots;
}

}  // namespace lwweb
