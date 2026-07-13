cmake_minimum_required(VERSION 3.15)
project(seq24sdl CXX)

# ---------------------------------------------------------------------------
# seq24 SDL2 frontend -- links the gtkmm-free engine core + the engine module
# libs, so the new UI drives the real sequencer / VST host / audio graph.
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui -B sdlui/build -G "MinGW Makefiles"
#   cmake --build sdlui/build -j
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC   ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(VEND  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor)
set(RTMIDI_DIR ${VEND}/rtmidi)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

# --- engine module libs (the audio/VST path used by audio_app) --------------
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst2   ${CMAKE_CURRENT_BINARY_DIR}/e_vst2   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst3   ${CMAKE_CURRENT_BINARY_DIR}/e_vst3   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_autom EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip  ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)

# --- gtkmm-free engine core sources -----------------------------------------
set(CORE_SRC
    ${SRC}/perform.cpp ${SRC}/sequence.cpp ${SRC}/event.cpp
    ${SRC}/midibus.cpp ${SRC}/midifile.cpp ${SRC}/mutex.cpp
    ${SRC}/audio_app.cpp ${SRC}/optionsfile.cpp ${SRC}/configfile.cpp
    ${SRC}/userfile.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
)

set(VIEW_SRC
    views/piano_roll/pianoroll.cpp
    views/tracker/tracker_view.cpp
    views/arrange/arrange_view.cpp
    views/mixer/mixer_view.cpp
    views/patchbay/patch_view.cpp
    views/browser/browser_view.cpp
    views/automation/automation_view.cpp
    views/waveform/waveform_view.cpp
    views/waveform/audio_track.cpp
    project_io.cpp
)

add_executable(seq24sdl main.cpp gui.cpp engine_globals.cpp ${VIEW_SRC} ${CORE_SRC})

target_include_directories(seq24sdl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_compile_definitions(seq24sdl PRIVATE __WINDOWS_MM__)
target_compile_options(seq24sdl PRIVATE -Wall)
target_link_directories(seq24sdl PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(seq24sdl PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    seq24_pluginhost seq24_vst2 seq24_vst3 seq24_graph seq24_audio
    seq24_automation seq24_audioclip
    winmm pthread)
