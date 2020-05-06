# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
add_subdirectory(googletest)

file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
file(GLOB INC *.h *.hpp)

add_executable(openclon12test ${SRC} ${INC})
target_link_libraries(openclon12test openclon12 gtest_main)
