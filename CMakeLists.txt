# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
cmake_minimum_required(VERSION 3.14)
project(openclon12)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS src/*.cpp src/*.def)
file(GLOB_RECURSE INC include/*.h include/*.hpp)
file(GLOB_RECURSE EXTERNAL_INC external/*.h external/*.hpp)

FetchContent_Declare(
    d3d12translationlayer
    GIT_REPOSITORY https://github.com/microsoft/D3D12TranslationLayer.git
    GIT_TAG        bea9842b008b25283e43955fbf8cf6b2e8755352
)
FetchContent_MakeAvailable(d3d12translationlayer)

FetchContent_Declare(
    opencl_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenCL-Headers.git
    GIT_TAG 1bb9ec797d14abed6167e3a3d66ede25a702a5c7
)
FetchContent_MakeAvailable(opencl_headers)
add_library(OpenCL::Headers ALIAS Headers)

set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG ed429e64eb3b91848bf19c17e1431c1b0f2c6d2b
)
FetchContent_MakeAvailable(wil)

add_library(openclon12 SHARED ${SRC} ${INC} ${EXTERNAL_INC})
target_include_directories(openclon12
    PRIVATE include
    PRIVATE external
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(openclon12 d3d12translationlayer OpenCL::Headers WIL)
source_group("Header Files\\External" FILES ${EXTERNAL_INC})

option(BUILD_TESTS "Build tests" ON)

if (BUILD_TESTS)
    add_subdirectory(test)
endif()
