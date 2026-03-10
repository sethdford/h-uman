# Build my_provider as a static library for linking into human.
# Adjust include path to match your human installation.
cmake_minimum_required(VERSION 3.20)
project(my_provider C)

add_library(my_provider STATIC my_provider.c)
target_include_directories(my_provider PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(my_provider PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(my_provider PROPERTIES C_STANDARD 11)
