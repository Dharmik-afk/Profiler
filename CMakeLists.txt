cmake_minimum_required(VERSION 3.15)
project(Profiler VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(EXAMPLE "Build example code" OFF)


set(PROFILER_SOURCE
  src/Profiler.cpp
)

add_library(Profiler STATIC ${PROFILER_SOURCE})

target_include_directories(Profiler PUBLIC "include/")

if (EXAMPLE)
  set(EXAMPLE_SOURCE
    usage/main.cpp
    usage/Application.cpp
  )

  add_executable(example ${EXAMPLE_SOURCE})


  target_include_directories(example PUBLIC "include/")

  target_link_libraries(example PRIVATE Profiler)
endif()
