# lw.Web2App

[简体中文](README.md) | [English](README_EN.md)

A lightweight desktop web-application packager. It supports Windows x64 and now includes an Ubuntu 22.04/24.04 x86_64 Beta, packaging HTML, Vue, React, Vite, and other static builds into single-file desktop applications without rebuilding the web project.

The target computer does not need Node.js, Rust, .NET, Electron, or a compiler toolchain. Windows uses Microsoft WebView2 Evergreen Runtime; Linux uses the system GTK3 and WebKitGTK 4.1 runtime.

> **Platform status:** Windows 10/11 x64 is stable. Ubuntu 22.04/24.04 x86_64 is the first Linux Beta, with a GTK3 GUI, CLI, ELF Runner, WebKitGTK Runtime, single-file payload, logs, and CI-built `.deb`/`.tar.gz` packages. Other distributions, ARM64, AppImage, and RPM are not supported yet.

Windows graphical packager:

<img src="docs/assets/lw.Web2App.png" alt="lw.Web2App graphical packager on Windows" width="760">

The Windows GUI uses a two-column layout. The left side selects a local build or URL, entry page, start path, and controlled backend proxy. The right side configures the window title, PE ProductName, FileDescription, company, version, copyright, live icon preview, dimensions, and runtime options. The title continues to synchronize into ProductName and FileDescription until either field is edited manually. Output and progress share a full-width bottom area, while resource compression remains on a worker thread so large projects do not freeze the UI. After a successful package, File Explorer opens automatically with the generated EXE selected.

Versions may contain one to four numeric components and are normalized to `a.b.c.d`; every component must be within `0..65535`. PNG and ICO files are previewed immediately and validated before packaging; common PNG images larger than 256 pixels are proportionally resized, and the built-in icon can be restored at any time. Both the sponsor QR image and application icon are embedded in the GUI executable and do not depend on sidecar files.

Linux graphical packager (Ubuntu 22.04):

<img src="docs/assets/lw.Web2App-linux.png" alt="lw.Web2App graphical packager on Linux" width="760">

## Features

- Native Win32 + WebView2 on Windows and GTK3 + WebKitGTK 4.1 on Linux, with GUI and CLI on both.
- Live packaging-stage and result status in the GUI, with an independently repainted status area for clear rapid updates.
- Per-Monitor V2 high-DPI layout on Windows; controls, fonts, spacing, and painted regions are rescaled when the monitor DPI changes.
- Package a local HTML, Vue, React, or Vite build into one Windows EXE or Linux ELF application.
- Package an online `http://` or `https://` URL into a single-file desktop application.
- WebView2 Evergreen Runtime with the WebView2 Loader statically linked.
- Independently designed `LWWEB002` V2 payload container with `LWWEB001` V1 read compatibility.
- Combined SHA-256 verification of the resource ZIP and manifest before embedded content is opened.
- Generated applications start in borderless fullscreen by default; `F11` toggles and `Esc` exits fullscreen. On Windows, standard web Fullscreen API requests also synchronize the native host window with borderless fullscreen mode.
- spdlog-based rotating packager/runtime logs: INFO by default, 2 MiB per file, five files retained.
- ZIP central-directory indexing and per-request decompression instead of eager extraction.
- A 32 MiB LRU cache for small hot resources; large files do not remain in memory.
- SPA fallback for history-mode Vue Router and React Router applications.
- Deterministic GUI discovery of `.html`/`.htm` launch pages; separate Manifest, GUI, and CLI `entry` and `start_path` settings support both multi-page entry files and initial SPA routes.
- The local HTTP service binds only to `127.0.0.1` and prefers a stable per-`app_id` dynamic port, with deterministic fallback ports when an unrelated process occupies it.
- True cross-platform single-instance locking uses a Windows named mutex or Linux `flock`, independently of the HTTP port.
- An optional controlled backend proxy forwards same-origin `/__lw_proxy__/...` requests to one fixed legacy HTTP origin from the Manifest, without disabling WebView security; Windows and Linux share the same implementation.
- Optional Native IPC exposes app information, a system directory picker, and permission-scoped filesystem operations to trusted local pages through the stable `window.lw.invoke()` API; it is disabled by default and authorized per method.
- Exact Host validation, no wildcard CORS, no directory listing, and path traversal protection.
- Limits for entry count, individual file size, and total uncompressed size.
- PNG/ICO application icons and complete Windows PE version metadata.
- Unicode paths, filenames, and application titles.

