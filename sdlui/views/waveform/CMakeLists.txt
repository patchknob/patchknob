cmake_minimum_required(VERSION 3.15)
project(waveform_test CXX)

# ---------------------------------------------------------------------------
# Standalone test for the SDL2 waveform view + audio-track helper.
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build -j
#   ./build/waveform_test.exe          # or pass a .wav path
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../..)      # sdlui/
set(SRC   ${SDLUI}/../src)                         # project src/

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

# engine module libs used by the audio-track helper (graph + audioclip only;
# the master graph has no audio-engine dependency)
add_subdirectory(${SRC}/engine/graph     ${CMAKE_CURRENT_BINARY_DIR}/e_graph     EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip ${CMAKE_CURRENT_BINARY_DIR}/e_audioclip EXCLUDE_FROM_ALL)

add_executable(waveform_test
    test_main.cpp
    waveform_view.cpp
    audio_track.cpp
    ${SDLUI}/gui.cpp
)

target_include_directories(waveform_test PRIVATE
    ${SDLUI} ${SRC} ${SRC}/engine
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_compile_definitions(waveform_test PRIVATE __WINDOWS_MM__)
target_compile_options(waveform_test PRIVATE -Wall)
target_link_directories(waveform_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(waveform_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    PatchKnob_audioclip PatchKnob_graph
    winmm)
