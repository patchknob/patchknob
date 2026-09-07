# ---------------------------------------------------------------------------
# Standalone headless selftest for the Csound editor's Claude chat panel.
# Same shape as sdlui/views/arrange: SDL2 via pkg-config, the toolkit sources
# compiled in, no engine core needed -- the network layer is exercised with
# synthetic SSE/events, so the suite needs no key and no network.
#   cmake -S sdlui/views/csound_editor/test -B <dir>
#   cmake --build <dir> -j && SDL_VIDEODRIVER=dummy <dir>/chat_panel_test
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(chat_panel_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SRC   ${SDLUI}/../src)
set(VEND  ${SDLUI}/../vendor)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)
pkg_check_modules(PNG     REQUIRED libpng)
pkg_check_modules(FONTCONFIG fontconfig)

set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
add_subdirectory(${VEND}/litehtml ${CMAKE_CURRENT_BINARY_DIR}/vendor_litehtml EXCLUDE_FROM_ALL)
add_subdirectory(${SRC}/engine/ai ${CMAKE_CURRENT_BINARY_DIR}/e_ai EXCLUDE_FROM_ALL)

if(EXISTS ${VEND}/csound-manual/index.html)
    add_compile_definitions(PATCHKNOB_CSOUND_MANUAL_DIR="${VEND}/csound-manual")
endif()

add_executable(chat_panel_test
    chat_panel_test.cpp
    ${SDLUI}/gui.cpp
    ${SDLUI}/meter.cpp
    ${SDLUI}/html_container.cpp
    ${SDLUI}/platform/platform_ui_desktop.cpp
    ${SDLUI}/platform/mobile_ui_host_desktop.cpp
    ${SDLUI}/views/csound_editor/csound_editor_view.cpp
    ${SDLUI}/views/csound_editor/chat_panel.cpp
    # The compile oracle, WITHOUT PATCHKNOB_HAVE_CSOUND: that builds the
    # "Csound not in this build" stub, so the harness gets the same code path a
    # Csound-less build has -- no csound headers, no 30 s link -- and proves the
    # panel degrades gracefully when there is no compiler to ask.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/engine/patch/csound_dry_compile.cpp
    ${SDLUI}/views/csound_editor/ai_prefs.cpp
    ${SDLUI}/views/csound_editor/ai_settings_view.cpp
)
target_compile_definitions(chat_panel_test PRIVATE PATCHKNOB_HAS_AI=1)
target_include_directories(chat_panel_test PRIVATE
    ${SDLUI} ${SRC} ${SRC}/engine
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS} ${PNG_INCLUDE_DIRS})
if(FONTCONFIG_FOUND)
    target_compile_definitions(chat_panel_test PRIVATE PATCHKNOB_USE_FONTCONFIG)
endif()
target_compile_options(chat_panel_test PRIVATE -Wall)
target_link_directories(chat_panel_test PRIVATE
    ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS} ${FONTCONFIG_LIBRARY_DIRS} ${PNG_LIBRARY_DIRS})
target_link_libraries(chat_panel_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES} ${FONTCONFIG_LIBRARIES} ${PNG_LIBRARIES}
    litehtml PatchKnob_ai)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(chat_panel_test PRIVATE Threads::Threads)
endif()
