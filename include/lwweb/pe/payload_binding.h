#pragma once

#include "lwweb/common/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lwweb {

constexpr std::size_t kPayloadBindingSize = 48;
constexpr std::array<std::uint8_t, 8> kPayloadBindingMagic = {
    'L', 'W', 'B', 'I', 'N', 'D', '0', '1'};
constexpr std::uint32_t kPayloadBindingVersion = 1;
constexpr std::uint32_t kBindingAuthenticodeRequired = 1u << 0;
constexpr std::uint32_t kPayloadBindingKnownFlags =
    kBindingAuthenticodeRequired;

// PE 资源中 Signed Payload Binding 的内存表示。磁盘格式固定为 48 字节，
// 必须通过 EncodePayloadBinding 显式序列化，不能依赖结构体布局。
struct PayloadBinding {
  std::uint32_t version = kPayloadBindingVersion;
  std::uint32_t flags = 0;
  Sha256Digest payload_sha256{};
};

std::array<std::uint8_t, kPayloadBindingSize> EncodePayloadBinding(
    const PayloadBinding& binding);
PayloadBinding DecodePayloadBinding(
    const std::array<std::uint8_t, kPayloadBindingSize>& bytes);

}  // namespace lwweb
