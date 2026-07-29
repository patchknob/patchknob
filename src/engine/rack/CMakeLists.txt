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
else()
    file(WRITE ${FUNDAMENTAL_BRIDGE_SOURCE}
"#include \"fundamental_bridge.h\"\nnamespace rackx { namespace fundamental {\n")
    foreach(fn makeVCO makeVCF makeVCA1 makeADSR makeLFO makeMixer make8vert makeMerge makeMidSide makeOctave makeSplit makeSum makeVCA2 makeVCMixer makeMutes makePulses makeRandom makeSEQ3 makeSequentialSwitch1 makeSequentialSwitch2 makeStable16)
        file(APPEND ${FUNDAMENTAL_BRIDGE_SOURCE}
"std::unique_ptr<rack::engine::Module> ${fn}(){ return nullptr; }\n")
    endforeach()
    file(APPEND ${FUNDAMENTAL_BRIDGE_SOURCE} "} }\n")
endif()

add_library(PatchKnob_rack STATIC
    rack_factory.cpp
    rack_dynamic_modules.cpp
    rack_engine.cpp
    rack_node.cpp
    rack_modules.cpp
    rack_modules_ext.cpp
    rack_modules_cardinal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../vst2/seh_guard.cpp
    ${FUNDAMENTAL_BRIDGE_SOURCE}
)

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
)

target_compile_options(PatchKnob_rack PRIVATE -Wall)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_compile_options(PatchKnob_rack PRIVATE -msse4.2)
endif()

# --- test executable --------------------------------------------------------
# Headless self-test + 500-module scaling bench.  Uses RackEngine directly (no
# RackNode), so rack_node.o and the patch-graph library are never pulled in.
add_executable(rack_test rack_test.cpp)
target_link_libraries(rack_test PRIVATE PatchKnob_rack)
target_compile_options(rack_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME rack_test COMMAND rack_test)
