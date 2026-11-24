cmake_minimum_required(VERSION 3.20)
project(minidb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimized by default, with debug info so profilers and Valgrind can name frames.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Warnings are non-negotiable.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# pread/pwrite, fsync, clock_gettime and sigaction come from POSIX.1-2008.
add_compile_definitions(_XOPEN_SOURCE=700)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src)
add_subdirectory(bench)

enable_testing()
add_subdirectory(tests)
