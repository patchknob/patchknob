#-----------------------------------------------------------------------------
# PatchKnob — audio-clip module (built-in audio-track "instrument").
#
#   * PatchKnob_audioclip : static library (AudioClip, wav_loader, AudioClipPlayer).
#   * audioclip_test  : standalone self-test (synth clips + schedule + record).
#
# Self-contained so the module builds on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/audioclip_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_audioclip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Vendored MIT time-stretch (signalsmith-stretch, header-only, zero deps).
set(SIGNALSMITH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/signalsmith)

# --- static library ---------------------------------------------------------
add_library(PatchKnob_audioclip STATIC
    wav_loader.cpp
    audio_clip_player.cpp
    warp_stretch.cpp
)

# audio_clip.h is header-only. The parent engine dir is on the include path so
# "../plugin_api.h" resolves, matching the other engine modules.
target_include_directories(PatchKnob_audioclip PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${SIGNALSMITH_DIR}
)

target_compile_options(PatchKnob_audioclip PRIVATE -Wall -Wextra)
# signalsmith is much slower unoptimised; keep the stretch TU optimised.
set_source_files_properties(warp_stretch.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# --- test executable --------------------------------------------------------
add_executable(audioclip_test audioclip_test.cpp)
target_link_libraries(audioclip_test PRIVATE PatchKnob_audioclip)
target_compile_options(audioclip_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME audioclip_test COMMAND audioclip_test)
