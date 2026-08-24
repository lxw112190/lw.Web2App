#pragma once

#include <string>
#include <vector>

namespace lwweb {

// RELEASE_INFO.json 中的一项可下载产物。sha256 始终是整个文件的
// 小写十六进制摘要；signed 表示该文件本身是否经过平台代码签名。
struct ReleaseArtifact {
  std::string file;
  std::string type;
  std::string platform;
  std::string arch = "x64";
  bool signed_file = false;
  std::string sha256;
};

// 一次 publish 的机器可读发布清单，供 CI、更新器和发布页面复用。
struct ReleaseInfo {
  std::string app_id;
  std::string app_name;
  std::string app_version;
  std::vector<ReleaseArtifact> artifacts;
};

std::string SerializeReleaseInfo(const ReleaseInfo& info, bool pretty = true);

}  // namespace lwweb
