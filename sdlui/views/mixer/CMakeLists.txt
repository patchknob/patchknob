cmake_minimum_required(VERSION 3.15)
project(PatchKnob_mixer CXX)

# ---------------------------------------------------------------------------
# Stand-alone test build for the SDL2 MixerView.
#
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/views/mixer -B sdlui/views/mixer/build -G "MinGW Makefiles"
#   cmake --build sdlui/views/mixer/build -j
#   ./sdlui/views/mixer/build/mixer_test
#
# Compiles: mixer_view.cpp + the SDL toolkit (gui.cpp) + a test main, and links
# the REAL engine graph model (PatchKnob_graph = Track / MixerGraph / VuMeter) so
# the view drives a live MixerGraph. No alternate UI toolkit, no audio device.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC   ${SDLUI}/../src)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

# real engine model (Track / MixerGraph / VuMeter)
add_subdirectory(${SRC}/engine/graph ${CMAKE_CURRENT_BINARY_DIR}/e_graph EXCLUDE_FROM_ALL)

add_executable(mixer_test
    mixer_test.cpp
    mixer_view.cpp
    ${SDLUI}/gui.cpp)

target_include_directories(mixer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDLUI}
    ${SRC}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})

target_compile_definitions(mixer_test PRIVATE __WINDOWS_MM__)
target_compile_options(mixer_test PRIVATE -Wall)
target_link_directories(mixer_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(mixer_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    PatchKnob_graph
    winmm pthread)
