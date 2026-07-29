cmake_minimum_required(VERSION 3.15)
project(PatchKnob CXX)

# ---------------------------------------------------------------------------
# PatchKnob frontend -- links the UI-neutral engine core + the engine module
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
pkg_check_modules(PNG     REQUIRED libpng)
pkg_check_modules(RSVG    REQUIRED librsvg-2.0 cairo)

# --- engine module libs (the audio/VST path used by audio_app) --------------
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst2   ${CMAKE_CURRENT_BINARY_DIR}/e_vst2   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/vst3   ${CMAKE_CURRENT_BINARY_DIR}/e_vst3   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_autom EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip  ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/patch      ${CMAKE_CURRENT_BINARY_DIR}/e_patch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/rack       ${CMAKE_CURRENT_BINARY_DIR}/e_rack  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/kitchensink ${CMAKE_CURRENT_BINARY_DIR}/e_kitch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/buzz       ${CMAKE_CURRENT_BINARY_DIR}/e_buzz  EXCLUDE_FROM_ALL)

# --- UI-neutral engine core sources -----------------------------------------
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
    views/sample_editor/sample_editor_view.cpp
    views/sampler_editor/sampler_editor_view.cpp
    views/mixer/mixer_view.cpp
    views/patchbay/patch_view.cpp
    views/browser/browser_view.cpp
    views/automation/automation_view.cpp
    views/waveform/waveform_view.cpp
    views/waveform/audio_track.cpp
    views/audio_settings/audio_settings_view.cpp
    views/plugin_picker/plugin_picker_view.cpp
    views/plugin_param/plugin_param_view.cpp
    views/mixer_node/mixer_node_view.cpp
    views/master_mixer/master_mixer_view.cpp
    views/pd_editor/pd_editor_view.cpp
    views/csound_editor/csound_editor_view.cpp
    views/rack_editor/rack_editor_view.cpp
    views/rack_editor/cardinal_svg_textures.cpp
    project_io.cpp
)

add_executable(PatchKnob main.cpp gui.cpp engine_globals.cpp plugin_editor_window.cpp clip_host.cpp transport_bar.cpp ${VIEW_SRC} ${CORE_SRC})

set(CARDINAL_SVG_PACK ${CMAKE_BINARY_DIR}/cardinal_svg.pak)
set(RACK_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../modules)
add_executable(cardinal_svg_packer ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cardinal_svg_packer.cpp)
target_include_directories(cardinal_svg_packer PRIVATE ${RSVG_INCLUDE_DIRS})
target_compile_options(cardinal_svg_packer PRIVATE ${RSVG_CFLAGS_OTHER})
target_link_directories(cardinal_svg_packer PRIVATE ${RSVG_LIBRARY_DIRS})
target_link_libraries(cardinal_svg_packer PRIVATE ${RSVG_LIBRARIES})
add_custom_target(cardinal_svg_assets
    COMMAND $<TARGET_FILE:cardinal_svg_packer> ${VEND}/Cardinal/plugins ${CARDINAL_SVG_PACK} 4096 Fundamental,Befaco,GoodSheperd
    DEPENDS cardinal_svg_packer
    BYPRODUCTS ${CARDINAL_SVG_PACK}
    VERBATIM)
add_custom_target(rack_module_directory
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RACK_MODULE_DIR}
    VERBATIM)
add_dependencies(PatchKnob cardinal_svg_assets)
add_dependencies(PatchKnob rack_module_directory)

# Csound is linked via its DLL import lib -> ship csound64.dll next to the exe so
# the CsoundNode loads at runtime.  CSOUND_ROOT is the cache var from the patch
# module (defaults to the standard install; override for a vendored build).
if(NOT DEFINED CSOUND_ROOT)
    set(CSOUND_ROOT "C:/Program Files/Csound6_x64")
endif()
if(EXISTS "${CSOUND_ROOT}/bin/csound64.dll")
    add_custom_command(TARGET PatchKnob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CSOUND_ROOT}/bin/csound64.dll" "$<TARGET_FILE_DIR:PatchKnob>"
        VERBATIM)
endif()

target_include_directories(PatchKnob PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS} ${PNG_INCLUDE_DIRS})
target_compile_definitions(PatchKnob PRIVATE __WINDOWS_MM__)
target_compile_options(PatchKnob PRIVATE -Wall)
target_link_directories(PatchKnob PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS} ${PNG_LIBRARY_DIRS})

target_link_libraries(PatchKnob PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES} ${PNG_LIBRARIES}
    PatchKnob_pluginhost PatchKnob_vst2 PatchKnob_vst3 PatchKnob_graph PatchKnob_audio
    PatchKnob_automation PatchKnob_audioclip PatchKnob_patch PatchKnob_rack PatchKnob_kitchensink
    PatchKnob_buzz
    winmm comdlg32 pthread)

# The plugin scanner probes each plugin out-of-process via probe_vst2/3.exe,
# which it looks for next to the running exe.  Build them and stage them there.
add_dependencies(PatchKnob probe_vst2)
add_custom_command(TARGET PatchKnob POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:probe_vst2> $<TARGET_FILE_DIR:PatchKnob>
    VERBATIM)
if(TARGET probe_vst3)
    add_dependencies(PatchKnob probe_vst3)
    add_custom_command(TARGET PatchKnob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:probe_vst3> $<TARGET_FILE_DIR:PatchKnob>
        VERBATIM)
endif()

# Bundle the full transitive mingw64 DLL closure (SDL2/SDL2_ttf + freetype/
# harfbuzz/glib chain + libstdc++/libgcc/libwinpthread) next to the exe so it
# runs self-contained -- a detached launch otherwise loads stale DLLs off PATH
# and aborts with STATUS_ENTRYPOINT_NOT_FOUND (missing clock_gettime).  Runs
# last (after the probe helpers are staged, so ldd sees the whole set).
set(MSYS_BASH_HINT "C:/msys64/usr/bin/bash.exe")
if(EXISTS "${MSYS_BASH_HINT}")
    set(BASH_EXE "${MSYS_BASH_HINT}" CACHE FILEPATH "MSYS bash" FORCE)
else()
    find_program(BASH_EXE bash)
endif()
if(BASH_EXE)
    add_custom_command(TARGET PatchKnob POST_BUILD
        COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/deploy_dlls.sh $<TARGET_FILE:PatchKnob>
        VERBATIM)
else()
    message(WARNING "bash not found: run sdlui/deploy_dlls.sh manually to bundle DLLs")
endif()
