cmake_minimum_required(VERSION 3.15)
project(PatchKnob C CXX)

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
option(PATCHKNOB_ENABLE_VST "Build VST2/VST3 hosting and scanners" ON)
if(ANDROID)
    set(PATCHKNOB_ENABLE_VST OFF CACHE BOOL
        "VST hosting is not part of the Android/mobile build" FORCE)
    add_compile_definitions(PATCHKNOB_MOBILE=1 PATCHKNOB_NO_VST=1)
endif()

# litehtml -- vendored, dependency-free (bundles its own gumbo HTML parser)
# HTML+CSS layout/render engine used by html_container.{h,cpp} for the
# collapsible in-app help panels (see views/csound_editor). Pure C99/C++17,
# no image/network/JS support required, so it builds the same way on
# Android, Linux and Windows/MinGW.
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
add_subdirectory(${VEND}/litehtml ${CMAKE_CURRENT_BINARY_DIR}/vendor_litehtml EXCLUDE_FROM_ALL)

# The Csound editor's help drawer browses a local mirror of the Canonical
# Csound Reference Manual (vendor/csound-manual, ~33 MB / 1700 pages, fetched
# by tools/fetch_csound_manual.py -- not tracked in our history).  At runtime
# it is looked for BESIDE THE EXECUTABLE first, so a packaged build just ships
# the directory next to the exe; this define is the dev-tree fallback, which
# avoids copying 33 MB into every build dir.  If the mirror is absent the
# drawer says so and points at the fetch script, so the build never depends on
# it existing.
if(EXISTS ${VEND}/csound-manual/index.html)
    add_compile_definitions(PATCHKNOB_CSOUND_MANUAL_DIR="${VEND}/csound-manual")
endif()

if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    add_subdirectory(${VEND}/SDL2 ${CMAKE_CURRENT_BINARY_DIR}/vendor_sdl EXCLUDE_FROM_ALL)
    # SDL2_ttf chooses SDL2::SDL2 versus SDL2::SDL2-static from
    # BUILD_SHARED_LIBS (not SDL_SHARED). Match the shared SDL Android target.
    set(_PATCHKNOB_SAVED_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON)
    set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDL2TTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${VEND}/SDL2_ttf ${CMAKE_CURRENT_BINARY_DIR}/vendor_ttf EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${_PATCHKNOB_SAVED_BUILD_SHARED_LIBS}")
    unset(_PATCHKNOB_SAVED_BUILD_SHARED_LIBS)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${VEND}/zlib ${CMAKE_CURRENT_BINARY_DIR}/vendor_zlib EXCLUDE_FROM_ALL)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    # FindZLIB requires a file path. A target name stored as FILEPATH is emitted
    # literally by Android's linker ("zlibstatic") instead of resolving libz.a.
    set(ZLIB_LIBRARY
        "${CMAKE_CURRENT_BINARY_DIR}/vendor_zlib/libz.a"
        CACHE FILEPATH "" FORCE)
    add_subdirectory(${VEND}/libpng ${CMAKE_CURRENT_BINARY_DIR}/vendor_png EXCLUDE_FROM_ALL)
    set(SDL2_INCLUDE_DIRS ${VEND}/SDL2/include)
    set(SDL2TTF_INCLUDE_DIRS ${VEND}/SDL2_ttf/include)
    set(PNG_INCLUDE_DIRS ${VEND}/libpng ${CMAKE_CURRENT_BINARY_DIR}/vendor_png)
    set(SDL2_LIBRARIES SDL2::SDL2)
    set(SDL2TTF_LIBRARIES SDL2_ttf)
    # png_static already carries its zlibstatic dependency transitively.
    set(PNG_LIBRARIES png_static)
else()
    find_package(PkgConfig REQUIRED)
    # SDL3 is the default renderer.  It measured ~11% GPU against SDL2's ~31%
    # on identical code and window size, and it respects the playback frame cap
    # that SDL2 currently defeats (SDL2 appears to deliver a window-exposed event
    # per frame under DWM, which marks every frame dirty).
    #
    # -DPATCHKNOB_SDL3=OFF still builds against SDL2.  That path is kept working
    # -- not vestigial -- because the SDL3 build reaches SDL3 through
    # sdl3_compat.h rather than native calls, so being able to A/B the two is
    # what catches a shim that is subtly wrong.  It goes away when the shim does.
    option(PATCHKNOB_SDL3 "Build the UI against SDL3 instead of SDL2" ON)
    if(PATCHKNOB_SDL3)
        pkg_check_modules(SDL2    REQUIRED sdl3)
        pkg_check_modules(SDL2TTF REQUIRED sdl3-ttf)
        add_compile_definitions(PATCHKNOB_SDL3=1)
    else()
        pkg_check_modules(SDL2    REQUIRED sdl2)
        pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)
    endif()
    pkg_check_modules(PNG     REQUIRED libpng)
    pkg_check_modules(RSVG    REQUIRED librsvg-2.0 cairo)
    # Font::load()'s hardcoded candidate paths only cover Debian/Ubuntu's font
    # tree (/usr/share/fonts/truetype/...); every other desktop distro (Arch,
    # Fedora, NixOS, ...) lays out monospace fonts differently, so without
    # this the app cannot find ANY font on them. Optional: falls back to the
    # hardcoded list if unavailable.
    pkg_check_modules(FONTCONFIG fontconfig)
    if(FONTCONFIG_FOUND)
        add_compile_definitions(PATCHKNOB_USE_FONTCONFIG=1)
    endif()
