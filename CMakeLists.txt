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