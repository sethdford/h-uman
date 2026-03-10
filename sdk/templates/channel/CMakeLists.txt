# Build my_channel as a static library for linking into human.
cmake_minimum_required(VERSION 3.20)
project(my_channel C)

add_library(my_channel STATIC my_channel.c)
target_include_directories(my_channel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(my_channel PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(my_channel PROPERTIES C_STANDARD 11)
