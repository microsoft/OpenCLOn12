cmake_minimum_required(VERSION 2.6)
project(openclon12)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SRC CONFIGURE_DEPENDS src/*.cpp)
file(GLOB INC include/*.h include/*.hpp)

configure_file(OpenCL-ICD-Loader/loader/icd_dispatch.h icd_dispatch.h COPYONLY)

add_library(openclon12 SHARED ${SRC} ${INC})
target_include_directories(openclon12
    PRIVATE include
    PRIVATE OpenCL-Headers
    PRIVATE wil/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(openclon12 d3d12translationlayer)
