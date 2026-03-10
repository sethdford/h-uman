# Example: Weather tool
# Links against human (or include in main build)
cmake_minimum_required(VERSION 3.20)
project(weather_tool C)

add_library(weather_tool STATIC weather_tool.c)
target_include_directories(weather_tool PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
target_compile_options(weather_tool PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(weather_tool PROPERTIES C_STANDARD 11)
