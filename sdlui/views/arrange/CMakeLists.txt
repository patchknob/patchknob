cmake_minimum_required(VERSION 3.15)
project(arrange_view_test CXX)

# ---------------------------------------------------------------------------
# Standalone TEST exe for the SDL2 Arrangement (song) view.  Mirrors
# sdlui/CMakeLists.txt: compiles the UI-neutral engine core + the SDL toolkit +
# this view + a test main, and links the engine module libs.
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/views/arrange -B sdlui/views/arrange/build -G "MinGW Makefiles"
#   cmake --build sdlui/views/arrange/build -j
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
# fontconfig, like the main sdlui build: without it Font::load only knows the
# Debian font paths and the harness dies fontless on every other distro.
pkg_check_modules(FONTCONFIG fontconfig)

# --- engine module libs (the audio/VST path used by audio_app) --------------
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst2   ${CMAKE_CURRENT_BINARY_DIR}/e_vst2   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst3   ${CMAKE_CURRENT_BINARY_DIR}/e_vst3   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_autom EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip  ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/patch      ${CMAKE_CURRENT_BINARY_DIR}/e_patch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/cdp        ${CMAKE_CURRENT_BINARY_DIR}/e_cdp   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/rack       ${CMAKE_CURRENT_BINARY_DIR}/e_rack  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/kitchensink ${CMAKE_CURRENT_BINARY_DIR}/e_kitch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/sampler    ${CMAKE_CURRENT_BINARY_DIR}/e_sampler EXCLUDE_FROM_ALL)

# --- UI-neutral engine core sources -----------------------------------------
set(CORE_SRC
    ${SRC}/perform.cpp ${SRC}/sequence.cpp ${SRC}/event.cpp
    ${SRC}/midibus.cpp ${SRC}/midifile.cpp ${SRC}/mutex.cpp
    ${SRC}/audio_app.cpp ${SRC}/optionsfile.cpp ${SRC}/configfile.cpp
    ${SRC}/userfile.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
)

# audio_app references the embedded metronome click; generate the same
# byte-array TU the main sdlui build embeds.
file(READ "${SDLUI}/../Click.wav" CLICK_WAV_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," CLICK_WAV_BYTES "${CLICK_WAV_HEX}")
set(CLICK_WAV_CPP "${CMAKE_CURRENT_BINARY_DIR}/click_wav_embedded.cpp")
file(WRITE "${CLICK_WAV_CPP}"
    "#include <cstddef>\nextern const unsigned char g_click_wav[]={${CLICK_WAV_BYTES}};\n"
    "extern const std::size_t g_click_wav_size=sizeof(g_click_wav);\n")

add_executable(arrange_view_test
    test_main.cpp
    ${CLICK_WAV_CPP}
    arrange_view.cpp
    arrange_protools.cpp
    arrange_edit.cpp
    arrange_fades.cpp
    ${SDLUI}/gui.cpp
    ${SDLUI}/meter.cpp
    ${SDLUI}/platform/platform_ui_desktop.cpp
    ${SDLUI}/platform/mobile_ui_host_desktop.cpp
    ${SDLUI}/engine_globals.cpp
    ${CORE_SRC})

target_include_directories(arrange_view_test PRIVATE
    ${SDLUI} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
# RtMidi backend per platform: the harness was Windows-only (__WINDOWS_MM__),
# which #included windows.h and could not build on the Linux side at all.
if(WIN32)
    target_compile_definitions(arrange_view_test PRIVATE __WINDOWS_MM__)
else()
    target_compile_definitions(arrange_view_test PRIVATE __RTMIDI_DUMMY__)
endif()
if(FONTCONFIG_FOUND)
    target_compile_definitions(arrange_view_test PRIVATE PATCHKNOB_USE_FONTCONFIG)
    target_include_directories(arrange_view_test PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
    target_link_libraries(arrange_view_test PRIVATE ${FONTCONFIG_LIBRARIES})
endif()
target_compile_options(arrange_view_test PRIVATE -Wall)
target_link_directories(arrange_view_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(arrange_view_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    PatchKnob_pluginhost PatchKnob_vst2 PatchKnob_vst3 PatchKnob_graph PatchKnob_audio
    PatchKnob_automation PatchKnob_audioclip PatchKnob_patch PatchKnob_rack
    PatchKnob_kitchensink PatchKnob_cdp PatchKnob_sampler
    pthread)
if(WIN32)
    target_link_libraries(arrange_view_test PRIVATE winmm)
endif()
