#include "lwweb/packer/packer.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/common/sha256.h"
#include "lwweb/packer/payload.h"
#ifdef _WIN32
#include "lwweb/pe/authenticode.h"
#include "lwweb/pe/pe_resources.h"
#include <Windows.h>
#endif

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

namespace lwweb {
namespace {

// 为每次打包生成与最终产物同目录的唯一暂存文件。保持在同一目录可确保
// 最后发布时不会跨卷复制，并避免构建中的半成品被资源管理器当作 EXE 扫描。
std::filesystem::path StagingPathFor(const std::filesystem::path& output) {
  static std::atomic<std::uint64_t> sequence{0};
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::ostringstream suffix;
    suffix << ".lwweb-building-" << std::hex
           << std::chrono::steady_clock::now().time_since_epoch().count() << '-'
           << sequence.fetch_add(1, std::memory_order_relaxed) << ".tmp";
    auto staging = output;
    staging += std::filesystem::u8path(suffix.str());
    std::error_code error;
    if (!std::filesystem::exists(staging, error)) return staging;
  }
  throw Error("Cannot allocate a unique staging file in the output directory");
}

#ifdef _WIN32
bool IsTransientPublishError(DWORD code) {
  return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
         code == ERROR_LOCK_VIOLATION || code == ERROR_OPEN_FAILED ||
         code == ERROR_USER_MAPPED_FILE || code == ERROR_RETRY;
}
#endif

// 将已经完整校验的暂存文件发布为最终产物。暂存文件与目标位于同一目录，
// Windows 使用 MoveFileEx(REPLACE_EXISTING)，Linux 使用同文件系统 rename；
// 两者都不会先删除旧产物，因此发布失败时旧文件仍然可用。
void PublishCompletedFile(const std::filesystem::path& staging,
                          const std::filesystem::path& output) {
#ifdef _WIN32
  constexpr std::array<DWORD, 3> retry_delays_ms = {75, 200, 500};
  for (std::size_t attempt = 0; attempt <= retry_delays_ms.size(); ++attempt) {
    if (attempt != 0) Sleep(retry_delays_ms[attempt - 1]);
    if (MoveFileExW(staging.c_str(), output.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return;
    const auto code = GetLastError();
    if (!IsTransientPublishError(code) || attempt == retry_delays_ms.size()) {
      throw Error("Cannot publish completed application (Win32 error " +
                  std::to_string(code) + "): " + WindowsErrorMessage(code));
    }
  }
#else
  std::error_code error;
  std::filesystem::rename(staging, output, error);
  if (error)
    throw Error("Cannot publish completed application: " + error.message());
#endif
}

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
  temporary += ".payload.tmp";
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
    const auto relative =
        std::filesystem::relative(item.path(), source).generic_u8string();
    const auto normalized = NormalizeArchivePath(relative);
    if (!normalized) throw Error("Source contains an unsafe relative path");
    if (*normalized == "__lw_file__" ||
        normalized->rfind("__lw_file__/", 0) == 0)
      throw Error("Source resource conflicts with the local file bridge prefix");
    if (options.manifest.backend_proxy.enabled) {
      const auto proxy = options.manifest.backend_proxy.prefix.substr(1);
      if (*normalized == proxy || normalized->rfind(proxy + "/", 0) == 0)
        throw Error("Source resource conflicts with the backend proxy prefix");
    }
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
  out.flush();
  if (!out) throw Error("Cannot flush copied runner");
  out.close();
  if (!out) throw Error("Cannot close copied runner");
  input.close();
}

}  // namespace

PreparedPayload PreparePayload(const PackOptions& options,
                               Manifest manifest) {
  ValidateManifest(manifest);
  if (manifest.mode == AppMode::Local) {
    if (!IsCanonicalArchivePath(manifest.entry))
      throw Error("Entry HTML path must be a canonical relative archive path");
    if (!std::filesystem::is_directory(options.source_directory))
      throw Error("Local mode requires a source directory");
    const auto entry =
        options.source_directory / std::filesystem::u8path(manifest.entry);
    if (!std::filesystem::is_regular_file(entry))
      throw Error("Entry HTML file does not exist");
  }

  PreparedPayload prepared;
  try {
    if (manifest.mode == AppMode::Local) {
      const auto zip = BuildZip(options);
      prepared.zip = zip.path;
      prepared.file_count = zip.file_count;
      prepared.source_size = zip.source_size;
      prepared.compressed_size = zip.compressed_size;
      prepared.flags |= kPayloadHasZip;
    } else {
      prepared.flags |= kPayloadUrlMode;
    }
    manifest.legacy_payload_sha256.clear();
    prepared.manifest_json = SerializeManifest(manifest);
    prepared.sha256 =
        Sha256FileWithSuffix(prepared.zip, prepared.manifest_json);
    return prepared;
  } catch (...) {
    auto temporary = options.output;
    temporary += ".payload.tmp";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

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
  std::filesystem::path staging;
  try {
    phase = "manifest validation";
    auto manifest = options.manifest;
    if (manifest.app_id.empty())
      manifest.app_id = "app-" + HexDigest(Sha256(
          reinterpret_cast<const std::uint8_t*>(manifest.title.data()), manifest.title.size()))
                              .substr(0, 24);
    if (manifest.mode == AppMode::Local && !IsCanonicalArchivePath(manifest.entry))
      throw Error("Entry HTML path must be a canonical relative archive path");
    ValidateManifest(manifest);
    log.Info("Packaging started");
    log.Info("Mode: " + std::string(manifest.mode == AppMode::Local ? "local" : "url"));
    log.Info("Entry: " + (manifest.mode == AppMode::Local ? manifest.entry : manifest.url));
    if (manifest.mode == AppMode::Local) log.Info("Start path: " + manifest.start_path);
    log.Info("SPA fallback: " + std::string(manifest.spa_fallback ? "true" : "false"));
    if (manifest.mode == AppMode::Local)
      log.Info("Source: " + options.source_directory.u8string());
    if (options.runner.empty() || options.output.empty())
      throw Error("Runner and output paths are required");
#ifndef _WIN32
    if (options.signing.enabled)
      throw Error("Authenticode signing is available only on Windows");
#endif
    if (std::filesystem::absolute(options.runner).lexically_normal() ==
        std::filesystem::absolute(options.output).lexically_normal())
      throw Error("Output path must differ from the runner path");
    if (manifest.mode == AppMode::Local) {
      if (!std::filesystem::is_directory(options.source_directory))
        throw Error("Local mode requires a source directory");
      const auto entry = options.source_directory / std::filesystem::u8path(manifest.entry);
      if (!std::filesystem::is_regular_file(entry)) throw Error("Entry HTML file does not exist");
    }
    if (!options.output.parent_path().empty()) {
      std::filesystem::create_directories(options.output.parent_path(), error);
      if (error) throw Error("Cannot create output directory: " + error.message());
    }

    staging = StagingPathFor(options.output);
    auto working = options;
    working.output = staging;
    log.Debug("Staging: " + staging.u8string());

    phase = "Payload preparation";
    Progress(options, manifest.mode == AppMode::Local
                          ? "Compressing static resources"
                          : "Preparing payload");
    auto prepared = PreparePayload(working, manifest);
    if (manifest.mode == AppMode::Local) {
      log.Info("Files: " + std::to_string(prepared.file_count));
      log.Info("Source size: " + HumanBytes(prepared.source_size));
      log.Info("Compressed size: " + HumanBytes(prepared.compressed_size));
    }
    log.Info("Prepared Payload SHA-256: " + HexDigest(prepared.sha256));

    phase = "runner copy";
    Progress(options, "Copying runner");
#ifdef _WIN32
    const bool inherited_signature =
        HasAuthenticodeSignature(options.runner);
    if (inherited_signature) {
      // 先在 Certificate Table 仍存在时定位 Payload；SignTool 可能在
      // Footer 与证书表之间加入对齐字节，截掉证书后不能再依赖文件末尾定位。
      const auto content_end = AuthenticodeContentEnd(options.runner);
      if (!content_end)
        throw Error("Signed runner does not contain a Certificate Table");
      const auto prefix_size = HasPayload(options.runner)
                                   ? RunnerPrefixSize(options.runner)
                                   : *content_end;
      std::filesystem::copy_file(options.runner, staging,
                                 std::filesystem::copy_options::overwrite_existing,
                                 error);
      if (error) throw Error("Cannot copy signed runner: " + error.message());
      phase = "inherited Authenticode removal";
      (void)StripAuthenticodeSignature(staging);
      std::filesystem::resize_file(staging, prefix_size, error);
      if (error) throw Error("Cannot remove inherited payload: " + error.message());
    } else {
      CopyRunnerPrefix(options.runner, staging);
    }
#else
    CopyRunnerPrefix(options.runner, staging);
#endif
#ifdef _WIN32
    log.Info(inherited_signature
                 ? "Inherited Authenticode signature removed"
                 : "Runner does not contain an Authenticode signature");
#endif
    phase = "platform metadata update";
    Progress(options, "Writing platform metadata");
#ifdef _WIN32
    std::optional<PayloadBinding> binding;
    if (options.signing.enabled) {
      binding.emplace();
      binding->flags = kBindingAuthenticodeRequired;
      binding->payload_sha256 = prepared.sha256;
    }
    UpdatePeResources(staging, options.metadata, binding);
    log.Info("PE version metadata updated successfully");
    log.Info(options.metadata.icon.empty() ? "PE icon: default icon retained"
                                          : "PE icon updated successfully");
    log.Info(binding ? "Signed LWWEB_BINDING resource updated successfully"
                     : "Unsigned output does not contain LWWEB_BINDING");
#else
    std::filesystem::permissions(
        staging,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, error);
    if (error) throw Error("Cannot make the generated Linux application executable");
    log.Info("Linux executable permissions updated successfully");
#endif
    phase = "Prepared Payload append";
    Progress(options, "Appending prepared payload");
    AppendPreparedPayload(staging, prepared);
#ifdef _WIN32
    if (options.signing.enabled) {
      phase = "Authenticode signing";
      Progress(options, "Signing application");
      SignAuthenticode(staging, options.signing);
      log.Info("Authenticode signature created successfully");
    }
#endif
    const auto loaded = LoadPayload(staging);
#ifdef _WIN32
    VerifyPePayloadBinding(staging, loaded.footer.sha256);
#endif
    log.Info("Payload SHA-256: " + HexDigest(loaded.footer.sha256));
    if (!prepared.zip.empty()) std::filesystem::remove(prepared.zip, error);

    phase = "atomic output publication";
    Progress(options, "Publishing output");
    PublishCompletedFile(staging, options.output);
    staging.clear();
    log.Info("Output: " + options.output.u8string());
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
    // 失败只清理本次暂存文件，绝不删除用户原有的最终产物。
    if (!staging.empty()) {
      std::filesystem::remove(staging, error);
      auto payload_temporary = staging;
      payload_temporary += ".payload.tmp";
      std::filesystem::remove(payload_temporary, error);
      auto pe_backup = staging;
      pe_backup += L".pe-resource-backup.tmp";
      std::filesystem::remove(pe_backup, error);
    }
    throw;
  }
}

}  // namespace lwweb
