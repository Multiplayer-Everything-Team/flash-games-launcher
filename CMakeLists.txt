cmake_minimum_required(VERSION 3.20)
project(MultiplayerFlashLauncher VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the launcher is a single self-contained .exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LAUNCHER_NAME "FlashLauncher" CACHE STRING "Output name of the launcher executable")
set(LAUNCHER_ICON "${CMAKE_SOURCE_DIR}/assets/app.ico" CACHE FILEPATH "Executable icon")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc /wd4100)
endif()

# ---- core: no UI, shared by the launcher and the tests --------------------
add_library(launcher_core STATIC
  src/core/config.cpp
  src/core/fs.cpp
  src/core/install.cpp
  src/core/json.cpp
  src/core/library.cpp
  src/core/log.cpp
  src/core/process.cpp
  src/core/relay.cpp
  src/core/rooms.cpp
  src/core/settings.cpp
  src/core/strings.cpp
  src/core/vars.cpp
)
target_include_directories(launcher_core PUBLIC src)
target_link_libraries(launcher_core PUBLIC bcrypt shell32 ole32 uuid advapi32 user32 ws2_32 iphlpapi winhttp)

# ---- launcher executable --------------------------------------------------
set(APP_RC "${CMAKE_BINARY_DIR}/app.rc")
if(EXISTS "${LAUNCHER_ICON}")
  file(TO_CMAKE_PATH "${LAUNCHER_ICON}" ICON_PATH)
  file(WRITE "${APP_RC}" "1 ICON \"${ICON_PATH}\"\n")
else()
  file(WRITE "${APP_RC}" "// no icon\n")
endif()
# Version shown in the exe's Properties > Details, and passed to the code.
set(V "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
file(APPEND "${APP_RC}" "1 VERSIONINFO\nFILEVERSION ${V}\nPRODUCTVERSION ${V}\nBEGIN\n BLOCK \"StringFileInfo\"\n BEGIN\n  BLOCK \"040904B0\"\n  BEGIN\n   VALUE \"CompanyName\", \"Multiplayer Everything\"\n   VALUE \"FileDescription\", \"Multiplayer Everything Flash Launcher\"\n   VALUE \"FileVersion\", \"${PROJECT_VERSION}\"\n   VALUE \"ProductName\", \"Multiplayer Everything Flash Launcher\"\n   VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"\n   VALUE \"OriginalFilename\", \"${LAUNCHER_NAME}.exe\"\n  END\n END\n BLOCK \"VarFileInfo\"\n BEGIN\n  VALUE \"Translation\", 0x409, 1200\n END\nEND\n")

add_executable(launcher WIN32
  src/app/main.cpp
  src/app/cli.cpp
  src/ui/theme.cpp
  src/ui/launcher.cpp
  src/ui/settings.cpp
  src/app/app.manifest
  "${APP_RC}"
)
set_target_properties(launcher PROPERTIES OUTPUT_NAME "${LAUNCHER_NAME}")
target_compile_definitions(launcher PRIVATE LAUNCHER_VERSION=L"${PROJECT_VERSION}")
target_link_libraries(launcher PRIVATE launcher_core gdiplus comctl32 comdlg32 uxtheme dwmapi gdi32)

# ---- tests ----------------------------------------------------------------
enable_testing()
add_executable(launcher_tests tests/test_main.cpp tests/test_core.cpp tests/test_relay.cpp)
target_link_libraries(launcher_tests PRIVATE launcher_core shell32)
add_test(NAME core_and_relay COMMAND launcher_tests)
add_test(NAME end_to_end
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/e2e.ps1"
          -Launcher "$<TARGET_FILE:launcher>" -Root "${CMAKE_SOURCE_DIR}")
set_tests_properties(end_to_end PROPERTIES LABELS "ui" RUN_SERIAL TRUE)

# Run straight from the build folder: the launcher reads launcher.json next to the exe.
add_custom_command(TARGET launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/launcher/launcher.json" "$<TARGET_FILE_DIR:launcher>/launcher.json")
