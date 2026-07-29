#-----------------------------------------------------------------------------
# PatchKnob -- built-in SAMPLER instrument (PatchKnob_buzz).
#
#   * PatchKnob_buzz : static library holding the self-contained polyphonic sampler
#                  (sampler_instrument.cpp) exposed as an IPluginInstance.  It no
#                  longer depends on the Buzz/Unwieldy backend -- that machine-host
#                  code (dsplib / mdk / OscTable / buzz_host / buzz_machine_host /
#                  unwieldy_tracker + buzz_test) is retired and NOT built anymore;
#                  the sources remain in-tree for reference only.
#
# (The library target keeps its historical name so the app's link line is unchanged.)
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_buzz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(PatchKnob_buzz STATIC
    sampler_instrument.cpp      # self-contained polyphonic sampler (IPluginInstance)
)

# own dir for sampler_instrument.h; parent (src/engine) for plugin_api.h (the bridge).
target_include_directories(PatchKnob_buzz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(PatchKnob_buzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- standalone sampler self-tests (only when this dir is the top-level build) ---
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    foreach(t sine_render_test wav_playback_test sampler_multinote_test
              sampler_saveload_test sampler_meow_test)
        add_executable(${t} ${t}.cpp)
        target_link_libraries(${t} PRIVATE PatchKnob_buzz)
    endforeach()
    enable_testing()
    add_test(NAME sine_render_test    COMMAND sine_render_test)
    add_test(NAME sampler_saveload_test COMMAND sampler_saveload_test)
endif()
