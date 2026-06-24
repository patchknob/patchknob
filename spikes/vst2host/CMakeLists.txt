# spikes/vst2host/CMakeLists.txt
#
# Standalone build for the ZERO-JUCE VST2 host spike.
# Self-contained: depends only on Win32 (LoadLibrary) and the clean-room
# vestige aeffectx.h header in ./vestige.
#
# Build (mingw64):
#   cmake -G "MinGW Makefiles" -S . -B build
#   cmake --build build
#
# Run:
#   build/spike_vst2host.exe "C:/Program Files/VstPlugins/808 Machine x64.dll"

cmake_minimum_required(VERSION 3.16)
project(spike_vst2host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(spike_vst2host spike_vst2host.cpp)
target_include_directories(spike_vst2host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# No extra libraries: Win32 LoadLibrary/GetProcAddress are in kernel32,
# linked by default with mingw64.
