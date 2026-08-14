#include "lwweb/common/http_origin.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace lwweb {
namespace {

bool ValidDnsOrIpv4Host(const std::string& host) {
  if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.')
    return false;
  return std::all_of(host.begin(), host.end(), [](unsigned char value) {
    return std::isalnum(value) || value == '.' || value == '-';
  });
}

bool ValidBracketedIpv6Host(const std::string& host) {
  if (host.size() < 4 || host.front() != '[' || host.back() != ']') return false;
  return std::all_of(host.begin() + 1, host.end() - 1, [](unsigned char value) {
    return std::isxdigit(value) || value == ':' || value == '.';
  });
}

}  // namespace

std::string HttpOrigin::Authority() const {
  const bool default_port = (scheme == "http" && port == 80) ||
                            (scheme == "https" && port == 443);
  return host + ((explicit_port || !default_port) ? ":" + std::to_string(port) : "");
}

std::string HttpOrigin::ToString() const { return scheme + "://" + Authority(); }

std::optional<HttpOrigin> ParseHttpOrigin(const std::string& value) {
  if (value.empty() || value.size() > 2048) return std::nullopt;
  auto text = value;
  if (text.size() > 1 && text.back() == '/') text.pop_back();
  if (text.find_first_of("?#\\\r\n\t ") != std::string::npos) return std::nullopt;

  const auto delimiter = text.find("://");
  if (delimiter == std::string::npos) return std::nullopt;
  auto scheme = text.substr(0, delimiter);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (scheme != "http" && scheme != "https") return std::nullopt;

  const auto authority = text.substr(delimiter + 3);
  if (authority.empty() || authority.find('@') != std::string::npos ||
      authority.find('/') != std::string::npos)
    return std::nullopt;

  std::string host;
  std::string port_text;
  if (authority.front() == '[') {
    const auto bracket = authority.find(']');
    if (bracket == std::string::npos) return std::nullopt;
    host = authority.substr(0, bracket + 1);
    if (bracket + 1 < authority.size()) {
      if (authority[bracket + 1] != ':') return std::nullopt;
      port_text = authority.substr(bracket + 2);
    }
    if (!ValidBracketedIpv6Host(host)) return std::nullopt;
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
      if (authority.find(':') != colon) return std::nullopt;
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      host = authority;
    }
    if (!ValidDnsOrIpv4Host(host)) return std::nullopt;
  }

  std::uint32_t port = scheme == "http" ? 80u : 443u;
  if (!port_text.empty()) {
    if (!std::all_of(port_text.begin(), port_text.end(), [](unsigned char value) {
          return std::isdigit(value);
        }))
      return std::nullopt;
    try {
      std::size_t consumed = 0;
      const auto parsed = std::stoul(port_text, &consumed);
      if (consumed != port_text.size() || parsed == 0 ||
          parsed > std::numeric_limits<std::uint16_t>::max())
        return std::nullopt;
      port = parsed;
    } catch (...) {
      return std::nullopt;
    }
  } else if (authority.back() == ':') {
    return std::nullopt;
  }

  return HttpOrigin{scheme, host, static_cast<std::uint16_t>(port), !port_text.empty()};
}

}  // namespace lwweb
