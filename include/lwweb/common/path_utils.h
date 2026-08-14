#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lwweb {

std::optional<std::string> NormalizeArchivePath(std::string path);
bool IsCanonicalArchivePath(const std::string& path);
bool IsSafeStartPath(const std::string& path);
std::string SuggestedStartPath(const std::string& entry);
std::string MimeTypeForPath(const std::string& path);
std::vector<std::string> FindHtmlEntries(const std::filesystem::path& root);
std::filesystem::path FindDefaultEntry(const std::filesystem::path& root);
bool IsSupportedHttpUrl(const std::string& url);

}  // namespace lwweb
