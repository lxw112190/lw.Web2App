#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lwweb {

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path);
std::string ReadFileText(const std::filesystem::path& path);
void WriteFileBytes(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& bytes);
std::uint64_t FileSize(const std::filesystem::path& path);
std::filesystem::path CurrentExecutablePath();
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::string WindowsErrorMessage(unsigned long error);

}  // namespace lwweb

