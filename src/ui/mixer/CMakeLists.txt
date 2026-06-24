#-----------------------------------------------------------------------------
#
#  CMake build for the seq24 Windows port mixer UI module.
#
#  Builds a standalone test executable `mixer_test` linked against gtkmm-2.4.
#  The mixer sources (vu_widget / channel_strip / mixer_window) are compiled
#  into a small static library `seq24mixer` so the engine can link them later
#  without pulling in mixer_test.cpp.
#
#  Build (MSYS2 / mingw64):
#      export PATH="/c/msys64/mingw64/bin:$PATH"
#      cmake -G "MinGW Makefiles" -S . -B build
#      cmake --build build
#      ./build/mixer_test.exe
#
#-----------------------------------------------------------------------------

cmake_minimum_required( VERSION 3.10 )
project( seq24_mixer CXX )

set( CMAKE_CXX_STANDARD 11 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

# Locate gtkmm-2.4 via pkg-config.
find_package( PkgConfig REQUIRED )
pkg_check_modules( GTKMM REQUIRED gtkmm-2.4 )

# Reusable mixer widgets (engine links this).
add_library( seq24mixer STATIC
    vu_widget.cpp
    channel_strip.cpp
    mixer_window.cpp
)
target_include_directories( seq24mixer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GTKMM_INCLUDE_DIRS}
)
target_compile_options( seq24mixer PUBLIC ${GTKMM_CFLAGS_OTHER} )
target_link_libraries( seq24mixer PUBLIC ${GTKMM_LIBRARIES} )
target_link_directories( seq24mixer PUBLIC ${GTKMM_LIBRARY_DIRS} )

# Standalone test / demo executable.
add_executable( mixer_test mixer_test.cpp )
target_link_libraries( mixer_test PRIVATE seq24mixer )
