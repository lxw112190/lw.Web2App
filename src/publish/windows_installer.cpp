#include "lwweb/publish/windows_installer.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/windows_process.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

namespace lwweb {
namespace {

std::filesystem::path RequireExecutable(const std::filesystem::path& path,
                                        const char* description) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error || !std::filesystem::is_regular_file(absolute, error))
    throw Error(std::string(description) + " does not exist: " +
                path.u8string());
  return absolute.lexically_normal();
}

std::filesystem::path SearchPathForIscc() {
  const auto required =
      SearchPathW(nullptr, L"ISCC.exe", nullptr, 0, nullptr, nullptr);
  if (required == 0) return {};
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1);
  if (SearchPathW(nullptr, L"ISCC.exe", nullptr,
                  static_cast<DWORD>(buffer.size()), buffer.data(), nullptr) == 0)
    return {};
  return std::filesystem::path(buffer.data());
}

std::filesystem::path EnvironmentPath(const wchar_t* name,
                                      const wchar_t* fallback) {
  std::array<wchar_t, 32768> buffer{};
  const auto length = GetEnvironmentVariableW(
      name, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0 && length < buffer.size())
    return std::filesystem::path(buffer.data());
  return std::filesystem::path(fallback);
}

std::filesystem::path CommonInstallIscc() {
  const std::array<std::filesystem::path, 2> roots = {
      EnvironmentPath(L"ProgramFiles(x86)", L"C:\\Program Files (x86)"),
      EnvironmentPath(L"ProgramFiles", L"C:\\Program Files")};
  const std::array<const wchar_t*, 3> directories = {
      L"Inno Setup 7", L"Inno Setup 6", L"Inno Setup"};
  std::error_code error;
  for (const auto& root : roots) {
    for (const auto* directory : directories) {
      const auto candidate = root / directory / L"ISCC.exe";
      if (std::filesystem::is_regular_file(candidate, error)) return candidate;
      error.clear();
    }
  }
  return {};
}

void RunIscc(const std::filesystem::path& iscc,
             const std::filesystem::path& script) {
  RunWindowsExternalTool(iscc, {L"/Qp", script.wstring()},
                         script.parent_path(), std::chrono::minutes(3),
                         "Inno Setup compiler");
}

std::string EscapeInnoValue(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto character : value) {
    if (character == '\r' || character == '\n' || character == '\0')
      throw Error("Installer metadata contains an unsupported control character");
    if (character == '{') escaped.push_back('{');
    escaped.push_back(character);
  }
  return escaped;
}

std::string QuotedInnoParameter(const std::filesystem::path& value) {
  auto text = EscapeInnoValue(value.u8string());
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (const auto character : text) {
    if (character == '"') escaped.push_back('"');
    escaped.push_back(character);
  }
  escaped.push_back('"');
  return escaped;
}

std::string EscapeInnoQuotedFragment(const std::string& value) {
  const auto text = EscapeInnoValue(value);
  std::string escaped;
  escaped.reserve(text.size());
  for (const auto character : text) {
    if (character == '"') escaped.push_back('"');
    escaped.push_back(character);
  }
  return escaped;
}

