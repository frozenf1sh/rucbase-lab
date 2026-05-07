find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

bison_target(yacc yacc.y ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
        DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h)
flex_target(lex lex.l ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp)
add_flex_bison_dependency(lex yacc)

set(SOURCES ${BISON_yacc_OUTPUT_SOURCE} ${FLEX_lex_OUTPUTS} ast.cpp)
add_library(parser STATIC ${SOURCES})
# brew 的 flex 把 FlexLexer.h 安装在 keg-only 目录下，需要显式 include。
if(FLEX_INCLUDE_DIRS)
    target_include_directories(parser PUBLIC ${FLEX_INCLUDE_DIRS})
endif()

add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser parser)
add_test(NAME test_parser COMMAND test_parser
        WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
