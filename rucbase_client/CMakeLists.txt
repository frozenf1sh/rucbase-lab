cmake_minimum_required(VERSION 3.21)
project(rucbase_client CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_library(READLINE_LIBRARY NAMES readline REQUIRED)
find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${READLINE_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME}
        Threads::Threads
        ${READLINE_LIBRARY}
)
