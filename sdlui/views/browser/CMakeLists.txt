cmake_minimum_required(VERSION 3.15)
project(browser_view_test CXX)

# ---------------------------------------------------------------------------
# Standalone test for the SDL2 VST plugin browser view.
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/views/browser -B sdlui/views/browser/build -G "MinGW Makefiles"
#   cmake --build sdlui/views/browser/build -j
#   ./sdlui/views/browser/build/browser_view_test
#
# BrowserView is PURE UI: it depends only on the header-only engine contract
# (seq24::engine::PluginDescriptor from src/engine/plugin_api.h) plus the
# toolkit (sdlui/gui.cpp).  No engine libraries / core sources are linked -- the
# test feeds a hardcoded descriptor vector instead of running the slow scan.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../..)   # sdlui/
set(SRC   ${SDLUI}/../src)                     # src/

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

add_executable(browser_view_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/browser_view.cpp
    ${SDLUI}/gui.cpp)

target_include_directories(browser_view_test PRIVATE
    ${SDLUI} ${SRC} ${SRC}/engine
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_compile_options(browser_view_test PRIVATE -Wall)
target_link_directories(browser_view_test PRIVATE
    ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})
target_link_libraries(browser_view_test PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES})
