cmake_minimum_required(VERSION 3.15)
project(seq24win CXX)

# ---------------------------------------------------------------------------
# seq24 0.8.7 — Windows (MINGW64) port.
#
# Build with the MSYS2 MINGW64 toolchain (g++ 15.x) and the mingw64 gtkmm-2.4.
# From an MSYS2 MINGW64 shell, or with the mingw64 bin dir on PATH:
#
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S . -B build -G "Ninja"            # or "MinGW Makefiles"
#   cmake --build build -j
#
# The resulting seq24.exe needs the mingw64 GTK runtime DLLs on PATH to run
# (i.e. /c/msys64/mingw64/bin), or copy them next to the exe for distribution.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# --- gtkmm-2.4 via pkg-config ----------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-2.4)

# --- sources ---------------------------------------------------------------
# Everything in src/*.cpp EXCEPT dump.cpp (a standalone ALSA debug util with
# its own main()).  midibus.cpp is the new ALSA-free Windows stub backend.
file(GLOB SEQ24_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER SEQ24_SOURCES EXCLUDE REGEX "/dump\\.cpp$")

add_executable(seq24 ${SEQ24_SOURCES})

target_include_directories(seq24 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GTKMM_INCLUDE_DIRS}
)

target_compile_options(seq24 PRIVATE ${GTKMM_CFLAGS_OTHER} -Wall)

# HAVE_CONFIG_H is not used; src/config.h is included directly by the sources.
target_link_directories(seq24 PRIVATE ${GTKMM_LIBRARY_DIRS})
target_link_libraries(seq24 PRIVATE
    ${GTKMM_LIBRARIES}
    winmm          # reserved for the Phase 1 RtMidi (WinMM) backend
    pthread        # winpthreads (perform/mutex use pthreads)
)

# Console output is handy during the port; switch to WIN32 (no console) later.
# set_target_properties(seq24 PROPERTIES WIN32_EXECUTABLE TRUE)
