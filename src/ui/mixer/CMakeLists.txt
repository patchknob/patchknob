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

# C++17: the live test compiles the engine graph sources (authored for C++17)
# directly into the test executable.
set( CMAKE_CXX_STANDARD 17 )
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

# Standalone UI-only test / demo executable (dummy animated data).
add_executable( mixer_test mixer_test.cpp )
target_link_libraries( mixer_test PRIVATE seq24mixer )

# -----------------------------------------------------------------------------
# Live test: binds the mixer UI to a REAL engine::MixerGraph via
# seq24::mixer::show_mixer() (src/mixerapp.cpp) and drives it with a fake
# render loop so the strips show live, moving VU meters.
#
# It compiles the engine graph sources + mixerapp.cpp straight into the exe so
# it builds both standalone (cmake -S src/ui/mixer) and in-tree, without needing
# the top-level target graph.
# -----------------------------------------------------------------------------
set( SEQ24_SRC_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../.." )
set( SEQ24_GRAPH_DIR "${SEQ24_SRC_DIR}/engine/graph" )

add_executable( mixer_live_test
    mixer_live_test.cpp
    "${SEQ24_SRC_DIR}/mixerapp.cpp"
    "${SEQ24_GRAPH_DIR}/track.cpp"
    "${SEQ24_GRAPH_DIR}/mixer_graph.cpp"
)
target_include_directories( mixer_live_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # mixer widget headers ("mixer_window.h")
    ${SEQ24_SRC_DIR}                # src root ("mixerapp.h", "engine/...", "ui/...")
    ${GTKMM_INCLUDE_DIRS}
)
target_compile_options( mixer_live_test PRIVATE ${GTKMM_CFLAGS_OTHER} )
target_link_libraries( mixer_live_test PRIVATE seq24mixer )
