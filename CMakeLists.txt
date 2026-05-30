cmake_minimum_required(VERSION 3.20)
project(fdm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(CURL REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql Network Concurrent)

enable_testing()

add_subdirectory(engine)
add_subdirectory(store)
add_subdirectory(cli)
add_subdirectory(gui)
add_subdirectory(host)
add_subdirectory(tests)
