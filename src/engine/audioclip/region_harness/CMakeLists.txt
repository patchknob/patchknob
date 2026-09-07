cmake_minimum_required(VERSION 3.16)
project(PatchKnob_region_harness CXX)

# ---------------------------------------------------------------------------
# Head-less harness for the audio_app PROJECT-REGION layer: proves what the
# engine actually schedules when the arrange view performs a clip split
# (attach-shared right half + set_region left half), and renders audio through
# audio_render() with the device stream stopped, so no GUI and no live audio
# device output is required beyond opening the stream.
#
#   cmake -S src/engine/audioclip/region_harness -B <dir>
#   cmake --build <dir> -j
#   <dir>/region_harness_test
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(SRC   ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(VEND  ${SRC}/../vendor)
set(RTMIDI_DIR ${VEND}/rtmidi)

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

add_executable(region_harness_test
    region_harness.cpp
    ${SRC}/audio_app.cpp
    ${RTMIDI_DIR}/RtMidi.cpp)

target_include_directories(region_harness_test PRIVATE
    ${SRC} ${SRC}/engine ${RTMIDI_DIR})
target_compile_options(region_harness_test PRIVATE -Wall)

target_link_libraries(region_harness_test PRIVATE
    PatchKnob_pluginhost PatchKnob_graph PatchKnob_audio
    PatchKnob_automation PatchKnob_audioclip PatchKnob_patch PatchKnob_cdp
    PatchKnob_rack PatchKnob_kitchensink PatchKnob_sampler)
if(PATCHKNOB_ENABLE_VST)
    target_link_libraries(region_harness_test PRIVATE PatchKnob_vst2 PatchKnob_vst3)
endif()

if(WIN32)
    target_compile_definitions(region_harness_test PRIVATE __WINDOWS_MM__)
    target_link_libraries(region_harness_test PRIVATE winmm pthread)
elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_compile_definitions(region_harness_test PRIVATE __LINUX_ALSA__)
    target_include_directories(region_harness_test PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_directories(region_harness_test PRIVATE ${ALSA_LIBRARY_DIRS})
    find_package(Threads REQUIRED)
    target_link_libraries(region_harness_test PRIVATE ${ALSA_LIBRARIES} Threads::Threads dl)
endif()

enable_testing()
add_test(NAME region_harness_test COMMAND region_harness_test)
