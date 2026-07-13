cmake_minimum_required(VERSION 3.15)
project(seq24sdl CXX)

# ---------------------------------------------------------------------------
# seq24 SDL2 frontend (cross-platform: Windows now, Linux/KMSDRM appliance
# later).  Links only SDL2 + SDL2_ttf for the toolkit foundation; the DAW
# engine libs get linked in once views are wired.
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -S sdlui -B sdlui/build -G "Ninja"   (or "MinGW Makefiles")
#   cmake --build sdlui/build -j
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2    REQUIRED sdl2)
pkg_check_modules(SDL2TTF REQUIRED SDL2_ttf)

add_executable(seq24sdl main.cpp gui.cpp)

target_include_directories(seq24sdl PRIVATE ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
target_link_directories(seq24sdl PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})
target_compile_options(seq24sdl PRIVATE ${SDL2_CFLAGS_OTHER} -Wall)
# SDL2_ttf must precede SDL2; SDL2_LIBRARIES already carries mingw32/SDL2main.
target_link_libraries(seq24sdl PRIVATE ${SDL2TTF_LIBRARIES} ${SDL2_LIBRARIES})