endif()

# --- engine module libs (the audio/VST path used by audio_app) --------------
add_subdirectory(${SRC}/engine/audio  ${CMAKE_CURRENT_BINARY_DIR}/e_audio  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/graph  ${CMAKE_CURRENT_BINARY_DIR}/e_graph  EXCLUDE_FROM_ALL)
if(PATCHKNOB_ENABLE_VST)
    add_subdirectory(${SRC}/engine/vst2 ${CMAKE_CURRENT_BINARY_DIR}/e_vst2 EXCLUDE_FROM_ALL)
    add_subdirectory(${SRC}/engine/vst3 ${CMAKE_CURRENT_BINARY_DIR}/e_vst3 EXCLUDE_FROM_ALL)
endif()
add_subdirectory(${SRC}/engine/host   ${CMAKE_CURRENT_BINARY_DIR}/e_host   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/automation ${CMAKE_CURRENT_BINARY_DIR}/e_autom EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/audioclip  ${CMAKE_CURRENT_BINARY_DIR}/e_aclip EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/patch      ${CMAKE_CURRENT_BINARY_DIR}/e_patch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/cdp        ${CMAKE_CURRENT_BINARY_DIR}/e_cdp   EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/rack       ${CMAKE_CURRENT_BINARY_DIR}/e_rack  EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/kitchensink ${CMAKE_CURRENT_BINARY_DIR}/e_kitch EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/sampler    ${CMAKE_CURRENT_BINARY_DIR}/e_sampler EXCLUDE_FROM_ALL)
# SoundFont (.sf2/.sf3) reader + the mapping onto the built-in sampler's zone
# model -- used by the disk browser's expandable soundfont node and by the
# shell's drag-a-preset-into-the-Sampler import path.  Depends on
# PatchKnob_sampler (already added above), which its own CMakeLists detects.
add_subdirectory(${SRC}/engine/sf2        ${CMAKE_CURRENT_BINARY_DIR}/e_sf2   EXCLUDE_FROM_ALL)

# --- ai module: Claude client for the Csound chat panel ---------------------
# Desktop only: its Linux HTTPS backend requires libcurl (find_package REQUIRED)
# which the Android toolchain does not provide; Windows uses WinHTTP.  The
# module's own CMakeLists probes for SDL2 (it only needs SDL_GetPrefPath), but
# THIS build must hand it the same SDL the app links -- an SDL3 build that let
# it find real SDL2 would drag a second SDL runtime into the executable.  So
# its internal probe is disabled and the app's SDL is injected below.
if(NOT ANDROID)
    set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
    add_subdirectory(${SRC}/engine/ai     ${CMAKE_CURRENT_BINARY_DIR}/e_ai    EXCLUDE_FROM_ALL)
    set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 FALSE)
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig FALSE)
    if(PATCHKNOB_SDL3)
        # `#include <SDL.h>` inside the module resolves through the same shim
        # the app's 71 files use, so it compiles against SDL3.
        target_include_directories(PatchKnob_ai BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/compat_sdl3)
        target_compile_definitions(PatchKnob_ai PRIVATE PATCHKNOB_SDL3=1)
    endif()
    target_include_directories(PatchKnob_ai PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_directories(PatchKnob_ai PUBLIC ${SDL2_LIBRARY_DIRS})
    target_link_libraries(PatchKnob_ai PUBLIC ${SDL2_LIBRARIES})
endif()

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
    views/arrange/arrange_protools.cpp
    views/arrange/arrange_edit.cpp
    views/arrange/arrange_fades.cpp
    views/sample_editor/sample_editor_view.cpp
    views/sampler_editor/sampler_editor_view.cpp
    views/sample_slot/sample_slot_editor.cpp
    views/sample_slot/sample_browser.cpp
    screen_recorder.cpp
    folder_picker.cpp
    views/file_dialog/file_dialog.cpp
    views/video_settings/video_settings_view.cpp
    views/sample_slot/sampler_instrument_slot.cpp
    views/mixer/mixer_view.cpp
    views/patchbay/patch_view.cpp
    views/browser/browser_view.cpp
    views/automation/automation_view.cpp
    views/waveform/waveform_view.cpp
    views/waveform/audio_track.cpp
    views/audio_settings/audio_settings_view.cpp
    views/plugin_picker/plugin_picker_view.cpp
    views/plugin_param/plugin_param_view.cpp
    views/master_mixer/master_mixer_view.cpp
    views/pd_editor/pd_editor_view.cpp
    views/csound_editor/csound_editor_view.cpp
    views/cdp_editor/cdp_editor_view.cpp
    views/rack_editor/rack_editor_view.cpp
    views/rack_editor/cardinal_svg_textures.cpp
    views/mobile_keyboard/mobile_keyboard.cpp
    project_io.cpp
)
# The Claude chat panel rides with the ai module (desktop only, see above).
if(NOT ANDROID)
    list(APPEND VIEW_SRC
        views/csound_editor/chat_panel.cpp
        views/csound_editor/ai_prefs.cpp
        views/csound_editor/ai_settings_view.cpp)
