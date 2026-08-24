#include "lwweb/publish/publisher.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/sha256.h"
#include "lwweb/packer/packer.h"
#include "lwweb/publish/publish_config.h"
#ifdef _WIN32
#include "lwweb/pe/authenticode.h"
#include "lwweb/publish/windows_installer.h"
#endif

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace lwweb {
namespace {

void Progress(const PublishOptions& options, const std::string& message) {
  if (options.progress) options.progress(message);
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
  std::error_code error;
  const auto result = std::filesystem::absolute(path, error);
  if (error) throw Error("Cannot resolve publish path: " + error.message());
  const auto canonical = std::filesystem::weakly_canonical(result, error);
  return error ? result.lexically_normal() : canonical;
}

bool IsInside(const std::filesystem::path& child,
              const std::filesystem::path& parent) {
  const auto normalized_child = AbsolutePath(child);
  const auto normalized_parent = AbsolutePath(parent);
  auto child_it = normalized_child.begin();
  for (auto parent_it = normalized_parent.begin();
       parent_it != normalized_parent.end(); ++parent_it, ++child_it) {
    if (child_it == normalized_child.end()) return false;
#ifdef _WIN32
    auto child_part = child_it->native();
    auto parent_part = parent_it->native();
    std::transform(child_part.begin(), child_part.end(), child_part.begin(),
                   [](wchar_t value) { return std::towlower(value); });
    std::transform(parent_part.begin(), parent_part.end(), parent_part.begin(),
                   [](wchar_t value) { return std::towlower(value); });
    if (child_part != parent_part) return false;
#else
    if (*child_it != *parent_it)
      return false;
#endif
  }
  return true;
}

std::string SafeArtifactName(const std::string& name) {
  std::string safe;
  safe.reserve((std::min)(name.size(), std::size_t{80}));
  for (std::size_t index = 0; index < name.size();) {
    const auto character = name[index];
    const auto byte = static_cast<unsigned char>(character);
    std::size_t character_size = 1;
    if ((byte & 0xe0) == 0xc0) character_size = 2;
    else if ((byte & 0xf0) == 0xe0) character_size = 3;
    else if ((byte & 0xf8) == 0xf0) character_size = 4;
    if (index + character_size > name.size() ||
        safe.size() + character_size > 80)
      break;
    if (byte < 0x20 || character == '<' || character == '>' ||
        character == ':' || character == '"' || character == '/' ||
        character == '\\' || character == '|' || character == '?' ||
        character == '*') {
      safe.push_back('_');
    } else {
      safe.append(name, index, character_size);
    }
    index += character_size;
  }
  while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.'))
    safe.pop_back();
  if (safe.empty()) safe = "WebApp";
#ifdef _WIN32
  auto device_name = safe;
  std::transform(device_name.begin(), device_name.end(), device_name.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::toupper(value));
                 });
  const auto first_dot = device_name.find('.');
  if (first_dot != std::string::npos) device_name.resize(first_dot);
  while (!device_name.empty() && device_name.back() == ' ')
    device_name.pop_back();
  const bool numbered_device =
      device_name.size() == 4 &&
      (device_name.rfind("COM", 0) == 0 || device_name.rfind("LPT", 0) == 0) &&
      device_name[3] >= '1' && device_name[3] <= '9';
  if (device_name == "CON" || device_name == "PRN" ||
      device_name == "AUX" || device_name == "NUL" || numbered_device)
    safe.insert(safe.begin(), '_');
#endif
  return safe;
}

std::filesystem::path UniqueDirectory(const std::filesystem::path& root,
                                      const std::string& prefix) {
  static std::atomic<std::uint64_t> sequence{0};
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::ostringstream name;
    name << prefix << std::hex
         << std::chrono::steady_clock::now().time_since_epoch().count()
         << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
    const auto candidate = root / std::filesystem::u8path(name.str());
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) return candidate;
    if (error && error != std::errc::file_exists)
      throw Error("Cannot create publish staging directory: " + error.message());
  }
  throw Error("Cannot allocate a unique publish staging directory");
}

// 临时发布目录的异常安全清理器。只有 Commit 后目录才由最终发布位置接管。
class StagingDirectory {
 public:
  explicit StagingDirectory(std::filesystem::path path) : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  const std::filesystem::path& Path() const { return path_; }
  void Commit() { committed_ = true; }

 private:
  std::filesystem::path path_;
  bool committed_ = false;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw Error("Cannot create publish metadata: " + path.u8string());
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output) throw Error("Cannot write publish metadata: " + path.u8string());
}

