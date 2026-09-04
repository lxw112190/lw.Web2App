#include "lwweb/runtime/system_paths.h"

#include "lwweb/ipc/ipc_message.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <string_view>

#ifdef _WIN32
#include <ShlObj.h>
#endif

namespace lwweb {
namespace {

std::filesystem::path EnvironmentPath(const char* name) {
  const auto* value = std::getenv(name);
  return value && *value ? std::filesystem::u8path(value) : std::filesystem::path{};
}

std::filesystem::path HomePath() {
#ifdef _WIN32
  auto home = EnvironmentPath("USERPROFILE");
  if (home.empty()) home = EnvironmentPath("HOME");
#else
  auto home = EnvironmentPath("HOME");
#endif
  if (home.empty()) throw IpcException("UNSUPPORTED", "User home directory is unavailable");
  return home;
}

#ifdef _WIN32
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id,
                                  const std::filesystem::path& fallback) {
  PWSTR value = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) && value) {
    std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
  }
  if (value) CoTaskMemFree(value);
  return fallback;
}
#else
std::filesystem::path LinuxUserDir(const std::filesystem::path& home,
                                   const char* variable, const char* fallback) {
  const auto config = EnvironmentPath("XDG_CONFIG_HOME").empty()
                          ? home / ".config" / "user-dirs.dirs"
                          : EnvironmentPath("XDG_CONFIG_HOME") / "user-dirs.dirs";
  std::ifstream input(config);
  std::string line;
  const std::string key = std::string(variable) + "=";
  while (std::getline(input, line)) {
    if (line.rfind(key, 0) != 0) continue;
    auto value = line.substr(key.size());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    constexpr std::string_view home_token = "$HOME";
    if (value.rfind(home_token, 0) == 0)
      return home / value.substr(home_token.size() + (value.size() > home_token.size() && value[home_token.size()] == '/' ? 1 : 0));
    if (!value.empty() && value.front() == '/') return std::filesystem::u8path(value);
  }
  return home / fallback;
}
#endif

}  // namespace

std::filesystem::path SystemPaths::Resolve(const std::string& name,
                                            const std::string& app_id) {
  if (name.empty() || name.size() > 64)
    throw IpcException("INVALID_ARGUMENT", "Path name is invalid");
  const auto home = HomePath();
  const auto app = std::filesystem::u8path(app_id.empty() ? "default" : app_id);
#ifdef _WIN32
  const auto local = KnownFolder(FOLDERID_LocalAppData, home / "AppData" / "Local");
  const std::map<std::string, std::filesystem::path> paths = {
      {"home", home},
      {"desktop", KnownFolder(FOLDERID_Desktop, home / "Desktop")},
      {"documents", KnownFolder(FOLDERID_Documents, home / "Documents")},
      {"pictures", KnownFolder(FOLDERID_Pictures, home / "Pictures")},
      {"downloads", KnownFolder(FOLDERID_Downloads, home / "Downloads")},
      {"appData", local / "lw.Web2App" / "apps" / app / "data"},
      {"appCache", local / "lw.Web2App" / "apps" / app / "cache"},
      {"temp", std::filesystem::temp_directory_path()}};
#else
  const auto data_home = EnvironmentPath("XDG_DATA_HOME").empty()
                             ? home / ".local" / "share"
                             : EnvironmentPath("XDG_DATA_HOME");
  const auto cache_home = EnvironmentPath("XDG_CACHE_HOME").empty()
                              ? home / ".cache"
                              : EnvironmentPath("XDG_CACHE_HOME");
  const std::map<std::string, std::filesystem::path> paths = {
      {"home", home},
      {"desktop", LinuxUserDir(home, "XDG_DESKTOP_DIR", "Desktop")},
      {"documents", LinuxUserDir(home, "XDG_DOCUMENTS_DIR", "Documents")},
      {"pictures", LinuxUserDir(home, "XDG_PICTURES_DIR", "Pictures")},
      {"downloads", LinuxUserDir(home, "XDG_DOWNLOAD_DIR", "Downloads")},
      {"appData", data_home / "lw.Web2App" / "apps" / app / "data"},
      {"appCache", cache_home / "lw.Web2App" / "apps" / app / "cache"},
      {"temp", std::filesystem::temp_directory_path()}};
#endif
  const auto found = paths.find(name);
  if (found == paths.end())
    throw IpcException("INVALID_ARGUMENT", "Unknown system path name");
  return found->second.lexically_normal();
}

}  // namespace lwweb
