#include "lwweb/cli/command_line.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/pe_version.h"
#include "lwweb/packer/payload.h"
#ifdef _WIN32
#include "lwweb/pe/pe_resources.h"
#endif
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

std::vector<std::string> ArgumentValues(const std::vector<std::string>& args,
                                        const std::string& name) {
  std::vector<std::string> values;
  for (auto found = args.begin(); found != args.end();) {
    found = std::find(found, args.end(), name);
    if (found == args.end()) break;
    const auto value = std::next(found);
    if (value == args.end() || value->rfind("--", 0) == 0)
      throw Error("Missing value for " + name);
    values.push_back(*value);
    found = std::next(value);
  }
  return values;
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

  if (args[1] == "publish") {
    command.action = CliAction::Publish;
    command.publish.runner = runner;
#ifdef _WIN32
    command.publish.platform = PublishPlatform::Windows;
#else
    command.publish.platform = PublishPlatform::Linux;
#endif
    for (std::size_t index = 2; index < args.size(); ++index) {
      const auto& argument = args[index];
      if (argument != "--config" && argument != "--output")
        throw Error("Unknown publish option: " + argument);
      if (++index == args.size() || args[index].rfind("--", 0) == 0)
        throw Error("Missing value for " + argument);
      if (argument == "--config")
        command.publish.config_file = std::filesystem::u8path(args[index]);
      else
        command.publish.output_override = std::filesystem::u8path(args[index]);
    }
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
  options.manifest.external_links.policy =
      ArgumentValue(args, "--external-links", "auto");
  options.manifest.backend_proxy.origin = ArgumentValue(args, "--backend-origin");
  options.manifest.backend_proxy.enabled = !options.manifest.backend_proxy.origin.empty();
  options.manifest.ipc.enabled = HasArgument(args, "--ipc");
  options.manifest.ipc.capabilities = ArgumentValues(args, "--ipc-capability");
  options.manifest.ipc.filesystem_roots = ArgumentValues(args, "--ipc-root");
  options.manifest.logging.enabled = !HasArgument(args, "--no-log");
  options.manifest.logging.level = HasArgument(args, "--debug-log") ? "debug" : "info";

  options.metadata.product_name =
      Utf8ToWide(ArgumentValue(args, "--product-name", options.manifest.title));
  options.metadata.file_description =
      Utf8ToWide(ArgumentValue(args, "--file-description", options.manifest.title));
  options.metadata.icon = std::filesystem::u8path(ArgumentValue(args, "--icon"));
  options.metadata.company_name = Utf8ToWide(ArgumentValue(args, "--company"));
  options.metadata.version =
      NormalizePeVersion(Utf8ToWide(ArgumentValue(args, "--version", "1.0.0.0")));
  options.metadata.copyright = Utf8ToWide(ArgumentValue(args, "--copyright"));
  options.signing.certificate_thumbprint =
      ArgumentValue(args, "--sign-cert-thumbprint");
  options.signing.enabled =
      !options.signing.certificate_thumbprint.empty();
  options.signing.timestamp_url = ArgumentValue(args, "--timestamp-url");
  options.signing.signtool =
      std::filesystem::u8path(ArgumentValue(args, "--signtool"));
  if (!options.signing.enabled &&
      (!options.signing.timestamp_url.empty() || !options.signing.signtool.empty()))
    throw Error("--timestamp-url and --signtool require --sign-cert-thumbprint");
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
       << "             [--external-links auto|allow|block|browser]\n"
       << "             [--backend-origin http://host:port]\n"
       << "             [--ipc --ipc-capability app.info --ipc-capability app.paths]\n"
       << "             [--ipc-capability fs.watch --ipc-capability window.control]\n"
       << "             [--ipc-capability app.lifecycle --ipc-capability tray]\n"
       << "             [--product-name App] [--file-description Description]\n"
       << "             [--icon app.png] [--company Company] [--version 1.0.0.0]\n"
       << "             [--copyright Copyright]\n";
  if (platform == CliPlatform::Windows)
    help << "             [--sign-cert-thumbprint SHA1] [--timestamp-url URL]\n"
         << "             [--signtool path\\to\\signtool.exe]\n";
  help
       << "  " << executable << " pack-url <url> <" << application
       << "> [--title App] [--product-name App] [--file-description Description]\n"
       << "             [--app-id com.example.app] [--windowed]\n"
       << "  " << executable
       << " publish [--config ./lwweb.json] [--output ./release]\n"
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
    const auto payload = LoadPayload(command.inspect_target);
#ifdef _WIN32
    VerifyPePayloadBinding(command.inspect_target, payload.footer.sha256);
#endif
    output << SerializeManifest(payload.manifest, true) << '\n';
    return 0;
  }
  if (command.action == CliAction::Publish) {
    command.publish.platform = platform == CliPlatform::Windows
                                   ? PublishPlatform::Windows
                                   : PublishPlatform::Linux;
    command.publish.progress =
        [&output](const std::string& message) { output << message << '\n'; };
    const auto result = PublishProject(command.publish);
    output << "Published release: " << result.directory.u8string() << '\n';
    return 0;
  }
  command.pack.progress = [&output](const std::string& message) { output << message << '\n'; };
  PackApplication(command.pack);
  output << "Created " << (platform == CliPlatform::Windows ? "Windows" : "Linux")
         << " application: " << command.pack.output.u8string() << '\n';
  return 0;
}

}  // namespace lwweb
