# lw.Web2App

[简体中文](README.md) | [English](README_EN.md)

A lightweight desktop web-application packager. It supports Windows x64 and now includes an Ubuntu 22.04/24.04 x86_64 Beta, packaging HTML, Vue, React, Vite, and other static builds into single-file desktop applications without rebuilding the web project.

The target computer does not need Node.js, Rust, .NET, Electron, or a compiler toolchain. Windows uses Microsoft WebView2 Evergreen Runtime; Linux uses the system GTK3 and WebKitGTK 4.1 runtime.

> **Platform status:** Windows 10/11 x64 is stable. Ubuntu 22.04/24.04 x86_64 is the first Linux Beta, with a GTK3 GUI, CLI, ELF Runner, WebKitGTK Runtime, single-file payload, logs, and CI-built `.deb`/`.tar.gz` packages. Other distributions, ARM64, AppImage, and RPM are not supported yet.

Windows graphical packager:

<img src="docs/assets/lw.Web2App.png" alt="lw.Web2App graphical packager on Windows" width="760">

Linux graphical packager (Ubuntu 22.04):

<img src="docs/assets/lw.Web2App-linux.png" alt="lw.Web2App graphical packager on Linux" width="760">

## Features

- Native Win32 + WebView2 on Windows and GTK3 + WebKitGTK 4.1 on Linux, with GUI and CLI on both.
- Live packaging-stage and result status in the GUI, with an independently repainted status area for clear rapid updates.
- Package a local HTML, Vue, React, or Vite build into one Windows EXE or Linux ELF application.
- Package an online `http://` or `https://` URL into a single-file desktop application.
- WebView2 Evergreen Runtime with the WebView2 Loader statically linked.
- Independently designed `LWWEB002` V2 payload container with `LWWEB001` V1 read compatibility.
- Combined SHA-256 verification of the resource ZIP and manifest before embedded content is opened.
- Generated applications start in borderless fullscreen by default; `F11` toggles and `Esc` exits fullscreen.
- spdlog-based rotating packager/runtime logs: INFO by default, 2 MiB per file, five files retained.
- ZIP central-directory indexing and per-request decompression instead of eager extraction.
- A 32 MiB LRU cache for small hot resources; large files do not remain in memory.
- SPA fallback for history-mode Vue Router and React Router applications.
- The local HTTP service binds only to `127.0.0.1` and uses a stable per-`app_id` dynamic port so the LocalStorage/IndexedDB origin survives restarts.
- Exact Host validation, no wildcard CORS, no directory listing, and path traversal protection.
- Limits for entry count, individual file size, and total uncompressed size.
- PNG/ICO application icons and complete Windows PE version metadata.
- Unicode paths, filenames, and application titles.

## How It Works

lw.Web2App creates a single-file application using a **platform Runner plus an end-of-file payload**. It does not translate web source into C++, nor bundle Chromium, Node.js, or Electron into every generated app. Windows delegates rendering to WebView2 and Linux to WebKitGTK; both share the manifest, ZIP, SHA-256, local HTTP server, path-security, and LRU-cache core.

### Packaging

1. **Validate the input**: local mode checks that the static directory and entry HTML exist, then enforces limits on file count, individual size, and total size.
2. **Copy the Runner**: the current `lw.Web2App.exe` or `lw.Web2App` supplies the native PE/ELF prefix. If an already packaged app is used as the packer, only its original Runner prefix is copied, so old payloads are not nested.
3. **Write platform metadata**: Windows updates PE icons and version fields; Linux adds executable permissions. Per-generated-app desktop files, icons, and DEB metadata are planned separately.
4. **Build the ZIP**: files are streamed into a temporary ZIP under normalized relative paths, avoiding simultaneous in-memory copies of the source and complete archive. Absolute paths, drive letters, `..`, NUL bytes, and duplicate archive paths are rejected.
5. **Append the container**: the ZIP, Manifest JSON, and fixed 80-byte footer are streamed onto the PE/ELF file. The V2 footer stores the format, flags, section offsets/lengths, and a combined SHA-256 of ZIP plus manifest.

Online URL mode does not snapshot or embed the remote website. Its ZIP is empty and the manifest records only the target URL and window settings. The generated application therefore requires network access and follows future changes to that website. On Linux, the runtime passes `http_proxy`, `https_proxy`, `all_proxy`, and `no_proxy` (including their uppercase forms) to WebKitGTK.

### Runtime

