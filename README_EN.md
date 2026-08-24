# lw.Web2App

[简体中文](README.md) | [English](README_EN.md)

A lightweight desktop web-application packager. It supports Windows x64 and now includes an Ubuntu 22.04/24.04 x86_64 Beta, packaging HTML, Vue, React, Vite, and other static builds into single-file desktop applications without rebuilding the web project.

The target computer does not need Node.js, Rust, .NET, Electron, or a compiler toolchain. Windows uses Microsoft WebView2 Evergreen Runtime; Linux uses the system GTK3 and WebKitGTK 4.1 runtime.

> **Platform status:** Windows 10/11 x64 is stable. Ubuntu 22.04/24.04 x86_64 is the first Linux Beta, with a GTK3 GUI, CLI, ELF Runner, WebKitGTK Runtime, single-file payload, logs, and CI-built `.deb`/`.tar.gz` packages. Other distributions, ARM64, AppImage, and RPM are not supported yet.

Windows graphical packager:

<img src="docs/assets/lw.Web2App.png" alt="lw.Web2App graphical packager on Windows" width="760">

The Windows GUI uses a two-column layout. The left side selects a local build or URL, entry page, start path, and controlled backend proxy. The right side configures the window title, PE ProductName, FileDescription, company, version, copyright, live icon preview, dimensions, and runtime options. Trusted local pages can opt into **Local interaction (Native IPC)** and use a separate permissions window to apply a preset, grant individual capabilities, and add optional fixed roots. It remains disabled by default and is unavailable in URL mode. The title continues to synchronize into ProductName and FileDescription until either field is edited manually. Output and progress share a full-width bottom area; the default is `out\MyWebApp.exe` beside the packer, and `out` is created on the first build. Resource compression remains on a worker thread so large projects do not freeze the UI. After a successful package, File Explorer opens automatically with the generated EXE selected.

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
- Windows CLI Authenticode signing through a Certificate Store code-signing certificate, binding publisher trust to the final payload digest.
- Generated applications start in borderless fullscreen by default; `F11` toggles and `Esc` exits fullscreen. On Windows, standard web Fullscreen API requests also synchronize the native host window with borderless fullscreen mode.
- spdlog-based rotating packager/runtime logs: INFO by default, 2 MiB per file, five files retained.
- ZIP central-directory indexing and per-request decompression instead of eager extraction.
- A 32 MiB LRU cache for small hot resources; large files do not remain in memory.
- SPA fallback for history-mode Vue Router and React Router applications.
- Deterministic GUI discovery of `.html`/`.htm` launch pages; separate Manifest, GUI, and CLI `entry` and `start_path` settings support both multi-page entry files and initial SPA routes.
- The local HTTP service binds only to `127.0.0.1` and prefers a stable per-`app_id` dynamic port, with deterministic fallback ports when an unrelated process occupies it.
- True cross-platform single-instance locking uses a Windows named mutex or Linux `flock`, independently of the HTTP port.
- An optional controlled backend proxy forwards same-origin `/__lw_proxy__/...` requests to one fixed legacy HTTP origin from the Manifest, without disabling WebView security; Windows and Linux share the same implementation.
- Optional Native IPC exposes app information, system directory/file pickers, and permission-scoped filesystem operations to trusted local pages through the stable `window.lw.invoke()` API. A session Local File Bridge streams large local files over localhost HTTP. Both are disabled by default and authorized per method.
- Exact Host validation, no wildcard CORS, no directory listing, and path traversal protection.
- Limits for entry count, individual file size, and total uncompressed size.
- PNG/ICO application icons and complete Windows PE version metadata.
- Unicode paths, filenames, and application titles.

## How It Works

lw.Web2App creates a single-file application using a **platform Runner plus an end-of-file payload**. It does not translate web source into C++, nor bundle Chromium, Node.js, or Electron into every generated app. Windows delegates rendering to WebView2 and Linux to WebKitGTK; both share the manifest, ZIP, SHA-256, local HTTP server, path-security, and LRU-cache core.

### Packaging

