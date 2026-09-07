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
# libpd's own CMakeLists.txt only appends "-static" to the static target's
# OUTPUT_NAME on WIN32 (see its "set_target_properties(libpd_static ...
# OUTPUT_NAME ..." branch) -- a native Linux/macOS build of the same PD_MULTI
# static lib comes out as libpd-multi.a instead. Accept either.
set(LIBPD_LIB_WIN     "${LIBPD_DIR}/build/libs/liblibpd-multi-static.a")
set(LIBPD_LIB_NATIVE  "${LIBPD_DIR}/build/libs/libpd-multi.a")
if(EXISTS "${LIBPD_LIB_WIN}")
    set(LIBPD_LIB "${LIBPD_LIB_WIN}")
else()
    set(LIBPD_LIB "${LIBPD_LIB_NATIVE}")
endif()
if(ANDROID)
    set(PD_MULTI ON CACHE BOOL "" FORCE)
    set(PD_UTILS OFF CACHE BOOL "" FORCE)
    set(PD_EXTRA ON CACHE BOOL "" FORCE)
    set(LIBPD_STATIC ON CACHE BOOL "" FORCE)
    set(LIBPD_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LIBPD_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/vendor_libpd"
                     EXCLUDE_FROM_ALL)
endif()

# --- static library ---------------------------------------------------------
add_library(PatchKnob_patch STATIC
    patch_graph.cpp
    patch_nodes.cpp
    pd_node.cpp
    csound_node.cpp
    csound_dry_compile.cpp
    remote_node.cpp
    rack_ext_modules.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../rack   # rack.hpp / rack_factory.h for rack_ext_modules.cpp
    ${LIBPD_DIR}/libpd_wrapper
    ${LIBPD_DIR}/pure-data/src
)

# libpd was built with PD_MULTI -> PDINSTANCE/PDTHREADS change m_pd.h struct
# layout, so the consumer (pd_node.cpp) must define them identically.  PD_INTERNAL
# flips m_pd.h's EXTERN from __declspec(dllimport) (expects a DLL) to a plain
# symbol so we can link the STATIC libpd.
target_compile_definitions(PatchKnob_patch PRIVATE PDINSTANCE PDTHREADS PD_INTERNAL)

# Link the static libpd + its Windows deps (Pd uses winsock).
if(ANDROID)
    target_link_libraries(PatchKnob_patch PUBLIC libpd_static)
elseif(EXISTS "${LIBPD_LIB}")
    target_link_libraries(PatchKnob_patch PUBLIC "${LIBPD_LIB}" pthread)
    if(WIN32)
        target_link_libraries(PatchKnob_patch PUBLIC ws2_32)
    endif()
else()
    message(WARNING "libpd static lib not found at ${LIBPD_LIB}; PdNode will not link")
endif()

