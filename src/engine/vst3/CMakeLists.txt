cmake_minimum_required(VERSION 3.16)
project(PatchKnob_vst3 CXX)

# -----------------------------------------------------------------------------
# PatchKnob - VST3 HOST module (ZERO-JUCE, mingw64 / g++).
#
# Builds:
#   * static lib  PatchKnob_vst3   (Vst3PluginInstance + the minimal VST3 SDK
#                               hosting sources, mirroring the proven spike)
#   * test exe    vst3_test    (loads an instrument, sends a note, checks audio)
#
# We compile only the minimal SDK hosting subset directly (not the SDK's own
# CMake which pulls in VSTGUI/validator/samples). See HOSTING_NOTES.md.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate the VST3 SDK relative to this module (lives under spikes/vst3host).
set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../../spikes/vst3host/vst3sdk")
get_filename_component(SDK "${SDK}" ABSOLUTE)

if(NOT EXISTS "${SDK}/pluginterfaces/vst/ivstaudioprocessor.h")
    message(FATAL_ERROR "VST3 SDK not found at ${SDK}.")
endif()

# SDK includes are written relative to the repo root.
include_directories("${SDK}")
# Engine headers (plugin_api.h is one dir up).
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/..")

# --- Minimal SDK source set (mirrors spikes/vst3host/CMakeLists.txt) ----------
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

    # CRITICAL: defines all Steinberg::Vst::*::iid symbols. Compile exactly once.
    ${SDK}/public.sdk/source/vst/vstinitiids.cpp

    # public.sdk vst hosting
    ${SDK}/public.sdk/source/vst/hosting/module.cpp
    ${SDK}/public.sdk/source/vst/hosting/module_win32.cpp
    ${SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${SDK}/public.sdk/source/vst/hosting/eventlist.cpp
    ${SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${SDK}/public.sdk/source/vst/hosting/processdata.cpp
    ${SDK}/public.sdk/source/vst/utility/stringconvert.cpp
)

# --- static lib: PatchKnob_vst3 ---------------------------------------------------
add_library(PatchKnob_vst3 STATIC
    vst3_host.cpp
    ${SDK_SOURCES}
)

target_include_directories(PatchKnob_vst3 PUBLIC
    "${SDK}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# mingw64 / Windows defines (proven in the spike).
target_compile_definitions(PatchKnob_vst3 PUBLIC
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0601
    RELEASE=1
)

target_compile_options(PatchKnob_vst3 PRIVATE -Wno-unknown-pragmas -Wno-multichar)

target_link_libraries(PatchKnob_vst3 PUBLIC ole32 uuid shlwapi)

# --- test exe: vst3_test ------------------------------------------------------
add_executable(vst3_test vst3_test.cpp)
target_link_libraries(vst3_test PRIVATE PatchKnob_vst3)
target_compile_options(vst3_test PRIVATE -Wno-unknown-pragmas -Wno-multichar)
