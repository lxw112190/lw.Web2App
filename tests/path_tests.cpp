#include "lwweb/common/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
  Check(lwweb::IsCanonicalArchivePath("pages/login.html"), "canonical entry accepted");
  Check(!lwweb::IsCanonicalArchivePath("/login.html"), "absolute-looking entry rejected");
  Check(!lwweb::IsCanonicalArchivePath("pages\\login.html"), "backslash entry rejected");
  Check(lwweb::IsSafeStartPath("/"), "root start path accepted");
  Check(lwweb::IsSafeStartPath("/login?from=app#form"), "route start path accepted");
  Check(lwweb::IsSafeStartPath("/#/login"), "hash route accepted");
  Check(!lwweb::IsSafeStartPath("login"), "relative start path rejected");
  Check(!lwweb::IsSafeStartPath("//example.com"), "scheme-relative start path rejected");
  Check(!lwweb::IsSafeStartPath("/pages\\login"), "backslash start path rejected");
  Check(!lwweb::IsSafeStartPath(std::string("/a\0b", 4)), "NUL start path rejected");
  Check(lwweb::SuggestedStartPath("index.html") == "/", "root index suggests root route");
  Check(lwweb::SuggestedStartPath("login.html") == "/login.html",
        "alternate entry suggests its file route");
  Check(lwweb::SuggestedStartPath("pages/login.html") == "/pages/login.html",
        "nested entry suggests a nested route");
  Check(lwweb::MimeTypeForPath("index.HTML") == "text/html; charset=utf-8", "HTML MIME");
  Check(lwweb::MimeTypeForPath("dotnet.wasm") == "application/wasm", "WASM MIME");
  Check(lwweb::IsSupportedHttpUrl("https://example.com"), "HTTPS URL accepted");
  Check(!lwweb::IsSupportedHttpUrl("file:///tmp/a"), "file URL rejected");

  const auto root = std::filesystem::temp_directory_path() / "lwweb-entry-list-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root / "pages");
  std::ofstream(root / "login.html") << "login";
  std::ofstream(root / "index.HTML") << "index";
  std::ofstream(root / "pages" / "admin.htm") << "admin";
  const auto entries = lwweb::FindHtmlEntries(root);
  Check(entries.size() == 2, "only root HTML and HTM entries are discovered");
  Check(entries.front() == "index.HTML", "root index entry is preferred deterministically");
  Check(entries[1] == "login.html", "remaining root HTML entries are sorted");
  Check(std::find(entries.begin(), entries.end(), "pages/admin.htm") == entries.end(),
        "nested HTML entries are not offered as launch pages");
  std::filesystem::remove_all(root, ignored);
}
