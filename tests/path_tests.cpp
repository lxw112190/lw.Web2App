#include "lwweb/common/path_utils.h"

#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunPathTests() {
  Check(lwweb::NormalizeArchivePath("assets/app.js") == "assets/app.js", "normal path");
  Check(lwweb::NormalizeArchivePath("/assets//app.js") == "assets/app.js", "slash cleanup");
  Check(!lwweb::NormalizeArchivePath("../secret.txt"), "parent traversal rejected");
  Check(!lwweb::NormalizeArchivePath("C:/secret.txt"), "drive path rejected");
  Check(lwweb::MimeTypeForPath("index.HTML") == "text/html; charset=utf-8", "HTML MIME");
  Check(lwweb::IsSupportedHttpUrl("https://example.com"), "HTTPS URL accepted");
  Check(!lwweb::IsSupportedHttpUrl("file:///tmp/a"), "file URL rejected");
}