1. **Validate the input**: local mode scans and verifies the HTML selected by `entry`, ensures `start_path` cannot leave the private local service, then enforces limits on file count, individual size, and total size.
2. **Prepare the payload**: normalized resources are streamed to a temporary ZIP; the Manifest is serialized exactly once, and the final combined ZIP-plus-Manifest SHA-256 is computed before the Runner is modified. Absolute paths, drive letters, `..`, NUL bytes, and duplicate archive paths are rejected.
3. **Copy and clean the Runner**: only the native PE/ELF prefix is copied. Windows always removes an inherited Certificate Table and old `LWWEB_BINDING`, so unsigned output cannot impersonate the template's signature and repeated packaging never nests an old payload.
4. **Write platform metadata**: Windows updates PE icons and version fields and, for a signed package, writes an Authenticode-required payload binding. Linux adds executable permissions.
5. **Append the complete container**: the prepared ZIP, Manifest, and fixed 80-byte Footer are streamed onto the executable, then reloaded to validate the digest.
6. **Optionally sign and publish**: Windows signs the complete EXE through the Windows SDK SignTool and a Certificate Store thumbprint, then jointly verifies WinVerifyTrust, the PE Binding, and Footer. The Certificate Table remains at physical EOF; Runtime skips it and up to seven preceding alignment bytes to locate the Payload Footer. Only a fully verified file is atomically published.

Online URL mode does not snapshot or embed the remote website. Its ZIP is empty and the manifest records only the target URL and window settings. The generated application therefore requires network access and follows future changes to that website. On Linux, the runtime passes `http_proxy`, `https_proxy`, `all_proxy`, and `no_proxy` (including their uppercase forms) to WebKitGTK.

### Runtime

1. The executable reads the `LWWEB002` footer from its own end. Without a footer it opens the packager; with a footer it enters generated-application mode. Legacy `LWWEB001` packages remain readable.
2. The Runner verifies that every offset and length is inside the EXE, limits manifest size, and hashes the resource ZIP plus manifest. A signed Windows package additionally requires the Footer digest to match the Authenticode-protected `LWWEB_BINDING` and pass WinVerifyTrust. Any failure prevents embedded content from opening.
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

Linux artifacts include a lw.Web2App `.deb`, a portable `.tar.gz`, the generated single-file `examples/wechat-article-formatter`, an application-specific Native IPC example DEB, and SHA-256 files. DEBs install desktop-menu metadata and declare GTK/WebKitGTK dependencies; portable packages and generated apps still require those libraries on the target Ubuntu system.

### CI Integration-Test Project

