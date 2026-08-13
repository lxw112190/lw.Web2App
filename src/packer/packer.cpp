#include "lwweb/packer/packer.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/sha256.h"
#include "lwweb/packer/payload.h"
#include "lwweb/pe/pe_resources.h"

#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace lwweb {
namespace {

void Progress(const PackOptions& options, const std::string& message) {
  if (options.progress) options.progress(message);
}

std::string HumanBytes(std::uint64_t bytes) {
  static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' '
         << units[unit] << " (" << bytes << " bytes)";
  return output.str();
}

// 在磁盘临时文件中流式构建 ZIP，避免源文件和完整 ZIP 同时驻留内存。
struct ZipBuildResult {
  std::filesystem::path path;
  std::uint32_t file_count = 0;
  std::uint64_t source_size = 0;
  std::uint64_t compressed_size = 0;
};

ZipBuildResult BuildZip(const PackOptions& options) {
  auto temporary = options.output;
  temporary += L".payload.tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  std::ofstream archive(temporary, std::ios::binary | std::ios::trunc);
  if (!archive) throw Error("Cannot create temporary ZIP payload");
  mz_zip_archive zip{};
  zip.m_pIO_opaque = &archive;
  zip.m_pWrite = [](void* opaque, mz_uint64 offset, const void* buffer,
                    size_t size) -> size_t {
    auto* output = static_cast<std::ofstream*>(opaque);
    output->clear();
    output->seekp(static_cast<std::streamoff>(offset));
    if (!*output) return 0;
    output->write(static_cast<const char*>(buffer),
                  static_cast<std::streamsize>(size));
    return *output ? size : 0;
  };
  if (!mz_zip_writer_init(&zip, 0)) {
    archive.close();
    std::filesystem::remove(temporary, ignored);
    throw Error("Cannot initialize ZIP writer");
  }
  // 确保 BuildZip 的任意异常路径都会释放 miniz writer 状态。
  struct Guard {
    mz_zip_archive* zip;
    ~Guard() { mz_zip_writer_end(zip); }
  } guard{&zip};

  std::uint64_t total = 0;
  std::uint32_t count = 0;
  const auto source = std::filesystem::weakly_canonical(options.source_directory);
  for (const auto& item : std::filesystem::recursive_directory_iterator(source)) {
    if (!item.is_regular_file()) continue;
    if (++count > options.limits.max_file_count) throw Error("Source contains too many files");
    const auto size = item.file_size();
    if (size > options.limits.max_file_size) throw Error("A source file exceeds the safety limit");
    if (size > options.limits.max_total_size - total) throw Error("Source exceeds the total size limit");
    total += size;
    const auto relative = WideToUtf8(
        std::filesystem::relative(item.path(), source).generic_wstring());
    const auto normalized = NormalizeArchivePath(relative);
    if (!normalized) throw Error("Source contains an unsafe relative path");
    std::ifstream source_file(item.path(), std::ios::binary);
    if (!source_file) throw Error("Cannot open source file: " + *normalized);
    const auto read = [](void* opaque, mz_uint64 offset, void* buffer,
                         size_t bytes) -> size_t {
      auto* input = static_cast<std::ifstream*>(opaque);
      input->clear();
      input->seekg(static_cast<std::streamoff>(offset));
      if (!*input) return 0;
      input->read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
      return static_cast<size_t>(input->gcount());
    };
    if (!mz_zip_writer_add_read_buf_callback(
            &zip, normalized->c_str(), read, &source_file, size, nullptr, nullptr, 0,
            MZ_BEST_SPEED, nullptr, 0, nullptr, 0))
      throw Error("Cannot add file to ZIP: " + *normalized);
  }
  if (count == 0) throw Error("Source directory is empty");
  if (!mz_zip_writer_finalize_archive(&zip))
    throw Error("Cannot finalize ZIP archive");
  archive.flush();
  if (!archive) throw Error("Cannot flush temporary ZIP payload");
  archive.close();
  return {temporary, count, total, std::filesystem::file_size(temporary)};
}

void CopyRunnerPrefix(const std::filesystem::path& runner,
                      const std::filesystem::path& output) {
  if (std::filesystem::absolute(runner).lexically_normal() ==
      std::filesystem::absolute(output).lexically_normal())
    throw Error("Output path must differ from the runner path");
  const auto prefix = RunnerPrefixSize(runner);
  std::ifstream input(runner, std::ios::binary);
  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!input || !out) throw Error("Cannot create output executable");
  std::vector<char> buffer(1024 * 1024);
  std::uint64_t remaining = prefix;
  while (remaining) {
    const auto count = static_cast<std::size_t>((std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
    input.read(buffer.data(), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count)) throw Error("Runner is truncated");
    out.write(buffer.data(), static_cast<std::streamsize>(count));
    if (!out) throw Error("Cannot copy runner");
    remaining -= count;
  }
}

}  // namespace

