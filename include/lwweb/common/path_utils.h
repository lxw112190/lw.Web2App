#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lwweb {

std::optional<std::string> NormalizeArchivePath(std::string path);
std::string MimeTypeForPath(const std::string& path);
std::filesystem::path FindDefaultEntry(const std::filesystem::path& root);
bool IsSupportedHttpUrl(const std::string& url);

}  // namespace lwweb

