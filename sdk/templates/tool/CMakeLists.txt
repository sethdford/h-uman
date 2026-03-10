# Build my_tool as a static library for linking into human.
cmake_minimum_required(VERSION 3.20)
project(my_tool C)

add_library(my_tool STATIC my_tool.c)
target_include_directories(my_tool PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(my_tool PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(my_tool PROPERTIES C_STANDARD 11)