## How It Works

lw.Web2App creates a single-file application using a **platform Runner plus an end-of-file payload**. It does not translate web source into C++, nor bundle Chromium, Node.js, or Electron into every generated app. Windows delegates rendering to WebView2 and Linux to WebKitGTK; both share the manifest, ZIP, SHA-256, local HTTP server, path-security, and LRU-cache core.

### Packaging

1. **Validate the input**: local mode scans and verifies the HTML selected by `entry`, ensures `start_path` cannot leave the private local service, then enforces limits on file count, individual size, and total size.
2. **Copy the Runner**: the current `lw.Web2App.exe` or `lw.Web2App` supplies the native PE/ELF prefix. If an already packaged app is used as the packer, only its original Runner prefix is copied, so old payloads are not nested.
3. **Write platform metadata**: Windows updates PE icons and version fields; Linux adds executable permissions. Per-generated-app desktop files, icons, and DEB metadata are planned separately.
4. **Build the ZIP**: files are streamed into a temporary ZIP under normalized relative paths, avoiding simultaneous in-memory copies of the source and complete archive. Absolute paths, drive letters, `..`, NUL bytes, and duplicate archive paths are rejected.
5. **Append the container**: the ZIP, Manifest JSON, and fixed 80-byte footer are streamed onto the PE/ELF file. The V2 footer stores the format, flags, section offsets/lengths, and a combined SHA-256 of ZIP plus manifest.

Online URL mode does not snapshot or embed the remote website. Its ZIP is empty and the manifest records only the target URL and window settings. The generated application therefore requires network access and follows future changes to that website. On Linux, the runtime passes `http_proxy`, `https_proxy`, `all_proxy`, and `no_proxy` (including their uppercase forms) to WebKitGTK.

### Runtime

1. The executable reads the `LWWEB002` footer from its own end. Without a footer it opens the packager; with a footer it enters generated-application mode. Legacy `LWWEB001` packages remain readable.
2. The Runner verifies that every offset and length is inside the EXE, limits manifest size, and hashes the resource ZIP plus manifest. It refuses to open embedded content when validation fails.
3. Local mode indexes only the ZIP central directory. It neither extracts the whole site to disk nor loads every resource into memory at startup.
4. The Runner first acquires an `app_id`-specific single-instance lock through a Windows named mutex or Linux `flock`, then starts the private HTTP service on its stable preferred `127.0.0.1` port. The service validates the exact Host, decompresses one requested resource at a time, sets its MIME type, and applies SPA fallback. If an unrelated process occupies the preferred port, deterministic alternatives are tried and logged.
5. When `backend_proxy` is enabled, the local server matches `/__lw_proxy__/` before static resources. Only GET, HEAD, POST, PUT, PATCH, DELETE, and OPTIONS are forwarded to the fixed `origin`; query strings and bodies are preserved, while cookies and same-origin redirects are rewritten. The page never contacts the LAN backend directly.
6. Small frequently used resources are held in a 32 MiB LRU cache; large files are read on demand and do not remain resident.
7. Windows creates a WebView2 Controller; Linux creates a WebKitGTK WebView inside a GTK3 window. Local mode combines the private service origin with Manifest `start_path`; when a requested file is absent and SPA fallback is enabled, the server returns `entry`. A traditional multi-page app can use `entry=login.html` with `start_path=/login.html`, while a Vue/React history route can use `entry=index.html` with `start_path=/login`. Legacy packages without `start_path` default to `/`. Window behavior also comes from the Manifest; `F11` toggles fullscreen and `Esc` exits it.
8. Each app has a stable `app_id`. Windows data lives under `%LOCALAPPDATA%\lw.Web2App\apps\<app_id>\WebView2`; Linux data and cache use `$XDG_DATA_HOME/lw.Web2App/apps/<app_id>/webkitgtk` and `$XDG_CACHE_HOME/lw.Web2App/apps/<app_id>/webkitgtk`.

The `WebView2` directory is the writable user-data directory required by Microsoft WebView2. It contains cookies, sign-in state, localStorage, IndexedDB, HTTP cache, service workers, and site permissions. It cannot be eliminated while retaining complete browser behavior; placing it under `%LOCALAPPDATA%` also avoids startup failures when the EXE is installed in a read-only location such as `Program Files`. It may be deleted after the app has fully exited to reset web data, but WebView2 creates it again on the next launch.

### Legacy HTTP Backend Compatibility

