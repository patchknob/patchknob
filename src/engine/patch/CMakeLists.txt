#-----------------------------------------------------------------------------
# PatchKnob — modular patch-graph engine module.
#
#   * PatchKnob_patch : static library (PatchGraph + built-in nodes).
#   * patch_test  : standalone self-test exe (no real plugins required).
#
# Self-contained so the module can be built on its own:
#       cmake -G "MinGW Makefiles" -B build -S .
#       cmake --build build
#       ./build/patch_test
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_patch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- embedded Pure Data (libpd, built static multi-instance) ----------------
set(LIBPD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/libpd")
set(LIBPD_LIB "${LIBPD_DIR}/build/libs/liblibpd-multi-static.a")

# --- static library ---------------------------------------------------------
add_library(PatchKnob_patch STATIC
    patch_graph.cpp
    patch_nodes.cpp
    pd_node.cpp
    csound_node.cpp
    remote_node.cpp
)

# remote_node uses UDP sockets (Winsock2 on Windows) -- link unconditionally,
# not just when libpd is present.
if(WIN32)
    target_link_libraries(PatchKnob_patch PUBLIC ws2_32)
endif()

# The parent engine dir is on the include path so "../plugin_api.h",
# "../graph/track.h" and "../graph/vu_meter.h" resolve. track.h / vu_meter.h are
# header-only from this module's perspective (RenderContext + VuMeter), so no
# link dependency on PatchKnob_graph is required.
target_include_directories(PatchKnob_patch PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../graph
    ${LIBPD_DIR}/libpd_wrapper
    ${LIBPD_DIR}/pure-data/src
)

# libpd was built with PD_MULTI -> PDINSTANCE/PDTHREADS change m_pd.h struct
# layout, so the consumer (pd_node.cpp) must define them identically.  PD_INTERNAL
# flips m_pd.h's EXTERN from __declspec(dllimport) (expects a DLL) to a plain
# symbol so we can link the STATIC libpd.
target_compile_definitions(PatchKnob_patch PRIVATE PDINSTANCE PDTHREADS PD_INTERNAL)

# Link the static libpd + its Windows deps (Pd uses winsock).
if(EXISTS "${LIBPD_LIB}")
    target_link_libraries(PatchKnob_patch PUBLIC "${LIBPD_LIB}" ws2_32 pthread)
else()
    message(WARNING "libpd static lib not found at ${LIBPD_LIB}; PdNode will not link")
endif()

# --- embedded Csound (.csd) node --------------------------------------------
# CsoundNode links a Csound library.  CSOUND_ROOT defaults to the standard
# Windows install but can be pointed at a vendored/self-built Csound tree
# (set -DCSOUND_ROOT=<path> with <path>/include/csound/csound.h and a
# <path>/lib import/static lib).  When absent the node compiles inert.
set(CSOUND_ROOT "C:/Program Files/Csound6_x64" CACHE PATH "Csound install or vendored root")
set(_cs_inc "${CSOUND_ROOT}/include")
# IMPORTANT: prefer the DLL *import* lib (csound64.lib), NOT the MSVC *static* lib
# (libcsound64.lib).  The static lib carries MSVC-CRT object code (fprintf,
# __security_cookie, __GSHandlerCheck, ...) that MinGW's ld cannot resolve; the
# import lib is just thunks into csound64.dll, which MinGW links cleanly.
set(CSOUND_LIB "")
foreach(_cand "${CSOUND_ROOT}/lib/csound64.lib"          # install import lib (MinGW-safe)
              "${CSOUND_ROOT}/lib/libcsound64.dll.a"      # MinGW-style import lib (vendored)
              "${CSOUND_ROOT}/lib/csound64.dll.a"
              "${CSOUND_ROOT}/bin/csound64.dll")          # last resort: link the DLL directly
    if(NOT CSOUND_LIB AND EXISTS "${_cand}")
        set(CSOUND_LIB "${_cand}")
    endif()
endforeach()
if(NOT CSOUND_LIB)
    find_library(CSOUND_LIB NAMES csound64 HINTS "${CSOUND_ROOT}/lib" "${CSOUND_ROOT}/build")
endif()
if(EXISTS "${_cs_inc}/csound/csound.h" AND CSOUND_LIB)
    target_include_directories(PatchKnob_patch PRIVATE "${_cs_inc}")
    target_link_libraries(PatchKnob_patch PUBLIC "${CSOUND_LIB}")
    target_compile_definitions(PatchKnob_patch PRIVATE PATCHKNOB_HAVE_CSOUND)
    message(STATUS "Csound found (CsoundNode enabled): ${CSOUND_LIB}")
else()
    message(WARNING "Csound not found under ${CSOUND_ROOT}; CsoundNode will be inert")
endif()

target_compile_options(PatchKnob_patch PRIVATE -Wall)

# --- test executables -------------------------------------------------------
add_executable(patch_test patch_test.cpp)
target_link_libraries(patch_test PRIVATE PatchKnob_patch)
target_compile_options(patch_test PRIVATE -Wall -Wextra)

# Parallel/hardening self-test: sequential-vs-parallel bit-exactness, 500-node
# capacity, throwing-node containment, RCU compile stress, gc epoch gating.
add_executable(patch_parallel_test patch_parallel_test.cpp)
target_link_libraries(patch_parallel_test PRIVATE PatchKnob_patch)
target_compile_options(patch_parallel_test PRIVATE -Wall -Wextra)

# Node-hardening self-test: RT-safe plugin swap, master-mixer RCU snapshots,
# NaN/Inf fences, frame clamps, sine sr guard.
add_executable(patch_nodes_test patch_nodes_test.cpp)
target_link_libraries(patch_nodes_test PRIVATE PatchKnob_patch)
target_compile_options(patch_nodes_test PRIVATE -Wall -Wextra)

# Distributed increment 1: RemoteNode UDP loopback (bit-exact round trip,
# loss concealment, no syscalls on the audio thread).
add_executable(remote_test remote_test.cpp)
target_link_libraries(remote_test PRIVATE PatchKnob_patch)
target_compile_options(remote_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME patch_test COMMAND patch_test)
add_test(NAME patch_parallel_test COMMAND patch_parallel_test)
add_test(NAME patch_nodes_test COMMAND patch_nodes_test)
add_test(NAME remote_test COMMAND remote_test)