void CreateZip(const std::filesystem::path& source,
               const std::string& archive_name,
               const std::filesystem::path& output) {
  std::ofstream archive(output, std::ios::binary | std::ios::trunc);
  if (!archive) throw Error("Cannot create ZIP archive");
  mz_zip_archive zip{};
  zip.m_pIO_opaque = &archive;
  zip.m_pWrite = [](void* opaque, mz_uint64 offset, const void* buffer,
                    size_t size) -> size_t {
    auto* stream = static_cast<std::ofstream*>(opaque);
    stream->clear();
    stream->seekp(static_cast<std::streamoff>(offset));
    if (!*stream) return 0;
    stream->write(static_cast<const char*>(buffer),
                  static_cast<std::streamsize>(size));
    return *stream ? size : 0;
  };
  if (!mz_zip_writer_init(&zip, 0))
    throw Error("Cannot create ZIP archive");
  struct Guard {
    mz_zip_archive* zip;
    ~Guard() { mz_zip_writer_end(zip); }
  } guard{&zip};
  std::ifstream input(source, std::ios::binary);
  if (!input) throw Error("Cannot open application for ZIP archive");
  const auto read = [](void* opaque, mz_uint64 offset, void* buffer,
                       size_t bytes) -> size_t {
    auto* stream = static_cast<std::ifstream*>(opaque);
    stream->clear();
    stream->seekg(static_cast<std::streamoff>(offset));
    if (!*stream) return 0;
    stream->read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    return static_cast<size_t>(stream->gcount());
  };
  if (!mz_zip_writer_add_read_buf_callback(
          &zip, archive_name.c_str(), read, &input, FileSize(source), nullptr,
          nullptr, 0, MZ_BEST_COMPRESSION, nullptr, 0, nullptr, 0) ||
      !mz_zip_writer_finalize_archive(&zip))
    throw Error("Cannot finalize ZIP archive");
  archive.flush();
  if (!archive) throw Error("Cannot flush ZIP archive");
}

void SetOctal(char* target, std::size_t size, std::uint64_t value) {
  std::ostringstream text;
  text << std::oct << std::setfill('0') << std::setw(static_cast<int>(size - 1))
       << value;
  const auto value_text = text.str();
  if (value_text.size() >= size) throw Error("TAR value exceeds header capacity");
  std::memset(target, '0', size);
  std::memcpy(target + size - 1 - value_text.size(), value_text.data(),
              value_text.size());
  target[size - 1] = '\0';
}

void CreateTar(const std::filesystem::path& source,
               const std::string& archive_name,
               const std::filesystem::path& output) {
  if (archive_name.size() > 99) throw Error("Application name is too long for TAR");
  std::ofstream tar(output, std::ios::binary | std::ios::trunc);
  if (!tar) throw Error("Cannot create temporary TAR archive");
  std::array<char, 512> header{};
  std::memcpy(header.data(), archive_name.data(), archive_name.size());
  SetOctal(header.data() + 100, 8, 0755);
  SetOctal(header.data() + 108, 8, 0);
  SetOctal(header.data() + 116, 8, 0);
  SetOctal(header.data() + 124, 12, FileSize(source));
  SetOctal(header.data() + 136, 12, 0);
  std::memset(header.data() + 148, ' ', 8);
  header[156] = '0';
  std::memcpy(header.data() + 257, "ustar", 5);
  std::memcpy(header.data() + 263, "00", 2);
  std::uint64_t checksum = 0;
  for (const auto byte : header)
    checksum += static_cast<unsigned char>(byte);
  std::ostringstream checksum_text;
  checksum_text << std::oct << std::setfill('0') << std::setw(6) << checksum;
  const auto encoded_checksum = checksum_text.str();
  if (encoded_checksum.size() != 6) throw Error("TAR checksum exceeds header capacity");
  std::memcpy(header.data() + 148, encoded_checksum.data(), 6);
  header[154] = '\0';
  header[155] = ' ';
  tar.write(header.data(), static_cast<std::streamsize>(header.size()));

  std::ifstream input(source, std::ios::binary);
  if (!input) throw Error("Cannot open application for TAR archive");
  std::vector<char> buffer(1024 * 1024);
  std::uint64_t copied = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      tar.write(buffer.data(), count);
      copied += static_cast<std::uint64_t>(count);
    }
  }
  if (!input.eof() || !tar) throw Error("Cannot write TAR archive");
  const auto padding = (512 - (copied % 512)) % 512;
  std::array<char, 1024> zeros{};
  tar.write(zeros.data(), static_cast<std::streamsize>(padding + 1024));
  if (!tar) throw Error("Cannot finalize TAR archive");
}

