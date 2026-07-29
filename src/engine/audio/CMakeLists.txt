#-----------------------------------------------------------------------------
# PatchKnob — real-time audio engine module.
#
# Builds:
#   * PatchKnob_audio  : static library wrapping PortAudio as AudioEngine.
#   * audio_test   : headless tests (throwing callback, FTZ/DAZ, callback-swap
#                    stress, device-loss flag) + 440 Hz sine device smoke test.
#
# Standalone build (MinGW Makefiles, MSYS2 mingw64):
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -G "MinGW Makefiles" -S . -B build
#   cmake --build build
#   ./build/audio_test.exe
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_audio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Locate the vendored PortAudio (built static: vendor/portaudio/build/libportaudio.a).
# PortAudio gives the multi-backend driver model the DAW needs: ASIO / WASAPI /
# WDM-KS / DirectSound / MME on Windows, ALSA / JACK / Pulse on Linux.
set(PORTAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/portaudio"
    CACHE PATH "Path to vendored PortAudio")

if(NOT EXISTS "${PORTAUDIO_DIR}/build/libportaudio.a")
    message(FATAL_ERROR "PortAudio static lib not found at ${PORTAUDIO_DIR}/build/libportaudio.a. "
                        "Build it: cmake -S ${PORTAUDIO_DIR} -B ${PORTAUDIO_DIR}/build "
                        "-G \"MinGW Makefiles\" -DBUILD_SHARED_LIBS=OFF && cmake --build ${PORTAUDIO_DIR}/build")
endif()

# Path to the shared engine contract (plugin_api.h), so consumers of the lib
# can include it through the same include path.
set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

#-----------------------------------------------------------------------------
# Static library: PatchKnob_audio (= AudioEngine + PortAudio multi-backend)
#-----------------------------------------------------------------------------
add_library(PatchKnob_audio STATIC audio_engine.cpp)

target_include_directories(PatchKnob_audio
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"       # audio_engine.h
        "${PORTAUDIO_DIR}/include"          # portaudio.h
        "${ENGINE_DIR}"                     # plugin_api.h (shared contract)
)

# Static PortAudio + the Windows backend link deps (WASAPI / WDM-KS / DSound /
# MME need these system libs; PortAudio has no external runtime dependency).
target_link_libraries(PatchKnob_audio PUBLIC "${PORTAUDIO_DIR}/build/libportaudio.a")
if(WIN32)
    target_link_libraries(PatchKnob_audio
        PUBLIC winmm ole32 uuid ksuser setupapi mfplat mfuuid wmcodecdspuuid)
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(PatchKnob_audio PUBLIC Threads::Threads asound jack pulse pulse-simple dl m)
elseif(APPLE)
    target_link_libraries(PatchKnob_audio
        PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreServices")
endif()

#-----------------------------------------------------------------------------
# Test executable: audio_test
#-----------------------------------------------------------------------------
add_executable(audio_test audio_test.cpp)
target_link_libraries(audio_test PRIVATE PatchKnob_audio)
