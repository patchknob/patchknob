cmake_minimum_required(VERSION 3.15)
project(PatchKnob_root CXX)

# ---------------------------------------------------------------------------
# PatchKnob 0.8.7 - SDL2 multi-platform build.
#
# Build with the MSYS2 MINGW64 toolchain:
#
#   export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build -j
#
# On Windows the SDL target stages its MSYS runtime DLL closure next to PatchKnob.exe.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_subdirectory(sdlui)

if(WIN32)
    add_custom_target(nsis_installer
        COMMAND powershell -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_nsis.ps1"
                -ProjectRoot "${CMAKE_CURRENT_SOURCE_DIR}"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -SkipBuild
        DEPENDS PatchKnob
        VERBATIM)
endif()
