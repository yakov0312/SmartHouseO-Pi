cmake_minimum_required(VERSION 3.16)

project(SmartH)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Runtime")

file(GLOB_RECURSE SOURCES
        "${CMAKE_SOURCE_DIR}/Application/*.cpp"
)

file(GLOB_RECURSE HEADERS
        "${CMAKE_SOURCE_DIR}/Application/*.h"
)

add_executable(SmartH
        ${SOURCES}
        ${HEADERS}
)

target_include_directories(SmartH PRIVATE
        "${CMAKE_SOURCE_DIR}/Application"
)

find_package(PostgreSQL REQUIRED)

target_link_libraries(SmartH
        PostgreSQL::PostgreSQL
)