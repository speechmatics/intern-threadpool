# Copyright 2022 Cantab Research Ltd.
# Licensed under the MIT license. See LICENSE.txt in the project root for details.

cmake_minimum_required(VERSION 3.20)

project(Threadpool)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/if")
find_package(Boost REQUIRED)

find_package(Threads REQUIRED)

set(threadpool_sources
    main.cpp
)

list(TRANSFORM threadpool_sources PREPEND "src/")

add_executable(threadpool ${threadpool_sources})

set_property(TARGET threadpool PROPERTY CXX_STANDARD 20)

target_include_directories(threadpool PUBLIC "${CMAKE_SOURCE_DIR}/inc")

target_link_libraries(threadpool PRIVATE Threads::Threads Boost::coroutine Boost::system)

# Demo Project

set(CMAKE_PREFIX_PATH "lib/cmake/easy_profiler")

find_package(easy_profiler)

set(demo_sources
    demo.cpp
)

list(TRANSFORM demo_sources PREPEND "src/")

add_executable(demo ${demo_sources})

set_property(TARGET demo PROPERTY CXX_STANDARD 20)

target_include_directories(demo PUBLIC "${CMAKE_SOURCE_DIR}/inc")

target_link_libraries(demo PRIVATE Threads::Threads Boost::coroutine Boost::system easy_profiler)

target_compile_definitions(demo PUBLIC BUILD_WITH_EASY_PROFILER)
