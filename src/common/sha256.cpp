#include "lwweb/common/sha256.h"

#include "lwweb/common/error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace lwweb {
namespace {

// 对 Windows CNG SHA-256 句柄进行 RAII 管理，并支持分块增量计算。
class Hash {
 public:
  Hash() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
      throw Error("BCryptOpenAlgorithmProvider(SHA-256) failed");
    DWORD bytes = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size_), sizeof(object_size_),
                          &bytes, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      throw Error("BCryptGetProperty failed");
    }
    object_.resize(object_size_);
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(), object_size_, nullptr, 0, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      throw Error("BCryptCreateHash failed");
    }
  }

  ~Hash() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }

  void Update(const std::uint8_t* data, std::size_t size) {
    while (size) {
      const auto chunk = static_cast<ULONG>((std::min)(size, std::size_t{1u << 30}));
      if (BCryptHashData(hash_, const_cast<PUCHAR>(data), chunk, 0) < 0)
        throw Error("BCryptHashData failed");
      data += chunk;
      size -= chunk;
    }
  }

  Sha256Digest Finish() {
    Sha256Digest digest{};
    if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
      throw Error("BCryptFinishHash failed");
    return digest;
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  DWORD object_size_ = 0;
  std::vector<std::uint8_t> object_;
};

}  // namespace

Sha256Digest Sha256(const std::uint8_t* data, std::size_t size) {
  Hash hash;
  hash.Update(data, size);
  return hash.Finish();
}

Sha256Digest Sha256(const std::vector<std::uint8_t>& data) {
  return Sha256(data.data(), data.size());
}

Sha256Digest Sha256FileRange(const std::filesystem::path& path,
                             std::uint64_t offset, std::uint64_t size) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw Error("Cannot open file for hashing");
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input) throw Error("Cannot seek to payload");
  Hash hash;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  while (size) {
    const auto count = static_cast<std::size_t>((std::min)(size, static_cast<std::uint64_t>(buffer.size())));
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
      throw Error("Payload ended while hashing");
    hash.Update(buffer.data(), count);
    size -= count;
  }
  return hash.Finish();
}

std::string HexDigest(const Sha256Digest& digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

Sha256Digest ParseHexDigest(const std::string& text) {
  if (text.size() != 64) throw Error("SHA-256 digest must contain 64 hex digits");
  Sha256Digest digest{};
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const auto part = text.substr(i * 2, 2);
    std::size_t parsed = 0;
    digest[i] = static_cast<std::uint8_t>(std::stoul(part, &parsed, 16));
    if (parsed != 2) throw Error("Invalid SHA-256 digest");
  }
  return digest;
}

}  // namespace lwweb
