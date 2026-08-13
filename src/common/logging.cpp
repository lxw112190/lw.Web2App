#include "lwweb/common/logging.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/sha256.h"

#include <ShlObj.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>

namespace lwweb {
namespace {

std::atomic<std::uint64_t> sequence{0};

spdlog::level::level_enum ParseLevel(const std::string& level) {
  if (level == "debug") return spdlog::level::debug;
  if (level == "warn") return spdlog::level::warn;
  if (level == "error") return spdlog::level::err;
  return spdlog::level::info;
}

}  // namespace

std::filesystem::path LocalAppDataRoot() {
  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                  &local_app_data)))
    throw Error("Cannot locate the local application data directory");
  const std::filesystem::path root(local_app_data);
  CoTaskMemFree(local_app_data);
  return root / L"lw.Web2App";
}

std::string EffectiveAppId(const Manifest& manifest) {
  if (IsValidAppId(manifest.app_id)) return manifest.app_id;
  return "legacy-" + HexDigest(Sha256(
      reinterpret_cast<const std::uint8_t*>(manifest.title.data()), manifest.title.size()))
                         .substr(0, 24);
}

Logger::Logger(std::shared_ptr<spdlog::logger> logger, std::filesystem::path file)
    : logger_(std::move(logger)), file_(std::move(file)) {}

Logger Logger::Rotating(const std::string& name, const std::filesystem::path& file,
                        const LoggingConfig& config) {
  if (!config.enabled) return {};
  if (config.max_file_size < 64 * 1024 || config.max_file_size > 128ull * 1024 * 1024 ||
      config.max_files == 0 || config.max_files > 20)
    throw lwweb::Error("Logging rotation configuration is outside the supported range");
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  if (error) throw lwweb::Error("Cannot create the log directory");
  try {
    const auto unique_name = name + "-" + std::to_string(GetCurrentProcessId()) + "-" +
                             std::to_string(++sequence);
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        file.wstring(), static_cast<std::size_t>(config.max_file_size), config.max_files);
    auto logger = std::make_shared<spdlog::logger>(unique_name, sink);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v");
    logger->set_level(ParseLevel(config.level));
    // INFO volume is intentionally low; flushing each operational event preserves
    // diagnostics even if a customer terminates a hung runtime process.
    logger->flush_on(spdlog::level::info);
    return Logger(std::move(logger), file);
  } catch (const spdlog::spdlog_ex& error) {
    throw lwweb::Error(std::string("Cannot initialize log file: ") + error.what());
  }
}

Logger Logger::Packer(const LoggingConfig& config) {
  return Rotating("lw.Web2App.Packer", LocalAppDataRoot() / L"logs" / L"packer.log",
                  config);
}

Logger Logger::Runtime(const Manifest& manifest) {
  return Rotating("lw.WebRuntime",
                  LocalAppDataRoot() / L"apps" / Utf8ToWide(EffectiveAppId(manifest)) /
                      L"logs" / L"app.log",
                  manifest.logging);
}

bool Logger::Enabled() const { return static_cast<bool>(logger_); }
bool Logger::DebugEnabled() const {
  return logger_ && logger_->should_log(spdlog::level::debug);
}
const std::filesystem::path& Logger::File() const { return file_; }
void Logger::Debug(const std::string& message) const { if (logger_) logger_->debug(message); }
void Logger::Info(const std::string& message) const { if (logger_) logger_->info(message); }
void Logger::Warn(const std::string& message) const { if (logger_) logger_->warn(message); }
void Logger::Error(const std::string& message) const { if (logger_) logger_->error(message); }
void Logger::Flush() const { if (logger_) logger_->flush(); }

}  // namespace lwweb
