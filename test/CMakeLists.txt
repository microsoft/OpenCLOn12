# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        063de7e9578f82b369302001269680b4b1553359
)
FetchContent_MakeAvailable(googletest)

file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
file(GLOB INC *.h *.hpp)

add_executable(openclon12test ${SRC} ${INC})
target_include_directories(openclon12test PRIVATE ../src/openclon12)
target_link_libraries(openclon12test openclon12 gtest_main opengl32 gdi32 user32)
