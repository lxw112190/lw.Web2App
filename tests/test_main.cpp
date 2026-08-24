#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

void RunCliTests();
void RunFilesystemAccessTests();
void RunIpcTests();
void RunLocalFileBridgeTests();
void RunPayloadBindingTests();
void RunPayloadTests();
void RunPathTests();
void RunPublishConfigTests();
void RunPublisherTests();
void RunResourceTests();
void RunSigningTests();

#ifdef _WIN32
namespace {

// 测试进程同时充当一个最小的假 ISCC.exe：Installer 集成测试以真实的
// CreateProcess 路径调用它，由它检查关键 .iss 字段并生成 MZ 测试产物。
int RunFakeIscc(const std::filesystem::path& script_path) {
  std::ifstream input(script_path, std::ios::binary);
  const std::string script((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  const auto value = [&script](const std::string& field) {
    const auto start = script.find(field);
    if (start == std::string::npos) return std::string{};
    const auto value_start = start + field.size();
    const auto end = script.find_first_of("\r\n", value_start);
    return script.substr(value_start, end - value_start);
  };
  if (script.find("[Setup]") == std::string::npos ||
      script.find("AppId=com.example.publish-test") == std::string::npos ||
      script.find("DefaultDirName={autopf}\\Publish Test") ==
          std::string::npos ||
      script.find("UninstallDisplayIcon={app}\\Publish Test.exe") ==
          std::string::npos ||
      script.find("[Files]") == std::string::npos ||
      script.find("[Icons]") == std::string::npos ||
      script.find("{autoprograms}\\Publish Test") == std::string::npos ||
      script.find("{autodesktop}") != std::string::npos)
    return 7;
  const auto output_directory = value("OutputDir=");
  const auto output_basename = value("OutputBaseFilename=");
  if (output_directory.empty() || output_basename.empty()) return 8;
  const auto output = std::filesystem::u8path(output_directory) /
                      std::filesystem::u8path(output_basename + ".exe");
  std::ofstream installer(output, std::ios::binary | std::ios::trunc);
  installer << "MZfake-inno-setup";
  return installer ? 0 : 9;
}

}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc >= 2) {
    const auto candidate = std::filesystem::u8path(argv[argc - 1]);
    if (candidate.extension() == ".iss") return RunFakeIscc(candidate);
  }
#else
  (void)argc;
  (void)argv;
#endif
  try {
    RunCliTests();
    RunFilesystemAccessTests();
    RunIpcTests();
    RunLocalFileBridgeTests();
    RunPayloadBindingTests();
    RunPayloadTests();
    RunPathTests();
    RunPublishConfigTests();
    RunPublisherTests();
    RunResourceTests();
    RunSigningTests();
    std::cout << "All tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
