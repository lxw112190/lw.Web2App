# lw.Web2App — Visual Studio 2022 完整源码包

此目录是可直接复制给其他开发者的离线 VS2022 解决方案，工程和依赖路径全部相对于解决方案目录，不包含当前电脑的绝对路径。

## 环境要求

- Windows 10 或 Windows 11 x64
- Visual Studio 2022
- 安装工作负载：**使用 C++ 的桌面开发**
- 安装 MSVC v143 与任意 Windows 10/11 SDK

无需安装 CMake、Ninja、vcpkg、Node.js，也无需联网恢复 C++ 依赖。

## 编译

1. 双击 `lw.Web2App.sln`。
2. 工具栏选择 `Release` 和 `x64`。
3. 右键 `lw.Web2App`，选择“设为启动项目”。
4. 选择“生成 → 生成解决方案”。
5. 产物位于 `bin\Release\lw.Web2App.exe`。

也可以双击 `build-release.cmd` 执行完整 Release 编译与测试。

## 解决方案项目

- `lw.Web2App`：Windows GUI 打包器与 Runtime。
- `lwweb_core`：打包、Payload、日志、资源服务和 PE 资源模块。
- `lwweb_tests`：Payload、安全路径、日志轮转及原子发布回归测试。

## 运行要求

编译器本身不需要额外 DLL。运行 `lw.Web2App.exe` 或它生成的应用时，目标电脑需要 Microsoft Edge WebView2 Runtime；Windows 10/11 通常已经安装。

## 内置第三方依赖

`deps` 目录包含固定版本的 nlohmann/json、cpp-httplib、miniz、spdlog 和 Microsoft WebView2 SDK。许可证位于各依赖目录以及 `THIRD_PARTY_NOTICES.md`。
