cmake_minimum_required(VERSION 3.22)

project(lw.Web2App VERSION 0.2.3 LANGUAGES C CXX)

configure_file(resources/version.h.in generated/lwweb/version.h @ONLY)
if(WIN32)
  set(LWWEB_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/lw.Web2App.ico")
  configure_file(resources/lwweb.rc.in generated/lwweb.rc @ONLY)
  set(LWWEB_SPONSOR_RESOURCE "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets/sponsor.jpg")
  configure_file(resources/gui_resources.rc.in generated/gui_resources.rc @ONLY)
endif()

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "lw.Web2App currently supports x64 builds only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

set(LWWEB_DEPS_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.deps" CACHE PATH
    "Optional directory containing cached dependency archives")

if(EXISTS "${LWWEB_DEPS_CACHE}/json.tar.xz")
  set(LWWEB_JSON_URL "${LWWEB_DEPS_CACHE}/json.tar.xz")
else()
  set(LWWEB_JSON_URL "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz")
endif()
FetchContent_Declare(nlohmann_json
  URL "${LWWEB_JSON_URL}"
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(nlohmann_json)

if(EXISTS "${LWWEB_DEPS_CACHE}/cpp-httplib-v0.51.0.tar.gz")
  set(LWWEB_HTTPLIB_URL "${LWWEB_DEPS_CACHE}/cpp-httplib-v0.51.0.tar.gz")
else()
  set(LWWEB_HTTPLIB_URL "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.51.0.tar.gz")
endif()
FetchContent_Declare(cpp_httplib
  URL "${LWWEB_HTTPLIB_URL}"
  URL_HASH SHA256=d740ced75352f44e9d66d08806dc231b5621bb3592b5a5d5b2bd890a9a9d86cc)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

if(EXISTS "${LWWEB_DEPS_CACHE}/miniz.tar.gz")
  set(LWWEB_MINIZ_URL "${LWWEB_DEPS_CACHE}/miniz.tar.gz")
else()
  set(LWWEB_MINIZ_URL "https://github.com/richgel999/miniz/archive/refs/tags/3.1.0.tar.gz")
endif()
FetchContent_Declare(miniz
  URL "${LWWEB_MINIZ_URL}"
  URL_HASH SHA256=09569fc19d060ac9f5999ba9356728c2494ebe6a24ac0eb0a6b6ae3d396cfea6)
FetchContent_MakeAvailable(miniz)

if(EXISTS "${LWWEB_DEPS_CACHE}/spdlog.tar.gz")
  set(LWWEB_SPDLOG_URL "${LWWEB_DEPS_CACHE}/spdlog.tar.gz")
else()
  set(LWWEB_SPDLOG_URL "https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz")
endif()
FetchContent_Declare(spdlog
  URL "${LWWEB_SPDLOG_URL}"
  URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(spdlog)

if(WIN32)
  set(WEBVIEW2_VERSION "1.0.4078.44" CACHE STRING "WebView2 SDK version")
  if(EXISTS "${LWWEB_DEPS_CACHE}/webview2.zip")
    set(LWWEB_WEBVIEW2_URL "${LWWEB_DEPS_CACHE}/webview2.zip")
  else()
    set(LWWEB_WEBVIEW2_URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}")
  endif()
  FetchContent_Declare(webview2_sdk
    URL "${LWWEB_WEBVIEW2_URL}"
    DOWNLOAD_NAME webview2.zip
    URL_HASH SHA256=dc4d1d9168df26b830398303e50210b6e1729f6ce5a7ac69d2c766852f489962
    SOURCE_SUBDIR _lwweb_no_cmakelists)
  FetchContent_MakeAvailable(webview2_sdk)
  set(WEBVIEW2_ROOT "${webview2_sdk_SOURCE_DIR}")
  set(WEBVIEW2_INCLUDE "${WEBVIEW2_ROOT}/build/native/include")
  set(WEBVIEW2_LOADER "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")
  if(NOT EXISTS "${WEBVIEW2_INCLUDE}/WebView2.h" OR NOT EXISTS "${WEBVIEW2_LOADER}")
    message(FATAL_ERROR "Unexpected WebView2 NuGet package layout at ${WEBVIEW2_ROOT}")
  endif()
endif()

add_library(lwweb_core STATIC
  src/cli/command_line.cpp
  src/common/file_utils.cpp
  src/common/http_origin.cpp
  src/common/logging.cpp
  src/common/pe_version.cpp
  src/common/path_utils.cpp
  src/common/sha256.cpp
  src/ipc/filesystem_access.cpp
  src/ipc/ipc_dispatcher.cpp
  src/ipc/ipc_message.cpp
  src/ipc/ipc_permissions.cpp
  src/packer/manifest.cpp
  src/packer/payload.cpp
  src/packer/packer.cpp
  src/pe/payload_binding.cpp
  src/publish/linux_deb.cpp
  src/publish/publish_config.cpp
  src/publish/publisher.cpp
  src/publish/release_info.cpp
  src/runtime/resource_cache.cpp
  src/runtime/backend_proxy.cpp
  src/runtime/local_file_grant.cpp
  src/runtime/proxy_stream.cpp
  src/runtime/single_instance.cpp
  src/runtime/resource_server.cpp)
target_include_directories(lwweb_core PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(lwweb_core SYSTEM PRIVATE ${miniz_SOURCE_DIR})
target_link_libraries(lwweb_core PUBLIC
  nlohmann_json::nlohmann_json
  httplib::httplib
  miniz
  spdlog::spdlog)

if(WIN32)
  target_sources(lwweb_core PRIVATE src/pe/authenticode.cpp src/pe/pe_resources.cpp
    src/publish/windows_installer.cpp)
  target_link_libraries(lwweb_core PUBLIC bcrypt ws2_32 version shell32 wintrust)
  target_compile_definitions(lwweb_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    SPDLOG_WCHAR_FILENAMES _WIN32_WINNT=0x0A00)
  add_executable(lwweb WIN32
    src/app/main.cpp src/app/main_window.cpp src/webview/webview_host.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lwweb.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/gui_resources.rc")
  target_include_directories(lwweb PRIVATE include ${WEBVIEW2_INCLUDE})
  target_link_libraries(lwweb PRIVATE lwweb_core "${WEBVIEW2_LOADER}" comctl32 comdlg32
    dwmapi ole32 shell32 shlwapi uxtheme windowscodecs user32 gdi32)
  target_compile_definitions(lwweb PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _WIN32_WINNT=0x0A00)
elseif(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
  target_link_libraries(lwweb_core PUBLIC OpenSSL::Crypto Threads::Threads)
  add_executable(lwweb src/linux/main.cpp)
  target_include_directories(lwweb PRIVATE include)
  target_link_libraries(lwweb PRIVATE lwweb_core PkgConfig::GTK3 PkgConfig::WEBKIT)
else()
  message(FATAL_ERROR "lw.Web2App supports Windows and Ubuntu Linux only")
endif()

if(MSVC)
  target_compile_options(lwweb_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(lwweb PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
  target_compile_options(lwweb_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(lwweb PRIVATE -Wall -Wextra -Wpedantic)
  # miniz exposes optional zlib-compatible helpers as static functions in its
  # public header. GCC reports those helpers as unused in every translation
  # unit that includes it, even though lw.Web2App only uses the ZIP API.
  target_compile_options(miniz PRIVATE -Wno-unused-function)
endif()
set_target_properties(lwweb PROPERTIES OUTPUT_NAME "lw.Web2App")

install(TARGETS lwweb RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE)
  install(FILES resources/linux/lw.Web2App.desktop DESTINATION share/applications)
  install(FILES resources/linux/lw.Web2App.svg DESTINATION share/icons/hicolor/scalable/apps)
  install(FILES README.md README_EN.md DESTINATION share/doc/lw-web2app)
  install(FILES docs/assets/lw.Web2App.png docs/assets/lw.Web2App-linux.png
          DESTINATION share/doc/lw-web2app/docs/assets)

  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_PACKAGE_NAME "lw-web2app")
  set(CPACK_PACKAGE_VENDOR "天天代码码天天")
  set(CPACK_PACKAGE_CONTACT "QQ 819069052")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "将静态网站打包为 Windows EXE 或 Linux 应用")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
  # Let dpkg-shlibdeps resolve Ubuntu-version-specific names (for example t64
  # transitions) from the actual ELF dependencies on each CI runner.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(lwweb_tests
    tests/cli_tests.cpp
    tests/filesystem_access_tests.cpp
    tests/ipc_tests.cpp
    tests/local_file_bridge_tests.cpp
    tests/payload_binding_tests.cpp
    tests/test_main.cpp
    tests/payload_tests.cpp
    tests/path_tests.cpp
    tests/publish_config_tests.cpp
    tests/publisher_tests.cpp
    tests/resource_tests.cpp
    tests/signing_tests.cpp)
  target_include_directories(lwweb_tests PRIVATE include)
  target_include_directories(lwweb_tests SYSTEM PRIVATE ${miniz_SOURCE_DIR})
  target_link_libraries(lwweb_tests PRIVATE lwweb_core)
  if(MSVC)
    target_compile_definitions(lwweb_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(lwweb_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  else()
    target_compile_options(lwweb_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME lwweb_tests COMMAND lwweb_tests)
endif()
