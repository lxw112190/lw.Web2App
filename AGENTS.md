# 仓库开发约定

## Windows 本地编译

- 当前普通 PowerShell 终端不会自动加载 MSVC/Windows SDK 的头文件和库路径。
- 编译前必须先通过 Visual Studio 2022 的 `VsDevCmd.bat` 初始化 x64 开发环境；不要直接在普通 PowerShell 中运行 `cmake --build`。
- 本机开发环境路径：`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat`。
- PowerShell 执行策略会阻止 `Launch-VsDevShell.ps1`，因此优先通过 `cmd.exe` 调用上述批处理文件。
- 编译与测试命令：

  ```powershell
  $command = '"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 && cmake --build build-local-windows-ninja --config Release && ctest --test-dir build-local-windows-ninja -C Release --output-on-failure'
  cmd.exe /d /s /c $command
  ```

