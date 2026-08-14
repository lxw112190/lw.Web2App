#include "lwweb/packer/payload.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace lwweb {
namespace {

void Put32(std::uint8_t*& output, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) *output++ = static_cast<std::uint8_t>(value >> (i * 8));
}

void Put64(std::uint8_t*& output, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) *output++ = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint32_t Get32(const std::uint8_t*& input) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(*input++) << (i * 8);
  return value;
}

std::uint64_t Get64(const std::uint8_t*& input) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(*input++) << (i * 8);
  return value;
}

std::array<std::uint8_t, kPayloadFooterSize> ReadFooterBytes(
    const std::filesystem::path& executable) {
  const auto size = FileSize(executable);
  if (size < kPayloadFooterSize) throw Error("Executable does not contain a payload footer");
  std::ifstream input(executable, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(size - kPayloadFooterSize));
  std::array<std::uint8_t, kPayloadFooterSize> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!input) throw Error("Cannot read payload footer");
  return bytes;
}

}  // namespace

void ValidatePayloadBounds(const PayloadFooter& footer, std::uint64_t file_size) {
  if (file_size < kPayloadFooterSize) throw Error("Executable is smaller than the payload footer");
  if (footer.version != kPayloadVersionV1 && footer.version != kPayloadVersion)
    throw Error("Unsupported payload version");
  const std::uint64_t footer_offset = file_size - kPayloadFooterSize;
  if (footer.payload_offset > footer_offset ||
      footer.payload_size > footer_offset - footer.payload_offset)
    throw Error("Payload range is outside the executable");
  if (footer.manifest_offset != footer.payload_offset + footer.payload_size ||
      footer.manifest_offset > footer_offset ||
      footer.manifest_size > footer_offset - footer.manifest_offset ||
      footer.manifest_offset + footer.manifest_size != footer_offset)
    throw Error("Manifest range is invalid");
  if (footer.manifest_size > 1024 * 1024) throw Error("Manifest exceeds the 1 MiB limit");
}

std::array<std::uint8_t, kPayloadFooterSize> EncodeFooter(const PayloadFooter& footer) {
  std::array<std::uint8_t, kPayloadFooterSize> bytes{};
  auto* output = bytes.data();
  const auto& magic = footer.version == kPayloadVersionV1 ? kPayloadMagicV1 : kPayloadMagicV2;
  std::copy(magic.begin(), magic.end(), output);
  output += magic.size();
  Put32(output, footer.version);
  Put32(output, footer.flags);
  Put64(output, footer.payload_offset);
  Put64(output, footer.payload_size);
  Put64(output, footer.manifest_offset);
  Put64(output, footer.manifest_size);
  std::copy(footer.sha256.begin(), footer.sha256.end(), output);
  return bytes;
}

PayloadFooter DecodeFooter(const std::array<std::uint8_t, kPayloadFooterSize>& bytes) {
  const bool v1 = std::equal(kPayloadMagicV1.begin(), kPayloadMagicV1.end(), bytes.begin());
  const bool v2 = std::equal(kPayloadMagicV2.begin(), kPayloadMagicV2.end(), bytes.begin());
  if (!v1 && !v2)
    throw Error("Payload magic was not found");
  const auto* input = bytes.data() + kPayloadMagicV2.size();
  PayloadFooter footer;
  footer.version = Get32(input);
  if ((v1 && footer.version != kPayloadVersionV1) ||
      (v2 && footer.version != kPayloadVersion))
    throw Error("Payload magic and version do not match");
  footer.flags = Get32(input);
  footer.payload_offset = Get64(input);
  footer.payload_size = Get64(input);
  footer.manifest_offset = Get64(input);
  footer.manifest_size = Get64(input);
  std::copy(input, input + footer.sha256.size(), footer.sha256.begin());
  return footer;
}

bool HasPayload(const std::filesystem::path& executable) {
  try {
    (void)DecodeFooter(ReadFooterBytes(executable));
    return true;
  } catch (...) {
    return false;
  }
}

LoadedPayload LoadPayload(const std::filesystem::path& executable, bool verify_hash) {
  const auto file_size = FileSize(executable);
  const auto footer = DecodeFooter(ReadFooterBytes(executable));
  ValidatePayloadBounds(footer, file_size);
  if (verify_hash) {
    const auto hashed_size = footer.version == kPayloadVersionV1
                                 ? footer.payload_size
                                 : footer.payload_size + footer.manifest_size;
    const auto actual = Sha256FileRange(executable, footer.payload_offset, hashed_size);
    if (actual != footer.sha256)
      throw Error(footer.version == kPayloadVersionV1
                      ? "Application resources failed SHA-256 validation"
                      : "Application content or manifest failed SHA-256 validation");
  }
  std::ifstream input(executable, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(footer.manifest_offset));
  std::string text(static_cast<std::size_t>(footer.manifest_size), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!input && !text.empty()) throw Error("Cannot read manifest");
  auto manifest = ParseManifest(text);
  if (footer.version == kPayloadVersionV1 &&
      manifest.legacy_payload_sha256 != HexDigest(footer.sha256))
    throw Error("Manifest payload digest does not match the footer");
  return {executable, footer, std::move(manifest)};
}

std::uint64_t RunnerPrefixSize(const std::filesystem::path& executable) {
  if (!HasPayload(executable)) return FileSize(executable);
  return DecodeFooter(ReadFooterBytes(executable)).payload_offset;
}

void AppendPayload(const std::filesystem::path& output,
                   const std::filesystem::path& payload_file,
                   const Manifest& input_manifest, std::uint32_t flags) {
  PayloadFooter footer;
  footer.flags = flags;
  footer.payload_offset = FileSize(output);
  footer.payload_size = payload_file.empty() ? 0 : FileSize(payload_file);
  auto manifest = input_manifest;
  manifest.legacy_payload_sha256.clear();
  const auto json = SerializeManifest(manifest);
  footer.manifest_offset = footer.payload_offset + footer.payload_size;
  footer.manifest_size = json.size();
  std::ofstream stream(output, std::ios::binary | std::ios::app);
  if (!stream) throw Error("Cannot append application payload");
  if (!payload_file.empty()) {
    std::ifstream payload(payload_file, std::ios::binary);
    if (!payload) throw Error("Cannot open temporary ZIP payload");
    std::vector<char> buffer(1024 * 1024);
    std::uint64_t remaining = footer.payload_size;
    while (remaining) {
      const auto count = static_cast<std::size_t>((std::min)(
          remaining, static_cast<std::uint64_t>(buffer.size())));
      payload.read(buffer.data(), static_cast<std::streamsize>(count));
      if (payload.gcount() != static_cast<std::streamsize>(count))
        throw Error("Temporary ZIP payload is truncated");
      stream.write(buffer.data(), static_cast<std::streamsize>(count));
      remaining -= count;
    }
  }
  stream.write(json.data(), static_cast<std::streamsize>(json.size()));
  stream.close();
  if (!stream) throw Error("Failed while writing application content");
  footer.sha256 = Sha256FileRange(output, footer.payload_offset,
                                  footer.payload_size + footer.manifest_size);
  const auto footer_bytes = EncodeFooter(footer);
  std::ofstream footer_stream(output, std::ios::binary | std::ios::app);
  footer_stream.write(reinterpret_cast<const char*>(footer_bytes.data()), footer_bytes.size());
  if (!footer_stream) throw Error("Failed while writing application payload");
}

}  // namespace lwweb
