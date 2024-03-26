# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        2d924d7a971e9667d76ad09727fb2402b4f8a1e3
)
FetchContent_MakeAvailable(googletest)

file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
file(GLOB INC *.h *.hpp)

add_executable(openclon12test ${SRC} ${INC})
target_include_directories(openclon12test PRIVATE ../src/openclon12)
target_link_libraries(openclon12test openclon12 gtest_main opengl32 gdi32 user32)
