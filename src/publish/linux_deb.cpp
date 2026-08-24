#include "lwweb/publish/linux_deb.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

constexpr std::size_t kMaxToolOutput = 1024 * 1024;

// 外部 Debian 工具的受控执行结果。参数直接传给 execvp，不经过 shell。
struct CommandResult {
  int exit_code = -1;
  std::string output;
};

// 临时 DEB 根目录清理器。成功生成的 .deb 位于根目录之外，不受其影响。
class TemporaryTree {
 public:
  explicit TemporaryTree(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TemporaryTree() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

 private:
  std::filesystem::path path_;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    throw Error("Cannot create Linux package metadata: " + path.u8string());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output)
    throw Error("Cannot write Linux package metadata: " + path.u8string());
}

std::string SingleLine(std::string value, const char* field,
                       const std::string& fallback) {
  if (value.empty()) value = fallback;
  if (value.empty()) throw Error(std::string("Linux DEB requires ") + field);
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f)
      throw Error(std::string("Linux DEB ") + field +
                  " must be a single line");
  }
  return value;
}

std::string DebianPackageName(const std::string& app_id) {
  const auto separator = app_id.find_last_of('.');
  const auto source = separator == std::string::npos
                          ? app_id
                          : app_id.substr(separator + 1);
  std::string name;
  name.reserve(source.size());
  for (const auto character : source) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte)) {
      name.push_back(static_cast<char>(std::tolower(byte)));
    } else if (character == '+' || character == '-' || character == '.') {
      name.push_back(character);
    } else if (name.empty() || name.back() != '-') {
      name.push_back('-');
    }
  }
  while (!name.empty() &&
         (name.front() == '+' || name.front() == '-' || name.front() == '.'))
    name.erase(name.begin());
  while (!name.empty() &&
         (name.back() == '+' || name.back() == '-' || name.back() == '.'))
    name.pop_back();
  if (name.size() < 2) name = "lw-" + name;
  if (name.size() < 2 || !std::isalnum(static_cast<unsigned char>(name[0])))
    throw Error("Cannot derive a safe Debian package name from app.id");
  return name;
}

CommandResult RunCommand(const std::vector<std::string>& arguments,
                         const std::filesystem::path& working_directory) {
  if (arguments.empty()) throw Error("Cannot run an empty Debian tool command");
#ifdef _WIN32
  (void)working_directory;
  throw Error("Linux DEB publishing requires a Linux host");
#else
  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0)
    throw Error("Cannot create Debian tool output pipe: " +
                std::string(std::strerror(errno)));
  const auto child = fork();
  if (child < 0) {
    const auto message = std::string(std::strerror(errno));
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw Error("Cannot start Debian tool: " + message);
  }
  if (child == 0) {
    close(output_pipe[0]);
    if (chdir(working_directory.c_str()) != 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(output_pipe[1], STDERR_FILENO) < 0)
      _exit(126);
    close(output_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  close(output_pipe[1]);
  CommandResult result;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count > 0) {
      if (result.output.size() + static_cast<std::size_t>(count) <=
          kMaxToolOutput)
        result.output.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    break;
  }
  close(output_pipe[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      throw Error("Cannot wait for Debian tool: " +
                  std::string(std::strerror(errno)));
  }
  if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
  return result;
#endif
}

void RequireSuccess(const CommandResult& result, const char* tool) {
  if (result.exit_code == 0) return;
  if (result.exit_code == 127)
    throw Error(std::string(tool) +
                " was not found; install the dpkg-dev package");
  auto detail = result.output;
  while (!detail.empty() &&
         (detail.back() == '\r' || detail.back() == '\n'))
    detail.pop_back();
  throw Error(std::string(tool) + " failed with exit code " +
              std::to_string(result.exit_code) +
              (detail.empty() ? std::string{} : ": " + detail));
}

std::string ParseDependencies(const std::string& output) {
  constexpr char prefix[] = "shlibs:Depends=";
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(prefix, 0) == 0) {
      auto dependencies = line.substr(sizeof(prefix) - 1);
      if (!dependencies.empty()) return dependencies;
    }
  }
  throw Error("dpkg-shlibdeps did not return shlibs:Depends");
}

std::string DesktopValue(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto character : value) {
    if (character == '\\') escaped += "\\\\";
    else escaped.push_back(character);
  }
  return escaped;
}

std::string DefaultIconSvg() {
  return R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
<defs><linearGradient id="g" x1="24" y1="20" x2="230" y2="238" gradientUnits="userSpaceOnUse"><stop stop-color="#38bdf8"/><stop offset="1" stop-color="#4f46e5"/></linearGradient></defs>
<rect width="256" height="256" rx="52" fill="url(#g)"/><circle cx="78" cy="128" r="42" fill="none" stroke="white" stroke-width="10"/><path d="M36 128h84M78 86c18 22 18 62 0 84M78 86c-18 22-18 62 0 84M42 108h72M42 148h72" fill="none" stroke="white" stroke-width="6"/><path d="M139 91h80v82h-80z" fill="none" stroke="white" stroke-width="10" stroke-linejoin="round"/><path d="M139 111h80M174 91v20" fill="none" stroke="white" stroke-width="8"/></svg>
)SVG";
}

