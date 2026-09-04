#include "lwweb/runtime/single_instance.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

void RunSingleInstanceTests() {
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto app_id = std::string("com.lwweb.test.single-instance-") + suffix;
  {
    lwweb::SingleInstanceGuard first(app_id);
    Check(first.IsPrimary(), "first process acquires the single-instance lock");
    lwweb::SingleInstanceGuard second(app_id);
    Check(!second.IsPrimary(), "second process is recognized as a duplicate");
  }
  lwweb::SingleInstanceGuard after_release(app_id);
  Check(after_release.IsPrimary(), "single-instance lock is released on destruction");
}