1. The executable reads the `LWWEB002` footer from its own end. Without a footer it opens the packager; with a footer it enters generated-application mode. Legacy `LWWEB001` packages remain readable.
2. The Runner verifies that every offset and length is inside the EXE, limits manifest size, and hashes the resource ZIP plus manifest. It refuses to open embedded content when validation fails.
3. Local mode indexes only the ZIP central directory. It neither extracts the whole site to disk nor loads every resource into memory at startup.
4. A private HTTP service starts on a stable per-app dynamic port at `127.0.0.1`. It validates the exact Host, decompresses one requested resource at a time, sets its MIME type, and applies SPA fallback when configured. The stable port preserves the LocalStorage/IndexedDB origin across restarts; only one instance of the same app can currently run at a time.
5. Small frequently used resources are held in a 32 MiB LRU cache; large files are read on demand and do not remain resident.
6. Windows creates a WebView2 Controller; Linux creates a WebKitGTK WebView inside a GTK3 window. Both navigate to the private local address or online URL and honor title, dimensions, resizing, fullscreen, and developer-tool settings. `F11` toggles fullscreen and `Esc` exits it.
7. Each app has a stable `app_id`. Windows data lives under `%LOCALAPPDATA%\lw.Web2App\apps\<app_id>\WebView2`; Linux data and cache use `$XDG_DATA_HOME/lw.Web2App/apps/<app_id>/webkitgtk` and `$XDG_CACHE_HOME/lw.Web2App/apps/<app_id>/webkitgtk`.

SHA-256 detects resource corruption or modification; it does not authenticate a publisher. Trusted distribution of the manifest and complete EXE still requires a signing mechanism such as Authenticode.

## Logging and Diagnostics

lw.Web2App uses synchronous spdlog rotating-file logging. INFO is the default level; each file is limited to 2 MiB and five rotated files are retained. Logs are never written beside the EXE, so an application installed under `Program Files` does not require write access to its installation directory.

- Packager: `%LOCALAPPDATA%\lw.Web2App\logs\packer.log`
- Generated application: `%LOCALAPPDATA%\lw.Web2App\apps\<app_id>\logs\app.log`
- Startup failures before a Manifest/Payload can be loaded: `%LOCALAPPDATA%\lw.Web2App\logs\launcher.log`

Linux follows the XDG Base Directory convention:

- Packager: `${XDG_STATE_HOME:-$HOME/.local/state}/lw.Web2App/logs/packer.log`
- Generated app: `${XDG_STATE_HOME:-$HOME/.local/state}/lw.Web2App/apps/<app_id>/logs/app.log`
- Launcher failures: `${XDG_STATE_HOME:-$HOME/.local/state}/lw.Web2App/logs/launcher.log`

INFO records packaging stages, resource counts and sizes, payload digest, server port, WebView2 version, initialization, and navigation results. DEBUG additionally records static requests, ZIP cache hits/misses, and SPA fallback. The Runtime also records `console.error`, uncaught script errors, and unhandled Promise rejections as `[WEB-ERROR]`; ordinary `console.log` calls are not logged.

The GUI enables runtime logging by default. Selecting **Detailed logging** maps to DEBUG. Clearing **Enable runtime logging** disables only the generated application's Runtime log; the packager retains its own diagnostics. Logging initialization failures never prevent packaging or application startup.

Typical Runtime output:

```text
2026-08-13 18:37:27.820 [info] [lw.WebRuntime] Payload format: LWWEB002
2026-08-13 18:37:27.820 [info] [lw.WebRuntime] Payload verification OK
2026-08-13 18:37:27.821 [info] [lw.WebRuntime] Resource server: 127.0.0.1:60435
2026-08-13 18:37:27.876 [info] [lw.WebRuntime] WebView2 Runtime: 135.0.3179.98
2026-08-13 18:37:28.503 [info] [lw.WebRuntime] WebView2 initialized
2026-08-13 18:37:28.623 [info] [lw.WebRuntime] Navigation completed
```

For a blank screen or startup failure, inspect `app.log` first. If no per-app directory was created, inspect the shared `launcher.log`. Comparing Windows, WebView2, and payload versions plus the first ERROR line between affected and working computers usually isolates environment, integrity, or web-script failures quickly.

Logging settings are stored in the Manifest. The GUI currently exposes only enablement and INFO/DEBUG selection; rotation settings retain these safe defaults:

```json
{
  "logging": {
    "enabled": true,
    "level": "info",
    "max_file_size": 2097152,
    "max_files": 5
  }
}
```

## System Requirements

- Windows 10 1809+ / Windows 11 x64 with Microsoft WebView2 Evergreen Runtime; or
- Ubuntu 22.04 / 24.04 x86_64 with GTK3, WebKitGTK 4.1, and OpenSSL 3 runtime libraries.

Windows 11 and most maintained Windows 10 installations already include WebView2 Runtime. If it is unavailable, the generated application displays an installation prompt.

Windows 7/8 are unsupported. The Linux Beta is limited to Ubuntu 22.04/24.04 x86_64; Debian, Mint, ARM64, RPM-based systems, and other WebKitGTK ABIs are not compatibility claims. The project does not bundle a complete browser runtime.

