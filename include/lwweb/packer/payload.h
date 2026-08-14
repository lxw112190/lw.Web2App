#pragma once

#include "lwweb/common/sha256.h"
#include "lwweb/packer/manifest.h"

#include <array>
#include <cstdint>
#include <filesystem>

namespace lwweb {

constexpr std::size_t kPayloadFooterSize = 80;
constexpr std::array<std::uint8_t, 8> kPayloadMagicV1 = {
    'L', 'W', 'W', 'E', 'B', '0', '0', '1'};
constexpr std::array<std::uint8_t, 8> kPayloadMagicV2 = {
    'L', 'W', 'W', 'E', 'B', '0', '0', '2'};
constexpr std::uint32_t kPayloadVersionV1 = 1;
constexpr std::uint32_t kPayloadVersion = 2;

enum PayloadFlags : std::uint32_t {
  kPayloadHasZip = 1u << 0,
  kPayloadUrlMode = 1u << 1,
};

// V1/V2 Footer 的内存表示。V1 的 sha256 只覆盖 ZIP；V2 覆盖 ZIP + Manifest。
// 写入磁盘时必须通过 EncodeFooter 显式序列化，不能依赖编译器结构体布局。
struct PayloadFooter {
  std::uint32_t version = kPayloadVersion;
  std::uint32_t flags = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_size = 0;
  std::uint64_t manifest_offset = 0;
  std::uint64_t manifest_size = 0;
  Sha256Digest sha256{};
};

// 已完成边界、Manifest 和可选 SHA-256 校验的应用载荷视图。
// ZIP 数据仍保留在 EXE 中，不会在加载阶段整体复制到内存。
struct LoadedPayload {
  std::filesystem::path executable;
  PayloadFooter footer;
  Manifest manifest;
};

std::array<std::uint8_t, kPayloadFooterSize> EncodeFooter(const PayloadFooter& footer);
PayloadFooter DecodeFooter(const std::array<std::uint8_t, kPayloadFooterSize>& bytes);
// 纯边界校验函数，供加载器、表驱动测试和 fuzz target 共同使用。
void ValidatePayloadBounds(const PayloadFooter& footer, std::uint64_t file_size);
bool HasPayload(const std::filesystem::path& executable);
LoadedPayload LoadPayload(const std::filesystem::path& executable,
                          bool verify_hash = true);
std::uint64_t RunnerPrefixSize(const std::filesystem::path& executable);
void AppendPayload(const std::filesystem::path& output,
                   const std::filesystem::path& payload_file,
                   const Manifest& manifest, std::uint32_t flags);

}  // namespace lwweb
