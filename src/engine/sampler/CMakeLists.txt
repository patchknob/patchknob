cmake_minimum_required(VERSION 3.16)
project(PatchKnob_sampler LANGUAGES CXX)

add_library(PatchKnob_sampler STATIC samplerinstrument.cpp)
target_compile_features(PatchKnob_sampler PUBLIC cxx_std_17)
target_include_directories(PatchKnob_sampler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(PatchKnob_sampler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Headless engine tests (declick/click metrics, per-tracker-column FX isolation,
# and per-zone SF2-parity parameters).  OFF by default so the app build is
# unaffected; configure with -DPATCHKNOB_SAMPLER_TESTS=ON to get the binary.
option(PATCHKNOB_SAMPLER_TESTS "Build the headless sampler engine tests" OFF)
if(PATCHKNOB_SAMPLER_TESTS)
    add_executable(PatchKnob_sampler_test sampler_declick_test.cpp)
    target_link_libraries(PatchKnob_sampler_test PRIVATE PatchKnob_sampler)
    target_include_directories(PatchKnob_sampler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Headless performance benchmark: synthesises the 2976-zone concert-grand
    # shape from the large-soundfont regression and times load / setters /
    # note-on lookup / render / read-back / save, plus the worst mutex stall a
    # concurrent audio thread would see.  Same option gates it as the tests.
    find_package(Threads REQUIRED)
    add_executable(PatchKnob_sampler_perf sampler_perf_bench.cpp)
    target_link_libraries(PatchKnob_sampler_perf PRIVATE PatchKnob_sampler Threads::Threads)
    target_include_directories(PatchKnob_sampler_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()
