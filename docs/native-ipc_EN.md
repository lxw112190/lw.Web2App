# Native IPC Guide

[中文](native-ipc.md) | [Back to the English README](../README_EN.md)

Native IPC lets trusted static pages packaged by lw.Web2App call a small set of
permission-scoped native features through `window.lw.invoke()`. It is disabled by
default, works only in local package mode, and is unavailable in URL mode.

## Quick start

Package the repository's [simple example](../examples/native-ipc/index.html):

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

## Permissions

Enabling IPC does not grant every method. Capabilities are declared at package time:

| Capability | Methods | Purpose |
| --- | --- | --- |
| `app.info` | `app.getInfo` | Read app ID, title, platform, architecture, and Runtime version |
| `dialog.directory` | `dialog.selectDirectory` | Open the directory picker and create a Session Grant |
| `dialog.file` | `dialog.openFile`, `file.revoke` | Select files and manage read-only File Grants |
| `fs.exists` | `fs.exists` | Check an authorized path |
| `fs.list` | `fs.list` | List direct children of an authorized directory |
| `fs.copy` | `fs.copy` | Copy a regular file |
| `fs.move` | `fs.move` | Move a regular file or a same-filesystem directory |
| `fs.delete` | `fs.delete` | Delete an authorized path |

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

## Method reference

```js
const info = await lw.invoke("app.getInfo");
// { appId, title, platform: "windows" | "linux", arch: "x64", version }

const selected = await lw.invoke("dialog.selectDirectory");
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

`fs.copy` handles regular files only. `fs.move` first uses native rename. A regular file
crossing a disk or filesystem boundary falls back to “copy to a destination-side
temporary file, publish the complete destination, then delete the source.” Existing
destinations require `overwrite: true`. If source deletion fails, the complete target
is retained and `IO_ERROR` is returned. Cross-filesystem directory moves return
`UNSUPPORTED`. `fs.delete` is non-recursive unless `recursive: true` is specified.

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

Large files should not cross JSON IPC as Base64. `dialog.openFile` creates a random,
process-local grant and returns a same-origin HTTP streaming URL:

```js
const file = opened.files[0];
// { id, name, size, mime, url: "/__lw_file__/<opaque-id>/document.pdf" }
const loadingTask = pdfjsLib.getDocument({ url: file.url });
await lw.invoke("file.revoke", { id: file.id });
```

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
