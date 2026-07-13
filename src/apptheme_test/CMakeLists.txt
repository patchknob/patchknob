cmake_minimum_required(VERSION 3.15)
project(apptheme_test CXX)

# ---------------------------------------------------------------------------
# Standalone eyeball test for the seq24 "ancient" black & white GTK theme.
#
# This is a SCRATCH project -- it is intentionally NOT referenced by the
# top-level seq24 CMakeLists.  Configure it on its own from an MSYS2 MINGW64
# shell (mingw64 bin on PATH):
#
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ./build/apptheme_test.exe          # auto-quits after ~6s
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-2.4)

# The test compiles the real theme + palette sources from the parent src/ dir.
add_executable(apptheme_test
    apptheme_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../apptheme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../palette.cpp
)

target_include_directories(apptheme_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..     # apptheme.h
    ${GTKMM_INCLUDE_DIRS}
)

target_compile_options(apptheme_test PRIVATE ${GTKMM_CFLAGS_OTHER} -Wall)
target_link_directories(apptheme_test PRIVATE ${GTKMM_LIBRARY_DIRS})
target_link_libraries(apptheme_test PRIVATE ${GTKMM_LIBRARIES})
