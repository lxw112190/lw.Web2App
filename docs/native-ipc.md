# Native IPC 使用指南

[English](native-ipc_EN.md) | [返回中文 README](../README.md)

Native IPC 让打包进 lw.Web2App 的可信静态页面，通过统一的
`window.lw.invoke()` API 调用少量、受权限约束的本地能力。它默认关闭，仅适用于
本地打包模式，不支持在线 URL 模式。

## 快速开始

使用 Windows 图形界面时，先选择“本地静态目录”，再勾选“启用本地交互
（Native IPC）”并点击“配置 IPC 权限…”。权限窗口提供“只读文件查看”、
“文件夹浏览”和“完整文件管理”三个预设，也可以逐项授权；固定授权目录是可选项。
启用界面选项只负责写入权限配置，网页仍需调用 `window.lw.invoke()` 才会使用这些能力。

也可以通过 CLI 打包仓库内的[简单示例](../examples/native-ipc/index.html)：

```powershell
lw.Web2App.exe pack .\examples\native-ipc .\native-ipc.exe `
  --title "Native IPC 示例" --windowed --ipc `
  --ipc-capability app.info `
  --ipc-capability dialog.directory `
  --ipc-capability dialog.file `
  --ipc-capability fs.exists `
  --ipc-capability fs.list `
  --ipc-capability fs.read `
  --ipc-capability fs.mkdir `
  --ipc-capability fs.copy `
  --ipc-capability fs.move `
  --ipc-capability fs.trash `
  --ipc-capability fs.delete `
  --ipc-capability window.control `
  --ipc-capability app.lifecycle `
  --ipc-capability tray
```

网页端不需要引入额外 JavaScript 文件：

```html
<button id="choose">选择文件</button>
<pre id="result"></pre>
<script>
  const output = document.querySelector("#result");
  document.querySelector("#choose").onclick = async () => {
    try {
      const result = await window.lw.invoke("dialog.openFile", {
        multiple: false,
        filters: [{ name: "图片", extensions: ["png", "jpg", "webp"] }]
      });
      output.textContent = JSON.stringify(result.files[0], null, 2);
    } catch (error) {
      output.textContent = `${error.code}: ${error.message}`;
    }
  };
</script>
```

`window.lw.invoke(method, params)` 返回 Promise。Native 操作失败时 Promise 会被拒绝，
错误对象包含稳定的 `code` 和便于阅读的 `message`。

事件通道和 `app.getPath` 的设计说明请参阅 [Desktop Integration](desktop-integration.md)。

## 权限配置

启用 IPC 不等于授予所有能力。每一类方法都必须在打包时单独声明：

| Capability | 方法 | 用途 |
| --- | --- | --- |
| `app.info` | `app.getInfo` | 获取应用 ID、标题、平台、架构和 Runtime 版本 |
| `app.paths` | `app.getPath` | 解析受支持的系统目录（不会自动授予文件权限） |
| `dialog.directory` | `dialog.selectDirectory` | 打开系统目录选择器，并创建会话目录授权 |
| `dialog.file` | `dialog.openFile`、`file.revoke` | 选择本地文件并管理只读 File Grant |
| `fs.exists` | `fs.exists` | 检查授权路径是否存在 |
| `fs.list` | `fs.list` | 列出授权目录的直接子项及大小、MIME、修改时间 |
| `fs.read` | `fs.openRead`、`file.revoke` | 为授权普通文件创建只读 HTTP File Grant |
| `fs.mkdir` | `fs.mkdir` | 在授权目录中创建直接子目录 |
| `fs.copy` | `fs.copy` | 复制普通文件 |
| `fs.move` | `fs.move` | 移动普通文件或同文件系统目录 |
| `fs.trash` | `fs.trash` | 将授权普通文件移入系统回收站/Trash |
| `fs.delete` | `fs.delete` | 永久删除授权路径（高风险） |
| `fs.watch` | `fs.watch`、`fs.unwatch` | 监听授权目录的变化提示 |
| `window.control` | `window.getState`、`window.show`、`window.hide`、`window.minimize`、`window.maximize`、`window.restore`、`window.focus`、`window.setAlwaysOnTop`、`window.setCloseBehavior` | 控制当前应用窗口 |
| `app.lifecycle` | `app.quit` | 请求 Runtime 安全退出 |
| `tray` | `tray.create`、`tray.update`、`tray.destroy` | Windows 系统托盘图标和菜单（Linux 暂不支持） |

固定文件系统根目录使用可重复的 `--ipc-root` 指定：

```powershell
lw.Web2App.exe pack .\dist .\out\MyApp.exe --ipc `
  --ipc-capability fs.list `
  --ipc-root '${DOCUMENTS}' `
  --ipc-root '${APP_DATA}'
```

支持 `${HOME}`、`${DESKTOP}`、`${DOCUMENTS}`、`${PICTURES}`、`${DOWNLOADS}`、
`${APP_DATA}` 和 `${APP_CACHE}`。也可以使用绝对路径。通过系统目录选择器得到的
Session Grant 仅在本次 Runtime 进程内有效，应用退出后自动失效。

Manifest 中配置的固定根目录无需在应用启动时已经存在。Runtime 会保留声明的词法和
真实路径边界，目录之后被创建即可自动使用，无需重启应用；但如果 pending 根目录后来
通过符号链接、Junction 或其他重解析机制指向边界之外，访问仍会返回
`PERMISSION_DENIED`。不存在的授权根或其子路径调用 `fs.exists` 会返回
`{ "exists": false }`，而 `fs.watch` 仍要求目录已经真实存在。

## 方法参考

### 应用和对话框

