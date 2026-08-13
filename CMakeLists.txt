cmake_minimum_required(VERSION 3.24)

project(lw.Web2App VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR
    "lw.Web2App currently supports Windows 10/11 only; Linux support is planned")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "lw.Web2App v1 supports x64 builds only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

if(EXISTS "${LWWEB_DEPS_CACHE}/cpp-httplib.tar.gz")
  set(LWWEB_HTTPLIB_URL "${LWWEB_DEPS_CACHE}/cpp-httplib.tar.gz")
else()
  set(LWWEB_HTTPLIB_URL "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.30.0.tar.gz")
endif()
FetchContent_Declare(cpp_httplib
  URL "${LWWEB_HTTPLIB_URL}"
  URL_HASH SHA256=135adc029364a70d0c1769f60bef8a2cdc7915e2525eb3ec759e2084af7a7e7b)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
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
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

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

add_library(lwweb_core STATIC
  src/common/file_utils.cpp
  src/common/logging.cpp
  src/common/path_utils.cpp
  src/common/sha256.cpp
  src/packer/manifest.cpp
  src/packer/payload.cpp
  src/packer/packer.cpp
  src/pe/pe_resources.cpp
  src/runtime/resource_cache.cpp
  src/runtime/resource_server.cpp)
target_include_directories(lwweb_core PUBLIC include PRIVATE ${miniz_SOURCE_DIR})
target_link_libraries(lwweb_core PUBLIC
  nlohmann_json::nlohmann_json
  httplib::httplib
  miniz
  spdlog::spdlog
  bcrypt
  ws2_32
  version)
target_compile_definitions(lwweb_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  SPDLOG_WCHAR_FILENAMES _WIN32_WINNT=0x0A00)
target_compile_options(lwweb_core PRIVATE /W4 /permissive- /utf-8 /EHsc)

add_executable(lwweb WIN32
  src/app/main.cpp
  src/app/main_window.cpp
  src/webview/webview_host.cpp
  resources/lwweb.rc)
target_include_directories(lwweb PRIVATE include ${WEBVIEW2_INCLUDE})
target_link_libraries(lwweb PRIVATE
  lwweb_core
  "${WEBVIEW2_LOADER}"
  comctl32
  comdlg32
  dwmapi
  ole32
  shell32
  shlwapi
  uxtheme
  windowscodecs
  user32
  gdi32)
target_compile_definitions(lwweb PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_compile_options(lwweb PRIVATE /W4 /permissive- /utf-8 /EHsc)
set_target_properties(lwweb PROPERTIES OUTPUT_NAME "lw.Web2App")

include(CTest)
if(BUILD_TESTING)
  add_executable(lwweb_tests
    tests/test_main.cpp
    tests/payload_tests.cpp
    tests/path_tests.cpp)
  target_include_directories(lwweb_tests PRIVATE include)
  target_link_libraries(lwweb_tests PRIVATE lwweb_core)
  target_compile_definitions(lwweb_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(lwweb_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  add_test(NAME lwweb_tests COMMAND lwweb_tests)
endif()
