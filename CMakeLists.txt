cmake_minimum_required(VERSION 3.20)
project(fh2mouse VERSION 1.0.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(platform)   # fh2mod_platform (d3dcompiler_47.dll / DWrite.dll) and fh2mod_api

add_library(fh2mouse SHARED src/fh2mouse.cpp)
target_link_libraries(fh2mouse PRIVATE fh2mod_api user32)
set_target_properties(fh2mouse PROPERTIES OUTPUT_NAME FH2Mouse PREFIX "")
target_compile_definitions(fh2mouse PRIVATE FH2MOUSE_VERSION="${PROJECT_VERSION}"
                           _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(fh2mouse PRIVATE /W4 /utf-8)
else()
    # -fms-extensions: __try/__except around the calls into the game's UI code.
    target_compile_options(fh2mouse PRIVATE -Wall -Wextra -fms-extensions -O2)
    target_link_options(fh2mouse PRIVATE -static -s)
endif()