void PackApplication(const PackOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  Logger log;
  std::string phase = "logger initialization";
  try {
    LoggingConfig config;
    config.enabled = true;
    config.level = options.manifest.logging.level;
    log = Logger::Packer(config);
  } catch (...) {
    // Logging is diagnostic and must never prevent packaging.
  }
  std::error_code error;
  try {
  phase = "manifest validation";
  auto manifest = options.manifest;
  if (manifest.app_id.empty())
    manifest.app_id = "app-" + HexDigest(Sha256(
        reinterpret_cast<const std::uint8_t*>(manifest.title.data()), manifest.title.size()))
                            .substr(0, 24);
  ValidateManifest(manifest);
  log.Info("Packaging started");
  log.Info("Mode: " + std::string(manifest.mode == AppMode::Local ? "local" : "url"));
  log.Info("Entry: " + (manifest.mode == AppMode::Local ? manifest.entry : manifest.url));
  log.Info("SPA fallback: " + std::string(manifest.spa_fallback ? "true" : "false"));
  if (manifest.mode == AppMode::Local)
    log.Info("Source: " + WideToUtf8(options.source_directory.wstring()));
  if (options.runner.empty() || options.output.empty())
    throw Error("Runner and output paths are required");
  if (options.manifest.mode == AppMode::Local) {
    if (!std::filesystem::is_directory(options.source_directory))
      throw Error("Local mode requires a source directory");
    const auto entry = options.source_directory / Utf8ToWide(options.manifest.entry);
    if (!std::filesystem::is_regular_file(entry)) throw Error("Entry HTML file does not exist");
  }
  if (!options.output.parent_path().empty())
    std::filesystem::create_directories(options.output.parent_path(), error);
  phase = "runner copy";
  Progress(options, "Copying runner");
  CopyRunnerPrefix(options.runner, options.output);
    phase = "PE resource update";
    Progress(options, "Writing PE metadata");
    UpdatePeResources(options.output, options.metadata);
    log.Info("PE version metadata updated successfully");
    log.Info(options.metadata.icon.empty() ? "PE icon: default icon retained"
                                          : "PE icon updated successfully");
    ZipBuildResult zip;
    std::uint32_t flags = 0;
    if (options.manifest.mode == AppMode::Local) {
      phase = "ZIP compression";
      Progress(options, "Compressing static resources");
      zip = BuildZip(options);
      log.Info("Files: " + std::to_string(zip.file_count));
      log.Info("Source size: " + HumanBytes(zip.source_size));
      log.Info("Compressed size: " + HumanBytes(zip.compressed_size));
      flags |= kPayloadHasZip;
    } else {
      flags |= kPayloadUrlMode;
    }
    phase = "Manifest, SHA-256 and Payload append";
    Progress(options, "Appending payload and SHA-256");
    AppendPayload(options.output, zip.path, manifest, flags);
    const auto loaded = LoadPayload(options.output);
    log.Info("Payload SHA-256: " + HexDigest(loaded.footer.sha256));
    log.Info("Output: " + WideToUtf8(options.output.wstring()));
    if (!zip.path.empty()) std::filesystem::remove(zip.path, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log.Info("Packaging completed in " + std::to_string(elapsed.count()) + " ms");
    log.Flush();
    Progress(options, "Done");
  } catch (...) {
    try {
      throw;
    } catch (const std::exception& exception) {
      log.Error("Packaging failed at " + phase + ": " + exception.what());
      log.Flush();
    } catch (...) {
      log.Error("Packaging failed with an unknown error");
      log.Flush();
    }
    std::filesystem::remove(options.output, error);
    auto temporary = options.output;
    temporary += L".payload.tmp";
    std::filesystem::remove(temporary, error);
    throw;
  }
}

}  // namespace lwweb
