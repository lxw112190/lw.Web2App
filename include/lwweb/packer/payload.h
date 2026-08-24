#pragma once

#include "lwweb/common/sha256.h"
#include "lwweb/packer/manifest.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

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

// 在修改 Runner 之前准备好的不可变 Payload 描述。manifest_json 是参与
// SHA-256 计算并最终写入 EXE 的同一份字节，禁止在追加阶段重新序列化。
struct PreparedPayload {
  std::filesystem::path zip;
  std::string manifest_json;
  Sha256Digest sha256{};
  std::uint32_t flags = 0;
  std::uint32_t file_count = 0;
  std::uint64_t source_size = 0;
  std::uint64_t compressed_size = 0;
};

std::array<std::uint8_t, kPayloadFooterSize> EncodeFooter(const PayloadFooter& footer);
PayloadFooter DecodeFooter(const std::array<std::uint8_t, kPayloadFooterSize>& bytes);
// 纯边界校验函数，供加载器、表驱动测试和 fuzz target 共同使用。
void ValidatePayloadBounds(const PayloadFooter& footer, std::uint64_t file_size);
bool HasPayload(const std::filesystem::path& executable);
LoadedPayload LoadPayload(const std::filesystem::path& executable,
                          bool verify_hash = true);
std::uint64_t RunnerPrefixSize(const std::filesystem::path& executable);
void AppendPreparedPayload(const std::filesystem::path& executable,
                           const PreparedPayload& payload);
// 兼容已有调用方的包装；新打包流程应先 PreparePayload，再调用
// AppendPreparedPayload。
void AppendPayload(const std::filesystem::path& output,
                   const std::filesystem::path& payload_file,
                   const Manifest& manifest, std::uint32_t flags);

}  // namespace lwweb
