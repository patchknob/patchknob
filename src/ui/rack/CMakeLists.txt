#-----------------------------------------------------------------------------
#
#  CMake build for the seq24 Windows port VST "rack" UI module.
#
#  The rack UI (plugin browser + hosted-plugin editor) is HEADER-ONLY:
#      plugin_browser.h   pure-UI, engine-free plugin list (searchable)
#      plugin_editor.h    native editor window w/ HWND embed + idle pump
#  so the engine-coupled coordinator (src/rackapp.cpp) is compiled straight into
#  seq24 by the top-level glob -- no static library to wire into the frozen
#  top-level CMakeLists.
#
#  This file only builds the standalone demo/test executable `rack_test`, which
#  drives the browser with a hard-coded descriptor list (instant; no real scan)
#  and links nothing but gtkmm-2.4.
#
#  Build (MSYS2 / mingw64):
#      export PATH="/c/msys64/mingw64/bin:$PATH"
#      cmake -G "MinGW Makefiles" -S . -B build
#      cmake --build build
#      ./build/rack_test.exe
#
#-----------------------------------------------------------------------------

cmake_minimum_required( VERSION 3.10 )
project( seq24_rack CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

# Locate gtkmm-2.4 via pkg-config.
find_package( PkgConfig REQUIRED )
pkg_check_modules( GTKMM REQUIRED gtkmm-2.4 )

# Standalone test / demo executable (header-only rack UI -> single TU).
add_executable( rack_test rack_test.cpp )

target_include_directories( rack_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GTKMM_INCLUDE_DIRS}
)
target_compile_options( rack_test PRIVATE ${GTKMM_CFLAGS_OTHER} )
target_link_directories( rack_test PRIVATE ${GTKMM_LIBRARY_DIRS} )
target_link_libraries( rack_test PRIVATE ${GTKMM_LIBRARIES} )
