#-----------------------------------------------------------------------------
#
#  CMake build for the seq24 Windows port modular node-editor / patchbay UI.
#
#  The patchbay UI is HEADER-ONLY:
#      patch_view_model.h   engine-free description of nodes / ports / conns
#      patch_canvas.h       the Gtk::DrawingArea node-editor canvas
#  so the engine-coupled coordinator (src/patchbayapp.cpp) is compiled straight
#  into seq24 by the top-level src/*.cpp glob -- no static library to wire into
#  the frozen top-level CMakeLists.
#
#  This file only builds the standalone demo/test executable `patchbay_test`,
#  which drives the canvas with a hard-coded 4-node graph (instant; no engine)
#  and links gtkmm-2.4 plus the runtime palette (src/palette.cpp) so the
#  black & white theme colours resolve.
#
#  Build (MSYS2 / mingw64):
#      export PATH="/c/msys64/mingw64/bin:$PATH"
#      cmake -G "MinGW Makefiles" -S . -B build
#      cmake --build build
#      ./build/patchbay_test.exe
#
#-----------------------------------------------------------------------------

cmake_minimum_required( VERSION 3.10 )
project( seq24_patchbay CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

# Locate gtkmm-2.4 via pkg-config.
find_package( PkgConfig REQUIRED )
pkg_check_modules( GTKMM REQUIRED gtkmm-2.4 )

# src root, so "ui/palette.h" (included as "../palette.h") and palette.cpp's
# own #include "ui/palette.h" both resolve.
set( SEQ24_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." )

# Standalone test / demo executable (header-only canvas -> single UI TU, plus
# the palette definitions it reads at draw time).
add_executable( patchbay_test
    patchbay_test.cpp
    "${SEQ24_SRC_DIR}/palette.cpp"
)

target_include_directories( patchbay_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # patch_canvas.h / patch_view_model.h
    ${SEQ24_SRC_DIR}                # src root ("ui/palette.h")
    ${GTKMM_INCLUDE_DIRS}
)
target_compile_options( patchbay_test PRIVATE ${GTKMM_CFLAGS_OTHER} )
target_link_directories( patchbay_test PRIVATE ${GTKMM_LIBRARY_DIRS} )
target_link_libraries( patchbay_test PRIVATE ${GTKMM_LIBRARIES} )