# --- embedded Csound (.csd) node --------------------------------------------
# CsoundNode links a Csound library.  CSOUND_ROOT defaults to the standard
# Windows install but can be pointed at a vendored/self-built Csound tree
# (set -DCSOUND_ROOT=<path> with <path>/include/csound/csound.h and a
# <path>/lib import/static lib).  When absent the node compiles inert.
if(ANDROID)
    # Build the pinned vendored Csound source as an ARM shared library. Optional
    # desktop-only utilities, tests, network opcodes, and gettext are omitted.
    set(BUILD_UTILITIES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
    set(BUILD_MULTI_CORE ON CACHE BOOL "" FORCE)
    set(USE_DOUBLE OFF CACHE BOOL "" FORCE)
    set(USE_CURL OFF CACHE BOOL "" FORCE)
    set(USE_GETTEXT OFF CACHE BOOL "" FORCE)
    set(USE_GIT_COMMIT OFF CACHE BOOL "" FORCE)
    set(BUILD_INSTALLER OFF CACHE BOOL "" FORCE)
    set(BUILD_RELEASE ON CACHE BOOL "" FORCE)
    set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(LIBSNDFILE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/libsndfile"
                     "${CMAKE_CURRENT_BINARY_DIR}/vendor_sndfile" EXCLUDE_FROM_ALL)
    set(LIBSNDFILE_LIBRARY sndfile CACHE STRING "Vendored libsndfile target" FORCE)
    set(SNDFILE_H_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/libsndfile/include"
        CACHE PATH "Vendored libsndfile headers" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound"
                     "${CMAKE_CURRENT_BINARY_DIR}/vendor_csound" EXCLUDE_FROM_ALL)
    target_include_directories(PatchKnob_patch PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound/H"
        "${CMAKE_CURRENT_BINARY_DIR}/vendor_csound/include")
    # USE_DOUBLE=OFF names the Csound shared target "csound" (the double
    # precision desktop build is "csound64").
    target_link_libraries(PatchKnob_patch PUBLIC csound)
    target_compile_definitions(PatchKnob_patch PRIVATE PATCHKNOB_HAVE_CSOUND)
    set(CSOUND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound")
elseif(UNIX)
    # Desktop Linux/macOS: no system Csound install to assume (unlike Windows,
    # where CSOUND_ROOT below defaults to the official installer's path), and
    # Csound is LGPL -- static-linking the vendored source into this GPL
    # binary needs no shared-library carve-out. Same recipe as the ANDROID
    # branch above, minus the mobile-specific bits, with USE_DOUBLE ON so the
    # produced target is named "csound64" like the Windows DLL build expects.
    if(NOT DEFINED CSOUND_ROOT OR CSOUND_ROOT STREQUAL "" OR NOT EXISTS "${CSOUND_ROOT}/include/csound/csound.h")
        set(BUILD_UTILITIES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # Csound's own add_library() for the main "csound64" target hardcodes
        # SHARED regardless of BUILD_SHARED_LIBS -- BUILD_STATIC_LIBRARY is
        # what actually produces the static "csound64-static" target this
        # links against. BUILD_SHARED_LIBS is left OFF anyway so vendored
        # libsndfile doesn't ALSO produce a .so nothing here uses.
        set(BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
        set(BUILD_MULTI_CORE ON CACHE BOOL "" FORCE)
        set(USE_DOUBLE ON CACHE BOOL "" FORCE)
        set(USE_CURL OFF CACHE BOOL "" FORCE)
        set(USE_GETTEXT OFF CACHE BOOL "" FORCE)
        set(USE_GIT_COMMIT OFF CACHE BOOL "" FORCE)
        set(BUILD_INSTALLER OFF CACHE BOOL "" FORCE)
        set(BUILD_RELEASE ON CACHE BOOL "" FORCE)
        set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(LIBSNDFILE_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        # The shared "csound64" target still gets built alongside the static
        # one (Csound's CMake doesn't offer a way to skip it) and needs every
        # constituent object -- including vendored libsndfile's -- compiled
        # -fPIC to link as a .so. Force it on for this whole subtree rather
        # than track down each culprit archive.
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        # Both vendored trees predate modern CMake's minimum-supported policy
        # floor; this only relaxes the "too old, refusing to configure" check
        # for their own cmake_minimum_required(), not this project's.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/libsndfile"
                         "${CMAKE_CURRENT_BINARY_DIR}/vendor_sndfile" EXCLUDE_FROM_ALL)
        set(LIBSNDFILE_LIBRARY sndfile CACHE STRING "Vendored libsndfile target" FORCE)
        set(SNDFILE_H_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/libsndfile/include"
            CACHE PATH "Vendored libsndfile headers" FORCE)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound"
                         "${CMAKE_CURRENT_BINARY_DIR}/vendor_csound" EXCLUDE_FROM_ALL)
        target_include_directories(PatchKnob_patch PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/csound/H"
            "${CMAKE_CURRENT_BINARY_DIR}/vendor_csound/include")
        # USE_DOUBLE=ON names it "csound64" (single precision would be "csound");
        # "-static" is BUILD_STATIC_LIBRARY's separate target for the same source.
        target_link_libraries(PatchKnob_patch PUBLIC csound64-static)
        target_compile_definitions(PatchKnob_patch PRIVATE PATCHKNOB_HAVE_CSOUND)
        message(STATUS "Csound found (CsoundNode enabled): vendored, built from source")
    else()
        # CSOUND_ROOT was pointed at a real install/prefix (e.g. a system
        # package) -- link that instead of building from source.
        target_include_directories(PatchKnob_patch PRIVATE "${CSOUND_ROOT}/include")
        find_library(CSOUND_LIB NAMES csound64 csound HINTS "${CSOUND_ROOT}/lib" "${CSOUND_ROOT}/lib64")
        if(CSOUND_LIB)
            target_link_libraries(PatchKnob_patch PUBLIC "${CSOUND_LIB}")
            target_compile_definitions(PatchKnob_patch PRIVATE PATCHKNOB_HAVE_CSOUND)
            message(STATUS "Csound found (CsoundNode enabled): ${CSOUND_LIB}")
        else()
            message(WARNING "CSOUND_ROOT=${CSOUND_ROOT} has no lib/libcsound64|csound; CsoundNode will be inert")
        endif()
    endif()
else()
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

add_executable(rack_ext_test rack_ext_test.cpp)
target_link_libraries(rack_ext_test PRIVATE PatchKnob_patch PatchKnob_rack)
target_compile_options(rack_ext_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME patch_test COMMAND patch_test)
add_test(NAME patch_parallel_test COMMAND patch_parallel_test)
add_test(NAME patch_nodes_test COMMAND patch_nodes_test)
add_test(NAME remote_test COMMAND remote_test)
add_test(NAME rack_ext_test COMMAND rack_ext_test)
