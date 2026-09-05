#include "lwweb/cli/command_line.h"
#include "lwweb/common/pe_version.h"

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
  Check(command.pack.metadata.product_name == L"Example",
        "product name defaults to title");
  Check(command.pack.metadata.file_description == L"Example",
        "file description defaults to title");

  const auto metadata_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack-url", "https://example.com", "example.exe",
       "--title", "Window title", "--product-name", "Product name",
       "--file-description", "File description", "--company", "Company",
       "--version", "2.3", "--copyright", "Copyright 2026"},
      "runner.exe");
  Check(metadata_command.pack.metadata.product_name == L"Product name",
        "explicit product name parsed");
  Check(metadata_command.pack.metadata.file_description == L"File description",
        "explicit file description parsed");
  Check(metadata_command.pack.metadata.company_name == L"Company",
        "company parsed");
  Check(metadata_command.pack.metadata.version == L"2.3.0.0",
        "PE version normalized");
  Check(metadata_command.pack.metadata.copyright == L"Copyright 2026",
        "copyright parsed");

  const auto signing_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack-url", "https://example.com", "signed.exe",
       "--sign-cert-thumbprint", "00112233445566778899AABBCCDDEEFF00112233",
       "--timestamp-url", "https://timestamp.example.com",
       "--signtool", "tools/signtool.exe"},
      "runner.exe");
  Check(signing_command.pack.signing.enabled &&
            signing_command.pack.signing.certificate_thumbprint ==
                "00112233445566778899AABBCCDDEEFF00112233" &&
            signing_command.pack.signing.timestamp_url ==
                "https://timestamp.example.com" &&
            signing_command.pack.signing.signtool ==
                std::filesystem::path("tools/signtool.exe"),
        "Authenticode Certificate Store options parsed");
  bool orphaned_signing_option_rejected = false;
  try {
    (void)lwweb::ParseCommandLine(
        {"lw.Web2App", "pack-url", "https://example.com", "app.exe",
         "--timestamp-url", "https://timestamp.example.com"},
        "runner.exe");
  } catch (...) {
    orphaned_signing_option_rejected = true;
  }
  Check(orphaned_signing_option_rejected,
        "timestamp option requires a signing certificate");

  const auto proxy_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack", ".", "example.exe", "--entry", "index.html",
       "--backend-origin", "http://192.0.2.10:8080"},
      "runner.exe");
  Check(proxy_command.pack.manifest.backend_proxy.enabled,
        "controlled backend proxy option parsed");
  Check(proxy_command.pack.manifest.backend_proxy.origin ==
            "http://192.0.2.10:8080",
        "backend proxy origin parsed");
  const auto external_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack-url", "https://example.com", "example.exe",
       "--external-links", "browser"},
      "runner.exe");
  Check(external_command.pack.manifest.external_links.policy == "browser",
        "external link policy parsed");

  const auto ipc_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "pack", ".", "example.exe", "--entry", "index.html",
       "--ipc", "--ipc-capability", "app.info", "--ipc-capability", "fs.list",
       "--ipc-root", "${DOCUMENTS}"},
      "runner.exe");
  Check(ipc_command.pack.manifest.ipc.enabled &&
            ipc_command.pack.manifest.ipc.capabilities.size() == 2 &&
            ipc_command.pack.manifest.ipc.filesystem_roots.size() == 1,
        "repeatable Native IPC options parsed");

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
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--external-links") !=
            std::string::npos,
        "help includes external link policy option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--ipc-capability") !=
            std::string::npos,
        "help includes Native IPC capability option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("--product-name") !=
            std::string::npos,
        "help includes PE product name option");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find(
            "--sign-cert-thumbprint") != std::string::npos,
        "Windows help includes Authenticode signing options");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Linux).find(
            "--sign-cert-thumbprint") == std::string::npos,
        "Linux help does not advertise Windows Authenticode options");
  const auto publish_command = lwweb::ParseCommandLine(
      {"lw.Web2App", "publish", "--config", "config/lwweb.json",
       "--output", "dist/release"},
      "runner.exe");
  Check(publish_command.action == lwweb::CliAction::Publish &&
            publish_command.publish.config_file ==
                std::filesystem::path("config/lwweb.json") &&
            publish_command.publish.output_override ==
                std::filesystem::path("dist/release"),
        "publish command and explicit paths parsed");
  Check(lwweb::CommandLineHelp(lwweb::CliPlatform::Windows).find("publish") !=
            std::string::npos,
        "help includes project publish command");
  bool unknown_publish_option_rejected = false;
  try {
    (void)lwweb::ParseCommandLine(
        {"lw.Web2App", "publish", "--unknown", "value"}, "runner.exe");
  } catch (...) {
    unknown_publish_option_rejected = true;
  }
  Check(unknown_publish_option_rejected,
        "publish rejects unknown options instead of ignoring mistakes");
  Check(lwweb::NormalizePeVersion(L"1.2.3") == L"1.2.3.0",
        "PE version helper pads missing components");
  bool invalid_version_rejected = false;
  try {
    (void)lwweb::NormalizePeVersion(L"1.2.3.4.5");
  } catch (...) {
    invalid_version_rejected = true;
  }
  Check(invalid_version_rejected, "PE version rejects more than four components");
  bool overflowing_version_rejected = false;
  try {
    (void)lwweb::NormalizePeVersion(L"4294967296");
  } catch (...) {
    overflowing_version_rejected = true;
  }
  Check(overflowing_version_rejected, "PE version rejects overflowing components");
}
