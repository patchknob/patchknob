cmake_minimum_required(VERSION 3.15)
project(PatchKnob_projectio CXX)

# ---------------------------------------------------------------------------
# Stand-alone test build for project_io (save_project / load_project).
#
#   Windows (MSYS2 MINGW64):
#     export PATH="/c/msys64/mingw64/bin:$PATH"
#     cmake -S sdlui/project_io_test -B sdlui/project_io_test/build -G "MinGW Makefiles"
#     cmake --build sdlui/project_io_test/build -j
#     ./sdlui/project_io_test/build/project_io_test
#
#   Linux:
#     cmake -S sdlui/project_io_test -B <builddir>
#     cmake --build <builddir> -j
#     <builddir>/project_io_test
#
# Compiles project_io.cpp + a test main and links the real UI-neutral PatchKnob
# core (perform/sequence/event/...) plus the engine module libs that
# audio_app.cpp and project_io.cpp reference.  No audio device and no window are
# opened by the test: ui::mode()/ui::set_mode() (the only gui.cpp symbols
# project_io needs) are supplied by the test TU itself, so the whole SDL /
# libpng / librsvg toolkit stays out of this build.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SRC   ${SDLUI}/../src)
set(VEND  ${SDLUI}/../vendor)
set(RTMIDI_DIR ${VEND}/rtmidi)

# The UI toolkit header is included transitively by project_io.cpp; it needs the
# SDL headers to parse even though nothing here opens a window.  Match the app's
# default (SDL3 via sdl3_compat.h) so both builds see the same declarations.
find_package(PkgConfig REQUIRED)
option(PATCHKNOB_SDL3 "Build against SDL3 instead of SDL2" ON)
if(PATCHKNOB_SDL3)
    pkg_check_modules(SDL2    REQUIRED sdl3)
    pkg_check_modules(SDL2TTF REQUIRED sdl3-ttf)
    add_compile_definitions(PATCHKNOB_SDL3=1)
    # AHEAD of the real SDL include dir -- this is what makes gui.h's
    # `#include <SDL.h>` resolve to SDL3 + the compat layer.
    set(PATCHKNOB_SDL3_INCLUDE ${SDLUI}/compat_sdl3)
else()
    pkg_check_modules(SDL2    REQUIRED sdl2)
    pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)
endif()

# engine module libs (audio_app.cpp / project_io.cpp link against these)
option(PATCHKNOB_ENABLE_VST "Build VST2/VST3 hosting" ON)
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
if(PATCHKNOB_ENABLE_VST)
    add_subdirectory(${SRC}/engine/vst2 ${CMAKE_CURRENT_BINARY_DIR}/e_vst2 EXCLUDE_FROM_ALL)
    add_subdirectory(${SRC}/engine/vst3 ${CMAKE_CURRENT_BINARY_DIR}/e_vst3 EXCLUDE_FROM_ALL)
endif()
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_autom EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/patch     ${CMAKE_CURRENT_BINARY_DIR}/e_patch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/cdp       ${CMAKE_CURRENT_BINARY_DIR}/e_cdp   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/rack      ${CMAKE_CURRENT_BINARY_DIR}/e_rack EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/kitchensink ${CMAKE_CURRENT_BINARY_DIR}/e_kitch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/sampler   ${CMAKE_CURRENT_BINARY_DIR}/e_sampler EXCLUDE_FROM_ALL)

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
    ${CORE_SRC})

target_include_directories(project_io_test PRIVATE
    ${PATCHKNOB_SDL3_INCLUDE}
    ${SDLUI} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})

target_compile_options(project_io_test PRIVATE -Wall)
target_link_directories(project_io_test PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})

target_link_libraries(project_io_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES}
    PatchKnob_pluginhost PatchKnob_graph PatchKnob_audio
    PatchKnob_automation PatchKnob_audioclip PatchKnob_patch PatchKnob_cdp
    PatchKnob_rack PatchKnob_kitchensink PatchKnob_sampler)
if(PATCHKNOB_ENABLE_VST)
    target_link_libraries(project_io_test PRIVATE PatchKnob_vst2 PatchKnob_vst3)
endif()

# RtMidi silently degrades to its Dummy API without one of these; the test never
# opens a port, but midibus.cpp must still link.
if(WIN32)
    target_compile_definitions(project_io_test PRIVATE __WINDOWS_MM__)
    target_link_libraries(project_io_test PRIVATE winmm pthread)
elseif(UNIX)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_compile_definitions(project_io_test PRIVATE __LINUX_ALSA__)
    target_include_directories(project_io_test PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_directories(project_io_test PRIVATE ${ALSA_LIBRARY_DIRS})
    find_package(Threads REQUIRED)
    target_link_libraries(project_io_test PRIVATE ${ALSA_LIBRARIES} Threads::Threads dl)
endif()

enable_testing()
add_test(NAME project_io_test COMMAND project_io_test)