Select **HTTP backend proxy** and enter a fixed backend origin. This controlled proxy is intended for legacy HTTP backends on a trusted LAN. For example, when the backend is `http://192.0.2.10:8080`, change the frontend API base from the absolute LAN URL as follows (`192.0.2.0/24` is reserved for documentation):

```javascript
const apiBase = "/__lw_proxy__";
```

The C++ Runtime forwards `/__lw_proxy__/sysUser/login` to `http://192.0.2.10:8080/sysUser/login`. To the page, the request remains same-origin with its `127.0.0.1` resource server, so it does not depend on CORS, PNA, or `--disable-web-security`. The proxy fixes the target Host, rejects cross-site callers and cross-host redirects, filters hop-by-hop and proxy-authentication headers, limits requests to 16 MiB and ordinary API responses to 64 MiB, and scopes backend cookies to the proxy prefix. Because legacy systems may place tokens in either URL path parameters or query parameters, proxy logs omit the entire target path and contain only the method, status, and elapsed time; they never include bodies, passwords, cookies, or authorization credentials.

#### Proxied Downloads

When the backend responds with `Content-Disposition: attachment`, the proxy automatically switches to streaming-download mode. A bounded queue of about 1 MiB forwards data as it arrives and applies backpressure when the WebView consumes it more slowly. The complete file is never buffered in memory, and the ordinary 64 MiB API-response limit does not apply. `Content-Disposition`, `Content-Type`, `Content-Length`, `Accept-Ranges`, `Content-Range`, `ETag`, and cache headers are preserved after security filtering. Browser `Range` requests are forwarded unchanged, so partial downloads work when supported by the backend. Responses without a known length use chunked transfer, and client cancellation or a write failure immediately cancels the upstream read.

Both Windows WebView2 and Linux WebKitGTK show a native **Save As** dialog and retain the filename suggested through `filename` or `filename*`. Runtime logs record only download start, completion, interruption, or user cancellation; URLs, filenames, and local paths are omitted. Frontends should use a normal link, form submission, or `window.location` to open a `/__lw_proxy__/...` download URL and let the WebView own the transfer. Avoid fetching a large file into a Blob through `fetch` or XHR, because that buffers it again in the web process. The backend should emit `Content-Disposition: attachment`; responses without it remain ordinary buffered API responses.

This release supports `http://` backends only and is intended for trusted legacy LAN systems. HTTPS proxying, WebSocket, NTLM/Kerberos, and multiple backend origins are not yet supported. Projects with hard-coded absolute API URLs must still change their base URL to `/__lw_proxy__`; lw.Web2App does not rewrite minified JavaScript automatically.

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

The GUI and CLI scan only the first level of the selected directory for `.html`/`.htm`, sort the results deterministically, and prefer `index.html`. Nested business pages are never selected accidentally. `entry` names the real HTML inside the ZIP; `start_path` is the first URL path opened by the WebView:

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe --title "My App"
```

Full example:

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --entry index.html `
  --start-path /login `
  --title "My App" `
  --product-name "My App" `
  --file-description "My App desktop client" `
  --app-id com.example.myapp `
  --width 1280 `
  --height 800 `
  --windowed `
  --debug-log `
  --backend-origin http://192.0.2.10:8080 `
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

- `--entry`: select the real archived HTML, such as `login.html` or `pages/login.html`.
- `--start-path`: select the initial navigation path, such as `/login.html`, `/login`, or `/#/login`; when omitted it is derived from `entry`, with a root `index.html` mapping to `/`.
- `--backend-origin`: enable the cross-platform controlled proxy and fix its only HTTP origin, for example `http://192.0.2.10:8080`; frontend API requests should use `/__lw_proxy__` as their base.
- `--ipc`: enable Native IPC for a local package; URL mode cannot enable it.
- `--ipc-capability`: repeat for each capability, such as `app.info`, `dialog.directory`, `fs.list`, `fs.move`, or `fs.delete`.
- `--ipc-root`: repeat for each fixed filesystem root; `${HOME}`, `${DESKTOP}`, `${DOCUMENTS}`, `${PICTURES}`, `${DOWNLOADS}`, `${APP_DATA}`, and `${APP_CACHE}` are supported.
- `--no-spa`: disable SPA fallback.
- `--windowed`: override the default and start the generated app in a normal window.
- `--no-log`: disable runtime logging in the generated application.
- `--debug-log`: enable DEBUG runtime logs for resource requests, ZIP cache activity, and SPA fallback.
- `--devtools`: enable developer tools and the default context menu.
- `--app-id`: explicitly set the stable application ID; Windows and Linux share the same parser.
- `--product-name`: write the Windows PE ProductName; defaults to `--title`.
- `--file-description`: write the Windows PE FileDescription; defaults to `--title`.
- `--company`: write the company name.
- `--version`: write file and product versions; one to four numeric components are padded to four.
- `--copyright`: write copyright metadata.