## Download CI Builds

Every push and pull request builds and tests Windows x64, Ubuntu 22.04 x86_64, and Ubuntu 24.04 x86_64 through GitHub Actions:

1. Open the repository's **Actions** page.
2. Select the latest successful `Windows and Linux x64` workflow run.
3. Download the Windows or matching Ubuntu artifact.

The artifact contains:

- `lw.Web2App.exe`
- `README.md` and `README_EN.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- Original third-party license texts under `third_party/licenses/`
- `examples/wechat-article-formatter.exe`, the packaged integration-test application
- `examples/wechat-article-formatter-LICENSE.txt`, its project license
- `lw.Web2App-vs2022-source.zip`, a portable, offline-buildable VS2022 solution
- `SHA256SUMS.txt`

Pushing a `v*` tag also creates a GitHub Release containing the ZIP distribution and its SHA-256 checksum file.

Linux artifacts include a lw.Web2App `.deb`, a portable `.tar.gz`, the generated single-file `examples/wechat-article-formatter`, and SHA-256 files. The DEB installs desktop-menu metadata and declares GTK/WebKitGTK dependencies; the portable package and generated apps still require those libraries on the target Ubuntu system.

### CI Integration-Test Project

[wechat-article-formatter](https://github.com/lxw112190/wechat-article-formatter) is a Vite, React, and TypeScript Markdown editor for WeChat Official Account articles, with themes, mobile preview, and rich-text copying. CI checks out its `main` branch, runs `npm ci` and `npm run build`, and packages the resulting `dist` directory with the freshly built lw.Web2App. It then runs `inspect` against `examples/wechat-article-formatter.exe` to reload the manifest and verify the payload SHA-256. Any failure stops the workflow.

This example exercises a real Vite/React build, a Chinese title, a non-trivial asset set, SPA fallback, Windows PE metadata, Linux ELF permissions, and final distribution packaging. Linux CI also launches the generated app under Xvfb and checks WebKitGTK initialization and navigation logs.

## Build from Source

Requirements:

- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.22+
- Internet access during the first configure, unless a dependency cache is provided

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The output is `build/Release/lw.Web2App.exe`. Launching it without arguments opens the graphical packager.

### Portable Offline VS2022 Solution

After configuring the CMake project once so that its pinned dependencies are available, create a complete VS2022 source package with no paths tied to the current computer:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\create-vs2022-package.ps1 -Force
```

The outputs are `dist/lw.Web2App-vs2022-source/` and the matching ZIP. The package contains `lw.Web2App.sln`, the application, core-library and test projects, all pinned C++ dependencies, and a one-click build script. A recipient only needs Visual Studio 2022 with the **Desktop development with C++** workload; CMake, Ninja, vcpkg, and network dependency downloads are not required.

Ubuntu 22.04/24.04:

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ pkg-config libgtk-3-dev \
  libwebkit2gtk-4.1-dev libssl-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B dist
```

The Linux executable is `build/lw.Web2App`, and the DEB is written under `dist/`. Launching it without arguments opens the GTK3 packager.

### Optional Offline Dependency Cache

CMake first checks `.deps` in the repository root for these archives:

```text
.deps/json.tar.xz
.deps/cpp-httplib.tar.gz
.deps/miniz.tar.gz
.deps/spdlog.tar.gz
.deps/webview2.zip
```

If they are absent, CMake downloads pinned versions. Set `-DLWWEB_DEPS_CACHE=<directory>` to use another cache location.

## CLI

### Package a local static directory

The entry is automatically detected, preferring `index.html`:

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe --title "My App"
```

Full example:

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --entry index.html `
  --title "My App" `
  --width 1280 `
  --height 800 `
  --windowed `
  --debug-log `
  --icon .\app.png `
  --company "Example Company" `
  --version 1.2.0.0 `
  --copyright "Copyright © 2026"
```

### Package an online URL

```powershell
lw.Web2App.exe pack-url https://example.com .\Example.exe --title "Online App"
```

### Inspect a generated application

`inspect` verifies the payload SHA-256 and prints its manifest:

```powershell
lw.Web2App.exe inspect .\MyApp.exe
```

Linux uses the same command structure without `.exe`; generated files automatically receive executable permissions:

```bash
./lw.Web2App pack ./dist ./MyApp --title "My App"
./lw.Web2App inspect ./MyApp
./MyApp
```

Additional options:

- `--no-spa`: disable SPA fallback.
- `--windowed`: override the default and start the generated app in a normal window.
- `--no-log`: disable runtime logging in the generated application.
- `--debug-log`: enable DEBUG runtime logs for resource requests, ZIP cache activity, and SPA fallback.
- `--devtools`: enable developer tools and the default context menu.
- `--company`: write the company name.
- `--version`: write file and product versions.
- `--copyright`: write copyright metadata.

