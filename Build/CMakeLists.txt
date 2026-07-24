cmake_minimum_required(VERSION 3.16)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(MyProject)

file(GLOB_RECURSE SOURCES
        "${CMAKE_SOURCE_DIR}/*.cpp"
)

file(GLOB_RECURSE HEADERS
        "${CMAKE_SOURCE_DIR}/*.h"
)

add_executable(MyProgram ${SOURCES} ${HEADERS})

target_include_directories(MyProgram PRIVATE
        ${CMAKE_SOURCE_DIR}
)