# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
cmake_minimum_required(VERSION 3.14)
project(openclon12)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

file(GLOB SRC CONFIGURE_DEPENDS src/*.cpp src/*.def)
file(GLOB INC include/*.h include/*.hpp)
file(GLOB EXTERNAL_INC external/*.h external/*.hpp)

FetchContent_Declare(
    d3d12translationlayer
    GIT_REPOSITORY https://github.com/microsoft/D3D12TranslationLayer.git
    GIT_TAG        3bc4d17fc83ccbbe341a3446b9f66999076b7a2b
)
FetchContent_MakeAvailable(d3d12translationlayer)

FetchContent_Declare(
    opencl_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenCL-Headers.git
    GIT_TAG 1bb9ec797d14abed6167e3a3d66ede25a702a5c7
)
FetchContent_MakeAvailable(opencl_headers)
add_library(OpenCL::Headers ALIAS Headers)

add_library(openclon12 SHARED ${SRC} ${INC} ${EXTERNAL_INC})
target_include_directories(openclon12
    PRIVATE include
    PRIVATE external
    PRIVATE wil/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(openclon12 d3d12translationlayer OpenCL::Headers)
source_group("Header Files\\External" FILES ${EXTERNAL_INC})

option(BUILD_TESTS "Build tests" ON)

if (BUILD_TESTS)
    add_subdirectory(test)
endif()
