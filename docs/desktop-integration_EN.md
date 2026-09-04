# Desktop Integration

Desktop Integration is lw.Web2App's permission-scoped Native IPC layer. It lets a local web page use selected desktop runtime features without exposing arbitrary shell commands, registry access, environment access, or a general-purpose proxy.

## Enable it

```powershell
lw.Web2App.exe pack .\dist .\MyApp.exe `
  --ipc --ipc-capability app.info --ipc-capability app.paths
```

```js
const cache = await window.lw.invoke("app.getPath", { name: "appCache" });
console.log(cache.path);
```

## Native → Web events

Events use one envelope on Windows and Linux:

```json
{ "v": 1, "kind": "event", "event": "fs.changed",
  "data": { "watcherId": "watch_1" } }
```

```js
function onChanged(data) { console.log("changed", data); }
window.lw.on("fs.changed", onChanged);
window.lw.off("fs.changed", onChanged);
```

Delivery is best effort. Event names are limited to 128 bytes and a serialized event is limited to 256 KiB. Events can be lost while the page is reloading, so applications should re-query state after recovery.

## `app.getPath`

Capability: `app.paths`. Supported names are `home`, `desktop`, `documents`, `pictures`, `downloads`, `appData`, `appCache`, and `temp`.

Application data and cache paths are isolated by app ID. Resolving a path does not grant file access: `fs.*` still requires a Manifest filesystem root or a session directory grant. Windows uses Known Folder APIs; Linux uses XDG variables and `user-dirs.dirs` when available.

See the [Native IPC guide](native-ipc_EN.md) for request/response methods.

## Single-instance behavior

Each generated app has an `app_id`-scoped single-instance lock. On Windows, a second
launch notifies the first process, which shows and focuses its existing window. On Linux,
the second launch exits quietly. This is independent of the local HTTP port and does not
forward web-page arguments to the existing process.

## `fs.watch`

Capability: `fs.watch`. Only directories covered by a fixed root or Session Grant can be watched:

```js
const result = await window.lw.invoke("fs.watch", {
  path: "/authorized/directory", recursive: true, debounceMs: 150
});
window.lw.on("fs.changed", console.log);
await window.lw.invoke("fs.unwatch", { watcherId: result.watcherId });
```

`relativePath` is relative to the watched root and always uses `/`. Watching is a change hint,
not an audit log. Snapshots are bounded at 10,000 entries and batches at 512 changes; overflow
requires a full rescan. A Runtime allows at most 32 watchers.

## `window.control`

Capability: `window.control`. Window methods always target the current Runtime window:

```js
const state = await window.lw.invoke("window.getState");
await window.lw.invoke("window.minimize");
await window.lw.invoke("window.restore");
await window.lw.invoke("window.setAlwaysOnTop", { enabled: true });
```

`window.setCloseBehavior` accepts `exit` (the default) or `hide`. With `hide`, clicking
the close button hides the window instead of exiting; applications should provide their
own show/exit action so users can recover a hidden window.

## `app.lifecycle`

Capability: `app.lifecycle`. `app.quit` first returns `{ quitting: true }`, then the
Runtime message loop performs shutdown and resource cleanup:

```js
await window.lw.invoke("app.quit");
```

If the Runtime has no quit service the method returns `UNSUPPORTED`; URL mode can never
enable this capability.

## `tray` (Windows)

Capability: `tray`. Windows uses the generated application's own icon for a tray menu.
Menus allow up to 32 items, including regular entries and separators; clicks are emitted
as `tray.click`/`tray.menu` events:

```js
await window.lw.invoke("tray.create", {
  tooltip: "My app",
  menu: [{ id: "open", label: "Open" }, { type: "separator" },
         { id: "exit", label: "Exit" }]
});
window.lw.on("tray.menu", async ({ id }) => {
  if (id === "open") await window.lw.invoke("window.focus");
  if (id === "exit") await window.lw.invoke("app.quit");
});
```

Use `tray.update` to replace tooltip/menu data and `tray.destroy` to remove the icon.
Linux currently returns `UNSUPPORTED` and does not add another tray-library dependency.

Runnable example: [examples/desktop-integration](../examples/desktop-integration/index.html).
