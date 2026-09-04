#include "lwweb/ipc/ipc_message.h"
#include "lwweb/runtime/system_paths.h"

#include <filesystem>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunSystemPathsTests() {
  const auto temp = lwweb::SystemPaths::Resolve("temp", "test.system.paths");
  Check(temp.is_absolute(), "system temp path is absolute");
  const auto app_data = lwweb::SystemPaths::Resolve("appData", "test.system.paths");
  Check(app_data.is_absolute() && app_data.u8string().find("test.system.paths") !=
                                      std::string::npos,
        "application data path is scoped by app id");
  bool unknown_rejected = false;
  try {
    (void)lwweb::SystemPaths::Resolve("arbitrary", "test.system.paths");
  } catch (const lwweb::IpcException& error) {
    unknown_rejected = error.Code() == "INVALID_ARGUMENT";
  }
  Check(unknown_rejected, "unknown system path names are rejected");
}
