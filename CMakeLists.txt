cmake_minimum_required(VERSION 3.13)
project(openclon12)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SRC CONFIGURE_DEPENDS src/*.cpp src/*.def)
file(GLOB INC include/*.h include/*.hpp)
file(GLOB EXTERNAL_INC external/*.h external/*.hpp)

add_library(openclon12 SHARED ${SRC} ${INC} ${EXTERNAL_INC})
target_include_directories(openclon12
    PRIVATE include
    PRIVATE external
    PUBLIC OpenCL-Headers
    PRIVATE wil/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(openclon12 d3d12translationlayer)
source_group("Header Files\\External" FILES ${EXTERNAL_INC})
