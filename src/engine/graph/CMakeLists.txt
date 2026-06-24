#-----------------------------------------------------------------------------
# seq24 Windows port — per-track audio graph module.
#
#   * seq24_graph : static library (Track, MixerGraph, VuMeter).
#   * graph_test  : standalone test exe with fake IPluginInstance plugins.
#
# This CMakeLists is self-contained so the module can be built on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/graph_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(seq24_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- static library ---------------------------------------------------------
add_library(seq24_graph STATIC
    track.cpp
    mixer_graph.cpp
)

# Headers (vu_meter.h is header-only) live alongside the sources; the parent
# engine dir is on the include path so "../plugin_api.h" resolves.
target_include_directories(seq24_graph PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_options(seq24_graph PRIVATE -Wall -Wextra)

# --- test executable --------------------------------------------------------
add_executable(graph_test graph_test.cpp)
target_link_libraries(graph_test PRIVATE seq24_graph)
target_compile_options(graph_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME graph_test COMMAND graph_test)
