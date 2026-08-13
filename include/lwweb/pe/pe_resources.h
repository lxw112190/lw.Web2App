#pragma once

#include "lwweb/packer/packer.h"

#include <filesystem>

namespace lwweb {

void UpdatePeResources(const std::filesystem::path& executable,
                       const PeMetadata& metadata);

}  // namespace lwweb

