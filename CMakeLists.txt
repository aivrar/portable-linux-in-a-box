cmake_minimum_required(VERSION 3.20)
project(linux_template C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
option(ENABLE_WHPX "Build WHPX backend (requires Windows SDK)" ON)

# --------------------------------------------------------------------------
# Core library sources
# --------------------------------------------------------------------------
set(CORE_SOURCES
    src/linux/detect.c
    src/linux/stub_backend.c
    src/bridge/pipe_bridge.c
    src/bridge/http_bridge.c
    src/bridge/fs_bridge.c
    src/bridge/service.c
    src/bridge/provision.c
    src/gui/webview_gui.c
)

if(WIN32)
    list(APPEND CORE_SOURCES
        src/linux/wsl_backend.c
        src/linux/qemu_backend.c
        src/linux/tinyemu_backend.c
    )
    if(ENABLE_WHPX)
        include(CheckIncludeFiles)
        check_include_files("windows.h;WinHvPlatform.h" HAVE_WHPX_HEADER)
        if(HAVE_WHPX_HEADER)
            list(APPEND CORE_SOURCES src/linux/whpx_backend.c)
        else()
            message(STATUS "WHPX: header not found, disabling")
        endif()
    endif()
elseif(APPLE)
    list(APPEND CORE_SOURCES src/linux/qemu_backend_posix.c)
elseif(UNIX)
    list(APPEND CORE_SOURCES
        src/linux/native_backend.c
        src/linux/qemu_backend_posix.c
    )
endif()

# --------------------------------------------------------------------------
# WebView (auto-fetched)
# --------------------------------------------------------------------------
FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview
    GIT_TAG        0.12.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(webview)

# --------------------------------------------------------------------------
# Core library
# --------------------------------------------------------------------------
add_library(linux_core STATIC ${CORE_SOURCES})
target_include_directories(linux_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(linux_core PUBLIC webview::core_static)
target_compile_definitions(linux_core PUBLIC HAVE_WEBVIEW)

if(WIN32 AND ENABLE_WHPX AND HAVE_WHPX_HEADER)
    target_compile_definitions(linux_core PUBLIC HAVE_WHPX)
endif()

if(WIN32)
    target_compile_definitions(linux_core PUBLIC
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(linux_core PUBLIC winhttp ws2_32)
elseif(APPLE)
    target_compile_options(linux_core PUBLIC -Wall -Wextra)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(WEBKIT_FRAMEWORK WebKit)
    target_link_libraries(linux_core PUBLIC pthread ${COCOA_FRAMEWORK} ${WEBKIT_FRAMEWORK})
elseif(UNIX)
    target_compile_options(linux_core PUBLIC -Wall -Wextra -Wpedantic)
    target_link_libraries(linux_core PUBLIC pthread)
endif()

# --------------------------------------------------------------------------
# The one executable
# --------------------------------------------------------------------------
if(WIN32)
    add_executable(linux_template WIN32 src/main.c)
else()
    add_executable(linux_template src/main.c)
endif()
target_link_libraries(linux_template PRIVATE linux_core)

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------
install(TARGETS linux_template RUNTIME DESTINATION bin)

# --------------------------------------------------------------------------
# Info
# --------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== linux-template ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
if(WIN32)
    message(STATUS "WHPX:     ${ENABLE_WHPX} (header: ${HAVE_WHPX_HEADER})")
elseif(APPLE)
    message(STATUS "Backend:  QEMU (via Homebrew)")
    message(STATUS "GUI:      WebKit (Cocoa)")
endif()
message(STATUS "======================")
message(STATUS "")
