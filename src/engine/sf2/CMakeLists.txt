cmake_minimum_required(VERSION 3.16)
project(PatchKnob_sf2 LANGUAGES CXX)

# The mapping layer targets the sampler's zone API. When this directory is
# pulled in standalone (this test's own CI/dev build) that target doesn't
# exist yet; when it's pulled in from sdlui/CMakeLists.txt (which already
# add_subdirectory()s engine/sampler) it does -- the guard makes both work.
if(NOT TARGET PatchKnob_sampler)
    # A binary dir private to this fallback (not "e_sampler") so it can never
    # collide with a sibling add_subdirectory(engine/sampler ".../e_sampler")
    # a parent CMakeLists (e.g. sdlui/CMakeLists.txt) adds independently,
    # regardless of which one runs first.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sampler
                     ${CMAKE_CURRENT_BINARY_DIR}/e_sampler_via_sf2)
endif()

# sf2_reader.cpp is the SoundFont parser (owned separately; left untouched
# here). sf2_to_sampler.cpp is the mapping layer that turns a parsed preset
# into sampler zones. Both live in one static lib so the mapping layer's
# consumers (the sampler-drop UI) link one thing.
# sf2_import_service.cpp is the asynchronous import service (worker-thread
# prepare + message-thread install split); it needs std::thread, hence Threads.
find_package(Threads REQUIRED)
# sf2_writer.cpp joins the library so the reader suite can prove an EXPORT
# re-reads (24-bit sm24 in particular) rather than testing one direction only.
add_library(PatchKnob_sf2 STATIC sf2_reader.cpp sf2_to_sampler.cpp
                                 sf2_import_service.cpp sf2_writer.cpp)
target_compile_features(PatchKnob_sf2 PUBLIC cxx_std_17)
target_include_directories(PatchKnob_sf2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(PatchKnob_sf2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(PatchKnob_sf2 PUBLIC PatchKnob_sampler Threads::Threads)

# Headless engine tests (SoundFont reader parity + the SF2 -> sampler mapping
# layer: interning, stereo pairing, zone-cap, missing-sample handling). OFF by
# default so the app build is unaffected; configure with
# -DPATCHKNOB_SF2_TESTS=ON to get the binaries.
option(PATCHKNOB_SF2_TESTS "Build the headless SF2 engine tests" OFF)
if(PATCHKNOB_SF2_TESTS)
    add_executable(PatchKnob_sf2_reader_test sf2_reader_test.cpp)
    target_link_libraries(PatchKnob_sf2_reader_test PRIVATE PatchKnob_sf2)

    add_executable(PatchKnob_sf2_to_sampler_test sf2_to_sampler_test.cpp)
    target_link_libraries(PatchKnob_sf2_to_sampler_test PRIVATE PatchKnob_sf2 PatchKnob_sampler)
    target_include_directories(PatchKnob_sf2_to_sampler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Async import service: supersede/cancel correctness (a stale prepare must
    # never install), progress reporting, prefetch preemption.
    add_executable(PatchKnob_sf2_import_service_test sf2_import_service_test.cpp)
    target_link_libraries(PatchKnob_sf2_import_service_test PRIVATE PatchKnob_sf2 PatchKnob_sampler)
    target_include_directories(PatchKnob_sf2_import_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()
