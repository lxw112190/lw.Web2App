# Desktop Integration（桌面集成）

lw.Web2App 的 Desktop Integration 是一层受权限约束的 Native IPC 能力，用于让本地网页安全地使用桌面运行时能力。它不是任意 Shell、注册表或环境变量接口，也不会改变现有文件授权边界。

## 启用

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --ipc --ipc-capability app.info --ipc-capability app.paths
```

```js
const cache = await window.lw.invoke("app.getPath", { name: "appCache" });
console.log(cache.path);
```

## Native → Web 事件

事件使用统一协议：

```json
{ "v": 1, "kind": "event", "event": "fs.changed",
  "data": { "watcherId": "watch_1" } }
```

网页监听和移除监听：

```js
function onChanged(data) { console.log("changed", data); }
window.lw.on("fs.changed", onChanged);
window.lw.off("fs.changed", onChanged);
```

事件是尽力而为通知，不会进入请求/响应 pending 表。事件名最多 128 个字节，单个序列化事件最多 256 KiB；无效或超限事件会被丢弃。网页重新加载期间发生的事件也可能丢失，业务应在页面恢复后主动重新查询状态。

## `app.getPath`

能力：`app.paths`。支持 `home`、`desktop`、`documents`、`pictures`、`downloads`、`appData`、`appCache` 和 `temp`。

`appData` 和 `appCache` 按应用 ID 隔离。路径解析本身不会授予网页文件读写权限；`fs.*` 仍然必须匹配 Manifest 固定根目录或本次会话的目录授权。

Windows 使用 Known Folder；Linux 优先读取 XDG 环境变量和 `user-dirs.dirs`。

更多请求/响应接口请参阅 [Native IPC 指南](native-ipc.md)。

## 单实例行为

每个生成应用按 `app_id` 使用独立的单实例锁。Windows 第二次启动会通知第一个进程，
由第一个进程显示并聚焦已有窗口；Linux 第二次启动会直接退出。该机制与本地 HTTP
端口无关，也不会把网页参数传给已有进程。

## `fs.watch`

能力：`fs.watch`。只能监听已经通过固定根目录或 Session Grant 授权的目录：

```js
const result = await window.lw.invoke("fs.watch", {
  path: "/授权目录",
  recursive: true,
  debounceMs: 150
});
window.lw.on("fs.changed", console.log);
// 停止监听：
await window.lw.invoke("fs.unwatch", { watcherId: result.watcherId });
```

事件中的 `relativePath` 始终相对于监听根目录并使用 `/` 分隔符。监听是变更提示而不是完整审计日志；
超过 10,000 个快照项或 512 个变更时会设置 `overflow: true`，网页应重新扫描目录。每个 Runtime 最多 32 个监听器。

## `window.control`

能力：`window.control`。窗口控制始终作用于当前 Runtime 窗口：

```js
const state = await window.lw.invoke("window.getState");
await window.lw.invoke("window.minimize");
await window.lw.invoke("window.restore");
await window.lw.invoke("window.setAlwaysOnTop", { enabled: true });
```

`window.setCloseBehavior` 支持 `exit`（默认）和 `hide`。设置为 `hide` 后点击窗口
关闭按钮会隐藏窗口而不是退出进程；应用应同时提供自己的“显示/退出”入口，避免
用户无法找回隐藏窗口。

## `app.lifecycle`

能力：`app.lifecycle`。`app.quit` 会先返回 `{ quitting: true }`，再由 Runtime
消息循环执行退出和资源清理：

```js
await window.lw.invoke("app.quit");
```

如果 Runtime 没有注入退出服务，接口会返回 `UNSUPPORTED`；在线 URL 模式始终不能启用该能力。

## `tray`（Windows）

能力：`tray`。Windows Runtime 使用生成应用自身的图标创建托盘菜单；菜单最多 32 项，
支持普通菜单项和分隔线，点击后通过 `tray.click`/`tray.menu` 事件通知网页：

```js
await window.lw.invoke("tray.create", {
  tooltip: "我的应用",
  menu: [{ id: "open", label: "打开" }, { type: "separator" },
         { id: "exit", label: "退出" }]
});
window.lw.on("tray.menu", async ({ id }) => {
  if (id === "open") await window.lw.invoke("window.focus");
  if (id === "exit") await window.lw.invoke("app.quit");
});
```

`tray.update` 可更新提示文字和菜单，`tray.destroy` 移除图标。Linux 当前返回
`UNSUPPORTED`，不会引入额外托盘库依赖。

可运行示例：[examples/desktop-integration](../examples/desktop-integration/index.html)。
