#-----------------------------------------------------------------------------
# PatchKnob — ai module: the ONLY part of the DAW that talks to the network.
#
#   * PatchKnob_ai  : JSON, an HTTPS POST shim, machine-bound API key storage,
#                     and the Claude Messages API client.
#   * ai_test       : headless self-test. Everything except the live request is
#                     covered offline, so the suite needs no key and no network.
#-----------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(PatchKnob_ai LANGUAGES CXX)

add_library(PatchKnob_ai STATIC
    json.cpp
    http_client.cpp
    api_key_store.cpp
    claude_client.cpp
    csound_check.cpp
    cli_backend.cpp
)
target_compile_features(PatchKnob_ai PUBLIC cxx_std_17)
target_include_directories(PatchKnob_ai PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# SDL supplies the per-user preferences directory the key is stored in.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_include_directories(PatchKnob_ai PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(PatchKnob_ai PUBLIC ${SDL2_LIBRARIES})
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2PC QUIET sdl2)
        if(SDL2PC_FOUND)
            target_include_directories(PatchKnob_ai PRIVATE ${SDL2PC_INCLUDE_DIRS})
            target_link_libraries(PatchKnob_ai PUBLIC ${SDL2PC_LIBRARIES})
        endif()
    endif()
endif()

if(WIN32)
    # WinHTTP and DPAPI both ship with Windows: no vendored dependency, and
    # nothing extra for a user to install.
    target_link_libraries(PatchKnob_ai PUBLIC winhttp crypt32)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(PatchKnob_ai PUBLIC CURL::libcurl)
endif()

find_package(Threads REQUIRED)
target_link_libraries(PatchKnob_ai PUBLIC Threads::Threads)

option(PATCHKNOB_AI_TESTS "Build the headless ai self-test" OFF)
if(PATCHKNOB_AI_TESTS)
    add_executable(ai_test ai_test.cpp)
    target_link_libraries(ai_test PRIVATE PatchKnob_ai)
endif()
