# lw.Web2App

[简体中文](README.md) | [English](README_EN.md)

一个面向桌面平台的轻量网页应用打包工具。当前版本支持 Windows x64，可在无需重新编译网页项目的情况下，将 HTML、Vue、React、Vite 等静态产物快速打包为独立 EXE。

目标电脑不需要安装 Node.js、Rust、.NET 或 Electron，只需具备 Microsoft WebView2 Evergreen Runtime。

> **平台状态：** 当前稳定实现仅支持 Windows 10/11 x64。Linux 是下一阶段的首要目标，计划复用跨平台 Payload、ZIP、Manifest、SHA-256、本地资源服务和缓存核心，并增加 Linux 原生窗口/WebView 与 ELF/AppImage 分发实现。当前版本不能在 Linux 上构建或运行，详见[跨平台规划](#跨平台规划)。

<img src="docs/assets/lw.Web2App.png" alt="lw.Web2App Windows 图形界面" width="760">

## 功能特性

- 原生 Win32 图形界面，同时提供 CLI。
- 图形界面实时显示打包阶段和结果状态，状态区域采用独立重绘，连续更新时保持清晰。
- 本地 HTML、Vue、React、Vite 静态目录打包为单个 EXE。
- 在线 `http://`、`https://` 地址打包为单个 EXE。
- 使用 WebView2 Evergreen Runtime，WebView2 Loader 静态链接进 EXE。
- 自主设计的 `LWWEB002` V2 Payload 容器格式，并兼容读取 `LWWEB001` V1。
- 程序启动时验证资源 ZIP 与 Manifest 的联合 SHA-256，内容或配置损坏时拒绝继续运行。
- 生成应用默认无边框全屏显示，支持 `F11` 切换全屏、`Esc` 退出全屏。
- 基于 spdlog 的打包器/Runtime 滚动日志，默认 INFO、单文件 2 MiB、保留 5 个。
- 只索引 ZIP 中央目录，请求资源时才解压对应文件，不会启动即展开全部内容。
- 32 MiB LRU 缓存小型热点资源，大文件不长期驻留内存。
- 支持 Vue Router、React Router 等 history 模式所需的 SPA fallback。
- 本地 HTTP 服务仅监听 `127.0.0.1` 随机端口。
- 校验精确 Host，不默认开放跨域，不提供目录浏览。
- 拒绝绝对路径、盘符、`..`、NUL、重复 ZIP 路径。
- 限制资源数量、单文件大小和总解压大小，降低 ZIP 炸弹风险。
- 支持 PNG、ICO 应用图标和完整 Windows PE 版本信息。
- 支持中文目录、中文文件名和中文应用标题。

## 工作原理

lw.Web2App 采用“原生 Runner + 文件尾部载荷”的方式生成单文件应用。它不是把网页源码翻译成 C++，也不是把 Chromium、Node.js 或 Electron 塞进每个程序，而是复用 Windows 上的 WebView2 Runtime 来渲染网页。

### 打包阶段

1. **检查输入**：本地模式先确认静态目录和入口 HTML 存在，并对文件数量、单文件大小及总大小应用安全上限。
2. **复制 Runner**：以当前 `lw.Web2App.exe` 为原生程序模板，只复制它原始的 PE Runner 部分。即使使用已经打包过的 EXE 再次打包，也不会嵌套旧载荷。
3. **写入 PE 资源**：在副本中更新图标、产品名、文件说明、公司、版本和版权等 Windows 资源，因此资源管理器“属性 → 详细信息”可以直接显示这些字段。
4. **构建 ZIP**：递归读取静态目录，将规范化后的相对路径流式压缩到临时 ZIP 文件，避免把源文件和完整 ZIP 同时驻留内存。绝对路径、盘符、`..`、NUL 和重复路径会被拒绝。
5. **追加容器**：依次在 PE 文件尾部流式写入资源 ZIP、Manifest JSON 和固定 80 字节 Footer。V2 Footer 记录格式版本、标志、各区段偏移/长度，以及“资源 ZIP + Manifest”的联合 SHA-256。

在线 URL 模式不保存远端网站内容，ZIP 为空；Manifest 只记录目标 URL 和窗口配置，启动时直接导航到该地址。因此在线模式需要联网，页面变化也会随网站实时变化。

### 运行阶段

1. 程序从自身末尾读取 `LWWEB002` Footer；没有 Footer 时显示打包器界面，有 Footer 时进入生成应用模式，同时仍可读取旧版 `LWWEB001`。
2. Runner 检查所有偏移和长度是否位于 EXE 内，限制 Manifest 大小，并计算资源 ZIP 与 Manifest 的联合 SHA-256。校验失败时拒绝打开嵌入内容。
3. 本地模式只读取 ZIP 中央目录建立索引，不会把网站完整解压到磁盘或一次性载入内存。
4. Runner 在 `127.0.0.1` 随机端口启动仅供本进程页面访问的 HTTP 服务，严格检查 Host，按请求解压单个资源、设置 MIME 类型，并在需要时提供 SPA fallback。
5. 小型热点资源进入 32 MiB LRU 缓存；大文件按请求读取，不长期占用内存。
6. 原生窗口创建 WebView2 Controller，并导航到本地随机地址或在线 URL。窗口标题、尺寸、是否可调整、默认全屏和开发者工具策略来自 Manifest；全屏时可用 `F11` 切换、`Esc` 退出。
7. 每个应用使用稳定 `app_id`，WebView2 用户数据存放在 `%LOCALAPPDATA%\lw.Web2App\apps\<app_id>\WebView2`，重命名 EXE 不会改变存储位置。

SHA-256 用于发现资源损坏或修改，不等同于发布者认证；Manifest 配置和整个 EXE 的可信发布仍应依靠 Authenticode 等代码签名机制。

## 日志与排障

lw.Web2App 使用 spdlog 同步滚动文件日志，默认级别为 INFO，单文件最大 2 MiB，保留当前文件及 5 个轮转文件。日志不写在 EXE 旁边，因此应用安装到 `Program Files` 或只读目录时也不依赖该目录的写权限。

- 打包器日志：`%LOCALAPPDATA%\lw.Web2App\logs\packer.log`
- 生成应用日志：`%LOCALAPPDATA%\lw.Web2App\apps\<app_id>\logs\app.log`
- Manifest/Payload 尚未成功加载时的启动错误：`%LOCALAPPDATA%\lw.Web2App\logs\launcher.log`

INFO 记录打包阶段、资源数量与大小、Payload 摘要、服务端口、WebView2 版本、初始化和导航结果；DEBUG 额外记录静态资源请求、ZIP 缓存命中/未命中和 SPA fallback。Runtime 还会把 `console.error`、未捕获脚本错误和未处理 Promise rejection 记录为 `[WEB-ERROR]`，普通 `console.log` 不会写入日志。

图形界面默认勾选“启用运行日志”。勾选“详细日志”后对应 DEBUG；取消“启用运行日志”只关闭生成应用的 Runtime 日志，打包器仍会保留自己的打包诊断日志。日志初始化失败不会阻止应用继续启动或打包。

典型 Runtime 日志如下：

```text
2026-08-13 18:37:27.820 [info] [lw.WebRuntime] Payload format: LWWEB002
2026-08-13 18:37:27.820 [info] [lw.WebRuntime] Payload verification OK
2026-08-13 18:37:27.821 [info] [lw.WebRuntime] Resource server: 127.0.0.1:60435
2026-08-13 18:37:27.876 [info] [lw.WebRuntime] WebView2 Runtime: 135.0.3179.98
2026-08-13 18:37:28.503 [info] [lw.WebRuntime] WebView2 initialized
2026-08-13 18:37:28.623 [info] [lw.WebRuntime] Navigation completed
```

遇到白屏或启动失败时，建议先查看 `app.log`；如果没有生成应用目录，再查看公共 `launcher.log`。比较问题电脑和正常电脑的 Windows 版本、WebView2 版本、Payload 版本及首条 ERROR，通常可以快速定位运行环境、完整性校验或前端脚本问题。

日志设置保存在 Manifest 中，当前默认值如下；GUI 第一版只暴露启用状态和 INFO/DEBUG 级别，其余轮转参数使用安全默认值：

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

## 系统要求

- Windows 10 1809+ 或 Windows 11
- x64
- Microsoft WebView2 Evergreen Runtime

Windows 11 和大多数仍在维护的 Windows 10 设备已经安装 WebView2 Runtime。未安装时，生成的程序会提示用户安装。

不支持 Windows 7、Windows 8，也不会捆绑超过 250 MiB 的 Fixed Version Runtime。

## 下载 CI 构建产物

每次推送和 Pull Request 都会通过 GitHub Actions 构建并测试 Windows x64 版本：

1. 打开项目的 **Actions** 页面。
2. 选择最新一次成功的 `Windows x64` 工作流。
3. 在页面底部的 **Artifacts** 区域下载 `lw.Web2App-windows-x64`。

Artifact 中包含：

- `lw.Web2App.exe`
- `README.md` 和 `README_EN.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `third_party/licenses/` 第三方许可证原文
- `examples/wechat-article-formatter.exe` CI 集成测试生成的示例应用
- `examples/wechat-article-formatter-LICENSE.txt` 示例项目许可证
- `SHA256SUMS.txt`

推送 `v*` 标签时，CI 还会创建 GitHub Release，并附加可直接下载的 ZIP 包和 SHA-256 校验文件。

### CI 集成测试项目

[wechat-article-formatter](https://github.com/lxw112190/wechat-article-formatter) 是一个面向微信公众号的纯前端 Markdown 排版与编辑工具，采用 Vite、React 和 TypeScript，支持主题排版、手机预览及复制富文本正文。CI 会检出该项目的 `main` 分支，执行 `npm ci` 和 `npm run build` 生成 `dist`，再用本次构建的 lw.Web2App 将它打包成 `examples/wechat-article-formatter.exe`。随后运行 `inspect` 重新读取 Manifest 并验证 Payload SHA-256；任一步失败都会使工作流失败。

这个示例同时验证了真实 Vite/React 产物、中文标题、较多静态资源、SPA fallback、PE 元数据和最终分发打包流程。示例应用的数据仍保存在 WebView2 对应的本地浏览器存储中，请像使用网页版本一样定期导出备份。

## 从源码构建

构建环境：

- Visual Studio 2022，安装“使用 C++ 的桌面开发”工作负载
- CMake 3.24+
- 首次配置时可以访问依赖下载地址

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

构建产物位于：

```text
build/Release/lw.Web2App.exe
```

直接双击运行会打开图形界面。

### 可选离线依赖缓存

CMake 会优先从仓库根目录的 `.deps` 读取以下文件；文件不存在时才访问固定版本 URL：

```text
.deps/json.tar.xz
.deps/cpp-httplib.tar.gz
.deps/miniz.tar.gz
.deps/spdlog.tar.gz
.deps/webview2.zip
```

也可以用 `-DLWWEB_DEPS_CACHE=目录` 指向其他缓存位置。

## CLI 使用方法

### 打包本地静态目录

入口文件会自动优先寻找 `index.html`：

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe --title "我的应用"
```

完整示例：

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --entry index.html `
  --title "我的应用" `
  --width 1280 `
  --height 800 `
  --windowed `
  --debug-log `
  --icon .\app.png `
  --company "示例公司" `
  --version 1.2.0.0 `
  --copyright "Copyright © 2026"
```

### 打包在线网址

```powershell
lw.Web2App.exe pack-url https://example.com .\Example.exe --title "在线应用"
```

### 检查生成的 EXE

`inspect` 会验证 Payload SHA-256 并输出 Manifest：

```powershell
lw.Web2App.exe inspect .\MyApp.exe
```

其他开关：

- `--no-spa`：关闭 SPA fallback。
- `--windowed`：覆盖默认行为，使生成应用以普通窗口启动。
- `--no-log`：关闭生成应用的运行日志。
- `--debug-log`：将运行日志级别设为 DEBUG，记录资源请求、ZIP 缓存和 SPA fallback。
- `--devtools`：允许打开开发者工具和默认右键菜单。
- `--company`：写入公司名称。
- `--version`：写入文件和产品版本。
- `--copyright`：写入版权信息。

已经生成的 EXE 也可继续执行 CLI 打包命令。程序只会复制原始 Runner 前缀，不会嵌套旧 Payload。

## Payload V2 格式

```text
Runner PE
Resource ZIP       在线 URL 模式为空
Manifest JSON
Footer             固定 80 字节
  magic[8]         LWWEB002
  version u32      2
  flags u32
  payloadOffset u64
  payloadSize u64
  manifestOffset u64
  manifestSize u64
  SHA-256[32]
```

全部整数都以显式小端方式序列化，不依赖 C++ 编译器的结构体对齐规则。V2 的 SHA-256 覆盖 Resource ZIP 与 Manifest JSON 的连续字节，因此 URL、窗口配置等 Manifest 内容被修改时也会被发现。Runner 仍兼容读取旧版 [Payload V1 格式](docs/format-v1.md)，新生成应用统一写入 V2。

## 安全边界

本项目用于打包可信的静态网页应用，不应被视为运行恶意网页内容的安全沙箱。

- 默认最多允许 100,000 个资源文件。
- 默认单文件最大 512 MiB。
- 默认总解压大小最大 2 GiB。
- Manifest 最大 1 MiB。
- HTTP 服务仅绑定 IPv4 loopback，且必须提供精确随机端口 Host。
- SHA-256 可以检测损坏或修改，但不能验证发布者身份；数字签名属于后续规划。

## 项目结构

```text
src/app/       Win32 GUI、CLI 和程序入口
src/webview/   WebView2 宿主
src/packer/    Manifest、Payload 和打包器
src/runtime/   ZIP 资源读取、LRU 和本地 HTTP 服务
src/pe/        图标和版本资源更新
src/common/    文件、路径和 SHA-256 工具
tests/         单元测试与打包/读取集成测试
```

## CI 与发布

CI 配置位于 [.github/workflows/build.yml](.github/workflows/build.yml)，执行以下流程：

1. 使用 VS2022 配置 Windows x64 Release。
2. 编译 `lw.Web2App.exe`。
3. 运行全部 CTest 测试。
4. 构建 `wechat-article-formatter` 的 Vite 生产产物。
5. 将测试项目打包为 EXE，并用 `inspect` 校验生成载荷。
6. 生成分发目录和 `SHA256SUMS.txt`。
7. 压缩为 `lw.Web2App-windows-x64.zip`。
8. 上传 GitHub Actions Artifact。
9. 对 `v*` 标签创建 GitHub Release 并上传 ZIP 与校验文件。

## 第三方依赖

- Microsoft WebView2 SDK：Microsoft 软件许可
- miniz：MIT License
- cpp-httplib：MIT License
- nlohmann/json：MIT License
- spdlog（含 bundled fmt）：MIT License

具体信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 跨平台规划

当前为 V2 架构的早期版本，已经具备本地/URL 打包、GUI、CLI、资源与配置完整性、按需读取、SPA fallback 和 PE 元数据能力。

下一阶段将优先支持 Linux，而不是只把 Windows 代码加上条件编译。计划按以下边界演进：

1. 将 Payload、Manifest、ZIP、SHA-256、路径安全、资源服务和 LRU 缓存整理为无 Win32 依赖的跨平台核心。
2. 保留 Windows 的 Win32 + WebView2 + PE Resource 实现；Linux 增加原生窗口和系统 WebView 适配层，优先评估 GTK + WebKitGTK。
3. 为 Linux 增加 ELF Runner 和应用元数据处理；首批计划提供 x86_64 的 `.tar.gz`，并评估 AppImage。`.deb`、`.rpm` 和 ARM64 将在基础运行链路稳定后再决定。
4. 增加 Linux CI，覆盖编译、单元测试、静态目录打包、Payload 校验和最小启动冒烟测试。
5. 保持 `LWWEB001` V1 容器的解析兼容；新应用使用 `LWWEB002` V2，后续只有平台确实需要时才扩展版本。

Linux 支持完成前，README、Release 和仓库描述都应继续明确标注“当前仅支持 Windows”，不把规划当作已经交付的能力。

其他后续方向：

- 多分辨率图标生成
- Authenticode 代码签名
- 单实例、托盘和窗口置顶
- 文件下载与外部链接策略
- JS 与 C++ 双向 IPC
- 自定义 User-Agent、启动参数和 CSP

## 联系与支持

- 作者：天天代码码天天
- QQ：819069052
- QQ Group：C# 人工智能实践 | 群号：758616458

如果项目对你有帮助，可以扫码支持维护：

<img src="docs/assets/sponsor.jpg" alt="微信赞助二维码" width="260">

## 许可证

本项目采用 [MIT License](LICENSE)。
