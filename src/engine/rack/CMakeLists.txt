#-----------------------------------------------------------------------------
# PatchKnob — VCV-Rack-style modular engine module.
#
#   * PatchKnob_rack : static library (Rack-compatible Module API + RackEngine +
#                  RackNode + built-in module set).  GUI-free; the SDL editor
#                  lives in the frontend and drives RackNode::engine() live.
#   * rack_test  : standalone headless self-test + 500-module scaling bench.
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_rack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_program(POWERSHELL_EXECUTABLE powershell.exe)
# PowerShell is Windows-only in practice (an AUR/third-party package on every
# Linux distro); generate_fundamental_dsp.py is a line-for-line port of the
# .ps1 generator's text transform, so Linux/macOS builds get real Fundamental
# DSP too instead of the nullptr-returning stub below.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
set(FUNDAMENTAL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/Cardinal/plugins/Fundamental/src)
set(STABLE16_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/Cardinal/plugins/GoodSheperd/src/Stable16.cpp)
set(FUNDAMENTAL_BRIDGE_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/fundamental_dsp_bridge.cpp)
set(FUNDAMENTAL_BRIDGE_INPUTS
    ${FUNDAMENTAL_SOURCE_DIR}/VCO.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/VCF.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/VCA-1.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/ADSR.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/LFO.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Mixer.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/8vert.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Merge.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/MidSide.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Octave.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Split.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Sum.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/VCA.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/VCMixer.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Mutes.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Pulses.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/Random.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/SEQ3.cpp
    ${FUNDAMENTAL_SOURCE_DIR}/SequentialSwitch.cpp
    ${STABLE16_SOURCE}
)
if(POWERSHELL_EXECUTABLE AND EXISTS "${FUNDAMENTAL_SOURCE_DIR}/VCO.cpp")
    add_custom_command(
        OUTPUT ${FUNDAMENTAL_BRIDGE_SOURCE}
        COMMAND ${POWERSHELL_EXECUTABLE} -ExecutionPolicy Bypass -File
                ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/generate_fundamental_dsp.ps1
                -SourceRoot ${FUNDAMENTAL_SOURCE_DIR}
                -Output ${FUNDAMENTAL_BRIDGE_SOURCE}
                -Stable16Source ${STABLE16_SOURCE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/generate_fundamental_dsp.ps1
                ${FUNDAMENTAL_BRIDGE_INPUTS}
        VERBATIM
    )
elseif(PYTHON3_EXECUTABLE AND EXISTS "${FUNDAMENTAL_SOURCE_DIR}/VCO.cpp")
    add_custom_command(
        OUTPUT ${FUNDAMENTAL_BRIDGE_SOURCE}
        COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/generate_fundamental_dsp.py
                --source-root ${FUNDAMENTAL_SOURCE_DIR}
                --output ${FUNDAMENTAL_BRIDGE_SOURCE}
                --stable16-source ${STABLE16_SOURCE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/generate_fundamental_dsp.py
                ${FUNDAMENTAL_BRIDGE_INPUTS}
        VERBATIM
    )
else()
    # makeVCO() etc. return unique_ptr<rack::engine::Module>, and that alone
    # forces the type complete at every point one is constructed/destroyed --
    # even a stub that only ever returns nullptr.  fundamental_bridge.h just
    # forward-declares Module (so callers of THESE functions don't need the
    # real Rack headers); this generated stub does need them, or GCC/Clang
    # reject the destructor with "invalid application of sizeof to
    # incomplete type" while building nothing but a null return.
    file(WRITE ${FUNDAMENTAL_BRIDGE_SOURCE}
"#include \"fundamental_bridge.h\"\n#include \"engine/Module.hpp\"\nnamespace rackx { namespace fundamental {\n")
    foreach(fn makeVCO makeVCF makeVCA1 makeADSR makeLFO makeMixer make8vert makeMerge makeMidSide makeOctave makeSplit makeSum makeVCA2 makeVCMixer makeMutes makePulses makeRandom makeSEQ3 makeSequentialSwitch1 makeSequentialSwitch2 makeStable16)
        file(APPEND ${FUNDAMENTAL_BRIDGE_SOURCE}
"std::unique_ptr<rack::engine::Module> ${fn}(){ return nullptr; }\n")
    endforeach()
    file(APPEND ${FUNDAMENTAL_BRIDGE_SOURCE} "} }\n")
endif()

set(PATCHKNOB_RACK_SOURCES
    rack_factory.cpp
    rack_dynamic_modules.cpp
    rack_engine.cpp
    rack_node.cpp
    rack_modules.cpp
    rack_vco4_sse.cpp
    rack_env8.cpp
    rack_buffer_retrig.cpp
    rack_zdf_multi.cpp
    rack_zplane.cpp
    rack_acid303.cpp
    rack_sampler.cpp
    rack_tb303_filter8.cpp
    rack_acid_sequencer.cpp
    rack_acid_oscillator.cpp
    rack_harmonic_forge.cpp
    rack_cdp_modules.cpp
    rack_modules_ext.cpp
    rack_modules_cardinal.cpp
    ${FUNDAMENTAL_BRIDGE_SOURCE}
)
# seh_guard.cpp is Windows-only (Vectored Exception Handling, windows.h) --
# and only ever called from rack_dynamic_modules.cpp's #ifdef _WIN32 bridge-
# loading path.  "NOT ANDROID" pulled it into Linux desktop builds too, where
# windows.h doesn't exist.
if(WIN32)
    list(APPEND PATCHKNOB_RACK_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../vst2/seh_guard.cpp)
endif()
add_library(PatchKnob_rack STATIC ${PATCHKNOB_RACK_SOURCES})
# The streamable CDP ports are exposed as rack modules (rack_cdp_modules.cpp).
# The shell may already have added the CDP dir; only add it when standing alone
# (rack_test builds this directory directly).
if(NOT TARGET PatchKnob_cdp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../cdp ${CMAKE_CURRENT_BINARY_DIR}/e_cdp EXCLUDE_FROM_ALL)
endif()
target_link_libraries(PatchKnob_rack PUBLIC PatchKnob_cdp PatchKnob_audioclip)

# Parent engine dir on the path so "../plugin_api.h" and "../patch/patch_graph.h"
# resolve.  Those are header-only from this module's view (RackNode only needs
# the Node base + NodeProcessContext), so no link dependency is required here;
# the final executable links PatchKnob_patch anyway.
target_include_directories(PatchKnob_rack PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../graph
    ${CMAKE_CURRENT_SOURCE_DIR}/../patch
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/Cardinal/src/Rack/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/Cardinal/src/Rack/dep/simde
)

target_compile_options(PatchKnob_rack PRIVATE -Wall)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_compile_options(PatchKnob_rack PRIVATE -msse4.2)
    # The 303 ladder bank uses one 256-bit AVX1 vector across its eight lanes.
    # Keep the rest of the engine on the broader SSE4.2 baseline, and retain the
    # scalar path on mobile/ARM.
    set_source_files_properties(rack_tb303_filter8.cpp PROPERTIES COMPILE_OPTIONS "-mavx")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm")
    # Rack's SSE API is translated to native NEON by the vendored SIMDe headers.
    # Do not enable SIMDE_NO_NATIVE: arm64 guarantees Advanced SIMD.
    target_compile_definitions(PatchKnob_rack PRIVATE
        SIMDE_ENABLE_NATIVE_ALIASES SIMDE_NATURAL_VECTOR_SIZE=128)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        target_compile_options(PatchKnob_rack PRIVATE -mfpu=neon)
    endif()
endif()

# --- test executable --------------------------------------------------------
# Headless self-test + 500-module scaling bench.  Uses RackEngine directly (no
# RackNode), so rack_node.o and the patch-graph library are never pulled in.
add_executable(rack_test rack_test.cpp)
target_link_libraries(rack_test PRIVATE PatchKnob_rack)
target_compile_options(rack_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME rack_test COMMAND rack_test)
