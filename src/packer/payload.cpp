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

void AppendPreparedPayload(const std::filesystem::path& executable,
                           const PreparedPayload& payload) {
  if (payload.manifest_json.size() > 1024 * 1024)
    throw Error("Manifest exceeds the 1 MiB limit");
  const auto payload_size = payload.zip.empty() ? 0 : FileSize(payload.zip);
  if (payload.compressed_size != 0 &&
      payload.compressed_size != payload_size)
    throw Error("Prepared ZIP size has changed");
  if (Sha256FileWithSuffix(payload.zip, payload.manifest_json) != payload.sha256)
    throw Error("Prepared payload content has changed");

  PayloadFooter footer;
  footer.flags = payload.flags;
  footer.payload_offset = FileSize(executable);
  footer.payload_size = payload_size;
  if (footer.payload_size >
      (std::numeric_limits<std::uint64_t>::max)() - footer.payload_offset)
    throw Error("Prepared payload size is too large");
  footer.manifest_offset = footer.payload_offset + footer.payload_size;
  footer.manifest_size = payload.manifest_json.size();
  if (footer.manifest_size >
      (std::numeric_limits<std::uint64_t>::max)() - footer.manifest_offset)
    throw Error("Prepared manifest size is too large");
  footer.sha256 = payload.sha256;

  std::ofstream stream(executable, std::ios::binary | std::ios::app);
  if (!stream) throw Error("Cannot append application payload");
  if (!payload.zip.empty()) {
    std::ifstream zip(payload.zip, std::ios::binary);
    if (!zip) throw Error("Cannot open prepared ZIP payload");
    std::vector<char> buffer(1024 * 1024);
    std::uint64_t remaining = footer.payload_size;
    while (remaining) {
      const auto count = static_cast<std::size_t>((std::min)(
          remaining, static_cast<std::uint64_t>(buffer.size())));
      zip.read(buffer.data(), static_cast<std::streamsize>(count));
      if (zip.gcount() != static_cast<std::streamsize>(count))
        throw Error("Prepared ZIP payload is truncated");
      stream.write(buffer.data(), static_cast<std::streamsize>(count));
      remaining -= count;
    }
  }
  stream.write(payload.manifest_json.data(),
               static_cast<std::streamsize>(payload.manifest_json.size()));
  stream.close();
  if (!stream) throw Error("Failed while writing application content");
  const auto appended_digest = Sha256FileRange(
      executable, footer.payload_offset,
      footer.payload_size + footer.manifest_size);
  if (appended_digest != footer.sha256)
    throw Error("Appended payload does not match the prepared digest");
  const auto footer_bytes = EncodeFooter(footer);
  std::ofstream footer_stream(executable, std::ios::binary | std::ios::app);
  footer_stream.write(reinterpret_cast<const char*>(footer_bytes.data()), footer_bytes.size());
  if (!footer_stream) throw Error("Failed while writing application payload");
}

void AppendPayload(const std::filesystem::path& output,
                   const std::filesystem::path& payload_file,
                   const Manifest& input_manifest, std::uint32_t flags) {
  auto manifest = input_manifest;
  manifest.legacy_payload_sha256.clear();
  PreparedPayload prepared;
  prepared.zip = payload_file;
  prepared.manifest_json = SerializeManifest(manifest);
  prepared.sha256 = Sha256FileWithSuffix(prepared.zip,
                                         prepared.manifest_json);
  prepared.flags = flags;
  prepared.compressed_size =
      prepared.zip.empty() ? 0 : FileSize(prepared.zip);
  AppendPreparedPayload(output, prepared);
}

}  // namespace lwweb
