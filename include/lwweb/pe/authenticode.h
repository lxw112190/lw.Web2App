#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace lwweb {

struct SigningConfig;

// 返回 PE 是否声明了 Authenticode Certificate Table；结构异常会抛错，
// 不会把损坏的 Security Directory 当成“未签名”。
bool HasAuthenticodeSignature(const std::filesystem::path& executable);

// 返回 Authenticode Certificate Table 之前的逻辑文件末尾。未签名 PE
// 返回空值；签名表声明损坏时抛错，供 Payload 读取器安全定位 Footer。
std::optional<std::uint64_t> AuthenticodeContentEnd(
    const std::filesystem::path& executable);

// 清除 Runner 尾部的 Certificate Table 并将 Security Directory 置零。
// 只接受证书表严格位于文件末尾的标准 PE，避免猜测性截断用户数据。
bool StripAuthenticodeSignature(const std::filesystem::path& executable);

std::filesystem::path FindSignTool(
    const std::filesystem::path& configured = {});
void SignAuthenticode(const std::filesystem::path& executable,
                      const SigningConfig& config);
void VerifyAuthenticodeSignature(
    const std::filesystem::path& executable);

}  // namespace lwweb
