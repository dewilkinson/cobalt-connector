# CobaltConnector.exe - the Cobalt Connector app.
# Native Win32, C++20, MSVC, static CRT: nothing to install beside it but the embedded Python it supervises.
#
# The version is written HERE only. CMake turns it into version.h (for the code and for app.rc), and
# build.ps1 reads it from this line to pass to the installer.
cmake_minimum_required(VERSION 3.21)
project(CobaltConnector VERSION 1.2.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "CobaltConnector is built with MSVC (Visual Studio 2026, MSVC 14.51)")
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

add_executable(CobaltConnector WIN32
  src/main.cpp
  src/util.cpp
  src/keys.cpp
  src/check_key.cpp
  src/migrate.cpp
  src/supervisor.cpp
  src/update.cpp
  src/ui.cpp
  src/json.cpp
  res/app.rc
)
target_include_directories(CobaltConnector PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(CobaltConnector PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(CobaltConnector PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
target_link_options(CobaltConnector PRIVATE /MANIFEST:NO)
target_link_libraries(CobaltConnector PRIVATE
  winhttp crypt32 shell32 comctl32 advapi32 user32 gdi32 ole32 bcrypt shlwapi)
