# Native IPC Guide

[中文](native-ipc.md) | [Back to the English README](../README_EN.md)

Native IPC lets trusted static pages packaged by lw.Web2App call a small set of
permission-scoped native features through `window.lw.invoke()`. It is disabled by
default, works only in local package mode, and is unavailable in URL mode.

## Quick start

In the Windows GUI, select **Local static directory**, enable **Local interaction
(Native IPC)**, and open **Configure IPC permissions...**. The permissions window
provides read-only file viewing, folder browsing, and full file management presets,
plus individual capability checkboxes and optional fixed roots. Enabling the GUI
option only writes the permission policy; the page must still call
`window.lw.invoke()` to use it.

You can also package the repository's [simple example](../examples/native-ipc/index.html)
with the CLI:

```powershell
lw.Web2App.exe pack .\examples\native-ipc .\native-ipc.exe `
  --title "Native IPC Example" --windowed --ipc `
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

No additional JavaScript file is required:

```html
<button id="choose">Choose a file</button>
<pre id="result"></pre>
<script>
  const output = document.querySelector("#result");
  document.querySelector("#choose").onclick = async () => {
    try {
      const result = await window.lw.invoke("dialog.openFile", {
        multiple: false,
        filters: [{ name: "Images", extensions: ["png", "jpg", "webp"] }]
      });
      output.textContent = JSON.stringify(result.files[0], null, 2);
    } catch (error) {
      output.textContent = `${error.code}: ${error.message}`;
    }
  };
</script>
```

`window.lw.invoke(method, params)` returns a Promise. Rejections contain a stable
`code` and a human-readable `message`.

See [Desktop Integration](desktop-integration_EN.md) for the event channel and `app.getPath`.

## Permissions

Enabling IPC does not grant every method. Capabilities are declared at package time:

| Capability | Methods | Purpose |
| --- | --- | --- |
| `app.info` | `app.getInfo` | Read app ID, title, platform, architecture, and Runtime version |
| `app.paths` | `app.getPath` | Resolve supported system directories; this does not grant file access |
| `dialog.directory` | `dialog.selectDirectory` | Open the directory picker and create a Session Grant |
| `dialog.file` | `dialog.openFile`, `file.revoke` | Select files and manage read-only File Grants |
| `fs.exists` | `fs.exists` | Check an authorized path |
| `fs.list` | `fs.list` | List direct children with size, MIME, and modification time |
| `fs.read` | `fs.openRead`, `file.revoke` | Create a read-only HTTP File Grant for an authorized regular file |
| `fs.mkdir` | `fs.mkdir` | Create one direct child directory inside an authorized directory |
| `fs.copy` | `fs.copy` | Copy a regular file |
| `fs.move` | `fs.move` | Move a regular file or a same-filesystem directory |
| `fs.trash` | `fs.trash` | Move an authorized regular file to the system Trash/Recycle Bin |
| `fs.delete` | `fs.delete` | Permanently delete an authorized path (high risk) |
| `fs.watch` | `fs.watch`, `fs.unwatch` | Watch an authorized directory for change hints |
| `window.control` | `window.getState`, `window.show`, `window.hide`, `window.minimize`, `window.maximize`, `window.restore`, `window.focus`, `window.setAlwaysOnTop`, `window.setCloseBehavior` | Control the current application window |
| `app.lifecycle` | `app.quit` | Request a safe Runtime shutdown |
| `tray` | `tray.create`, `tray.update`, `tray.destroy` | Windows system tray icon and menu (not available on Linux yet) |

Use repeatable `--ipc-root` options for fixed filesystem roots:

```powershell
lw.Web2App.exe pack .\dist .\out\MyApp.exe --ipc `
  --ipc-capability fs.list `
  --ipc-root '${DOCUMENTS}' `
  --ipc-root '${APP_DATA}'
```

Supported placeholders are `${HOME}`, `${DESKTOP}`, `${DOCUMENTS}`, `${PICTURES}`,
`${DOWNLOADS}`, `${APP_DATA}`, and `${APP_CACHE}`. Absolute paths are also accepted.
A directory selected with the system picker is granted only to the current Runtime.
Configured filesystem roots do not need to exist when the application starts. The Runtime
keeps both the declared lexical boundary and the resolved policy boundary, so a root created
later becomes usable without restarting. If a pending root is redirected outside that
boundary through a symbolic link, junction, or another reparse mechanism, access still
returns `PERMISSION_DENIED`. `fs.exists` returns `{ "exists": false }` for a missing
authorized root or child, while `fs.watch` still requires a real directory.

