#-----------------------------------------------------------------------------
# seq24 Windows port — automation module (parameter/CC automation engine).
#
#   * seq24_automation : static library — the automation data model
#                        (AutomationLane / AutomationTrack, header-only) plus the
#                        playback runtime (AutomationPlayer).
#   * automation_test  : standalone self-test (build lanes, advance() across tick
#                        ranges, assert linear/step/hold interpolation + CC 0..127).
#
# Self-contained so the module builds on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/automation_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(seq24_automation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- static library ---------------------------------------------------------
# AutomationLane/AutomationTrack are header-only; AutomationPlayer has a .cpp.
add_library(seq24_automation STATIC
    automation_player.cpp
)

# The parent engine dir is on the include path to match the other engine
# modules (so a sibling can include "../automation/automation_player.h").
target_include_directories(seq24_automation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_options(seq24_automation PRIVATE -Wall -Wextra)

# --- test executable --------------------------------------------------------
add_executable(automation_test automation_test.cpp)
target_link_libraries(automation_test PRIVATE seq24_automation)
target_compile_options(automation_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME automation_test COMMAND automation_test)
