cmake_minimum_required(VERSION 3.15)
project(Profiler VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PROFILER_SOURCE
  src/Profiler.cpp
)

add_library(Profiler STATIC ${PROFILER_SOURCE})

target_include_directories(Profiler PUBLIC "include/")


set(EXAMPLE_SOURCE
    usage/main.cpp
    usage/Application.cpp
)

add_executable(example ${EXAMPLE_SOURCE})


target_include_directories(example PUBLIC "include/")

target_link_libraries(example PRIVATE Profiler)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/compile_commands.json")
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
     "${CMAKE_BINARY_DIR}/compile_commands.json"
     "${CMAKE_SOURCE_DIR}/compile_commands.json")
endif()