[wechat-article-formatter](https://github.com/lxw112190/wechat-article-formatter) is a Vite, React, and TypeScript Markdown editor for WeChat Official Account articles, with themes, mobile preview, and rich-text copying. CI checks out its `main` branch, runs `npm ci` and `npm run build`, and packages the resulting `dist` directory with the freshly built lw.Web2App. It then runs `inspect` against `examples/wechat-article-formatter.exe` to reload the manifest and verify the payload SHA-256. Any failure stops the workflow.

This example exercises a real Vite/React build, a Chinese title, a non-trivial asset set, SPA fallback, Windows PE metadata, Linux ELF permissions, and final distribution packaging. Windows CI also launches a generated app, invokes `app.getInfo` from its WebView2 page, and verifies WebView2 initialization, navigation, and the Native IPC round trip in the Runtime log. Linux CI launches the generated app under Xvfb and checks WebKitGTK initialization and navigation logs.

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
.deps/cpp-httplib-v0.51.0.tar.gz
.deps/miniz.tar.gz
.deps/spdlog.tar.gz
.deps/webview2.zip
```

If they are absent, CMake downloads pinned versions. Set `-DLWWEB_DEPS_CACHE=<directory>` to use another cache location.

## Project Configuration: `lwweb.json`

The repository has a strict `schema: 1` project model consumed as the single input to the `publish` command. It describes web input, application identity, window/runtime behavior, and publication targets. It is not the Runtime Manifest embedded in generated applications, and the existing `pack`, `pack-url`, and `inspect` commands remain unchanged.

```json
{
  "schema": 1,
  "app": {
    "id": "com.example.myapp",
    "name": "MyApp",
    "version": "1.2.0",
    "company": "Example Company",
    "description": "Example desktop application",
    "icon": "./icon.png"
  },
  "web": {
    "source": "./dist",
    "entry": "index.html",
    "start_path": "/"
  },
  "window": {
    "width": 1280,
    "height": 800,
    "fullscreen": false,
    "resizable": true
  },
  "runtime": {
    "spa_fallback": true,
    "devtools": false,
    "ipc": {
      "enabled": false,
      "capabilities": [],
      "filesystem_roots": []
    }
  },
  "publish": {
    "output": "./release",
    "windows": {
      "portable": true,
      "zip": true,
      "installer": { "enabled": false },
      "signing": { "enabled": false }
    },
    "linux": { "tar_gz": true, "deb": true }
  }
}
```

Relative paths such as `web.source`, `app.icon`, `publish.output`, `signtool`, and `iscc` are always resolved against the directory containing `lwweb.json`, never the shell's current directory. The parser rejects unknown fields, files over 1 MiB, invalid versions/manifests/capabilities, and secret-bearing fields such as `password`, `token`, `secret`, or PFX settings. A config may contain only a certificate thumbprint and timestamp URL, never a certificate password. The repository includes a complete example at `examples/project-config/lwweb.json`.

### Build a Release Directory with One Command

```powershell
# Reads lwweb.json from the current directory by default
lw.Web2App.exe publish

# An explicit --output overrides the project configuration
lw.Web2App.exe publish --config .\config\lwweb.json --output .\release
```

Windows produces a portable EXE and ZIP by default, with a Setup EXE added when the Installer is enabled. Ubuntu produces an executable and `tar.gz`, plus an application-specific DEB when `publish.linux.deb` is enabled. Every version gets an isolated directory plus `SHA256SUMS.txt` and a machine-readable `RELEASE_INFO.json`:

```text
release/
└── MyApp-1.2.0-windows-x64/
    ├── MyApp.exe
    ├── MyApp-Setup-1.2.0.exe
    ├── MyApp-1.2.0-windows-x64.zip
    ├── SHA256SUMS.txt
    └── RELEASE_INFO.json
```

An Ubuntu release looks like this:

```text
release/
└── MyApp-1.2.0-linux-x64/
    ├── MyApp
    ├── myapp_1.2.0_amd64.deb
    ├── MyApp-1.2.0-linux-x64.tar.gz
    ├── SHA256SUMS.txt
    └── RELEASE_INFO.json
```

Publishing completes packaging, Installer or DEB creation, compression, and SHA-256 generation in a hidden staging directory under the output root, then replaces the same-version directory only after every stage succeeds. A failure removes incomplete files and preserves the previous release. `publish.output` in the project file is relative to that file, while an explicit CLI `--output` is relative to the current working directory.

Windows Installers are built with [Inno Setup 6.3 or newer](https://jrsoftware.org/isinfo.php). When `publish.windows.installer.enabled` is true, lw.Web2App searches for `ISCC.exe` in `installer.iscc`, `PATH`, and common Inno Setup installation directories, in that order. A missing compiler is a hard error. The stable `app.id` becomes the upgrade identity; the default destination is `{autopf}\AppName`, with an uninstall entry, a configurable Start Menu shortcut, and an initially unchecked desktop-shortcut task. With Authenticode enabled, the portable application is signed before being embedded, then the Setup EXE is signed separately, so both final executables carry publisher trust.

Generated Linux DEBs run the system `dpkg-shlibdeps` against the final ELF, so GTK/WebKitGTK dependencies follow the actual Ubuntu 22.04 or 24.04 build environment instead of hard-coded package names. `dpkg-deb` then installs the executable under `/usr/bin/<last app.id segment>`, a desktop entry under `/usr/share/applications`, and a configured PNG/SVG icon under the hicolor theme; an embedded default SVG is used when no icon is configured. The build host must provide `dpkg-dev`. Missing tools, dependency-analysis errors, or invalid DEB output fail the atomic publication. This first stage is `amd64` only and does not produce ARM64, RPM, AppImage, Flatpak, or Snap packages.

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

`inspect` verifies the payload SHA-256 and prints its manifest. Signed Windows
packages additionally validate `LWWEB_BINDING` and Authenticode:

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
- `--ipc-capability`: repeat for each capability, such as `app.info`, `dialog.directory`, `dialog.file`, `fs.exists`, `fs.list`, `fs.copy`, `fs.move`, or `fs.delete`.
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

### Windows Authenticode signing

The first version uses a code-signing certificate already installed with its private key in the Windows Certificate Store. It intentionally does not accept PFX files or passwords on the command line:

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --title "My App" `
  --sign-cert-thumbprint 00112233445566778899AABBCCDDEEFF00112233 `
  --timestamp-url https://timestamp.example.com
```

- `--sign-cert-thumbprint`: enable signing with the certificate SHA-1 thumbprint; whitespace is ignored.
- `--timestamp-url`: optional RFC 3161 timestamp service using `http://` or `https://`.
- `--signtool`: optional full SignTool path; otherwise `PATH` and the newest installed x64 Windows SDK are searched.

The packer removes the template's old Certificate Table, writes PE metadata and a digest-bearing `LWWEB_BINDING`, appends the exact prepared payload, and signs the complete EXE last. Runtime recognizes the Certificate Table emitted at physical EOF and safely locates the Footer before it, completing the certificate → PE Binding → Payload trust chain. Without a certificate option, output remains unsigned and any inherited signature and binding are explicitly removed.

A generated EXE can also execute CLI packaging commands. Only its original Runner prefix is copied, so old payloads are never nested.

## Native IPC (experimental)

Native IPC is intended only for trusted static pages packaged with the application. It
is disabled by default and authorized per-method. Windows and Linux pages use one
Promise API:

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
  --ipc-capability dialog.file `
  --ipc-capability fs.exists `
  --ipc-capability fs.list `
  --ipc-capability fs.copy `
  --ipc-capability fs.move `
  --ipc-capability fs.delete
```

Directory selection creates a process-local Session Grant. The Local File Bridge uses
random File Grants and same-origin HTTP Range streaming for large files without exposing
disk paths. `fs.move` supports a safe copy-and-delete fallback when a regular file crosses
a disk or filesystem boundary.

See the **[complete Native IPC guide](docs/native-ipc_EN.md)** for capabilities,
parameters, results, errors, security boundaries, and File Bridge examples. A
[Chinese guide](docs/native-ipc.md) and the [runnable example](examples/native-ipc/index.html)
are also available.

## Payload V2 Format

The packer first creates a `PreparedPayload`: ZIP resources, the exact Manifest
bytes serialized once, and their combined SHA-256 are finalized before the Runner
is modified. Later PE metadata processing and Authenticode signing never
serialize the Manifest again; the exact ZIP and Manifest bytes covered by the
prepared digest are appended to the executable. This separation provides a stable
digest for Signed Payload Binding without changing the existing `LWWEB002` footer.

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

### Signed Payload Binding

Signed Windows applications store the same Payload SHA-256 in the PE
`LWWEB_BINDING` resource. Its fixed 48-byte `LWBIND01` format sets the
“Authenticode required” flag. At startup, the Runtime verifies `ZIP + Manifest`
against the Footer, compares the Binding and Footer digests, and finally validates
the signature protecting that PE resource through WinVerifyTrust. This completes
the certificate → PE Binding → Payload trust chain. Legacy and ordinary unsigned
applications without a Binding remain compatible; malformed data, unknown versions
or flags, digest mismatches, and invalid signatures are rejected.

## Security Boundary

This project packages trusted static web applications. It is not a sandbox for hostile web content.

- Default maximum resource count: 100,000.
- Default maximum individual file size: 512 MiB.
- Default maximum total uncompressed size: 2 GiB.
- Maximum manifest size: 1 MiB.
- The HTTP service listens only on IPv4 loopback and requires the exact per-app port Host value.
- The backend proxy accepts only the fixed Manifest `http://` origin; arbitrary URLs, cross-site callers, cross-host redirects, and ZIP resources that collide with its prefix are rejected, with explicit size and timeout limits.
- Native IPC is disabled by default and local-mode only; capabilities, exact origin, fixed roots, directory grants, and session FileGrants are enforced natively. The Local File Bridge never accepts a web-supplied disk path and streams only by opaque token.
- SHA-256 detects corruption or modification but does not authenticate a publisher. Use the Windows CLI Authenticode options when publisher identity is required, and protect the code-signing private key appropriately.

## Project Layout

```text
src/cli/       Shared UTF-8 CLI parsing and PackOptions construction
src/app/       Windows Win32 GUI, Runtime, and entry point
src/linux/     Linux GTK3 GUI, WebKitGTK Runtime, and entry point
src/webview/   WebView2 host
src/packer/    Manifest, payload, and packer
src/runtime/   ZIP resource access, LRU cache, local HTTP server, and controlled backend proxy
src/ipc/       Cross-platform IPC protocol, capabilities, path permissions, and dispatch
src/pe/        Icon/version resources, Payload Binding, and Authenticode
src/publish/   lwweb.json parsing, atomic publishing, archives, checksums, and release metadata
src/common/    File, path, and SHA-256 utilities
docs/          Payload format and bilingual Native IPC guides
examples/      Package-ready project configuration and Native IPC examples
tests/         Unit and packaging/resource integration tests
```

## CI and Releases

The workflow at [.github/workflows/build.yml](.github/workflows/build.yml):

1. Builds and tests Windows x64 with VS2022 on Windows 2022.
2. Builds and tests Linux x64 with Ninja, GTK3, WebKitGTK 4.1, and OpenSSL on Ubuntu 22.04 and 24.04.
3. Builds the `wechat-article-formatter` Vite bundle in all platform jobs.
4. Generates a Windows EXE or Linux ELF, validates its payload with `inspect`, and tests complete, HEAD, single-range, invalid-token, traversal, read-only-method, and revoke behavior for the Local File Bridge.
5. Runs `publish` smoke tests on Windows and Ubuntu and validates the ZIP/tar.gz, `SHA256SUMS.txt`, and `RELEASE_INFO.json`.
6. Builds an Installer with real Inno Setup on Windows CI.
7. Builds a real generated-app DEB on Ubuntu and checks its dependencies, ELF, desktop entry, and icon with `dpkg-deb --info/--contents`.
8. Launches a generated Windows EXE, invokes `app.getInfo` from its WebView2 page, validates initialization, navigation, IPC request/response markers in the Runtime log, and uploads diagnostics on failure.
9. Launches the Linux result under Xvfb and checks WebKitGTK initialization and navigation logs.
10. Publishes Windows ZIP, Linux `.tar.gz`/`.deb`, and SHA-256 artifacts; `v*` tags collect them into a GitHub Release.

## Dependencies

- Microsoft WebView2 SDK — Microsoft software license
- GTK3 / WebKitGTK / OpenSSL — Linux system dynamic dependencies under their respective licenses
- miniz — MIT License
- cpp-httplib v0.51.0 — MIT License
- nlohmann/json — MIT License
- spdlog (including bundled fmt) — MIT License

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Linux Beta and Roadmap

The first Ubuntu implementation now includes the cross-platform core, GTK3 GUI, CLI, ELF Runner, WebKitGTK Runtime, XDG data/cache/log directories, DEB/TGZ packaging, and dual-version Linux CI. It continues to write `LWWEB002` and read legacy `LWWEB001` payloads.

Explicit Linux Beta boundaries:

- Ubuntu 22.04/24.04 x86_64 only. Generated apps run within the same platform family; Windows and Linux Runners cannot generate each other's binaries.
- Generated output remains a single ELF with an appended payload. `publish.linux.deb` can additionally produce an application-specific DEB with desktop metadata, an icon, and automatically derived shared-library dependencies.
- Rendering uses the system WebKitGTK 4.1. Browser security updates and Web API behavior therefore follow Ubuntu updates.
- Both GTK and Win32 package resources on a worker thread so the GUI remains responsive for large projects. Cancellation and percentage progress remain future work.

Next priorities are AppImage evaluation and an external-link policy, followed by ARM64, Debian-family, and RPM-family evaluation after x86_64 stability work.

Other planned improvements:

- Multi-resolution icon generation
- Tray support, activating the existing window on a second launch, and always-on-top windows
- External-link policy
- Custom user agents, startup arguments, and CSP

## Contact and Support

- Author: 天天代码码天天
- QQ: 819069052
- QQ Group: C# 人工智能实践 | Group ID: 758616458

If this project is useful to you, you can scan the QR code to support its maintenance:

<img src="docs/assets/sponsor.jpg" alt="WeChat sponsor QR code" width="260">

## License

This project is licensed under the [MIT License](LICENSE).
