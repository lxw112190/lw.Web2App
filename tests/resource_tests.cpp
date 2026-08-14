#include "lwweb/runtime/resource_cache.h"
#include "lwweb/runtime/resource_server.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}

void RunResourceTests() {
  lwweb::ResourceCache cache(16);
  cache.Put("a", std::vector<std::uint8_t>(4, 1));
  cache.Put("b", std::vector<std::uint8_t>(4, 2));
  cache.Put("c", std::vector<std::uint8_t>(4, 3));
  cache.Put("d", std::vector<std::uint8_t>(4, 4));
  Check(cache.Get("a").has_value(), "LRU cache returns stored value");
  cache.Put("e", std::vector<std::uint8_t>(4, 5));
  Check(!cache.Get("b"), "LRU cache evicts least recently used value");
  Check(cache.Get("a").has_value(), "LRU cache preserves recently used value");
  cache.Put("too-large", std::vector<std::uint8_t>(5, 6));
  Check(!cache.Get("too-large"), "LRU cache skips oversized values");
  cache.Clear();
  Check(!cache.Get("a"), "LRU cache clear removes values");

  Check(lwweb::IsExpectedResourceHost("127.0.0.1:53182", 53182),
        "exact loopback Host accepted");
  Check(!lwweb::IsExpectedResourceHost("localhost:53182", 53182),
        "localhost alias rejected");
  Check(!lwweb::IsExpectedResourceHost("127.0.0.1:53183", 53182),
        "wrong Host port rejected");
  Check(!lwweb::IsExpectedResourceHost("evil.example", 53182),
        "remote Host rejected");
  Check(lwweb::BuildLocalStartUrl("http://127.0.0.1:53182/", "/") ==
            "http://127.0.0.1:53182/",
        "root start URL composed");
  Check(lwweb::BuildLocalStartUrl("http://127.0.0.1:53182/", "/pages/login.html") ==
            "http://127.0.0.1:53182/pages/login.html",
        "nested entry start URL composed");
  Check(lwweb::BuildLocalStartUrl("http://127.0.0.1:53182", "/login?from=app#form") ==
            "http://127.0.0.1:53182/login?from=app#form",
        "route query and fragment preserved");
  bool unsafe_start_rejected = false;
  try {
    (void)lwweb::BuildLocalStartUrl("http://127.0.0.1:53182/", "//example.com");
  } catch (...) {
    unsafe_start_rejected = true;
  }
  Check(unsafe_start_rejected, "unsafe start URL rejected");
}
