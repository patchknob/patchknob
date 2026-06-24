cmake_minimum_required(VERSION 3.16)
project(spike_vst3host CXX)

# -----------------------------------------------------------------------------
# ZERO-JUCE VST3 host spike (mingw64 / g++).
#
# We do NOT use the VST3 SDK's own CMake build (it pulls in vstgui, validators,
# samples, etc.). Instead we compile the minimal set of SDK hosting + base
# sources directly. This keeps the spike self-contained and makes the mingw
# workarounds explicit.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk)

if(NOT EXISTS ${SDK}/pluginterfaces/vst/ivstaudioprocessor.h)
    message(FATAL_ERROR "VST3 SDK not found at ${SDK}. Clone it first.")
endif()

# Include roots. The SDK includes are written relative to the repo root
# (e.g. "pluginterfaces/base/funknown.h" and
# "public.sdk/source/vst/hosting/module.h"), so the SDK root is the include dir.
include_directories(${SDK})

# --- Minimal SDK source set needed for module loading + hosting --------------
set(SDK_SOURCES
    # pluginterfaces (IIDs + FUnknown plumbing)
    ${SDK}/pluginterfaces/base/funknown.cpp
    ${SDK}/pluginterfaces/base/coreiids.cpp
    ${SDK}/pluginterfaces/base/conststringtable.cpp
    ${SDK}/pluginterfaces/base/ustring.cpp

    # base
    ${SDK}/base/source/baseiids.cpp
    ${SDK}/base/source/fbuffer.cpp
    ${SDK}/base/source/fdebug.cpp
    ${SDK}/base/source/fdynlib.cpp
    ${SDK}/base/source/fobject.cpp
    ${SDK}/base/source/fstreamer.cpp
    ${SDK}/base/source/fstring.cpp
    ${SDK}/base/source/updatehandler.cpp
    ${SDK}/base/thread/source/fcondition.cpp
    ${SDK}/base/thread/source/flock.cpp

    # public.sdk common
    ${SDK}/public.sdk/source/common/commoniids.cpp
    ${SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${SDK}/public.sdk/source/common/memorystream.cpp
    ${SDK}/public.sdk/source/common/pluginview.cpp
    ${SDK}/public.sdk/source/common/threadchecker_win32.cpp

    # CRITICAL: defines all Steinberg::Vst::*::iid symbols. Must be compiled
    # exactly once by the host. Without it the VST interface IIDs are
    # undefined references at link time.
    ${SDK}/public.sdk/source/vst/vstinitiids.cpp

    # public.sdk vst hosting
    ${SDK}/public.sdk/source/vst/hosting/module.cpp
    ${SDK}/public.sdk/source/vst/hosting/module_win32.cpp
    ${SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${SDK}/public.sdk/source/vst/utility/stringconvert.cpp
)

add_executable(spike_vst3host
    spike_vst3host.cpp
    ${SDK_SOURCES}
)

# --- mingw64 / Windows defines -----------------------------------------------
# UNICODE/_UNICODE: SDK Win32 code uses wide-char Win32 APIs.
# _WIN32_WINNT: ensure modern Win32 API surface is visible.
target_compile_definitions(spike_vst3host PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0601   # Windows 7+
)

# Release-style: the SDK's fdebug uses DEVELOPMENT/RELEASE switches.
target_compile_definitions(spike_vst3host PRIVATE RELEASE=1)

# mingw: the SDK's COM/GUID code and module_win32 need ole32/uuid for
# CoInitialize and known IIDs; shlwapi for path helpers.
target_link_libraries(spike_vst3host PRIVATE
    ole32
    uuid
    shlwapi
)

# Quiet the SDK's heavy warning surface for the spike.
target_compile_options(spike_vst3host PRIVATE -Wno-unknown-pragmas -Wno-multichar)
