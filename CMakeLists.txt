cmake_minimum_required(VERSION 3.21)
project(RMDB CXX C)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Debug，便于 Lab 调试；可通过 -DCMAKE_BUILD_TYPE=Release 覆盖。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# 通用警告；不再硬编码 -O0，由 BUILD_TYPE 决定优化级别。
add_compile_options(-Wall -Wno-unused-variable -Wno-unused-function)

# Debug/Release 的优化级别由 CMake 默认值控制；这里仅显式追加 -g 以便调试。
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG")

# 在 macOS（Apple Silicon）+ Homebrew 下，readline / flex 不在系统默认路径，
# 通过 CMAKE_PREFIX_PATH（由 preset 注入）让 find_xxx 命中 brew 安装位置。
find_library(READLINE_LIBRARY NAMES readline REQUIRED)
find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h REQUIRED)
message(STATUS "Found readline: ${READLINE_LIBRARY} (include: ${READLINE_INCLUDE_DIR})")

include_directories(${READLINE_INCLUDE_DIR})

enable_testing()
add_subdirectory(deps)
add_subdirectory(src)
