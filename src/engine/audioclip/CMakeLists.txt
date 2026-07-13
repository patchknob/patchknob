#-----------------------------------------------------------------------------
# seq24 Windows port — audio-clip module (built-in audio-track "instrument").
#
#   * seq24_audioclip : static library (AudioClip, wav_loader, AudioClipPlayer).
#   * audioclip_test  : standalone self-test (synth clips + schedule + record).
#
# Self-contained so the module builds on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/audioclip_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(seq24_audioclip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- static library ---------------------------------------------------------
add_library(seq24_audioclip STATIC
    wav_loader.cpp
    audio_clip_player.cpp
)

# audio_clip.h is header-only. The parent engine dir is on the include path so
# "../plugin_api.h" resolves, matching the other engine modules.
target_include_directories(seq24_audioclip PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_options(seq24_audioclip PRIVATE -Wall -Wextra)

# --- test executable --------------------------------------------------------
add_executable(audioclip_test audioclip_test.cpp)
target_link_libraries(audioclip_test PRIVATE seq24_audioclip)
target_compile_options(audioclip_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME audioclip_test COMMAND audioclip_test)
