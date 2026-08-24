# Native IPC 使用指南

[English](native-ipc_EN.md) | [返回中文 README](../README.md)

Native IPC 让打包进 lw.Web2App 的可信静态页面，通过统一的
`window.lw.invoke()` API 调用少量、受权限约束的本地能力。它默认关闭，仅适用于
本地打包模式，不支持在线 URL 模式。

## 快速开始

打包仓库内的[简单示例](../examples/native-ipc/index.html)：

```powershell
lw.Web2App.exe pack .\examples\native-ipc .\native-ipc.exe `
  --title "Native IPC 示例" --windowed --ipc `
  --ipc-capability app.info `
  --ipc-capability dialog.directory `
  --ipc-capability dialog.file `
  --ipc-capability fs.exists `
  --ipc-capability fs.list `
  --ipc-capability fs.copy `
  --ipc-capability fs.move `
  --ipc-capability fs.delete
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

## 权限配置

启用 IPC 不等于授予所有能力。每一类方法都必须在打包时单独声明：

| Capability | 方法 | 用途 |
| --- | --- | --- |
| `app.info` | `app.getInfo` | 获取应用 ID、标题、平台、架构和 Runtime 版本 |
| `dialog.directory` | `dialog.selectDirectory` | 打开系统目录选择器，并创建会话目录授权 |
| `dialog.file` | `dialog.openFile`、`file.revoke` | 选择本地文件并管理只读 File Grant |
| `fs.exists` | `fs.exists` | 检查授权路径是否存在 |
| `fs.list` | `fs.list` | 列出授权目录的直接子项 |
| `fs.copy` | `fs.copy` | 复制普通文件 |
| `fs.move` | `fs.move` | 移动普通文件或同文件系统目录 |
| `fs.delete` | `fs.delete` | 删除授权路径 |

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

`fs.delete` 默认不递归；删除非空目录必须显式传入 `recursive: true`。所有路径都会在
每次调用时重新校验，源路径和目标路径都必须位于固定根目录或 Session Grant 内。

## 受控本地文件桥

大型 PDF、视频、图片和音频不应经过 JSON/Base64 IPC。`dialog.openFile` 只负责显示
系统窗口并创建当前进程有效的随机授权，文件内容由同源 localhost HTTP 数据面流式
提供：

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
