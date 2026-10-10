# CobaltWorker.exe - the Cobalt Worker app.
# Native Win32, C++20, MSVC, static CRT: nothing to install beside it but the embedded Python it supervises.
cmake_minimum_required(VERSION 3.21)
project(CobaltWorker VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "CobaltWorker is built with MSVC (Visual Studio 2026, MSVC 14.51)")
endif()

add_executable(CobaltWorker WIN32
  src/main.cpp
  src/util.cpp
  src/keys.cpp
  src/check_key.cpp
  src/supervisor.cpp
  src/ui.cpp
  src/json.cpp
  res/app.rc
)
target_compile_definitions(CobaltWorker PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
  COBALT_WORKER_VERSION="${PROJECT_VERSION}")
target_compile_options(CobaltWorker PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
target_link_options(CobaltWorker PRIVATE /MANIFEST:NO)
target_link_libraries(CobaltWorker PRIVATE
  winhttp crypt32 shell32 comctl32 advapi32 user32 gdi32 ole32 bcrypt shlwapi)