void InstallIcon(const std::filesystem::path& icon,
                 const std::filesystem::path& package_root,
                 const std::string& package_name) {
  if (icon.empty()) {
    const auto directory = package_root / "usr/share/icons/hicolor/scalable/apps";
    std::filesystem::create_directories(directory);
    WriteText(directory / (package_name + ".svg"), DefaultIconSvg());
    return;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(icon, error))
    throw Error("Linux DEB icon does not exist: " + icon.u8string());
  auto extension = icon.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  std::filesystem::path destination;
  if (extension == ".png") {
    destination = package_root / "usr/share/icons/hicolor/256x256/apps" /
                  (package_name + ".png");
  } else if (extension == ".svg") {
    destination = package_root / "usr/share/icons/hicolor/scalable/apps" /
                  (package_name + ".svg");
  } else {
    throw Error("Linux DEB icon must be a PNG or SVG file");
  }
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error)
    throw Error("Cannot create Linux icon directory: " + error.message());
  std::filesystem::copy_file(icon, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) throw Error("Cannot copy Linux DEB icon: " + error.message());
}

}  // namespace

LinuxDebBuildResult BuildLinuxDeb(const LinuxDebBuildOptions& options) {
  std::error_code error;
  const auto application = std::filesystem::absolute(options.application, error);
  if (error || !std::filesystem::is_regular_file(application, error))
    throw Error("Linux DEB application does not exist: " +
                options.application.u8string());
  const auto output_directory =
      std::filesystem::absolute(options.output_directory, error);
  if (error) throw Error("Cannot resolve Linux DEB output directory");
  std::filesystem::create_directories(output_directory, error);
  if (error) throw Error("Cannot create Linux DEB output directory: " +
                         error.message());

  const auto package_name = DebianPackageName(options.app_id);
  const auto app_name = SingleLine(options.app_name, "app name", package_name);
  const auto version = SingleLine(options.app_version, "version", {});
  const auto publisher =
      SingleLine(options.publisher, "publisher", app_name);
  const auto description =
      SingleLine(options.description, "description", app_name);
  const auto package_root = output_directory / ".lwweb-deb-root";
  const auto analysis_root = output_directory / ".lwweb-deb-analysis";
  TemporaryTree package_cleanup(package_root);
  TemporaryTree analysis_cleanup(analysis_root);
  std::filesystem::remove_all(package_root, error);
  error.clear();
  std::filesystem::remove_all(analysis_root, error);
  error.clear();

  const auto installed_application = package_root / "usr/bin" / package_name;
  std::filesystem::create_directories(installed_application.parent_path(), error);
  if (error) throw Error("Cannot create Linux application directory: " +
                         error.message());
  std::filesystem::copy_file(application, installed_application,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) throw Error("Cannot copy Linux application into DEB: " +
                         error.message());
  std::filesystem::permissions(
      installed_application,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write |
          std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec |
          std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace, error);
  if (error) throw Error("Cannot set Linux application permissions: " +
                         error.message());

  const auto desktop_directory = package_root / "usr/share/applications";
  std::filesystem::create_directories(desktop_directory, error);
  if (error) throw Error("Cannot create Linux desktop directory: " +
                         error.message());
  std::ostringstream desktop;
  desktop << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=" << DesktopValue(app_name) << "\n"
          << "Comment=" << DesktopValue(description) << "\n"
          << "Exec=/usr/bin/" << package_name << "\n"
          << "Icon=" << package_name << "\n"
          << "Terminal=false\n"
          << "Categories=Utility;\n"
          << "StartupNotify=true\n";
  WriteText(desktop_directory / (package_name + ".desktop"), desktop.str());
  InstallIcon(options.icon, package_root, package_name);

  const auto analysis_debian = analysis_root / "debian";
  std::filesystem::create_directories(analysis_debian, error);
  if (error) throw Error("Cannot create dpkg-shlibdeps workspace: " +
                         error.message());
  std::ostringstream source_control;
  source_control << "Source: " << package_name << "\n"
                 << "Section: utils\nPriority: optional\n"
                 << "Maintainer: " << publisher << "\n"
                 << "Standards-Version: 4.6.2\n\n"
                 << "Package: " << package_name << "\n"
                 << "Architecture: amd64\n"
                 << "Depends: ${shlibs:Depends}\n"
                 << "Description: " << description << "\n";
  WriteText(analysis_debian / "control", source_control.str());
  const auto dependency_result = RunCommand(
      {"dpkg-shlibdeps", "-O", "-e" + installed_application.string()},
      analysis_root);
  RequireSuccess(dependency_result, "dpkg-shlibdeps");
  const auto dependencies = ParseDependencies(dependency_result.output);

  const auto control_directory = package_root / "DEBIAN";
  std::filesystem::create_directories(control_directory, error);
  if (error) throw Error("Cannot create DEBIAN control directory: " +
                         error.message());
  const auto installed_size =
      (FileSize(installed_application) + 1023) / 1024;
  std::ostringstream control;
  control << "Package: " << package_name << "\n"
          << "Version: " << version << "\n"
          << "Section: utils\nPriority: optional\n"
          << "Architecture: amd64\n"
          << "Depends: " << dependencies << "\n"
          << "Installed-Size: " << installed_size << "\n"
          << "Maintainer: " << publisher << "\n"
          << "Description: " << description << "\n"
          << " Generated by lw.Web2App from packaged web resources.\n";
  WriteText(control_directory / "control", control.str());

  const auto package = output_directory /
                       (package_name + "_" + version + "_amd64.deb");
  std::filesystem::remove(package, error);
  const auto build_result = RunCommand(
      {"dpkg-deb", "--root-owner-group", "--build", package_root.string(),
       package.string()},
      output_directory);
  RequireSuccess(build_result, "dpkg-deb");
  std::ifstream package_stream(package, std::ios::binary);
  std::array<char, 8> magic{};
  package_stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  constexpr std::array<char, 8> deb_magic = {
      '!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
  if (package_stream.gcount() != static_cast<std::streamsize>(magic.size()) ||
      magic != deb_magic)
    throw Error("dpkg-deb did not create a valid Debian archive");
  return {package, package_name, package_name, dependencies};
}

}  // namespace lwweb
