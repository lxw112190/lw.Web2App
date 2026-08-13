#include "lwweb/packer/packer.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/path_utils.h"
#include "lwweb/packer/payload.h"
#include "lwweb/pe/pe_resources.h"

#include <miniz.h>

#include <algorithm>
#include <fstream>
#include <vector>

namespace lwweb {
namespace {

void Progress(const PackOptions& options, const std::string& message) {
  if (options.progress) options.progress(message);
}

std::vector<std::uint8_t> BuildZip(const PackOptions& options) {
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 0)) throw Error("Cannot initialize ZIP writer");
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
    const auto file_bytes = ReadFileBytes(item.path());
    if (!mz_zip_writer_add_mem(&zip, normalized->c_str(), file_bytes.data(), file_bytes.size(),
                               MZ_BEST_SPEED))
      throw Error("Cannot add file to ZIP: " + *normalized);
  }
  if (count == 0) throw Error("Source directory is empty");
  void* buffer = nullptr;
  size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size))
    throw Error("Cannot finalize ZIP archive");
  std::vector<std::uint8_t> result(static_cast<std::uint8_t*>(buffer),
                                   static_cast<std::uint8_t*>(buffer) + size);
  mz_free(buffer);
  return result;
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
  ValidateManifest(options.manifest);
  if (options.runner.empty() || options.output.empty())
    throw Error("Runner and output paths are required");
  if (options.manifest.mode == AppMode::Local) {
    if (!std::filesystem::is_directory(options.source_directory))
      throw Error("Local mode requires a source directory");
    const auto entry = options.source_directory / Utf8ToWide(options.manifest.entry);
    if (!std::filesystem::is_regular_file(entry)) throw Error("Entry HTML file does not exist");
  }
  std::error_code error;
  if (!options.output.parent_path().empty())
    std::filesystem::create_directories(options.output.parent_path(), error);
  Progress(options, "Copying runner");
  CopyRunnerPrefix(options.runner, options.output);
  try {
    Progress(options, "Writing PE metadata");
    UpdatePeResources(options.output, options.metadata);
    std::vector<std::uint8_t> zip;
    std::uint32_t flags = 0;
    if (options.manifest.mode == AppMode::Local) {
      Progress(options, "Compressing static resources");
      zip = BuildZip(options);
      flags |= kPayloadHasZip;
    } else {
      flags |= kPayloadUrlMode;
    }
    Progress(options, "Appending payload and SHA-256");
    AppendPayload(options.output, zip, options.manifest, flags);
    Progress(options, "Done");
  } catch (...) {
    std::filesystem::remove(options.output, error);
    throw;
  }
}

}  // namespace lwweb
