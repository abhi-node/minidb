cmake_minimum_required(VERSION 3.20)
project(minidb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warnings are non-negotiable
add_compile_options(-Wall -Wextra -Wpedantic)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