endif()

# Embed the metronome click in the executable; no Click.wav is needed beside it.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../Click.wav" CLICK_WAV_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," CLICK_WAV_BYTES "${CLICK_WAV_HEX}")
set(CLICK_WAV_CPP "${CMAKE_CURRENT_BINARY_DIR}/click_wav_embedded.cpp")
file(WRITE "${CLICK_WAV_CPP}"
    "#include <cstddef>\nextern const unsigned char g_click_wav[]={${CLICK_WAV_BYTES}};\n"
    "extern const std::size_t g_click_wav_size=sizeof(g_click_wav);\n")

if(ANDROID)
    set(PATCHKNOB_PLATFORM_UI platform/platform_ui_android.cpp platform/mobile_ui_host_android.cpp)
else()
    set(PATCHKNOB_PLATFORM_UI platform/platform_ui_desktop.cpp platform/mobile_ui_host_desktop.cpp)
endif()
if(PATCHKNOB_SDL3)
    set(PATCHKNOB_SDL3_SHIM sdl3_compat.cpp)
    # AHEAD of the real SDL include dir: this is what makes the existing
    # `#include <SDL.h>` in 71 files resolve to SDL3 + the compat layer.
    set(PATCHKNOB_SDL3_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/compat_sdl3)
endif()
set(PATCHKNOB_APP_SOURCES main.cpp gui.cpp meter.cpp engine_globals.cpp plugin_editor_window.cpp
    html_container.cpp
    ${PATCHKNOB_PLATFORM_UI} ${PATCHKNOB_SDL3_SHIM} gpu_meter.cpp
    transport_bar.cpp ${CLICK_WAV_CPP} ${VIEW_SRC} ${CORE_SRC})
if(ANDROID)
    add_library(PatchKnob SHARED ${PATCHKNOB_APP_SOURCES})
    set_target_properties(PatchKnob PROPERTIES OUTPUT_NAME main)
else()
    add_executable(PatchKnob ${PATCHKNOB_APP_SOURCES})
    # (A screen_recorder_selftest target lived here.  It was written against the
    # SDL2 API while this build resolves SDL3, so it never compiled and only
    # broke `cmake --build`.  screen_recorder_selftest.cpp is still on disk if it
    # is ever worth porting.)
endif()

set(CARDINAL_SVG_PACK ${CMAKE_BINARY_DIR}/cardinal_svg.pak)
set(RACK_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../modules)
if(NOT ANDROID)
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
    add_dependencies(PatchKnob cardinal_svg_assets)
endif()
add_custom_target(rack_module_directory
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RACK_MODULE_DIR}
    VERBATIM)
add_dependencies(PatchKnob rack_module_directory)

# Csound is linked via its DLL import lib -> ship csound64.dll next to the exe so
# the CsoundNode loads at runtime.  CSOUND_ROOT is the cache var from the patch
# module (defaults to the standard install; override for a vendored build).
if(WIN32 AND NOT DEFINED CSOUND_ROOT)
    set(CSOUND_ROOT "C:/Program Files/Csound6_x64")
endif()
if(WIN32 AND EXISTS "${CSOUND_ROOT}/bin/csound64.dll")
    add_custom_command(TARGET PatchKnob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CSOUND_ROOT}/bin/csound64.dll" "$<TARGET_FILE_DIR:PatchKnob>"
        VERBATIM)
endif()

