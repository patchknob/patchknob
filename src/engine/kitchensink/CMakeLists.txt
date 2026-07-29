cmake_minimum_required(VERSION 3.10)
project(kitchensink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
	add_compile_options(-Wall)
endif()

# Static library with the ported tempo-map / transport math.
add_library(PatchKnob_kitchensink STATIC kitchensink.cpp)
target_include_directories(PatchKnob_kitchensink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test executable.
add_executable(kitchensink_test kitchensink_test.cpp)
target_link_libraries(kitchensink_test PRIVATE PatchKnob_kitchensink)

enable_testing()
add_test(NAME kitchensink_test COMMAND kitchensink_test)
