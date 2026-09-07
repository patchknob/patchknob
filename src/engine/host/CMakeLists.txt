# src/engine/host/CMakeLists.txt
#
# PatchKnob unified plugin host + scanner (ZERO-JUCE, mingw64).
#
# Targets:
#   PatchKnob_pluginhost  : static library implementing PatchKnob::engine::IPluginHost
#                       (PluginHost). The rest of the engine links this.
#   probe_vst2        : out-of-process VST2 introspection helper exe.
#   probe_vst3        : out-of-process VST3 introspection helper exe (compiles
#                       the same minimal Steinberg SDK hosting subset the vst3
#                       spike uses).
#   scan_test         : standalone inventory test exe. Links PatchKnob_pluginhost
#                       with WEAK stub plugin factories so it links without the
#                       real vst2/vst3 host modules. Runs scan({}) and prints
#                       the full plugin inventory.
#
# The scanner locates the probe exes next to the running executable, so all
# three probe/test binaries are placed in the same output directory.
#
# Build (mingw64):
#   export PATH="/c/msys64/mingw64/bin:$PATH"
#   cmake -G Ninja -S . -B build
#   cmake --build build
#   ./build/scan_test.exe
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_pluginhost CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep all runtime artifacts together so PluginHost::findProbeExe() locates the
# probe helpers next to scan_test.exe.
set(OUTDIR ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTDIR})

# Spike roots (read-only references).
set(SPIKES ${CMAKE_CURRENT_SOURCE_DIR}/../../../spikes)
set(VST3SDK ${SPIKES}/vst3host/vst3sdk)

# -----------------------------------------------------------------------------
# 1. The host library.
# -----------------------------------------------------------------------------
add_library(PatchKnob_pluginhost STATIC
    plugin_host.cpp
    ../native/native_effects.cpp
)
if(DEFINED PATCHKNOB_ENABLE_VST AND NOT PATCHKNOB_ENABLE_VST)
    # Keep the sampler/host inventory API, but satisfy the two factory symbols
    # with inert stubs. Android never builds, scans, loads, or packages VSTs.
    target_compile_definitions(PatchKnob_pluginhost PRIVATE PATCHKNOB_HOST_WEAK_STUBS)
endif()
target_include_directories(PatchKnob_pluginhost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..      # for plugin_api.h
)

# -----------------------------------------------------------------------------
# 2. Probe helpers.
#    probe_vst2 loads a plugin via the OS loader directly -- LoadLibrary for
#    Windows .dll VST2 plugins, dlopen for Linux/macOS .so/.dylib ones.
#    probe_vst3 uses the portable SDK hosting API (Module::create()), so it
#    builds on every platform the vendored SDK has a module_<os>.cpp for.
# -----------------------------------------------------------------------------
add_executable(probe_vst2 probe_vst2.cpp)
target_compile_options(probe_vst2 PRIVATE -O2 -Wno-multichar)
if(NOT WIN32)
    target_link_libraries(probe_vst2 PRIVATE ${CMAKE_DL_LIBS})
endif()

