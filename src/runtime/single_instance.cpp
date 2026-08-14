#include "lwweb/runtime/single_instance.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <filesystem>

namespace lwweb {

struct SingleInstanceGuard::Impl {
#ifdef _WIN32
  HANDLE mutex = nullptr;
#else
  int descriptor = -1;
#endif

  explicit Impl(const std::string& app_id) {
#ifdef _WIN32
    const auto name = L"Local\\lw.Web2App." + Utf8ToWide(app_id);
    mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex)
      throw Error("Cannot create the application single-instance mutex: " +
                  WindowsErrorMessage(GetLastError()));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(mutex);
      mutex = nullptr;
      throw Error("Another instance of this application is already running");
    }
#else
    const auto directory = LocalAppDataRoot() / "apps" / app_id;
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error)
      throw Error("Cannot create the application state directory: " +
                  filesystem_error.message());
    const auto lock_file = directory / "instance.lock";
    descriptor = open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
      throw Error("Cannot open the application single-instance lock: " +
                  std::string(std::strerror(errno)));
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid()) {
      close(descriptor);
      descriptor = -1;
      throw Error("The application single-instance lock is not a trusted regular file");
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      const auto error = errno;
      close(descriptor);
      descriptor = -1;
      if (error == EWOULDBLOCK || error == EAGAIN)
        throw Error("Another instance of this application is already running");
      throw Error("Cannot lock the application single-instance file: " +
                  std::string(std::strerror(error)));
    }
    (void)ftruncate(descriptor, 0);
    const auto pid = std::to_string(static_cast<long long>(getpid())) + "\n";
    (void)write(descriptor, pid.data(), pid.size());
#endif
  }

  ~Impl() {
#ifdef _WIN32
    if (mutex) CloseHandle(mutex);
#else
    if (descriptor >= 0) {
      (void)flock(descriptor, LOCK_UN);
      close(descriptor);
    }
#endif
  }
};

SingleInstanceGuard::SingleInstanceGuard(const std::string& app_id)
    : impl_(std::make_unique<Impl>(app_id)) {}

SingleInstanceGuard::~SingleInstanceGuard() = default;

}  // namespace lwweb
