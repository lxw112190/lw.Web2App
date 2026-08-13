# lw.Web2App

[简体中文](README.md) | [English](README_EN.md)

A lightweight desktop web-application packager. The current release supports Windows x64 and packages HTML, Vue, React, Vite, and other static builds into standalone EXE files without rebuilding the web project.

The target computer does not need Node.js, Rust, .NET, Electron, or a compiler toolchain. It only needs the Microsoft WebView2 Evergreen Runtime.

> **Platform status:** the stable implementation currently supports Windows 10/11 x64 only. Linux is the primary next milestone. The plan is to reuse the cross-platform payload, ZIP, manifest, SHA-256, local resource service, and cache core while adding a native Linux window/WebView layer and ELF/AppImage distribution. The current release cannot be built or run on Linux; see the [Cross-Platform Roadmap](#cross-platform-roadmap).

## Features

- Modern native Win32 graphical interface and a scriptable CLI.
- Package a local HTML, Vue, React, or Vite build directory into one EXE.
- Package an online `http://` or `https://` URL into one EXE.
- WebView2 Evergreen Runtime with the WebView2 Loader statically linked.
- Independently designed, versioned `LWWEB001` V1 payload container.
- SHA-256 verification before embedded content is opened.
- ZIP central-directory indexing and per-request decompression instead of eager extraction.
- A 32 MiB LRU cache for small hot resources; large files do not remain in memory.
- SPA fallback for history-mode Vue Router and React Router applications.
- Local HTTP service bound only to a random `127.0.0.1` port.
- Exact Host validation, no wildcard CORS, no directory listing, and path traversal protection.
- Limits for entry count, individual file size, and total uncompressed size.
- PNG/ICO application icons and complete Windows PE version metadata.
- Unicode paths, filenames, and application titles.

## How It Works

lw.Web2App creates a single-file application using a **native Runner plus an end-of-file payload**. It does not translate web source code into C++, nor does it bundle Chromium, Node.js, or Electron into every generated application. Rendering is delegated to the Microsoft WebView2 Runtime installed on Windows.

### Packaging

1. **Validate the input**: local mode checks that the static directory and entry HTML exist, then enforces limits on file count, individual size, and total size.
2. **Copy the Runner**: the current `lw.Web2App.exe` supplies the native PE prefix. If an already packaged application is used as the packer, only its original Runner prefix is copied, so old payloads are not nested.
3. **Update PE resources**: the copy receives the selected icon, product name, description, company, version, and copyright fields shown by Windows Explorer under **Properties → Details**.
4. **Build the ZIP**: files are read recursively and stored under normalized relative paths. Absolute paths, drive letters, `..`, NUL bytes, and duplicate archive paths are rejected.
5. **Append the container**: the resource ZIP, Manifest JSON, and a fixed 80-byte footer are appended to the PE file. The footer stores the format version, flags, section offsets and lengths, and the ZIP payload's SHA-256. The Windows PE loader ignores this trailing data, while the Runner can read it by offset.

Online URL mode does not snapshot or embed the remote website. Its ZIP is empty and the manifest records only the target URL and window settings. The generated application therefore requires network access and follows future changes to that website.

### Runtime

1. The executable reads the `LWWEB001` footer from its own end. Without a footer it opens the packager; with a footer it enters generated-application mode.
2. The Runner verifies that every offset and length is inside the EXE, limits manifest size, and hashes the resource ZIP. It refuses to open embedded content when validation fails.
3. Local mode indexes only the ZIP central directory. It neither extracts the whole site to disk nor loads every resource into memory at startup.
4. A private HTTP service starts on a random `127.0.0.1` port. It validates the exact Host, decompresses one requested resource at a time, sets its MIME type, and applies SPA fallback when configured.
5. Small frequently used resources are held in a 32 MiB LRU cache; large files are read on demand and do not remain resident.
6. The native window creates a WebView2 Controller and navigates to the private local address or configured online URL. Title, dimensions, resizing, and developer-tool policy come from the manifest.

SHA-256 detects resource corruption or modification; it does not authenticate a publisher. Trusted distribution of the manifest and complete EXE still requires a signing mechanism such as Authenticode.

## System Requirements

- Windows 10 1809+ or Windows 11
- x64
- Microsoft WebView2 Evergreen Runtime

Windows 11 and most maintained Windows 10 installations already include WebView2 Runtime. If it is unavailable, the generated application displays an installation prompt.

Windows 7 and Windows 8 are not supported. The project does not bundle the Fixed Version Runtime because it would add more than 250 MiB to the distribution.

## Download CI Builds

Every push and pull request produces a tested Windows x64 package through GitHub Actions:

1. Open the repository's **Actions** page.
2. Select the latest successful `Windows x64` workflow run.
3. Download `lw.Web2App-windows-x64` from the **Artifacts** section.

The artifact contains:

- `lw.Web2App.exe`
- `README.md` and `README_EN.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- Original third-party license texts under `third_party/licenses/`
- `examples/wechat-article-formatter.exe`, the packaged integration-test application
- `examples/wechat-article-formatter-LICENSE.txt`, its project license
- `SHA256SUMS.txt`

Pushing a `v*` tag also creates a GitHub Release containing the ZIP distribution and its SHA-256 checksum file.

### CI Integration-Test Project

[wechat-article-formatter](https://github.com/lxw112190/wechat-article-formatter) is a Vite, React, and TypeScript Markdown editor for WeChat Official Account articles, with themes, mobile preview, and rich-text copying. CI checks out its `main` branch, runs `npm ci` and `npm run build`, and packages the resulting `dist` directory with the freshly built lw.Web2App. It then runs `inspect` against `examples/wechat-article-formatter.exe` to reload the manifest and verify the payload SHA-256. Any failure stops the workflow.

This example exercises a real Vite/React production build, a Chinese application title, a non-trivial static asset set, SPA fallback, PE metadata, and final distribution packaging. Its application data remains in WebView2's local browser storage, so backups should be exported just as they are in the web version.

## Build from Source

Requirements:

- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.24+
- Internet access during the first configure, unless a dependency cache is provided

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The output is `build/Release/lw.Web2App.exe`. Launching it without arguments opens the graphical packager.

### Optional Offline Dependency Cache

CMake first checks `.deps` in the repository root for these archives:

```text
.deps/json.tar.xz
.deps/cpp-httplib.tar.gz
.deps/miniz.tar.gz
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

Additional options:

- `--no-spa`: disable SPA fallback.
- `--devtools`: enable developer tools and the default context menu.
- `--company`: write the company name.
- `--version`: write file and product versions.
- `--copyright`: write copyright metadata.

A generated EXE can also execute CLI packaging commands. Only its original Runner prefix is copied, so old payloads are never nested.

## Payload V1 Format

```text
Runner PE
Resource ZIP       empty in online URL mode
Manifest JSON
Footer             fixed 80 bytes
  magic[8]         LWWEB001
  version u32      1
  flags u32
  payloadOffset u64
  payloadSize u64
  manifestOffset u64
  manifestSize u64
  SHA-256[32]
```

All integers are explicitly serialized as little endian instead of relying on compiler structure layout. Both the manifest and footer carry the payload digest. The Runner checks their agreement and then hashes the actual payload before starting WebView2.

See [Payload V1 format](docs/format-v1.md) for the binary layout.

## Security Boundary

This project packages trusted static web applications. It is not a sandbox for hostile web content.

- Default maximum resource count: 100,000.
- Default maximum individual file size: 512 MiB.
- Default maximum total uncompressed size: 2 GiB.
- Maximum manifest size: 1 MiB.
- The HTTP service listens only on IPv4 loopback and requires the exact randomized Host value.
- SHA-256 detects corruption or modification but does not authenticate a publisher. Digital signatures are planned separately.

## Project Layout

```text
src/app/       Win32 GUI, CLI, and program entry point
src/webview/   WebView2 host
src/packer/    Manifest, payload, and packer
src/runtime/   ZIP resource access, LRU cache, and local HTTP server
src/pe/        Icon and version resource updates
src/common/    File, path, and SHA-256 utilities
tests/         Unit and packaging/resource integration tests
```

## CI and Releases

The workflow at [.github/workflows/build.yml](.github/workflows/build.yml):

1. Configures a VS2022 Windows x64 Release build.
2. Builds `lw.Web2App.exe`.
3. Runs all CTest tests.
4. Builds the `wechat-article-formatter` Vite production bundle.
5. Packages the test project as an EXE and validates it with `inspect`.
6. Creates a distribution directory and `SHA256SUMS.txt`.
7. Produces `lw.Web2App-windows-x64.zip`.
8. Uploads a GitHub Actions artifact.
9. Publishes the ZIP and checksum for `v*` tags.

## Dependencies

- Microsoft WebView2 SDK — Microsoft software license
- miniz — MIT License
- cpp-httplib — MIT License
- nlohmann/json — MIT License

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Cross-Platform Roadmap

Linux support is the primary next milestone. The implementation will evolve along explicit platform boundaries instead of merely wrapping Windows code in preprocessor checks:

1. Keep payload, manifest, ZIP, SHA-256, path validation, resource serving, and LRU caching in a platform-neutral core.
2. Retain the Win32 + WebView2 + PE-resource backend for Windows, and add a native Linux window and system-WebView backend, initially evaluating GTK + WebKitGTK.
3. Add an ELF Runner and Linux application metadata. The first intended package is an x86_64 `.tar.gz`, with AppImage under evaluation; `.deb`, `.rpm`, and ARM64 will follow only after the base runtime is stable.
4. Add Linux CI for compilation, unit tests, static-site packaging, payload inspection, and a minimal launch smoke test.
5. Preserve parsing compatibility with the `LWWEB001` V1 container. Platform-specific needs will use versioned manifest fields or a new container version only when necessary.

Until that work is delivered, the README, releases, and repository description should continue to say **Windows only today** rather than presenting Linux support as complete.

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

<p align="center">
  <img src="docs/assets/sponsor.jpg" alt="WeChat sponsor QR code" width="420">
</p>

## License

This project is licensed under the [MIT License](LICENSE).