target_include_directories(PatchKnob PRIVATE
    ${PATCHKNOB_SDL3_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR} ${SRC} ${SRC}/engine ${RTMIDI_DIR}
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS} ${PNG_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
if(WIN32)
    target_compile_definitions(PatchKnob PRIVATE __WINDOWS_MM__)
elseif(ANDROID)
    # Unchanged: RtMidi falls back to its Dummy API here, same as before --
    # Android MIDI (AMIDI) is a separate, not-yet-attempted effort.
elseif(UNIX)
    # Without one of RtMidi's API macros defined, it silently falls back to
    # RTMIDI_DUMMY ("no compiled support for specified API") and MIDI I/O is
    # a no-op -- which is exactly what was happening here. ALSA sequencer
    # MIDI is the baseline every Linux box with a soundcard has; JACK MIDI
    # is added on top when present, same optional-if-found treatment the
    # audio side already gives JACK.
    pkg_check_modules(ALSA REQUIRED alsa)
    target_compile_definitions(PatchKnob PRIVATE __LINUX_ALSA__)
    target_include_directories(PatchKnob PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_directories(PatchKnob PRIVATE ${ALSA_LIBRARY_DIRS})
    target_link_libraries(PatchKnob PRIVATE ${ALSA_LIBRARIES})
    pkg_check_modules(JACK jack)
    if(JACK_FOUND)
        target_compile_definitions(PatchKnob PRIVATE __UNIX_JACK__)
        target_include_directories(PatchKnob PRIVATE ${JACK_INCLUDE_DIRS})
        target_link_directories(PatchKnob PRIVATE ${JACK_LIBRARY_DIRS})
        target_link_libraries(PatchKnob PRIVATE ${JACK_LIBRARIES})
    endif()
    # libX11 for plugin_editor_window.cpp.  A Linux VST2/VST3 editor is ALWAYS an
    # X11 window (VST3's only Linux platform type is kPlatformTypeX11EmbedWindowID
    # and there is no Wayland equivalent), so hosting one needs Xlib no matter
    # which backend SDL itself picked -- under Wayland the editor becomes a
    # top-level XWayland window.  This used to be guarded on SDL_VIDEO_DRIVER_X11,
    # which SDL3 never defines (its build config is not installed), so the whole
    # Linux editor path silently compiled out and no plugin GUI could ever open.
    # Optional: without libX11 the app still builds and falls back to the SDL
    # parameter panel.
    pkg_check_modules(PK_X11 x11)
    if(PK_X11_FOUND)
        target_compile_definitions(PatchKnob PRIVATE PATCHKNOB_HAVE_X11=1)
        target_include_directories(PatchKnob PRIVATE ${PK_X11_INCLUDE_DIRS})
        target_link_directories(PatchKnob PRIVATE ${PK_X11_LIBRARY_DIRS})
        target_link_libraries(PatchKnob PRIVATE ${PK_X11_LIBRARIES})
    else()
        message(STATUS "libX11 not found: native plugin editors disabled "
                       "(the SDL parameter panel is used instead)")
    endif()
endif()
target_compile_options(PatchKnob PRIVATE -Wall)
target_link_directories(PatchKnob PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS} ${PNG_LIBRARY_DIRS} ${FONTCONFIG_LIBRARY_DIRS})

target_link_libraries(PatchKnob PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES} ${PNG_LIBRARIES} ${FONTCONFIG_LIBRARIES}
    litehtml
    PatchKnob_pluginhost PatchKnob_graph PatchKnob_audio
    PatchKnob_automation PatchKnob_audioclip PatchKnob_patch PatchKnob_rack PatchKnob_kitchensink PatchKnob_cdp
    PatchKnob_sampler PatchKnob_sf2)
if(NOT ANDROID)
    # Claude chat: the define is what compiles the panel INTO the Csound
    # editor and its wiring in main.cpp; without it those files build the
    # editor exactly as before (the Android path).
    target_compile_definitions(PatchKnob PRIVATE PATCHKNOB_HAS_AI=1)
    target_link_libraries(PatchKnob PRIVATE PatchKnob_ai)
endif()
if(PATCHKNOB_ENABLE_VST)
    target_link_libraries(PatchKnob PRIVATE PatchKnob_vst2 PatchKnob_vst3)
endif()
if(WIN32)
    # pdh: per-process GPU counters for the CPU/GPU meter (gpu_meter.cpp).
    target_link_libraries(PatchKnob PRIVATE winmm comdlg32 pthread pdh)
    # GUI subsystem: no console / debug window behind the app (SDL2main provides WinMain).
    set_target_properties(PatchKnob PROPERTIES WIN32_EXECUTABLE ON)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(PatchKnob PRIVATE Threads::Threads dl)
endif()
if(ANDROID)
    target_link_libraries(PatchKnob PRIVATE android log OpenSLES)
endif()

# The plugin scanner probes each plugin out-of-process via probe_vst2/3.exe,
# which it looks for next to the running exe.  Build them and stage them there.
if(TARGET probe_vst2)
    add_dependencies(PatchKnob probe_vst2)
    add_custom_command(TARGET PatchKnob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:probe_vst2> $<TARGET_FILE_DIR:PatchKnob>
        VERBATIM)
endif()
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
if(WIN32)
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
endif()
