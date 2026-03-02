# CMake toolchain file for wasm32-wasi target
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/wasm32-wasi.cmake ..

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET wasm32-wasi)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --target=wasm32-wasi -Os")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--export-all -Wl,--no-entry")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
