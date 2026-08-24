#include "lwweb/publish/release_info.h"

#include <nlohmann/json.hpp>

namespace lwweb {

std::string SerializeReleaseInfo(const ReleaseInfo& info, bool pretty) {
  nlohmann::json root = {
      {"format", "lw-web2app-release"},
      {"version", 1},
      {"app", {{"id", info.app_id},
               {"name", info.app_name},
               {"version", info.app_version}}},
      {"artifacts", nlohmann::json::array()}};
  for (const auto& artifact : info.artifacts) {
    root["artifacts"].push_back({
        {"file", artifact.file},
        {"type", artifact.type},
        {"platform", artifact.platform},
        {"arch", artifact.arch},
        {"signed", artifact.signed_file},
        {"sha256", artifact.sha256},
    });
  }
  return root.dump(pretty ? 2 : -1) + '\n';
}

}  // namespace lwweb