```js
const info = await lw.invoke("app.getInfo");
// { appId, title, platform: "windows" | "linux", arch: "x64", version }

const selected = await lw.invoke("dialog.selectDirectory");
// { path: "用户选择的绝对路径" }

const opened = await lw.invoke("dialog.openFile", {
  multiple: true,
  filters: [
    { name: "PDF", extensions: ["pdf"] },
    { name: "所有文件", extensions: ["*"] }
  ]
});
// { files: [{ id, name, size, mime, url }, ...] }
```

`filters` 最多 16 组，每组最多 32 个扩展名；扩展名可以带或不带点，`"*"`
表示所有文件。取消系统对话框会返回 `USER_CANCELLED`。

### 文件系统

```js
const state = await lw.invoke("fs.exists", { path: selected.path + "/a.txt" });
const listing = await lw.invoke("fs.list", { path: selected.path });

const preview = await lw.invoke("fs.openRead", {
  path: selected.path + "/photo.jpg"
});
document.querySelector("img").src = preview.url;

await lw.invoke("fs.mkdir", { path: selected.path + "/已选" });

await lw.invoke("fs.copy", {
  from: selected.path + "/a.txt",
  to: selected.path + "/a-copy.txt",
  overwrite: false
});

await lw.invoke("fs.move", {
  from: selected.path + "/a-copy.txt",
  to: selected.path + "/archive/a.txt",
  overwrite: true
});

await lw.invoke("fs.trash", {
  path: selected.path + "/废片.jpg"
});

await lw.invoke("fs.delete", {
  path: selected.path + "/archive/a.txt",
  recursive: false
});
```

`fs.copy` 只复制普通文件，不递归复制目录。`fs.move` 优先使用系统原生重命名；
普通文件跨磁盘或跨文件系统时，会自动回退为“目标目录临时复制 → 发布完整目标 →
删除源文件”。只有显式传入 `overwrite: true` 才会替换已有目标。如果最后删除源文件
失败，完整目标会被保留并返回 `IO_ERROR`，不会为了回滚而删除已经发布的数据。
跨文件系统移动目录暂不支持，并返回 `UNSUPPORTED`。

`fs.mkdir` 只创建父目录已经存在的直接子目录。`fs.trash` 仅接受普通文件，在 Windows
进入回收站，在 Linux 使用桌面环境的 Trash；系统不支持回收站时返回 `UNSUPPORTED`，
不会静默改成永久删除。`fs.delete` 默认不递归；删除非空目录必须显式传入
`recursive: true`。所有路径都会在
每次调用时重新校验，源路径和目标路径都必须位于固定根目录或 Session Grant 内。

## 受控本地文件桥

大型 PDF、视频、图片和音频不应经过 JSON/Base64 IPC。`dialog.openFile` 可为用户直接
选择的文件创建授权；`fs.openRead` 可为 `dialog.selectDirectory` 已授权目录中的普通
文件创建授权。文件内容都由同源 localhost HTTP 数据面流式提供：

```js
const result = await lw.invoke("dialog.openFile", {
  multiple: false,
  filters: [{ name: "PDF", extensions: ["pdf"] }]
});

const file = result.files[0];
// { id, name, size, mime, url: "/__lw_file__/<opaque-id>/document.pdf" }
const loadingTask = pdfjsLib.getDocument({ url: file.url });
await lw.invoke("file.revoke", { id: file.id });
```

`fs.openRead` 返回相同的 `{ id, name, size, mime, url }` 结构，适合图片目录、PDF
资料库和媒体列表。`file.revoke` 可由 `dialog.file` 或 `fs.read` 任一能力授权。

网页不会得到真实磁盘路径，URL 中的显示文件名也不参与磁盘定位。`/__lw_file__/`
只接受 `GET` 和 `HEAD`，支持单个固定、开放结束或 suffix Range，并返回正确的
`200`、`206`、`404`、`405` 或 `416`。64 KiB 流式缓冲使内存占用与文件大小无关；
多区间请求暂时返回 `416`。File Grant 可主动撤销，并会在 Runtime 退出时全部失效。

## 安全模型

- IPC 默认关闭，且 URL 模式无法启用。
- Runtime 只接受来自当前 `127.0.0.1` 应用端口精确 Origin 的消息；启用 IPC 时会阻止顶层页面跳转到外部 Origin。
- Linux IPC 传输还绑定每进程随机令牌，避免跨源 iframe 借用消息处理器。
- Capability、固定根目录、Session Grant 和 File Grant 都在 Native 侧强制执行，网页不能自行扩大权限。
- 现有路径按真实路径规范化，新目标按真实父目录规范化；符号链接和 Windows 重解析点不能用于逃逸授权根目录。
- Windows 设备路径、UNC 路径和 ADS 被拒绝。
- INFO 日志只记录方法名、结果码和安全拒绝，不记录参数、用户路径、文件名或完整授权 Token。

只应为打包进应用且由你控制的页面开启 Native IPC。不要给可被外部内容替换或注入脚本的页面授予本地文件能力。

## 协议限制与错误码

当前协议为 `lw-ipc-v1`。消息最大 1 MiB，ID 和方法名最大 128 字节，同一页面最多
保留 64 个待处理请求，重复 ID 返回 `BUSY`。

稳定错误码包括：`INVALID_REQUEST`、`INVALID_ARGUMENT`、`METHOD_NOT_FOUND`、
`PERMISSION_DENIED`、`USER_CANCELLED`、`NOT_FOUND`、`ALREADY_EXISTS`、
`IO_ERROR`、`UNSUPPORTED`、`BUSY` 和 `INTERNAL_ERROR`。

完整可运行页面见 [`examples/native-ipc/index.html`](../examples/native-ipc/index.html)。