## Method reference

```js
const info = await lw.invoke("app.getInfo");
// { appId, title, platform: "windows" | "linux", arch: "x64", version }

const selected = await lw.invoke("dialog.selectDirectory");
const state = await lw.invoke("fs.exists", { path: selected.path + "/a.txt" });
const listing = await lw.invoke("fs.list", { path: selected.path });
const preview = await lw.invoke("fs.openRead", {
  path: selected.path + "/photo.jpg"
});
document.querySelector("img").src = preview.url;

await lw.invoke("fs.mkdir", { path: selected.path + "/selected" });

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
  path: selected.path + "/rejected.jpg"
});

await lw.invoke("fs.delete", {
  path: selected.path + "/archive/a.txt",
  recursive: false
});
```

`fs.copy` handles regular files only. `fs.move` first uses native rename. A regular file
crossing a disk or filesystem boundary falls back to “copy to a destination-side
temporary file, publish the complete destination, then delete the source.” Existing
destinations require `overwrite: true`. If source deletion fails, the complete target
is retained and `IO_ERROR` is returned. Cross-filesystem directory moves return
`UNSUPPORTED`. `fs.mkdir` creates one child whose parent already exists. `fs.trash`
accepts regular files only and never silently falls back to permanent deletion when the
desktop Trash is unavailable. `fs.delete` is non-recursive unless `recursive: true` is
specified.

File dialog options and their stable result shape are:

```js
const opened = await lw.invoke("dialog.openFile", {
  multiple: true,
  filters: [
    { name: "PDF", extensions: ["pdf"] },
    { name: "All files", extensions: ["*"] }
  ]
});
// { files: [{ id, name, size, mime, url }, ...] }
```

Up to 16 filter groups and 32 extensions per group are accepted. `"*"` means all
files, and canceling a picker returns `USER_CANCELLED`.

## Controlled Local File Bridge

Large files should not cross JSON IPC as Base64. `dialog.openFile` grants a directly
selected file, while `fs.openRead` grants a regular file below an authorized directory.
Both return a random, process-local, same-origin HTTP streaming URL:

```js
const file = opened.files[0];
// { id, name, size, mime, url: "/__lw_file__/<opaque-id>/document.pdf" }
const loadingTask = pdfjsLib.getDocument({ url: file.url });
await lw.invoke("file.revoke", { id: file.id });
```

`fs.openRead` returns the same `{ id, name, size, mime, url }` shape. `file.revoke` is
available when either `dialog.file` or `fs.read` is granted.

The page never receives the disk path. `/__lw_file__/` accepts `GET` and `HEAD`, one
fixed, open-ended, or suffix byte range, and returns the appropriate `200`, `206`,
`404`, `405`, or `416`. A 64 KiB buffer keeps memory use independent of file size.
Grants can be revoked and always disappear when Runtime exits.

## Security and protocol

- Runtime requires the exact current `127.0.0.1` app-port origin and blocks external top-level navigation while IPC is enabled.
- Linux IPC transport also uses a random per-process token.
- Capabilities, roots, Session Grants, and File Grants are enforced in native code.
- Paths are canonicalized on every call; symlinks and Windows reparse points cannot escape a grant. Windows device paths, UNC paths, and alternate data streams are rejected.
- INFO logs never include method parameters, user paths, filenames, or complete grant tokens.

Enable IPC only for packaged pages you control. Protocol `lw-ipc-v1` limits messages to
1 MiB, IDs and methods to 128 bytes, and pending requests to 64 per page. Stable errors
are `INVALID_REQUEST`, `INVALID_ARGUMENT`, `METHOD_NOT_FOUND`, `PERMISSION_DENIED`,
`USER_CANCELLED`, `NOT_FOUND`, `ALREADY_EXISTS`, `IO_ERROR`, `UNSUPPORTED`, `BUSY`, and
`INTERNAL_ERROR`.

See [`examples/native-ipc/index.html`](../examples/native-ipc/index.html) for the full
runnable example.
