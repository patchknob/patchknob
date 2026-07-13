cmake_minimum_required(VERSION 3.15)
project(patchbay_view CXX)

# ---------------------------------------------------------------------------
# Standalone TEST exe for the SDL patchbay node editor.
#
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui/views/patchbay -B sdlui/views/patchbay/build -G "MinGW Makefiles"
#   cmake --build sdlui/views/patchbay/build -j
#   ./sdlui/views/patchbay/build/patchbay_view.exe
#
# This view only reads the ENGINE-FREE view-model (ui/patchbay/patch_view_model.h),
# so no engine libs are linked -- just the SDL toolkit (gui.cpp) + SDL2/SDL2_ttf.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDLUI ${CMAKE_CURRENT_SOURCE_DIR}/../..)   # .../sdlui
set(SRC   ${SDLUI}/../src)                      # .../src

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

add_executable(patchbay_view
    ${CMAKE_CURRENT_SOURCE_DIR}/patch_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp
    ${SDLUI}/gui.cpp)

target_include_directories(patchbay_view PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # patch_view.h
    ${SDLUI}                        # gui.h
    ${SRC}                          # ui/patchbay/patch_view_model.h
    ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})

target_compile_options(patchbay_view PRIVATE -Wall)
target_link_directories(patchbay_view PRIVATE
    ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})
target_link_libraries(patchbay_view PRIVATE
    ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES})
