#include "lwweb/common/file_utils.h"
#include "lwweb/pe/payload_binding.h"

#ifdef _WIN32
#include "lwweb/pe/pe_resources.h"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectoryGuard {
  std::filesystem::path path;
  ~TempDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool BindingRejected(
    const std::array<std::uint8_t, lwweb::kPayloadBindingSize>& bytes) {
  try {
    (void)lwweb::DecodePayloadBinding(bytes);
    return false;
  } catch (...) {
    return true;
  }
}

}  // namespace

void RunPayloadBindingTests() {
  lwweb::PayloadBinding binding;
  binding.flags = lwweb::kBindingAuthenticodeRequired;
  for (std::size_t index = 0; index < binding.payload_sha256.size(); ++index)
    binding.payload_sha256[index] = static_cast<std::uint8_t>(index + 1);
  const auto encoded = lwweb::EncodePayloadBinding(binding);
  const auto decoded = lwweb::DecodePayloadBinding(encoded);
  Check(decoded.version == lwweb::kPayloadBindingVersion &&
            decoded.flags == binding.flags &&
            decoded.payload_sha256 == binding.payload_sha256,
        "payload binding round-trips through the fixed binary format");

  auto invalid_magic = encoded;
  invalid_magic[0] ^= 1;
  Check(BindingRejected(invalid_magic),
        "payload binding rejects an invalid magic value");
  auto invalid_version = encoded;
  invalid_version[8] = 2;
  Check(BindingRejected(invalid_version),
        "payload binding rejects an unsupported version");
  auto invalid_flags = encoded;
  invalid_flags[12] = 0x80;
  Check(BindingRejected(invalid_flags),
        "payload binding rejects unknown security flags");

#ifdef _WIN32
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto directory = std::filesystem::temp_directory_path() /
                         ("lwweb-binding-" + unique);
  TempDirectoryGuard cleanup{directory};
  std::filesystem::create_directories(directory);
  const auto executable = directory / "binding-test.exe";
  std::filesystem::copy_file(lwweb::CurrentExecutablePath(), executable);

  Check(!lwweb::ReadPePayloadBinding(executable).has_value(),
        "legacy PE without a binding resource remains compatible");
  binding.flags = 0;
  lwweb::UpdatePeResources(executable, lwweb::PeMetadata{}, binding);
  {
    std::ofstream overlay(executable, std::ios::binary | std::ios::app);
    overlay << "LWWEB002 test overlay";
  }
  const auto resource = lwweb::ReadPePayloadBinding(executable);
  Check(resource && resource->payload_sha256 == binding.payload_sha256 &&
            resource->flags == 0,
        "Windows PE resource preserves the payload binding beside an overlay");
  lwweb::VerifyPePayloadBinding(executable, binding.payload_sha256);

  auto wrong_digest = binding.payload_sha256;
  wrong_digest[0] ^= 1;
  bool mismatch_rejected = false;
  try {
    lwweb::VerifyPePayloadBinding(executable, wrong_digest);
  } catch (...) {
    mismatch_rejected = true;
  }
  Check(mismatch_rejected,
        "runtime verification rejects a Footer and PE binding mismatch");

  binding.flags = lwweb::kBindingAuthenticodeRequired;
  lwweb::UpdatePeResources(executable, lwweb::PeMetadata{}, binding);
  bool unsigned_required_rejected = false;
  try {
    lwweb::VerifyPePayloadBinding(executable, binding.payload_sha256);
  } catch (...) {
    unsigned_required_rejected = true;
  }
  Check(unsigned_required_rejected,
        "Authenticode-required binding requires a valid PE signature");

  lwweb::UpdatePeResources(executable, lwweb::PeMetadata{}, std::nullopt);
  Check(!lwweb::ReadPePayloadBinding(executable).has_value(),
        "unsigned repackaging removes an inherited signed payload binding");
#endif
}