struct DeflateOutput {
  std::ofstream* output = nullptr;
  bool failed = false;
};

mz_bool WriteDeflate(const void* data, int size, void* user) {
  auto* state = static_cast<DeflateOutput*>(user);
  state->output->write(static_cast<const char*>(data), size);
  state->failed = !*state->output;
  return state->failed ? MZ_FALSE : MZ_TRUE;
}

void CreateGzip(const std::filesystem::path& source,
                const std::filesystem::path& output) {
  std::ifstream input(source, std::ios::binary);
  std::ofstream gzip(output, std::ios::binary | std::ios::trunc);
  if (!input || !gzip) throw Error("Cannot open TAR/GZip stream");
  const std::array<unsigned char, 10> header =
      {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0, 0x03};
  gzip.write(reinterpret_cast<const char*>(header.data()), header.size());
  DeflateOutput state{&gzip, false};
  const auto compressor = std::make_unique<tdefl_compressor>();
  const auto flags = tdefl_create_comp_flags_from_zip_params(
      MZ_BEST_COMPRESSION, -15, MZ_DEFAULT_STRATEGY);
  if (tdefl_init(compressor.get(), WriteDeflate, &state, flags) !=
      TDEFL_STATUS_OKAY)
    throw Error("Cannot initialize GZip compressor");
  std::vector<unsigned char> buffer(1024 * 1024);
  mz_ulong crc = MZ_CRC32_INIT;
  std::uint64_t total = 0;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      crc = mz_crc32(crc, buffer.data(), static_cast<std::size_t>(count));
      total += static_cast<std::uint64_t>(count);
      if (tdefl_compress_buffer(compressor.get(), buffer.data(),
                                static_cast<std::size_t>(count),
                                TDEFL_NO_FLUSH) < TDEFL_STATUS_OKAY)
        throw Error("Cannot compress TAR archive");
    }
  }
  if (!input.eof() ||
      tdefl_compress_buffer(compressor.get(), nullptr, 0, TDEFL_FINISH) !=
          TDEFL_STATUS_DONE || state.failed)
    throw Error("Cannot finalize GZip archive");
  const auto write_little_endian = [&gzip](std::uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24)};
    gzip.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  };
  write_little_endian(static_cast<std::uint32_t>(crc));
  write_little_endian(static_cast<std::uint32_t>(total));
  if (!gzip) throw Error("Cannot write GZip trailer");
}

void CreateTarGzip(const std::filesystem::path& source,
                   const std::string& archive_name,
                   const std::filesystem::path& output) {
  auto temporary = output;
  temporary += ".tar.tmp";
  struct RemoveFile {
    std::filesystem::path path;
    ~RemoveFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
  } cleanup{temporary};
  CreateTar(source, archive_name, temporary);
  CreateGzip(temporary, output);
}

std::string FileDigest(const std::filesystem::path& path) {
  return HexDigest(Sha256FileRange(path, 0, FileSize(path)));
}

void PublishDirectory(const std::filesystem::path& staging,
                      const std::filesystem::path& final,
                      const std::filesystem::path& output_root) {
  std::error_code error;
  if (!std::filesystem::exists(final, error)) {
    std::filesystem::rename(staging, final, error);
    if (error) throw Error("Cannot publish release directory: " + error.message());
    return;
  }
  const auto backup = UniqueDirectory(output_root, ".lwweb-previous-");
  std::filesystem::remove(backup, error);
  error.clear();
  std::filesystem::rename(final, backup, error);
  if (error) throw Error("Cannot preserve previous release: " + error.message());
  std::filesystem::rename(staging, final, error);
  if (error) {
    std::error_code restore_error;
    std::filesystem::rename(backup, final, restore_error);
    throw Error("Cannot publish release directory: " + error.message());
  }
  std::filesystem::remove_all(backup, error);
}

}  // namespace

