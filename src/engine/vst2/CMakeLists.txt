#-----------------------------------------------------------------------------
# PatchKnob — VST2 host module.
#
# Builds:
#   * PatchKnob_vst2  — static library implementing PatchKnob::engine::IPluginInstance
#                   for VST2 plugins (Vst2PluginInstance + createVst2Instance).
#   * vst2_test   — standalone test exe that loads a 64-bit VST2 instrument,
#                   renders a note, and asserts non-silent output.
#
# Toolchain: mingw64 g++ (C++17). Links only against Win32 (kernel32/user32),
# which are linked by default — no extra libraries required.
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.15)
project(PatchKnob_vst2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(PatchKnob_vst2 STATIC
    vst2_host.cpp
    seh_guard.cpp
)
target_include_directories(PatchKnob_vst2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(PatchKnob_vst2 PRIVATE -Wall -Wextra)

add_executable(vst2_test
    vst2_test.cpp
)
target_link_libraries(vst2_test PRIVATE PatchKnob_vst2)
target_compile_options(vst2_test PRIVATE -Wall -Wextra)
