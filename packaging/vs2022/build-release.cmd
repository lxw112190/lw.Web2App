@echo off
setlocal
rem Normalize duplicate PATH/Path entries that some automation terminals inject.
set "LWWEB_ORIGINAL_PATH=%PATH%"
set "PATH="
set "Path="
set "PATH=%LWWEB_ORIGINAL_PATH%"
set "LWWEB_ORIGINAL_PATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] Visual Studio Installer vswhere.exe was not found.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo [ERROR] Visual Studio 2022 C++ desktop tools were not found.
  exit /b 1
)
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b %errorlevel%
msbuild "%~dp0lw.Web2App.sln" /m /t:Build /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b %errorlevel%
"%~dp0bin\Release\lwweb_tests.exe"
if errorlevel 1 exit /b %errorlevel%
echo.
echo Build and tests completed successfully.
echo Output: %~dp0bin\Release\lw.Web2App.exe
endlocal
