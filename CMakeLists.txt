cmake_minimum_required(VERSION 3.28)
project(Lab2Os C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -D_GNU_SOURCE")

# Library source files
set(LIB_SOURCES
        vtpc.c
        cache.c
        direct_io.c
)

# Header files
set(LIB_HEADERS
        vtpc.h
        vtpc_internal.h
)

# Static library
add_library(vtpc_static STATIC ${LIB_SOURCES} ${LIB_HEADERS})
target_link_libraries(vtpc_static pthread)

# Shared library
add_library(vtpc_shared SHARED ${LIB_SOURCES} ${LIB_HEADERS})
target_link_libraries(vtpc_shared pthread)
set_target_properties(vtpc_shared PROPERTIES OUTPUT_NAME vtpc)

# Test executable
add_executable(test_vtpc test_vtpc.c)
target_link_libraries(test_vtpc vtpc_static pthread)

# Benchmark executable
add_executable(benchmark benchmark.c)
target_link_libraries(benchmark vtpc_static pthread)

# EMA Replace Int với VTPC
add_executable(ema_replace_int_vtpc ema_replace_int_vtpc.c)
target_link_libraries(ema_replace_int_vtpc vtpc_static pthread)