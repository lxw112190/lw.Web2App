# Third-party notices

lw.Web2App uses the following dependencies when built from source:

- Microsoft WebView2 SDK, copyright Microsoft Corporation. See
  [`third_party/licenses/Microsoft-WebView2-LICENSE.txt`](third_party/licenses/Microsoft-WebView2-LICENSE.txt)
  and [`third_party/licenses/Microsoft-WebView2-NOTICE.txt`](third_party/licenses/Microsoft-WebView2-NOTICE.txt).
- miniz, copyright RAD Game Tools, Valve Software, Rich Geldreich, Tenacious
  Software LLC, and contributors. See
  [`third_party/licenses/miniz-LICENSE.txt`](third_party/licenses/miniz-LICENSE.txt).
- cpp-httplib, copyright Yuji Hirose and contributors. See
  [`third_party/licenses/cpp-httplib-LICENSE.txt`](third_party/licenses/cpp-httplib-LICENSE.txt).
- JSON for Modern C++, copyright Niels Lohmann and contributors. See
  [`third_party/licenses/nlohmann-json-LICENSE.txt`](third_party/licenses/nlohmann-json-LICENSE.txt).
- spdlog, copyright Gabi Melman and contributors. The bundled spdlog release
  includes its bundled fmt dependency. See
  [`third_party/licenses/spdlog-LICENSE.txt`](third_party/licenses/spdlog-LICENSE.txt).
- Linux builds dynamically use GTK 3, WebKitGTK 4.1, GLib, and OpenSSL as
  system libraries supplied by Ubuntu. These libraries are not copied into the
  lw.Web2App DEB or portable archive; their licenses and source packages are
  provided by the Ubuntu repositories.

The corresponding license and notice files are included in source checkouts
and binary distribution archives.