if(EXISTS ${VST3SDK}/pluginterfaces/vst/ivstaudioprocessor.h)
    if(WIN32)
        set(PROBE_VST3_MODULE_SOURCE    ${VST3SDK}/public.sdk/source/vst/hosting/module_win32.cpp)
        set(PROBE_VST3_THREADCHK_SOURCE ${VST3SDK}/public.sdk/source/common/threadchecker_win32.cpp)
    elseif(APPLE)
        set(PROBE_VST3_MODULE_SOURCE    ${VST3SDK}/public.sdk/source/vst/hosting/module_mac.mm)
        set(PROBE_VST3_THREADCHK_SOURCE ${VST3SDK}/public.sdk/source/common/threadchecker_linux.cpp)
    else()
        set(PROBE_VST3_MODULE_SOURCE    ${VST3SDK}/public.sdk/source/vst/hosting/module_linux.cpp)
        set(PROBE_VST3_THREADCHK_SOURCE ${VST3SDK}/public.sdk/source/common/threadchecker_linux.cpp)
    endif()
    set(SDK_SOURCES
        ${VST3SDK}/pluginterfaces/base/funknown.cpp
        ${VST3SDK}/pluginterfaces/base/coreiids.cpp
        ${VST3SDK}/pluginterfaces/base/conststringtable.cpp
        ${VST3SDK}/pluginterfaces/base/ustring.cpp
        ${VST3SDK}/base/source/baseiids.cpp
        ${VST3SDK}/base/source/fbuffer.cpp
        ${VST3SDK}/base/source/fdebug.cpp
        ${VST3SDK}/base/source/fdynlib.cpp
        ${VST3SDK}/base/source/fobject.cpp
        ${VST3SDK}/base/source/fstreamer.cpp
        ${VST3SDK}/base/source/fstring.cpp
        ${VST3SDK}/base/source/updatehandler.cpp
        ${VST3SDK}/base/thread/source/fcondition.cpp
        ${VST3SDK}/base/thread/source/flock.cpp
        ${VST3SDK}/public.sdk/source/common/commoniids.cpp
        ${VST3SDK}/public.sdk/source/common/commonstringconvert.cpp
        ${VST3SDK}/public.sdk/source/common/memorystream.cpp
        ${VST3SDK}/public.sdk/source/common/pluginview.cpp
        ${PROBE_VST3_THREADCHK_SOURCE}
        ${VST3SDK}/public.sdk/source/vst/vstinitiids.cpp
        ${VST3SDK}/public.sdk/source/vst/hosting/module.cpp
        ${PROBE_VST3_MODULE_SOURCE}
        ${VST3SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${VST3SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
        ${VST3SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
        ${VST3SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
        ${VST3SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    )

    add_executable(probe_vst3 probe_vst3.cpp ${SDK_SOURCES})
    if(WIN32)
        # Restart-safe filename: a crashed/hung out-of-process scanner can
        # keep probe_vst3.exe file-locked on Windows, which otherwise blocks
        # rebuilding the main application in place. Linux has no such lock.
        set_target_properties(probe_vst3 PROPERTIES OUTPUT_NAME probe_vst3_next)
    endif()
    target_include_directories(probe_vst3 PRIVATE ${VST3SDK})
    target_compile_options(probe_vst3 PRIVATE -Wno-unknown-pragmas -Wno-multichar)
    if(WIN32)
        target_compile_definitions(probe_vst3 PRIVATE
            UNICODE _UNICODE _WIN32_WINNT=0x0601 RELEASE=1)
        target_link_libraries(probe_vst3 PRIVATE ole32 uuid shlwapi)
    else()
        target_compile_definitions(probe_vst3 PRIVATE RELEASE=1)
        find_package(Threads REQUIRED)
        target_link_libraries(probe_vst3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()
else()
    message(WARNING "VST3 SDK not found at ${VST3SDK}; probe_vst3 will not be built. "
                    "VST3 scanning will be unavailable at runtime.")
endif()

# -----------------------------------------------------------------------------
# 4. Standalone inventory test (weak stub factories).
# -----------------------------------------------------------------------------
add_executable(scan_test
    scan_test.cpp
    # Recompile plugin_host.cpp here with PATCHKNOB_HOST_WEAK_STUBS so the test
    # gets weak createVst2/3Instance definitions. (Linking the static lib AND
    # the stub object would otherwise duplicate symbols.)
    plugin_host.cpp
    ../native/native_effects.cpp
)
target_compile_definitions(scan_test PRIVATE PATCHKNOB_HOST_WEAK_STUBS)
target_include_directories(scan_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(TARGET probe_vst2)
    add_dependencies(scan_test probe_vst2)
endif()
if(TARGET probe_vst3)
    add_dependencies(scan_test probe_vst3)
endif()
