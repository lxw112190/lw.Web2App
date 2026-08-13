#pragma once

#include "lwweb/packer/manifest.h"

#include <filesystem>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace lwweb {

// spdlog 的轻量同步封装；负责路径、日志级别、轮转和异常降级。
class Logger {
 public:
  Logger() = default;

  static Logger Rotating(const std::string& name,
                         const std::filesystem::path& file,
                         const LoggingConfig& config);
  static Logger Packer(const LoggingConfig& config = {});
  static Logger Runtime(const Manifest& manifest);

  bool Enabled() const;
  bool DebugEnabled() const;
  const std::filesystem::path& File() const;

  void Debug(const std::string& message) const;
  void Info(const std::string& message) const;
  void Warn(const std::string& message) const;
  void Error(const std::string& message) const;
  void Flush() const;

 private:
  Logger(std::shared_ptr<spdlog::logger> logger, std::filesystem::path file);

  std::shared_ptr<spdlog::logger> logger_;
  std::filesystem::path file_;
};

std::filesystem::path LocalAppDataRoot();
std::string EffectiveAppId(const Manifest& manifest);

}  // namespace lwweb
