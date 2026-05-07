include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(analyze)
add_subdirectory(record)
add_subdirectory(index)
add_subdirectory(system)
add_subdirectory(execution)
add_subdirectory(parser)
add_subdirectory(optimizer)
add_subdirectory(storage)
add_subdirectory(common)
add_subdirectory(replacer)
add_subdirectory(transaction)
add_subdirectory(recovery)
add_subdirectory(test)


target_link_libraries(parser execution pthread)

add_executable(rmdb rmdb.cpp)
# brew readline 的头文件不自包含 <stdio.h>（FILE 类型缺失），mac 上需要 force-include。
if(APPLE)
    target_compile_options(rmdb PRIVATE -include cstdio)
endif()
target_link_libraries(rmdb parser execution ${READLINE_LIBRARY} pthread planner analyze)
