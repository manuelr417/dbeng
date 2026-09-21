cmake_minimum_required(VERSION 4.3)
project(ex12)

set(CMAKE_CXX_STANDARD 20)

set(CORE_SOURCES
    sql/Token.h
    sql/Lexer.h sql/Lexer.cpp
    sql/Ast.h
    sql/Parser.h sql/Parser.cpp
    sql/AstPrinter.h sql/AstPrinter.cpp
    sql/AstDraw.h sql/AstDraw.cpp)

add_library(ex12core STATIC ${CORE_SOURCES})
target_include_directories(ex12core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(ex12 main.cpp)
target_link_libraries(ex12 PRIVATE ex12core)

add_executable(ex12_demo ex12_demo.cpp)
target_link_libraries(ex12_demo PRIVATE ex12core)
