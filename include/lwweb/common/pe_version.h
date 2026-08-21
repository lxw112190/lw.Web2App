#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace lwweb {

// 解析 Windows PE 的四段式版本号；允许输入 1 到 4 段，每段范围为 0..65535。
std::array<std::uint16_t, 4> ParsePeVersion(const std::wstring& text);

// 将合法版本号补齐为 Windows 属性页使用的 a.b.c.d 形式。
std::wstring NormalizePeVersion(const std::wstring& text);

}  // namespace lwweb
