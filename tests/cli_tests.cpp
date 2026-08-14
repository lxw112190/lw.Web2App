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
}
