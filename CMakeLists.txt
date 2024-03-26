# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
cmake_minimum_required(VERSION 3.14)
project(openclon12)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

file(GLOB_RECURSE MAIN_SRC CONFIGURE_DEPENDS src/openclon12/*.cpp src/compilers/*.cpp src/openclon12/*.def)
file(GLOB MAIN_INC src/openclon12/*.h src/openclon12/*.hpp)
file(GLOB_RECURSE EXTERNAL_INC external/*.h external/*.hpp)

file(GLOB TRANS_SRC CONFIGURE_DEPENDS src/d3d12translationlayer/*.cpp)
file(GLOB TRANS_INC include/d3d12translationlayer/*.*)
file(GLOB TRANS_INL include/d3d12translationlayer/*.inl src/d3d12translationlayer/*.inl)

include(FetchContent)
FetchContent_Declare(
    DirectX-Headers
    GIT_REPOSITORY https://github.com/Microsoft/DirectX-Headers.git
    GIT_TAG v1.610.0
)
FetchContent_MakeAvailable(DirectX-Headers)

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

add_library(d3d12translationlayer STATIC ${TRANS_SRC} ${TRANS_INC} ${TRANS_INL})
target_include_directories(d3d12translationlayer
    PUBLIC include/d3d12translationlayer
    PRIVATE external)

target_link_libraries(d3d12translationlayer Microsoft::DirectX-Headers d3d12 dxgi atls dxcore)

target_compile_definitions(d3d12translationlayer PRIVATE $<$<CONFIG:DEBUG>:DBG>)
target_compile_definitions(d3d12translationlayer PUBLIC $<$<CONFIG:DEBUG>:TRANSLATION_LAYER_DBG=1>)

add_library(openclon12 SHARED ${MAIN_SRC} ${MAIN_INC} ${EXTERNAL_INC})
target_include_directories(openclon12
    PRIVATE src/openclon12
    PRIVATE external
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(openclon12
    d3d12translationlayer
    OpenCL::Headers
    WIL
    user32
    gdi32)
source_group("Header Files\\External" FILES ${EXTERNAL_INC})

option(BUILD_TESTS "Build tests" ON)

if (BUILD_TESTS)
    add_subdirectory(test)
endif()
