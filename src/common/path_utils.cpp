#include "lwweb/common/path_utils.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace lwweb {

std::optional<std::string> NormalizeArchivePath(std::string path) {
  if (path.size() > 4096) return std::nullopt;
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (path.empty() || path.find('\0') != std::string::npos ||
      path.find(':') != std::string::npos)
    return std::nullopt;

  std::stringstream stream(path);
  std::string part;
  std::vector<std::string> parts;
  while (std::getline(stream, part, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") return std::nullopt;
    parts.push_back(part);
  }
  if (parts.empty()) return std::nullopt;
  std::string result;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) result.push_back('/');
    result += parts[i];
  }
  return result;
}

bool IsCanonicalArchivePath(const std::string& path) {
  const auto normalized = NormalizeArchivePath(path);
  return normalized && *normalized == path;
}

bool IsSafeStartPath(const std::string& path) {
  if (path.empty() || path.size() > 4096 || path.front() != '/' ||
      (path.size() > 1 && path[1] == '/'))
    return false;
  return std::none_of(path.begin(), path.end(), [](unsigned char value) {
    return value == '\\' || value == 0 || value < 0x20 || value == 0x7f;
  });
}

std::string SuggestedStartPath(const std::string& entry) {
  if (!IsCanonicalArchivePath(entry)) throw Error("Unsafe entry HTML path");
  auto lower = entry;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return lower == "index.html" || lower == "index.htm" ? "/" : "/" + entry;
}

std::string MimeTypeForPath(const std::string& path) {
  auto dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const std::unordered_map<std::string, std::string> types = {
      {".html", "text/html; charset=utf-8"},
      {".htm", "text/html; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".mjs", "text/javascript; charset=utf-8"},
      {".json", "application/json; charset=utf-8"},
      {".svg", "image/svg+xml"}, {".png", "image/png"},
      {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
      {".gif", "image/gif"}, {".webp", "image/webp"},
      {".ico", "image/x-icon"}, {".woff", "font/woff"},
      {".woff2", "font/woff2"}, {".ttf", "font/ttf"},
      {".wasm", "application/wasm"}, {".xml", "application/xml"},
      {".txt", "text/plain; charset=utf-8"}, {".pdf", "application/pdf"},
      {".mp3", "audio/mpeg"}, {".mp4", "video/mp4"}};
  const auto it = types.find(ext);
  return it == types.end() ? "application/octet-stream" : it->second;
}

std::vector<std::string> FindHtmlEntries(const std::filesystem::path& root) {
  if (!std::filesystem::is_directory(root)) throw Error("Source is not a directory");
  std::vector<std::string> entries;
  for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
    if (!item.is_regular_file()) continue;
    auto extension = item.path().extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".html" && extension != ".htm") continue;
    entries.push_back(std::filesystem::relative(item.path(), root).generic_u8string());
  }
  std::sort(entries.begin(), entries.end(), [](const std::string& left,
                                                const std::string& right) {
    auto lower = [](std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    };
    const auto left_lower = lower(left);
    const auto right_lower = lower(right);
    const bool left_index = left_lower == "index.html" || left_lower == "index.htm";
    const bool right_index = right_lower == "index.html" || right_lower == "index.htm";
    if (left_index != right_index) return left_index;
    if (left_lower != right_lower) return left_lower < right_lower;
    return left < right;
  });
  return entries;
}

std::filesystem::path FindDefaultEntry(const std::filesystem::path& root) {
  const auto entries = FindHtmlEntries(root);
  if (entries.empty()) throw Error("No HTML entry file was found");
  return root / std::filesystem::u8path(entries.front());
}

bool IsSupportedHttpUrl(const std::string& url) {
  auto lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0;
}

}  // namespace lwweb
