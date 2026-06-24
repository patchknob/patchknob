#-----------------------------------------------------------------------------
# seq24 Windows port — real-time audio engine module.
#
# Builds:
#   * seq24_audio  : static library wrapping RtAudio (WASAPI) as AudioEngine.
#   * audio_test   : smoke-test exe (440 Hz sine, ~1.5 s, prints diagnostics).
#
# Standalone build (MinGW Makefiles, MSYS2 mingw64):
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -G "MinGW Makefiles" -S . -B build
#   cmake --build build
#   ./build/audio_test.exe
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(seq24_audio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Locate the vendored RtAudio (this module lives at src/engine/audio/).
set(RTAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/rtaudio"
    CACHE PATH "Path to vendored RtAudio")

if(NOT EXISTS "${RTAUDIO_DIR}/RtAudio.cpp")
    message(FATAL_ERROR "RtAudio not found at ${RTAUDIO_DIR}. Set -DRTAUDIO_DIR=...")
endif()

# Path to the shared engine contract (plugin_api.h), so consumers of the lib
# can include it through the same include path.
set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

#-----------------------------------------------------------------------------
# Static library: seq24_audio (= AudioEngine + RtAudio WASAPI backend)
#-----------------------------------------------------------------------------
add_library(seq24_audio STATIC
    audio_engine.cpp
    "${RTAUDIO_DIR}/RtAudio.cpp"
)

# Select the WASAPI backend (verified primary on this machine).
target_compile_definitions(seq24_audio PRIVATE __WINDOWS_WASAPI__)

target_include_directories(seq24_audio
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"   # audio_engine.h
        "${RTAUDIO_DIR}"                # RtAudio.h
        "${ENGINE_DIR}"                 # plugin_api.h (shared contract)
)

# WASAPI / Media Foundation link dependencies (per vendor/rtaudio/INTEGRATION.md).
target_link_libraries(seq24_audio
    PUBLIC
        ole32 winmm ksuser mfplat mfuuid wmcodecdspuuid
)

#-----------------------------------------------------------------------------
# Test executable: audio_test
#-----------------------------------------------------------------------------
add_executable(audio_test audio_test.cpp)
target_link_libraries(audio_test PRIVATE seq24_audio)