A generated EXE can also execute CLI packaging commands. Only its original Runner prefix is copied, so old payloads are never nested.

## Native IPC (experimental)

Native IPC is intended only for trusted static pages packaged with the application. It does not expose arbitrary native functions. Access requires three independent gates: explicit Manifest enablement, a capability for every method, and fresh native-side path validation for every filesystem operation. Windows uses WebView2 WebMessage and Linux uses a dedicated WebKitGTK `lwIpc` ScriptMessageHandler, while pages use one API:

```js
const info = await window.lw.invoke("app.getInfo");
// { appId, title, platform: "windows" | "linux", arch: "x64", version }
```

Package the [Native IPC example](examples/native-ipc/index.html):

```powershell
lw.Web2App.exe pack .\examples\native-ipc .\native-ipc.exe `
  --title "Native IPC Example" --windowed --ipc `
  --ipc-capability app.info `
  --ipc-capability dialog.directory `
  --ipc-capability fs.list `
  --ipc-capability fs.move `
  --ipc-capability fs.delete
```

The example first invokes `dialog.selectDirectory`. A directory selected through the system dialog becomes a session-only grant and is forgotten when the app exits. The page may then invoke `fs.list`, `fs.move`, and `fs.delete` within that grant. Fixed roots can instead be embedded with repeatable `--ipc-root` options.

The JSON protocol is `lw-ipc-v1`. Messages are capped at 1 MiB, IDs and method names at 128 bytes, and each page may have at most 64 pending requests; duplicate IDs return `BUSY`. Stable error codes are `INVALID_REQUEST`, `INVALID_ARGUMENT`, `METHOD_NOT_FOUND`, `PERMISSION_DENIED`, `USER_CANCELLED`, `NOT_FOUND`, `ALREADY_EXISTS`, `IO_ERROR`, `UNSUPPORTED`, `BUSY`, and `INTERNAL_ERROR`.

With IPC enabled, the Runtime accepts messages only from the exact current `127.0.0.1` application-port origin and blocks top-level navigation to external origins. The Linux transport also requires a random per-process session token so a cross-origin iframe cannot bypass the top-level restriction. URL mode cannot enable IPC. Filesystem paths must be absolute local paths; existing paths are canonicalized, new targets are validated through their canonical parent, and both source and destination must remain under a fixed Manifest root or session grant. Windows device paths, UNC paths, and ADS are rejected, and symbolic links/reparse points cannot escape a granted root. INFO logs record method names, result codes, and security rejections, but never method parameters or user paths.

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
- The backend proxy accepts only the fixed Manifest `http://` origin; arbitrary URLs, cross-site callers, cross-host redirects, and ZIP resources that collide with its prefix are rejected, with explicit size and timeout limits.
- Native IPC is disabled by default and local-mode only; capabilities, exact origin, fixed roots, and session grants are enforced natively.
- SHA-256 detects corruption or modification but does not authenticate a publisher. Digital signatures are planned separately.

## Project Layout

```text
src/cli/       Shared UTF-8 CLI parsing and PackOptions construction
src/app/       Windows Win32 GUI, Runtime, and entry point
src/linux/     Linux GTK3 GUI, WebKitGTK Runtime, and entry point
src/webview/   WebView2 host
src/packer/    Manifest, payload, and packer
src/runtime/   ZIP resource access, LRU cache, local HTTP server, and controlled backend proxy
src/ipc/       Cross-platform IPC protocol, capabilities, path permissions, and dispatch
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
- Both GTK and Win32 package resources on a worker thread so the GUI remains responsive for large projects. Cancellation and percentage progress remain future work.

Next priorities are generated-app desktop/icon/DEB metadata, AppImage evaluation, and an external-link policy, followed by ARM64, Debian-family, and RPM-family evaluation after x86_64 stability work.

Other planned improvements:

- Multi-resolution icon generation
- Authenticode code signing
- Tray support, activating the existing window on a second launch, and always-on-top windows
- External-link policy
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
