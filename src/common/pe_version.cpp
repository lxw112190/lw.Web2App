#include "lwweb/common/pe_version.h"

#include "lwweb/common/error.h"

namespace lwweb {

std::array<std::uint16_t, 4> ParsePeVersion(const std::wstring& text) {
  if (text.empty()) throw Error("Version must look like 1.2.3.4");

  std::array<std::uint16_t, 4> value{};
  std::size_t start = 0;
  std::size_t count = 0;
  while (start <= text.size()) {
    if (count == value.size()) throw Error("Version can contain at most four components");
    const auto end = text.find(L'.', start);
    const auto part = text.substr(start, end == std::wstring::npos ? end : end - start);
    if (part.empty()) throw Error("Version must look like 1.2.3.4");

    unsigned parsed = 0;
    for (const wchar_t character : part) {
      if (character < L'0' || character > L'9')
        throw Error("Version must contain only numbers and dots");
      const auto digit = static_cast<unsigned>(character - L'0');
      if (parsed > (65535U - digit) / 10U)
        throw Error("A version component exceeds 65535");
      parsed = parsed * 10U + digit;
    }
    value[count++] = static_cast<std::uint16_t>(parsed);
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return value;
}

std::wstring NormalizePeVersion(const std::wstring& text) {
  const auto value = ParsePeVersion(text);
  return std::to_wstring(value[0]) + L"." + std::to_wstring(value[1]) + L"." +
         std::to_wstring(value[2]) + L"." + std::to_wstring(value[3]);
}

}  // namespace lwweb
