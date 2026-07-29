cmake_minimum_required(VERSION 3.15)
project(PatchKnob_projectio CXX)

# ---------------------------------------------------------------------------
# Stand-alone test build for project_io (save_project / load_project).
#
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/project_io_test -B sdlui/project_io_test/build -G "MinGW Makefiles"
#   cmake --build sdlui/project_io_test/build -j
#   ./sdlui/project_io_test/build/project_io_test
#
# Compiles project_io.cpp + the SDL toolkit (gui.cpp, for the theme mode) + a
# test main, and links the real UI-neutral PatchKnob core (perform/sequence/event/
# ...) plus the engine module libs that audio_app.cpp references.  No audio
# device and no window are opened by the test.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SRC   ${SDLUI}/../src)
set(VEND  ${SDLUI}/../vendor)
set(RTMIDI_DIR ${VEND}/rtmidi)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

# engine module libs (audio_app.cpp links against these)
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst2   ${CMAKE_CURRENT_BINARY_DIR}/e_vst2   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst3   ${CMAKE_CURRENT_BINARY_DIR}/e_vst3   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/patch     ${CMAKE_CURRENT_BINARY_DIR}/e_patch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/rack      ${CMAKE_CURRENT_BINARY_DIR}/e_rack EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/kitchensink ${CMAKE_CURRENT_BINARY_DIR}/e_kitch EXCLUDE_FROM_ALL)

set(CORE_SRC
    ${SRC}/perform.cpp ${SRC}/sequence.cpp ${SRC}/event.cpp
    ${SRC}/midibus.cpp ${SRC}/midifile.cpp ${SRC}/mutex.cpp
    ${SRC}/audio_app.cpp ${SRC}/optionsfile.cpp ${SRC}/configfile.cpp
    ${SRC}/userfile.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
    ${SDLUI}/engine_globals.cpp
)

add_executable(project_io_test
    project_io_test.cpp
    ${SDLUI}/project_io.cpp
    ${SDLUI}/gui.cpp
    ${CORE_SRC})

target_include_directories(project_io_test PRIVATE
    ${SDLUI} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})

target_compile_definitions(project_io_test PRIVATE __WINDOWS_MM__)
target_compile_options(project_io_test PRIVATE -Wall)
target_link_directories(project_io_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(project_io_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    PatchKnob_pluginhost PatchKnob_vst2 PatchKnob_vst3 PatchKnob_graph PatchKnob_audio
    PatchKnob_audioclip PatchKnob_patch PatchKnob_rack PatchKnob_kitchensink
    winmm pthread)