std::string BuildScript(const WindowsInstallerBuildOptions& options) {
  const auto app_name = EscapeInnoValue(options.app_name);
  const auto quoted_app_name = EscapeInnoQuotedFragment(options.app_name);
  const auto app_id = EscapeInnoValue(options.app_id);
  const auto app_version = EscapeInnoValue(options.app_version);
  const auto publisher = EscapeInnoValue(options.publisher);
  const auto executable_name =
      EscapeInnoValue(options.application.filename().u8string());
  const auto install_folder =
      EscapeInnoValue(options.application.stem().u8string());
  const auto output_directory = EscapeInnoValue(
      std::filesystem::absolute(options.output_directory).u8string());
  const auto output_basename = EscapeInnoValue(options.output_basename);

  std::ostringstream script;
  script << "[Setup]\n"
         << "AppId=" << app_id << "\n"
         << "AppName=" << app_name << "\n"
         << "AppVersion=" << app_version << "\n"
         << "AppVerName=" << app_name << ' ' << app_version << "\n";
  if (!publisher.empty()) script << "AppPublisher=" << publisher << "\n";
  script << "DefaultDirName={autopf}\\" << install_folder << "\n"
         << "DefaultGroupName=" << app_name << "\n"
         << "DisableProgramGroupPage=yes\n"
         << "PrivilegesRequired=admin\n"
         << "ArchitecturesAllowed=x64compatible\n"
         << "ArchitecturesInstallIn64BitMode=x64compatible\n"
         << "Compression=lzma2\n"
         << "SolidCompression=yes\n"
         << "WizardStyle=modern\n"
         << "CloseApplications=yes\n"
         << "RestartApplications=no\n"
         << "OutputDir=" << output_directory << "\n"
         << "OutputBaseFilename=" << output_basename << "\n"
         << "UninstallDisplayIcon={app}\\" << executable_name << "\n"
         << "VersionInfoVersion=" << app_version << "\n"
         << "VersionInfoDescription=" << app_name << " Installer\n";
  if (!publisher.empty()) script << "VersionInfoCompany=" << publisher << "\n";

  if (options.desktop_shortcut) {
    script << "\n[Tasks]\n"
           << "Name: \"desktopicon\"; Description: \"{cm:CreateDesktopIcon}\"; "
              "GroupDescription: \"{cm:AdditionalIcons}\"; Flags: unchecked\n";
  }
  script << "\n[Files]\n"
         << "Source: " << QuotedInnoParameter(options.application)
         << "; DestDir: \"{app}\"; Flags: ignoreversion\n"
         << "\n[Icons]\n";
  if (options.start_menu)
    script << "Name: \"{autoprograms}\\" << quoted_app_name
           << "\"; Filename: \"{app}\\" << executable_name << "\"\n";
  if (options.desktop_shortcut)
    script << "Name: \"{autodesktop}\\" << quoted_app_name
           << "\"; Filename: \"{app}\\" << executable_name
           << "\"; Tasks: desktopicon\n";
  return script.str();
}

void ValidateInstaller(const std::filesystem::path& installer) {
  std::ifstream input(installer, std::ios::binary);
  std::array<char, 2> signature{};
  input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
  if (!input || signature[0] != 'M' || signature[1] != 'Z')
    throw Error("Inno Setup did not produce a valid Windows executable");
}

}  // namespace

std::filesystem::path FindInnoSetupCompiler(
    const std::filesystem::path& configured) {
  if (!configured.empty())
    return RequireExecutable(configured, "Configured ISCC.exe");
  const auto from_path = SearchPathForIscc();
  if (!from_path.empty()) return from_path;
  const auto common = CommonInstallIscc();
  if (!common.empty()) return common;
  throw Error("ISCC.exe was not found. Install Inno Setup 6.3 or newer, add ISCC.exe to "
              "PATH, or configure publish.windows.installer.iscc");
}

std::filesystem::path BuildWindowsInstaller(
    const WindowsInstallerBuildOptions& options) {
  if (options.application.empty() || options.output_directory.empty() ||
      options.output_basename.empty() || options.app_id.empty() ||
      options.app_name.empty() || options.app_version.empty())
    throw Error("Windows installer options are incomplete");
  (void)RequireExecutable(options.application, "Portable application");
  std::error_code error;
  std::filesystem::create_directories(options.output_directory, error);
  if (error) throw Error("Cannot create installer output directory: " +
                         error.message());
  const auto iscc = FindInnoSetupCompiler(options.configured_iscc);
  const auto script_path =
      options.output_directory / ".lwweb-installer.iss";
  const auto installer = options.output_directory /
                         std::filesystem::u8path(options.output_basename + ".exe");
  struct TemporaryScript {
    std::filesystem::path path;
    ~TemporaryScript() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{script_path};
  std::filesystem::remove(installer, error);
  error.clear();
  std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
  if (!script) throw Error("Cannot create temporary Inno Setup script");
  const std::array<unsigned char, 3> bom = {0xef, 0xbb, 0xbf};
  script.write(reinterpret_cast<const char*>(bom.data()), bom.size());
  const auto text = BuildScript(options);
  script.write(text.data(), static_cast<std::streamsize>(text.size()));
  script.close();
  if (!script) throw Error("Cannot write temporary Inno Setup script");
  RunIscc(iscc, script_path);
  if (!std::filesystem::is_regular_file(installer, error))
    throw Error("Inno Setup completed without creating the expected installer");
  ValidateInstaller(installer);
  return installer;
}

}  // namespace lwweb
