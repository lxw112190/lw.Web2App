#include "lwweb/common/path_utils.h"

#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunPathTests() {
  Check(lwweb::NormalizeArchivePath("assets/app.js") == "assets/app.js", "normal path");
  Check(lwweb::NormalizeArchivePath("/assets//app.js") == "assets/app.js", "slash cleanup");
  Check(!lwweb::NormalizeArchivePath(""), "empty path rejected");
  Check(!lwweb::NormalizeArchivePath("."), "dot-only path rejected");
  Check(!lwweb::NormalizeArchivePath("./"), "dot directory rejected");
  Check(lwweb::NormalizeArchivePath("a/./b") == "a/b", "dot component normalized");
  Check(!lwweb::NormalizeArchivePath("a/../b"), "embedded parent traversal rejected");
  Check(!lwweb::NormalizeArchivePath("../secret.txt"), "parent traversal rejected");
  Check(!lwweb::NormalizeArchivePath("C:/secret.txt"), "drive path rejected");
  Check(!lwweb::NormalizeArchivePath("//"), "slash-only path rejected");
  Check(!lwweb::NormalizeArchivePath("\\\\"), "backslash-only path rejected");
  Check(!lwweb::NormalizeArchivePath(std::string("a\0b", 3)), "NUL byte rejected");
  Check(!lwweb::NormalizeArchivePath(std::string(4097, 'a')), "overlong path rejected");
  Check(lwweb::NormalizeArchivePath(u8"资源/首页.html") == u8"资源/首页.html",
        "UTF-8 path accepted");
  Check(lwweb::MimeTypeForPath("index.HTML") == "text/html; charset=utf-8", "HTML MIME");
  Check(lwweb::MimeTypeForPath("dotnet.wasm") == "application/wasm", "WASM MIME");
  Check(lwweb::IsSupportedHttpUrl("https://example.com"), "HTTPS URL accepted");
  Check(!lwweb::IsSupportedHttpUrl("file:///tmp/a"), "file URL rejected");
}
