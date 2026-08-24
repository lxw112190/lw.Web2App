#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lwweb {

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest Sha256(const std::uint8_t* data, std::size_t size);
Sha256Digest Sha256(const std::vector<std::uint8_t>& data);
Sha256Digest Sha256FileRange(const std::filesystem::path& path,
                             std::uint64_t offset, std::uint64_t size);
// 流式读取文件并继续哈希内存中的后缀，避免为了计算 ZIP + Manifest
// 的联合摘要而把完整 ZIP 载入内存。path 为空时仅哈希 suffix。
Sha256Digest Sha256FileWithSuffix(const std::filesystem::path& path,
                                  const std::string& suffix);
std::string HexDigest(const Sha256Digest& digest);
Sha256Digest ParseHexDigest(const std::string& text);

}  // namespace lwweb
