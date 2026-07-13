cmake_minimum_required(VERSION 3.15)
project(seq24sdl_automation CXX)

# ---------------------------------------------------------------------------
# Self-contained test build for the SDL AutomationView + KeyFollowPanel.
# Mirrors sdlui/CMakeLists (gtkmm-free engine core + engine module libs) and
# additionally links seq24_automation (AutomationLane/Track/Player).
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/views/automation -B sdlui/views/automation/build -G "MinGW Makefiles"
#   cmake --build sdlui/views/automation/build -j
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC   ${SDLUI}/../src)
set(VEND  ${SDLUI}/../vendor)
set(RTMIDI_DIR ${VEND}/rtmidi)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

# --- engine module libs (the audio/VST path used by audio_app) --------------
add_subdirectory(${SRC}/engine/audio      ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph      ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst2       ${CMAKE_CURRENT_BINARY_DIR}/e_vst2   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst3       ${CMAKE_CURRENT_BINARY_DIR}/e_vst3   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/host       ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_auto   EXCLUDE_FROM_ALL)

# --- gtkmm-free engine core sources -----------------------------------------
set(CORE_SRC
    ${SRC}/perform.cpp ${SRC}/sequence.cpp ${SRC}/event.cpp
    ${SRC}/midibus.cpp ${SRC}/midifile.cpp ${SRC}/mutex.cpp
    ${SRC}/audio_app.cpp ${SRC}/optionsfile.cpp ${SRC}/configfile.cpp
    ${SRC}/userfile.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
)

add_executable(automation_view_test
    test_main.cpp
    automation_view.cpp
    ${SDLUI}/gui.cpp
    ${SDLUI}/engine_globals.cpp
    ${CORE_SRC})

target_include_directories(automation_view_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${SDLUI} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_compile_definitions(automation_view_test PRIVATE __WINDOWS_MM__)
target_compile_options(automation_view_test PRIVATE -Wall)
target_link_directories(automation_view_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(automation_view_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    seq24_automation
    seq24_pluginhost seq24_vst2 seq24_vst3 seq24_graph seq24_audio
    winmm pthread)
