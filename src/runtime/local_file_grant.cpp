#include "lwweb/runtime/local_file_grant.h"

#include "lwweb/common/error.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace lwweb {
namespace {

constexpr std::size_t kGrantTokenBytes = 16;
constexpr std::size_t kMaxSessionGrants = 1024;

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

std::string RandomGrantId() {
  std::array<unsigned char, kGrantTokenBytes> bytes{};
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw Error("Cannot generate a local file grant token");
#else
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw Error("Cannot generate a local file grant token");
#endif
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : bytes) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

std::string EncodePathSegment(const std::string& value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
      encoded.push_back(static_cast<char>(byte));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[byte >> 4]);
      encoded.push_back(hex[byte & 0x0f]);
    }
  }
  return encoded;
}

std::optional<std::filesystem::path> CurrentCanonicalFile(
    const std::filesystem::path& path, std::uint64_t expected_size) {
  if (path.empty() || !path.is_absolute() || UnsafeWindowsNamespace(path))
    return std::nullopt;
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || UnsafeWindowsNamespace(canonical) ||
      !std::filesystem::is_regular_file(canonical, error) || error)
    return std::nullopt;
  const auto size = std::filesystem::file_size(canonical, error);
  if (error || size != expected_size) return std::nullopt;
  return canonical.lexically_normal();
}

}  // namespace

LocalFileGrantManager::LocalFileGrantManager(const Logger* logger) : logger_(logger) {}

LocalFileGrant LocalFileGrantManager::Create(const std::filesystem::path& selected) {
  if (selected.empty() || !selected.is_absolute() || UnsafeWindowsNamespace(selected))
    throw Error("Selected file must be an absolute local path");
  std::error_code error;
  auto canonical = std::filesystem::canonical(selected, error);
  if (error || UnsafeWindowsNamespace(canonical) ||
      !std::filesystem::is_regular_file(canonical, error) || error)
    throw Error("Selected item is not a supported local file");
  canonical = canonical.lexically_normal();
  const auto size = std::filesystem::file_size(canonical, error);
  if (error || size > std::numeric_limits<std::size_t>::max())
    throw Error("Selected file is too large for this platform");
  const auto name = canonical.filename().u8string();
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
      std::any_of(name.begin(), name.end(), [](unsigned char character) {
        return character == 0 || character < 0x20 || character == 0x7f;
      }))
    throw Error("Selected file has an unsafe display name");

  LocalFileGrant grant;
  grant.name = name;
  grant.size = size;
  grant.mime = MimeTypeForPath(name);
  grant.path = canonical;
  {
    std::lock_guard lock(mutex_);
    if (grants_.size() >= kMaxSessionGrants)
      throw Error("Local file grant count exceeds the session limit");
    do {
      grant.id = RandomGrantId();
    } while (grants_.find(grant.id) != grants_.end());
    grant.url = "/__lw_file__/" + grant.id + "/" + EncodePathSegment(grant.name);
    grants_.emplace(grant.id, grant);
  }
  if (logger_) {
    logger_->Info("Local file grant created");
    if (logger_->DebugEnabled())
      logger_->Debug("Local file grant: id=" + grant.id.substr(0, 8) +
                     ", size=" + std::to_string(grant.size) +
                     ", mime=" + grant.mime);
  }
  return grant;
}

std::optional<LocalFileGrant> LocalFileGrantManager::Find(
    const std::string& id) const {
  LocalFileGrant grant;
  {
    std::lock_guard lock(mutex_);
    const auto found = grants_.find(id);
    if (found == grants_.end()) return std::nullopt;
    grant = found->second;
  }
  const auto current = CurrentCanonicalFile(grant.path, grant.size);
  if (!current || *current != grant.path) return std::nullopt;
  return grant;
}

bool LocalFileGrantManager::Revoke(const std::string& id) {
  bool removed = false;
  {
    std::lock_guard lock(mutex_);
    removed = grants_.erase(id) != 0;
  }
  if (removed && logger_) logger_->Info("Local file grant released");
  return removed;
}

std::size_t LocalFileGrantManager::Count() const {
  std::lock_guard lock(mutex_);
  return grants_.size();
}

}  // namespace lwweb
