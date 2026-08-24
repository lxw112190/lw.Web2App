#include "lwweb/pe/payload_binding.h"

#include "lwweb/common/error.h"

#include <algorithm>

namespace lwweb {
namespace {

void Put32(std::uint8_t*& output, std::uint32_t value) {
  for (int index = 0; index < 4; ++index)
    *output++ = static_cast<std::uint8_t>(value >> (index * 8));
}

std::uint32_t Get32(const std::uint8_t*& input) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index)
    value |= static_cast<std::uint32_t>(*input++) << (index * 8);
  return value;
}

}  // namespace

std::array<std::uint8_t, kPayloadBindingSize> EncodePayloadBinding(
    const PayloadBinding& binding) {
  if (binding.version != kPayloadBindingVersion)
    throw Error("Unsupported payload binding version");
  if ((binding.flags & ~kPayloadBindingKnownFlags) != 0)
    throw Error("Payload binding contains unsupported flags");
  std::array<std::uint8_t, kPayloadBindingSize> bytes{};
  auto* output = bytes.data();
  std::copy(kPayloadBindingMagic.begin(), kPayloadBindingMagic.end(), output);
  output += kPayloadBindingMagic.size();
  Put32(output, binding.version);
  Put32(output, binding.flags);
  std::copy(binding.payload_sha256.begin(), binding.payload_sha256.end(),
            output);
  return bytes;
}

PayloadBinding DecodePayloadBinding(
    const std::array<std::uint8_t, kPayloadBindingSize>& bytes) {
  if (!std::equal(kPayloadBindingMagic.begin(), kPayloadBindingMagic.end(),
                  bytes.begin()))
    throw Error("Payload binding magic was not found");
  const auto* input = bytes.data() + kPayloadBindingMagic.size();
  PayloadBinding binding;
  binding.version = Get32(input);
  binding.flags = Get32(input);
  if (binding.version != kPayloadBindingVersion)
    throw Error("Unsupported payload binding version");
  if ((binding.flags & ~kPayloadBindingKnownFlags) != 0)
    throw Error("Payload binding contains unsupported flags");
  std::copy(input, input + binding.payload_sha256.size(),
            binding.payload_sha256.begin());
  return binding;
}

}  // namespace lwweb
