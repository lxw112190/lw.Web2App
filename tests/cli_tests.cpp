#include "lwweb/cli/command_line.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunCliTests() {
  const std::vector<std::string> args = {
      "lw.Web2App", "pack-url", "https://example.com", "example.exe",
      "--title", "Example", "--app-id", "com.example.web", "--width", "1440",
      "--height", "900", "--windowed", "--debug-log", "--devtools"};
  const auto command = lwweb::ParseCommandLine(args, "runner.exe");
  Check(command.action == lwweb::CliAction::Pack, "pack-url command parsed");
  Check(command.pack.manifest.mode == lwweb::AppMode::Url, "URL mode parsed");
  Check(command.pack.manifest.app_id == "com.example.web", "app ID parsed on every platform");
  Check(command.pack.manifest.width == 1440 && command.pack.manifest.height == 900,
        "window dimensions parsed");
  Check(!command.pack.manifest.fullscreen, "windowed option parsed");
  Check(command.pack.manifest.logging.level == "debug", "debug logging parsed");
  Check(command.pack.manifest.devtools, "devtools option parsed");

  const auto proxy_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack", ".", "example.exe", "--entry", "index.html",
       "--backend-origin", "http://192.0.2.10:8080"},
      "runner.exe");
  Check(proxy_command.pack.manifest.backend_proxy.enabled,
        "controlled backend proxy option parsed");
  Check(proxy_command.pack.manifest.backend_proxy.origin ==
            "http://192.0.2.10:8080",
        "backend proxy origin parsed");

  const std::vector<std::string> local_args = {
      "lw.Web2App", "pack", ".", "example.exe", "--entry", "index.html",
      "--start-path", "/login"};
  const auto local_command = lwweb::ParseCommandLine(local_args, "runner.exe");
  Check(local_command.pack.manifest.entry == "index.html", "local entry parsed");
  Check(local_command.pack.manifest.start_path == "/login", "local start path parsed");
  const auto suggested_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack", ".", "example.exe", "--entry", "pages/login.html"},
      "runner.exe");
  Check(suggested_command.pack.manifest.start_path == "/pages/login.html",
        "start path is derived from an alternate entry");

  bool missing_value_rejected = false;
  try {
    (void)lwweb::ParseCommandLine(
        {"lw.Web2App", "pack-url", "https://example.com", "app", "--app-id"},
        "runner");
  } catch (...) {
    missing_value_rejected = true;
  }
  Check(missing_value_rejected, "missing option value rejected");

  bool invalid_dimension_rejected = false;
  try {
    (void)lwweb::ParseCommandLine(
        {"lw.Web2App", "pack-url", "https://example.com", "app", "--width", "0"},
        "runner");
  } catch (...) {
    invalid_dimension_rejected = true;
  }
  Check(invalid_dimension_rejected, "invalid dimension rejected");

  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--app-id") !=
            std::string::npos,
        "Windows help includes shared app ID option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Linux).find("--app-id") !=
            std::string::npos,
        "Linux help includes shared app ID option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--start-path") !=
            std::string::npos,
        "help includes start path option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--backend-origin") !=
            std::string::npos,
        "help includes controlled backend proxy option");
}