PublishResult PublishProject(const PublishOptions& options) {
  if (options.runner.empty()) throw Error("publish requires a runner executable");
  auto config = LoadProjectConfig(options.config_file);
  auto output_root = options.output_override
                         ? AbsolutePath(*options.output_override)
                         : config.publish.output;
  if (IsInside(output_root, config.pack.source_directory))
    throw Error("publish output must not be inside the web source directory");

  const bool windows = options.platform == PublishPlatform::Windows;
  if (!windows && config.publish.linux.deb)
    throw Error("Linux application DEB publishing is not available yet");
  const bool portable = windows ? config.publish.windows.portable : true;
  const bool archive = windows ? config.publish.windows.zip
                               : config.publish.linux.tar_gz;
  const bool installer =
      windows && config.publish.windows.installer.enabled;
#ifndef _WIN32
  if (installer)
    throw Error("Windows installer publishing requires a Windows host");
#endif
  if (!portable && !archive && !installer)
    throw Error("publish has no enabled artifacts for this platform");

  std::error_code error;
  std::filesystem::create_directories(output_root, error);
  if (error) throw Error("Cannot create publish output directory: " + error.message());
  const auto safe_name = SafeArtifactName(config.app.name);
  const auto platform = windows ? "windows" : "linux";
  const auto release_name = safe_name + "-" + config.app.version + "-" +
                            platform + "-x64";
  const auto final_directory = output_root / std::filesystem::u8path(release_name);
  StagingDirectory staging(UniqueDirectory(output_root, ".lwweb-publish-"));

  Progress(options, "Publish: pack application");
  auto pack = config.pack;
  pack.runner = options.runner;
  const auto application_name = safe_name + (windows ? ".exe" : "");
  pack.output = staging.Path() / std::filesystem::u8path(application_name);
  if (windows) pack.signing = config.publish.windows.signing;
  pack.progress = options.progress;
  PackApplication(pack);

  ReleaseInfo info{config.app.id, config.app.name, config.app.version, {}};
  std::vector<std::filesystem::path> distributables;
  std::optional<std::filesystem::path> installer_path;
  if (installer) {
    Progress(options, "Publish: build Windows installer");
#ifdef _WIN32
    const auto installer_basename =
        safe_name + "-Setup-" + config.app.version;
    WindowsInstallerBuildOptions installer_options;
    installer_options.application = pack.output;
    installer_options.output_directory = staging.Path();
    installer_options.configured_iscc =
        config.publish.windows.installer.iscc;
    installer_options.output_basename = installer_basename;
    installer_options.app_id = config.app.id;
    installer_options.app_name = config.app.name;
    installer_options.app_version = config.app.version;
    installer_options.publisher = config.app.company;
    installer_options.desktop_shortcut =
        config.publish.windows.installer.desktop_shortcut;
    installer_options.start_menu =
        config.publish.windows.installer.start_menu;
    installer_path = BuildWindowsInstaller(installer_options);
    if (pack.signing.enabled) {
      Progress(options, "Publish: sign Windows installer");
      SignAuthenticode(*installer_path, pack.signing);
    }
#endif
  }
  std::optional<std::filesystem::path> archive_path;
  if (archive) {
    Progress(options, windows ? "Publish: create ZIP archive"
                              : "Publish: create tar.gz archive");
    const auto extension = windows ? ".zip" : ".tar.gz";
    archive_path =
        staging.Path() / std::filesystem::u8path(release_name + extension);
    if (windows)
      CreateZip(pack.output, application_name, *archive_path);
    else
      CreateTarGzip(pack.output, application_name, *archive_path);
  }
  if (portable) {
    distributables.push_back(pack.output);
    info.artifacts.push_back({application_name, "portable", platform, "x64",
                              windows && pack.signing.enabled, {}});
  } else {
    std::filesystem::remove(pack.output, error);
    if (error) throw Error("Cannot remove intermediate portable application");
  }
  if (installer_path) {
    distributables.push_back(*installer_path);
    info.artifacts.push_back({installer_path->filename().u8string(), "installer",
                              platform, "x64", pack.signing.enabled, {}});
  }
  if (archive_path) {
    distributables.push_back(*archive_path);
    info.artifacts.push_back({archive_path->filename().u8string(), "archive",
                              platform, "x64", false, {}});
  }

  Progress(options, "Publish: calculate SHA-256 checksums");
  std::ostringstream checksums;
  for (std::size_t i = 0; i < distributables.size(); ++i) {
    const auto digest = FileDigest(distributables[i]);
    info.artifacts[i].sha256 = digest;
    checksums << digest << "  " << distributables[i].filename().u8string() << '\n';
  }
  WriteText(staging.Path() / "SHA256SUMS.txt", checksums.str());
  WriteText(staging.Path() / "RELEASE_INFO.json", SerializeReleaseInfo(info));

  Progress(options, "Publish: atomically replace release directory");
  PublishDirectory(staging.Path(), final_directory, output_root);
  staging.Commit();
  Progress(options, "Publish completed: " + final_directory.u8string());
  return {final_directory, std::move(info)};
}

}  // namespace lwweb
