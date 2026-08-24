#pragma once

#include "lwweb/packer/packer.h"
#include "lwweb/pe/payload_binding.h"

#include <filesystem>
#include <optional>

namespace lwweb {

inline constexpr wchar_t kPayloadBindingResourceType[] = L"LWWEB_BINDING";
constexpr std::uint16_t kPayloadBindingResourceId = 1;

void UpdatePeResources(const std::filesystem::path& executable,
                       const PeMetadata& metadata,
                       const std::optional<PayloadBinding>& binding =
                           std::nullopt);
std::optional<PayloadBinding> ReadPePayloadBinding(
    const std::filesystem::path& executable);
void VerifyPePayloadBinding(const std::filesystem::path& executable,
                            const Sha256Digest& footer_digest);

}  // namespace lwweb
