#-----------------------------------------------------------------------------
# seq24 Windows port — modular patch-graph engine module.
#
#   * seq24_patch : static library (PatchGraph + built-in nodes).
#   * patch_test  : standalone self-test exe (no real plugins required).
#
# Self-contained so the module can be built on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/patch_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(seq24_patch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- static library ---------------------------------------------------------
add_library(seq24_patch STATIC
    patch_graph.cpp
    patch_nodes.cpp
)

# The parent engine dir is on the include path so "../plugin_api.h",
# "../graph/track.h" and "../graph/vu_meter.h" resolve. track.h / vu_meter.h are
# header-only from this module's perspective (RenderContext + VuMeter), so no
# link dependency on seq24_graph is required.
target_include_directories(seq24_patch PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../graph
)

target_compile_options(seq24_patch PRIVATE -Wall -Wextra)

# --- test executable --------------------------------------------------------
add_executable(patch_test patch_test.cpp)
target_link_libraries(patch_test PRIVATE seq24_patch)
target_compile_options(patch_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME patch_test COMMAND patch_test)
