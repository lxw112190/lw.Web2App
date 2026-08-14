#include "lwweb/cli/command_line.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/payload.h"
#include "lwweb/version.h"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <utility>

namespace lwweb {
namespace {

std::string ArgumentValue(const std::vector<std::string>& args, const std::string& name,
                          std::string fallback = {}) {
  const auto found = std::find(args.begin(), args.end(), name);
  if (found == args.end()) return fallback;
  const auto value = std::next(found);
  if (value == args.end() || value->rfind("--", 0) == 0)
    throw Error("Missing value for " + name);
  return *value;
}

bool HasArgument(const std::vector<std::string>& args, const std::string& name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

std::uint32_t ParseDimension(const std::vector<std::string>& args, const std::string& name,
                             const std::string& fallback) {
  const auto text = ArgumentValue(args, name, fallback);
  try {
    std::size_t consumed = 0;
    const auto value = std::stoul(text, &consumed);
    if (consumed != text.size() || value == 0 || value > 16384)
      throw Error("Invalid value for " + name + ": " + text);
    return static_cast<std::uint32_t>(value);
  } catch (const Error&) {
    throw;
  } catch (...) {
    throw Error("Invalid value for " + name + ": " + text);
  }
}

}  // namespace

CliCommand ParseCommandLine(const std::vector<std::string>& args,
                            const std::filesystem::path& runner) {
  CliCommand command;
  if (args.size() < 2 || args[1] == "help" || args[1] == "--help") return command;

  if (args[1] == "inspect") {
    if (args.size() < 3) throw Error("inspect requires an application path");
    command.action = CliAction::Inspect;
    command.inspect_target = std::filesystem::u8path(args[2]);
    return command;
  }

  if (args[1] != "pack" && args[1] != "pack-url")
    throw Error("Unknown command; run with --help");
  if (args.size() < 4) throw Error(args[1] + " requires an input and output path");

  command.action = CliAction::Pack;
  auto& options = command.pack;
  options.runner = runner;
  options.output = std::filesystem::u8path(args[3]);
  options.manifest.mode = args[1] == "pack" ? AppMode::Local : AppMode::Url;
  if (options.manifest.mode == AppMode::Local) {
    options.source_directory = std::filesystem::u8path(args[2]);
    auto entry = ArgumentValue(args, "--entry");
    if (entry.empty()) {
      entry = std::filesystem::relative(FindDefaultEntry(options.source_directory),
                                        options.source_directory)
                  .generic_u8string();
    }
    options.manifest.entry = std::move(entry);
    options.manifest.start_path = ArgumentValue(args, "--start-path");
    if (options.manifest.start_path.empty())
      options.manifest.start_path = SuggestedStartPath(options.manifest.entry);
  } else {
    options.manifest.url = args[2];
  }

  options.manifest.title = ArgumentValue(args, "--title", "lw.Web2App App");
  options.manifest.width = ParseDimension(args, "--width", "1280");
  options.manifest.height = ParseDimension(args, "--height", "800");
  options.manifest.app_id = ArgumentValue(args, "--app-id");
  options.manifest.fullscreen = !HasArgument(args, "--windowed");
  options.manifest.spa_fallback = !HasArgument(args, "--no-spa");
  options.manifest.devtools = HasArgument(args, "--devtools");
  options.manifest.logging.enabled = !HasArgument(args, "--no-log");
  options.manifest.logging.level = HasArgument(args, "--debug-log") ? "debug" : "info";

  options.metadata.product_name = Utf8ToWide(options.manifest.title);
  options.metadata.file_description = options.metadata.product_name;
  options.metadata.icon = std::filesystem::u8path(ArgumentValue(args, "--icon"));
  options.metadata.company_name = Utf8ToWide(ArgumentValue(args, "--company"));
  options.metadata.version = Utf8ToWide(ArgumentValue(args, "--version", "1.0.0.0"));
  options.metadata.copyright = Utf8ToWide(ArgumentValue(args, "--copyright"));
  return command;
}

std::string CommandLineHelp(CliPlatform platform) {
  const auto executable = platform == CliPlatform::Windows ? "lw.Web2App.exe" : "lw.Web2App";
  const auto application = platform == CliPlatform::Windows ? "output.exe" : "output";
  std::ostringstream help;
  help << "lw.Web2App " << kVersion
       << (platform == CliPlatform::Windows ? " (Windows)\n\n" : " (Linux)\n\n")
       << "  " << executable << " pack <directory> <" << application
       << "> [--entry index.html] [--title App]\n"
       << "             [--start-path / | /login | /login.html]\n"
       << "             [--width 1280] [--height 800] [--app-id com.example.app]\n"
       << "             [--no-spa] [--windowed] [--no-log | --debug-log] [--devtools]\n"
       << "  " << executable << " pack-url <url> <" << application
       << "> [--title App] [--app-id com.example.app] [--windowed]\n"
       << "  " << executable << " inspect <application>\n";
  return help.str();
}

int RunCommandLine(const std::vector<std::string>& args,
                   const std::filesystem::path& runner, CliPlatform platform,
                   std::ostream& output) {
  auto command = ParseCommandLine(args, runner);
  if (command.action == CliAction::Help) {
    output << CommandLineHelp(platform);
    return 0;
  }
  if (command.action == CliAction::Inspect) {
    output << SerializeManifest(LoadPayload(command.inspect_target).manifest, true) << '\n';
    return 0;
  }
  command.pack.progress = [&output](const std::string& message) { output << message << '\n'; };
  PackApplication(command.pack);
  output << "Created " << (platform == CliPlatform::Windows ? "Windows" : "Linux")
         << " application: " << command.pack.output.u8string() << '\n';
  return 0;
}

}  // namespace lwweb
