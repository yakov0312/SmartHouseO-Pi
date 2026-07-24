cmake_minimum_required(VERSION 3.16)

project(SmartH)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES
        "${CMAKE_SOURCE_DIR}/*.cpp"
)

file(GLOB_RECURSE HEADERS
        "${CMAKE_SOURCE_DIR}/*.h"
)

list(FILTER SOURCES EXCLUDE REGEX "cmake-build-debug")
list(FILTER HEADERS EXCLUDE REGEX "cmake-build-debug")

add_executable(SmartH ${SOURCES} ${HEADERS})

target_include_directories(SmartH PRIVATE
        ${CMAKE_SOURCE_DIR}
)