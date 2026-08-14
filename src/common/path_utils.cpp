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

std::filesystem::path FindDefaultEntry(const std::filesystem::path& root) {
  if (!std::filesystem::is_directory(root)) throw Error("Source is not a directory");
  const auto preferred = root / L"index.html";
  if (std::filesystem::is_regular_file(preferred)) return preferred;
  for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
    if (item.is_regular_file() && item.path().extension() == L".html") return item.path();
  }
  throw Error("No HTML entry file was found");
}

bool IsSupportedHttpUrl(const std::string& url) {
  auto lower = url;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0;
}

}  // namespace lwweb