A generated EXE can also execute CLI packaging commands. Only its original Runner prefix is copied, so old payloads are never nested.

## Payload V2 Format

```text
Runner PE / ELF
Resource ZIP       empty in online URL mode
Manifest JSON
Footer             fixed 80 bytes
  magic[8]         LWWEB002
  version u32      2
  flags u32
  payloadOffset u64
  payloadSize u64
  manifestOffset u64
  manifestSize u64
  SHA-256[32]
```

All integers are explicitly serialized as little endian instead of relying on compiler structure layout. V2 hashes the consecutive Resource ZIP and Manifest JSON bytes, so URL and window-setting changes are detected as well. The Runner remains compatible with the legacy [Payload V1 format](docs/format-v1.md), while new applications are always written as V2.

## Security Boundary

This project packages trusted static web applications. It is not a sandbox for hostile web content.

- Default maximum resource count: 100,000.
- Default maximum individual file size: 512 MiB.
- Default maximum total uncompressed size: 2 GiB.
- Maximum manifest size: 1 MiB.
- The HTTP service listens only on IPv4 loopback and requires the exact per-app port Host value.
- SHA-256 detects corruption or modification but does not authenticate a publisher. Digital signatures are planned separately.

## Project Layout

```text
src/app/       Windows Win32 GUI, CLI, and entry point
src/linux/     Linux GTK3 GUI, CLI, and WebKitGTK Runtime
src/webview/   WebView2 host
src/packer/    Manifest, payload, and packer
src/runtime/   ZIP resource access, LRU cache, and local HTTP server
src/pe/        Icon and version resource updates
src/common/    File, path, and SHA-256 utilities
tests/         Unit and packaging/resource integration tests
```

## CI and Releases

The workflow at [.github/workflows/build.yml](.github/workflows/build.yml):

1. Builds and tests Windows x64 with VS2022 on Windows 2022.
2. Builds and tests Linux x64 with Ninja, GTK3, WebKitGTK 4.1, and OpenSSL on Ubuntu 22.04 and 24.04.
3. Builds the `wechat-article-formatter` Vite bundle in all platform jobs.
4. Generates a Windows EXE or Linux ELF and validates its payload with `inspect`.
5. Launches the Linux result under Xvfb and checks WebKitGTK initialization and navigation logs.
6. Publishes Windows ZIP, Linux `.tar.gz`/`.deb`, and SHA-256 artifacts.
7. Collects all tested platform files into releases for `v*` tags.

## Dependencies

- Microsoft WebView2 SDK — Microsoft software license
- GTK3 / WebKitGTK / OpenSSL — Linux system dynamic dependencies under their respective licenses
- miniz — MIT License
- cpp-httplib — MIT License
- nlohmann/json — MIT License
- spdlog (including bundled fmt) — MIT License

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Linux Beta and Roadmap

The first Ubuntu implementation now includes the cross-platform core, GTK3 GUI, CLI, ELF Runner, WebKitGTK Runtime, XDG data/cache/log directories, DEB/TGZ packaging, and dual-version Linux CI. It continues to write `LWWEB002` and read legacy `LWWEB001` payloads.

Explicit Linux Beta boundaries:

- Ubuntu 22.04/24.04 x86_64 only. Generated apps run within the same platform family; Windows and Linux Runners cannot generate each other's binaries.
- Generated output is a single ELF with an appended payload. The lw.Web2App tool has `.deb`/`.tar.gz` packages, but arbitrary generated apps do not yet receive their own DEB, desktop file, or icon.
- Rendering uses the system WebKitGTK 4.1. Browser security updates and Web API behavior therefore follow Ubuntu updates.
- GTK packaging is currently synchronous. Background packaging, cancellation, and percentage progress for very large projects remain future work.

Next priorities are generated-app desktop/icon/DEB metadata, AppImage evaluation, external-link and download policies, followed by ARM64, Debian-family, and RPM-family evaluation after x86_64 stability work.

Other planned improvements:

- Multi-resolution icon generation
- Authenticode code signing
- Single-instance mode, tray support, and always-on-top windows
- File download and external-link policies
- JavaScript/C++ IPC
- Custom user agents, startup arguments, and CSP

## Contact and Support

- Author: 天天代码码天天
- QQ: 819069052
- QQ Group: C# 人工智能实践 | Group ID: 758616458

If this project is useful to you, you can scan the QR code to support its maintenance:

<img src="docs/assets/sponsor.jpg" alt="WeChat sponsor QR code" width="260">

## License

This project is licensed under the [MIT License](LICENSE).
