#-----------------------------------------------------------------------------
# PatchKnob — VST2 host module.
#
# Builds:
#   * PatchKnob_vst2  — static library implementing PatchKnob::engine::IPluginInstance
#                   for VST2 plugins (Vst2PluginInstance + createVst2Instance).
#                   Loads Windows .dll VST2 plugins on Windows (LoadLibrary)
#                   and Linux/macOS .so/.dylib VST2 plugins on those platforms
#                   (dlopen) -- same ABI, OS-appropriate loader either way;
#                   see the pk_dlopen/pk_dlsym/pk_dlclose wrappers at the top
#                   of vst2_host.cpp. seh_guard(_posix).cpp contains the crash
#                   containment (VEH+RtlCaptureContext on Windows,
#                   sigaction+sigsetjmp on Linux/macOS) that keeps a faulting
#                   plugin from taking the whole app down.
#   * vst2_test   — standalone Windows test exe that loads a 64-bit VST2
#                   instrument, renders a note, and asserts non-silent output.
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.15)
project(PatchKnob_vst2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
    add_library(PatchKnob_vst2 STATIC vst2_host.cpp seh_guard.cpp)
else()
    add_library(PatchKnob_vst2 STATIC vst2_host.cpp seh_guard_posix.cpp)
endif()
target_include_directories(PatchKnob_vst2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(PatchKnob_vst2 PRIVATE -Wall -Wextra)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(PatchKnob_vst2 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    add_executable(vst2_test vst2_test.cpp)
    target_link_libraries(vst2_test PRIVATE PatchKnob_vst2)
    target_compile_options(vst2_test PRIVATE -Wall -Wextra)
endif()
